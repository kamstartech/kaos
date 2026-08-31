/* kaos-ui-capture: headless phoc frame capture -> kaos-display-bridge
 *
 * Replaces wf-recorder | ffmpeg in kaos-ui-stream.service.
 *
 * Connects to phoc's Wayland socket, uses wlr-screencopy-unstable-v1 to
 * capture the headless compositor output as RGBA8888, and writes raw
 * frames to /dev/socket/kaos_ui.sock (a unix stream socket consumed by
 * kaos-display-bridge on the Android side).
 *
 * Protocol expected by kaos-display-bridge: a continuous byte stream of
 * raw RGBA8888 pixels, width*height*4 bytes per frame, no header.
 *
 * wlroots 0.17 / phoc 0.38.0 / libwayland 1.22.0 / wlr-screencopy v3.
 *
 * Build (inside the chroot, native glibc toolchain):
 *   wayland-scanner client-header < wlr-screencopy-unstable-v1.xml > wlr-screencopy-unstable-v1-client-protocol.h
 *   wayland-scanner private-code < wlr-screencopy-unstable-v1.xml > wlr-screencopy-unstable-v1-client-protocol.c
 *   g++ -O2 -Wall -o kaos-ui-capture kaos-ui-capture.cpp wlr-screencopy-unstable-v1-client-protocol.c $(pkg-config --cflags --libs wayland-client)
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
    wl_buffer *buffer = nullptr;
    void *shm_data = nullptr;
    uint32_t shm_size = 0;

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

// Write exactly len bytes; returns false on unrecoverable error.
static bool write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Should not happen on blocking socket, but be safe.
                poll(nullptr, 0, 1);
                continue;
            }
            LOGE("write failed: %s", strerror(errno));
            return false;
        }
        if (n == 0) {
            LOGE("write returned 0");
            return false;
        }
        done += (size_t)n;
    }
    return true;
}

// Convert whatever wl_shm format phoc gave us into RGBA8888.
// Supported formats: ARGB8888, XRGB8888, ABGR8888, RGBA8888, BGRA8888.
static bool convert_to_rgba(const FrameCtx &f, uint8_t *dst) {
    const uint8_t *src = (const uint8_t *)f.shm_data;
    const uint32_t h = f.height;
    const uint32_t w = f.width;
    const uint32_t src_stride = f.stride;
    const uint32_t dst_stride = w * 4;

    auto copy_line = [&](uint32_t y, auto convert_pixel) {
        const uint8_t *s = src + y * src_stride;
        uint8_t *d = dst + y * dst_stride;
        for (uint32_t x = 0; x < w; ++x) {
            convert_pixel(s + x * 4, d + x * 4);
        }
    };

    switch (f.format) {
        case WL_SHM_FORMAT_ARGB8888:
        case WL_SHM_FORMAT_XRGB8888:
            // Source in memory (little-endian): B G R A or B G R X
            for (uint32_t y = 0; y < h; ++y) {
                copy_line(y, [](const uint8_t *s, uint8_t *d) {
                    d[0] = s[2]; // R
                    d[1] = s[1]; // G
                    d[2] = s[0]; // B
                    d[3] = 0xFF; // A (ignore source alpha for screen capture)
                });
            }
            return true;

        case WL_SHM_FORMAT_ABGR8888:
        case WL_SHM_FORMAT_XBGR8888:
            // Source in memory: R G B A or R G B X
            for (uint32_t y = 0; y < h; ++y) {
                copy_line(y, [](const uint8_t *s, uint8_t *d) {
                    d[0] = s[0]; // R
                    d[1] = s[1]; // G
                    d[2] = s[2]; // B
                    d[3] = 0xFF; // A
                });
            }
            return true;

        case WL_SHM_FORMAT_RGBA8888:
            // Already RGBA in memory.
            if (src_stride == dst_stride) {
                memcpy(dst, src, h * dst_stride);
            } else {
                for (uint32_t y = 0; y < h; ++y) {
                    memcpy(dst + y * dst_stride, src + y * src_stride, dst_stride);
                }
            }
            return true;

        case WL_SHM_FORMAT_BGRA8888:
            // Source in memory: B G R A
            for (uint32_t y = 0; y < h; ++y) {
                copy_line(y, [](const uint8_t *s, uint8_t *d) {
                    d[0] = s[2]; // R
                    d[1] = s[1]; // G
                    d[2] = s[0]; // B
                    d[3] = 0xFF; // A
                });
            }
            return true;

        default:
            LOGE("Unsupported wl_shm format 0x%08x (%c%c%c%c)",
                 f.format,
                 (f.format) & 0xFF,
                 (f.format >> 8) & 0xFF,
                 (f.format >> 16) & 0xFF,
                 (f.format >> 24) & 0xFF);
            return false;
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

    std::vector<uint8_t> rgba_frame;
    int consecutive_failures = 0;
    uint64_t frame_count = 0;
    uint64_t bytes_sent = 0;
    bool exit_failure = false;

    while (g_running) {
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

        f.shm_size = f.stride * f.height;
        int fd = create_shm_buffer(f.shm_size);
        if (fd < 0) {
            zwlr_screencopy_frame_v1_destroy(f.frame);
            exit_failure = true;
            goto cleanup;
        }

        f.shm_data = mmap(nullptr, f.shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (f.shm_data == MAP_FAILED) {
            LOGE("mmap failed: %s", strerror(errno));
            close(fd);
            zwlr_screencopy_frame_v1_destroy(f.frame);
            exit_failure = true;
            goto cleanup;
        }

        wl_shm_pool *pool = wl_shm_create_pool(g.shm, fd, f.shm_size);
        f.buffer = wl_shm_pool_create_buffer(pool, 0, width, height, f.stride, f.format);
        wl_shm_pool_destroy(pool);

        // Close fd after passing to pool; wayland keeps its own ref via mmap'd fd.
        close(fd);

        // Copy the next frame unconditionally. copy_with_damage was tried but
        // can hang on headless backends if the compositor considers the output
        // already up-to-date and never emits a damage/ready cycle.
        zwlr_screencopy_frame_v1_copy(f.frame, f.buffer);

        // Wait for ready/failed.
        while (f.state != FrameState::READY && f.state != FrameState::FAILED && g_running) {
            if (wl_display_dispatch(display) < 0) {
                LOGE("wl_display_dispatch failed");
                exit_failure = true;
                goto cleanup;
            }
        }

        if (f.state == FrameState::READY) {
            consecutive_failures = 0;

            rgba_frame.resize(width * height * 4);
            if (!convert_to_rgba(f, rgba_frame.data())) {
                // keep buffer alive until after munmap
            } else {
                if (!write_all(sock_fd, rgba_frame.data(), rgba_frame.size())) {
                    LOGE("Frame socket write failed; reconnecting");
                    close(sock_fd);
                    sock_fd = connect_frame_socket();
                    if (sock_fd < 0) {
                        LOGE("Reconnect failed");
                        exit_failure = true;
                        goto cleanup;
                    }
                    // Retry writing this frame once.
                    if (!write_all(sock_fd, rgba_frame.data(), rgba_frame.size())) {
                        LOGE("Retry write failed");
                        exit_failure = true;
                        goto cleanup;
                    }
                }
                frame_count++;
                bytes_sent += rgba_frame.size();
                if (frame_count % 300 == 0) {
                    LOGI("Captured %llu frames, %llu MiB sent",
                         (unsigned long long)frame_count,
                         (unsigned long long)(bytes_sent / (1024 * 1024)));
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

        // Cleanup per-frame objects.
        if (f.buffer) {
            wl_buffer_destroy(f.buffer);
        }
        if (f.shm_data && f.shm_data != MAP_FAILED) {
            munmap(f.shm_data, f.shm_size);
        }
        if (f.frame) {
            zwlr_screencopy_frame_v1_destroy(f.frame);
        }

        // Throttle to ~5 fps. Continuous 30 fps readback appears to stress
        // phoc's headless renderer and eventually causes screencopy failures.
        usleep(200000);
    }

cleanup:
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
