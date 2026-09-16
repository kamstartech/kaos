/* kaos-ui-capture: headless phoc frame capture -> kaos-display-bridge
 *
 * Replaces wf-recorder | ffmpeg in kaos-ui-stream.service.
 *
 * Connects to phoc's Wayland socket, uses wlr-screencopy-unstable-v1 to
 * capture the headless compositor output, and hands each frame to
 * kaos-display-bridge (Android side) over /dev/socket/kaos_ui.sock by
 * passing the shm buffer's fd with sendmsg(SCM_RIGHTS) — no multi-megabyte
 * pixel stream. Wire format ("KFR1"): a 24-byte header
 * {magic, seq, width, height, stride} followed by one fd via SCM_RIGHTS.
 * The bridge mmaps the fd, uploads it as a GL texture, and returns the seq
 * as a 4-byte ack; the capture rotates two buffers and only reuses a buffer
 * after its ack, so the compositor copy of frame N+1 overlaps the bridge
 * draw of frame N.
 *
 * Pixel layout on the wire is always memory bytes R,G,B,X (= GL_RGBA /
 * GL_UNSIGNED_BYTE, little-endian); the bridge's blit shader forces alpha
 * 1.0, so the X byte is never sampled. BGRA8888 shm frames are swizzled
 * in place before the fd is passed.
 *
 * wlroots 0.17 / phoc 0.38.0 / libwayland 1.22.0 / wlr-screencopy v3.
 *
 * Build (inside the chroot, native glibc toolchain):
 *   wayland-scanner client-header < wlr-screencopy-unstable-v1.xml > wlr-screencopy-unstable-v1-client-protocol.h
 *   wayland-scanner private-code < wlr-screencopy-unstable-v1.xml > wlr-screencopy-unstable-v1-client-protocol.c
 *   g++ -O2 -Wall -o kaos-ui-capture kaos-ui-capture.cpp $(pkg-config --cflags --libs wayland-client)
 *   (the protocol .c is #included by the .cpp; do NOT pass it separately)
 */

#include <wayland-client.h>
#include "wlr-screencopy-unstable-v1-client-protocol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <csignal>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>

static double now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
#include <sys/stat.h>

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int) {
    g_running = 0;
}

#define FRAME_SOCKET_PATH "/dev/socket/kaos_ui.sock"
#define LOG_TAG "KaosUICapture"

#define LOGI(fmt, ...) fprintf(stderr, "I/" LOG_TAG ": " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...) fprintf(stderr, "E/" LOG_TAG ": " fmt "\n", ##__VA_ARGS__)

enum class FrameState {
    PENDING_BUFFER,
    PENDING_COPY,
    READY,
    FAILED,
};

struct FrameCtx {
    zwlr_screencopy_frame_v1 *frame = nullptr;

    uint32_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;

    FrameState state = FrameState::PENDING_BUFFER;
    bool buffer_done = false;
    uint32_t flags = 0;

    int frame_failures = 0;
};

struct OutputCtx {
    wl_output *output = nullptr;
    uint32_t global_id = 0;
    int version = 0;
    bool used = false;
};

struct Globals {
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_shm *shm = nullptr;
    zwlr_screencopy_manager_v1 *screencopy = nullptr;
    OutputCtx output;
};

static int create_shm_buffer(int size) {
    char name[] = "/tmp/kaos-ui-capture-XXXXXX";
    int fd = mkstemp(name);
    if (fd < 0) {
        LOGE("mkstemp failed: %s", strerror(errno));
        return -1;
    }
    unlink(name);
    if (ftruncate(fd, size) < 0) {
        LOGE("ftruncate failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int connect_frame_socket() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOGE("socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FRAME_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE("connect(%s) failed: %s", FRAME_SOCKET_PATH, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

// Swap R and B bytes in place (memory B,G,R,A -> R,G,B,A). Used when phoc
// serves WL_SHM_FORMAT_BGRA8888; every other supported shm format already
// has R,G,B in memory byte order, which is what the bridge samples.
static void swizzle_bgra_in_place(uint8_t *data, uint32_t width, uint32_t height, uint32_t stride) {
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t *row = data + y * stride;
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t *p = row + x * 4;
            uint8_t t = p[0]; p[0] = p[2]; p[2] = t;
        }
    }
}

static void frame_buffer(void *data, zwlr_screencopy_frame_v1 *,
                         uint32_t format, uint32_t width, uint32_t height,
                         uint32_t stride) {
    FrameCtx *f = (FrameCtx *)data;
    f->format = format;
    f->width = width;
    f->height = height;
    f->stride = stride;
}

static void frame_flags(void *data, zwlr_screencopy_frame_v1 *, uint32_t flags) {
    FrameCtx *f = (FrameCtx *)data;
    f->flags = flags;
}

static void frame_ready(void *data, zwlr_screencopy_frame_v1 *,
                        uint32_t, uint32_t, uint32_t) {
    FrameCtx *f = (FrameCtx *)data;
    f->state = FrameState::READY;
}

static void frame_failed(void *data, zwlr_screencopy_frame_v1 *) {
    FrameCtx *f = (FrameCtx *)data;
    f->state = FrameState::FAILED;
}

static void frame_buffer_done(void *data, zwlr_screencopy_frame_v1 *) {
    FrameCtx *f = (FrameCtx *)data;
    f->buffer_done = true;
}

static void frame_damage(void *, zwlr_screencopy_frame_v1 *,
                         uint32_t, uint32_t, uint32_t, uint32_t) {
    // Ignored; we do full-frame copies only.
}

static void frame_linux_dmabuf(void *, zwlr_screencopy_frame_v1 *,
                               uint32_t, uint32_t, uint32_t) {
    // Ignored; we use wl_shm.
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = frame_buffer,
    .flags = frame_flags,
    .ready = frame_ready,
    .failed = frame_failed,
    .damage = frame_damage,
    .linux_dmabuf = frame_linux_dmabuf,
    .buffer_done = frame_buffer_done,
};

static void registry_global(void *data, wl_registry *registry,
                            uint32_t id, const char *interface,
                            uint32_t version) {
    Globals *g = (Globals *)data;
    if (strcmp(interface, wl_shm_interface.name) == 0) {
        g->shm = (wl_shm *)wl_registry_bind(registry, id, &wl_shm_interface, 1);
        LOGI("bound wl_shm");
    } else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
        int v = version < 3 ? (int)version : 3;
        g->screencopy = (zwlr_screencopy_manager_v1 *)wl_registry_bind(
            registry, id, &zwlr_screencopy_manager_v1_interface, v);
        LOGI("bound zwlr_screencopy_manager_v1 version %d", v);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (!g->output.used) {
            int v = version < 4 ? (int)version : 4;
            g->output.output = (wl_output *)wl_registry_bind(
                registry, id, &wl_output_interface, v);
            g->output.global_id = id;
            g->output.version = v;
            g->output.used = true;
            LOGI("bound wl_output id=%u version %d", id, v);
        }
    }
}

static void registry_global_remove(void *, wl_registry *, uint32_t) {}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int main(int argc, char *argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    // Optional command-line: kaos-ui-capture [width height]
    // If not given, use the compositor-reported output size.
    uint32_t forced_width = 0;
    uint32_t forced_height = 0;
    if (argc == 3) {
        forced_width = (uint32_t)atoi(argv[1]);
        forced_height = (uint32_t)atoi(argv[2]);
    }

    const char *display_name = getenv("WAYLAND_DISPLAY");
    if (!display_name) display_name = "wayland-0";

    wl_display *display = wl_display_connect(display_name);
    if (!display) {
        LOGE("Cannot connect to Wayland display '%s'", display_name);
        return 1;
    }

    Globals g{};
    g.display = display;
    g.registry = wl_display_get_registry(display);
    wl_registry_add_listener(g.registry, &registry_listener, &g);
    wl_display_roundtrip(display);

    if (!g.shm) {
        LOGE("No wl_shm");
        return 1;
    }
    if (!g.screencopy) {
        LOGE("No zwlr_screencopy_manager_v1");
        return 1;
    }
    if (!g.output.output) {
        LOGE("No wl_output");
        return 1;
    }

    int sock_fd = connect_frame_socket();
    if (sock_fd < 0) {
        return 1;
    }
    LOGI("Connected to %s", FRAME_SOCKET_PATH);

    // ------------------------------------------------------------------
    // KFR1 frame protocol: instead of streaming ~10 MB of pixels per frame
    // over the socket, the shm buffer's fd is passed with sendmsg(SCM_RIGHTS)
    // plus a small header, and kaos-display-bridge mmaps the same memory.
    // The bridge returns a 4-byte seq ack after uploading the frame; a
    // buffer is never re-copied while its fd is still with the bridge.
    // Two buffers are rotated so the compositor copy of frame N+1 overlaps
    // the bridge's texture upload of frame N.
    //
    // Wire pixel layout is always memory bytes R,G,B,X (= GL_RGBA /
    // GL_UNSIGNED_BYTE on little-endian). The bridge's blit fragment shader
    // forces alpha to 1.0, so the X byte is never sampled. ABGR/XBGR/RGBA
    // shm formats already have R,G,B in memory order; BGRA8888 is swizzled
    // in place before the fd is sent.
    struct FrameHdr {
        char magic[4];      // "KFR1"
        uint32_t seq;
        uint32_t width;
        uint32_t height;
        uint32_t stride;
    };

    struct CapBuffer {
        int fd = -1;
        uint8_t *data = nullptr;
        size_t size = 0;
        uint32_t width = 0, height = 0, stride = 0, format = 0;
        wl_buffer *wbuf = nullptr;
        int inflight_seq = -1;  // -1 = idle, else seq awaiting bridge ack
    };
    CapBuffer bufs[2];
    uint32_t next_seq = 1;

    auto release_buffer = [](CapBuffer &b) {
        if (b.wbuf) wl_buffer_destroy(b.wbuf);
        if (b.data) munmap(b.data, b.size);
        if (b.fd >= 0) close(b.fd);
        b = CapBuffer{};
    };

    auto setup_buffer = [&](CapBuffer &b, uint32_t width, uint32_t height,
                            uint32_t stride, uint32_t format) -> bool {
        release_buffer(b);
        b.size = (size_t)stride * height;
        b.fd = create_shm_buffer((int)b.size);
        if (b.fd < 0) return false;
        b.data = (uint8_t *)mmap(nullptr, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, b.fd, 0);
        if (b.data == MAP_FAILED) {
            LOGE("mmap failed: %s", strerror(errno));
            close(b.fd);
            b = CapBuffer{};
            return false;
        }
        wl_shm_pool *pool = wl_shm_create_pool(g.shm, b.fd, (int)b.size);
        b.wbuf = wl_shm_pool_create_buffer(pool, 0, width, height, stride, format);
        wl_shm_pool_destroy(pool);
        b.width = width; b.height = height; b.stride = stride; b.format = format;
        return true;
    };

    auto send_frame_fd = [&](CapBuffer &b, uint32_t seq) -> bool {
        FrameHdr hdr = {{'K', 'F', 'R', '1'}, seq, b.width, b.height, b.stride};
        struct iovec iov = {&hdr, sizeof(hdr)};
        char cbuf[CMSG_SPACE(sizeof(int))];
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cbuf;
        msg.msg_controllen = sizeof(cbuf);
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &b.fd, sizeof(int));
        msg.msg_controllen = c->cmsg_len;
        ssize_t n = sendmsg(sock_fd, &msg, 0);
        if (n != (ssize_t)sizeof(hdr)) {
            LOGE("sendmsg failed: %s", strerror(errno));
            return false;
        }
        b.inflight_seq = (int)seq;
        return true;
    };

    // Read pending acks. block=true waits for at least one ack (used when
    // both buffers are in flight). Returns false on EOF/fatal socket error;
    // the caller then reconnects and resets all inflight state.
    auto pump_acks = [&](bool block) -> bool {
        bool got_any = false;
        for (;;) {
            uint32_t ack;
            ssize_t n = recv(sock_fd, &ack, sizeof(ack), block && !got_any ? 0 : MSG_DONTWAIT);
            if (n == (ssize_t)sizeof(ack)) {
                got_any = true;
                for (auto &b : bufs) {
                    if (b.inflight_seq == (int)ack) b.inflight_seq = -1;
                }
                continue;
            }
            if (n == 0) {
                LOGE("Frame socket closed by bridge");
                return false;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
            LOGE("Ack recv failed: %s", strerror(errno));
            return false;
        }
    };

    auto reset_after_reconnect = [&]() {
        for (auto &b : bufs) b.inflight_seq = -1;
    };

    int consecutive_failures = 0;
    uint64_t frame_count = 0;
    bool exit_failure = false;
    const bool timing = getenv("KAOS_TIMING") != nullptr;

    // Timing accumulators (only logged when KAOS_TIMING=1).
    double t_last_done = 0.0, period_sum = 0.0, copy_sum = 0.0, ack_sum = 0.0;
    uint64_t timed_frames = 0;

    while (g_running) {
        // Wait until a buffer is free (its ack arrived).
        double t_ack0 = now_ms();
        if (!pump_acks(false)) {
            close(sock_fd);
            sock_fd = connect_frame_socket();
            if (sock_fd < 0) { exit_failure = true; goto cleanup; }
            reset_after_reconnect();
        }
        while (bufs[0].inflight_seq >= 0 && bufs[1].inflight_seq >= 0 && g_running) {
            if (!pump_acks(true)) {
                close(sock_fd);
                sock_fd = connect_frame_socket();
                if (sock_fd < 0) { exit_failure = true; goto cleanup; }
                reset_after_reconnect();
            }
        }
        double t_ack = now_ms();
        CapBuffer *buf = (bufs[0].inflight_seq < 0) ? &bufs[0] : &bufs[1];

        double t_req = now_ms();
        FrameCtx f{};
        f.frame = zwlr_screencopy_manager_v1_capture_output(
            g.screencopy, 0 /* overlay_cursor */, g.output.output);
        zwlr_screencopy_frame_v1_add_listener(f.frame, &frame_listener, &f);

        // Wait for buffer event(s) and buffer_done.
        while (!f.buffer_done && f.state != FrameState::FAILED && g_running) {
            if (wl_display_dispatch(display) < 0) {
                LOGE("wl_display_dispatch failed");
                exit_failure = true;
                goto cleanup;
            }
        }

        if (f.state == FrameState::FAILED) {
            consecutive_failures++;
            if (consecutive_failures >= 10) {
                LOGE("Frame failed before buffer %d times, giving up", consecutive_failures);
                zwlr_screencopy_frame_v1_destroy(f.frame);
                exit_failure = true;
                goto cleanup;
            }
            if (consecutive_failures % 5 == 1) {
                LOGE("Frame failed before buffer (%d/10)", consecutive_failures);
            }
            zwlr_screencopy_frame_v1_destroy(f.frame);
            usleep(200000);
            continue;
        }

        uint32_t width = forced_width ? forced_width : f.width;
        uint32_t height = forced_height ? forced_height : f.height;

        if (width == 0 || height == 0 || f.stride == 0) {
            LOGE("Invalid frame geometry %dx%d stride=%u", width, height, f.stride);
            zwlr_screencopy_frame_v1_destroy(f.frame);
            consecutive_failures++;
            usleep(100000);
            continue;
        }

        if (buf->wbuf == nullptr || buf->width != width || buf->height != height ||
            buf->stride != f.stride || buf->format != f.format) {
            LOGI("Allocating shared buffer %dx%d stride=%u format=0x%08x",
                 width, height, f.stride, f.format);
            if (!setup_buffer(*buf, width, height, f.stride, f.format)) {
                zwlr_screencopy_frame_v1_destroy(f.frame);
                exit_failure = true;
                goto cleanup;
            }
        }

        // Copy the next frame unconditionally. copy_with_damage was tried but
        // can hang on headless backends if the compositor considers the output
        // already up-to-date and never emits a damage/ready cycle.
        zwlr_screencopy_frame_v1_copy(f.frame, buf->wbuf);

        // Wait for ready/failed.
        while (f.state != FrameState::READY && f.state != FrameState::FAILED && g_running) {
            if (wl_display_dispatch(display) < 0) {
                LOGE("wl_display_dispatch failed");
                exit_failure = true;
                goto cleanup;
            }
        }

        zwlr_screencopy_frame_v1_destroy(f.frame);

        if (f.state == FrameState::READY) {
            consecutive_failures = 0;
            double t_ready = now_ms();

            if (f.format == WL_SHM_FORMAT_BGRA8888) {
                swizzle_bgra_in_place(buf->data, width, height, f.stride);
            }

            if (!send_frame_fd(*buf, next_seq)) {
                close(sock_fd);
                sock_fd = connect_frame_socket();
                if (sock_fd < 0) { exit_failure = true; goto cleanup; }
                reset_after_reconnect();
            } else {
                next_seq++;
                frame_count++;
                double t_done = now_ms();
                if (timing) {
                    if (t_last_done > 0) period_sum += t_done - t_last_done;
                    t_last_done = t_done;
                    copy_sum += t_ready - t_req;
                    ack_sum += t_ack - t_ack0;
                    timed_frames++;
                    if (timed_frames % 60 == 0) {
                        LOGI("TIMING avg period=%.1fms copy=%.1fms ackwait=%.1fms (n=%llu)",
                             period_sum / timed_frames, copy_sum / timed_frames,
                             ack_sum / timed_frames, (unsigned long long)timed_frames);
                    }
                }
                if (frame_count % 300 == 0) {
                    LOGI("Captured %llu frames (%llu fds passed)",
                         (unsigned long long)frame_count, (unsigned long long)frame_count);
                }
            }
        } else {
            LOGE("Frame copy failed");
            consecutive_failures++;
            if (consecutive_failures >= 10) {
                LOGE("Too many consecutive frame failures, exiting");
                exit_failure = true;
                goto cleanup;
            }
            usleep(50000);
        }

        // Frame pacing floor. With two rotating buffers the steady-state
        // period is max(screencopy copy, bridge draw) plus this sleep; the
        // 10 ms floor keeps idle readback from hammering phoc's renderer.
        usleep(10000);
    }

cleanup:
    for (auto &b : bufs) release_buffer(b);
    close(sock_fd);
    if (g.output.output) wl_output_destroy(g.output.output);
    if (g.screencopy) zwlr_screencopy_manager_v1_destroy(g.screencopy);
    if (g.shm) wl_shm_destroy(g.shm);
    if (g.registry) wl_registry_destroy(g.registry);
    wl_display_disconnect(display);
    return exit_failure ? 1 : 0;
}

// Include generated protocol code in this translation unit so its symbols are
// visible to the C++ code above. wayland-scanner's private-code output marks
// interface structs as hidden visibility, which breaks separate compilation.
#include "wlr-screencopy-unstable-v1-client-protocol.c"
