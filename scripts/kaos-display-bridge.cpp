/* kaos-display-bridge: SurfaceFlinger surface + compositor + touch daemon
 *
 * Runs on the Android side (as root). Creates a fullscreen SurfaceFlinger
 * surface via libgui's SurfaceComposerClient (the same real API any native
 * AOSP system service uses -- bootanimation, screenrecord, etc.) and
 * renders the chroot's desktop into it.
 *
 * This used to go through libhybris's surface_flinger_compatibility_layer
 * instead. Dropped 2026-08-28: that layer exists to bridge glibc/non-Android
 * processes to SurfaceFlinger across the bionic ABI gap, which this process
 * never needed -- it's already a native bionic AOSP binary. It also hides a
 * real bug: its createSurface() call hardcodes flags=0x300, and 0x200 of
 * that isn't a defined ISurfaceComposerClient flag anywhere in this Android
 * 15 tree (only 0x100 is, eNonPremultiplied) -- a leftover from whatever
 * far older Android version that Ubuntu-Touch-era shim was last updated
 * against. Confirmed live 2026-08-28: even with every other candidate ruled
 * out (surface layer value fixed, EGL window-surface connection deferred
 * past startup entirely), SurfaceFlinger still retriggered ctl.start for
 * bootanim and restarted zygote at the exact instant of the compat layer's
 * createSurface() call, every single time. Calling libgui directly instead
 * uses PIXEL_FORMAT_RGBA_8888 with flags=0 (its own documented default) --
 * no undefined bits, no vendored third-party shim in the path at all.
 *
 * Phoc itself cannot render into this surface directly — it's a stock
 * apt-installed binary (phoc 0.38.0+ds-1 / libwlroots.so.12), not built
 * from source anywhere in this tree, so it can't be given a custom
 * SurfaceFlinger-aware backend. Instead, kaos-ui-capture (a
 * wlr-screencopy client, replacing the old wf-recorder|ffmpeg pipeline)
 * captures phoc's headless output and hands frames over
 * /dev/socket/kaos_ui.sock (already bind-mounted, host-and-chroot-visible)
 * using the KFR1 protocol: a 24-byte header {magic, seq, width, height,
 * stride} plus the frame's shm fd via SCM_RIGHTS — no multi-megabyte pixel
 * stream. This daemon is the listener on that socket: it mmaps the passed
 * fd, uploads it as a GL texture, draws it into the SF surface with a
 * single textured-quad blit (avoiding any Android Activity/View/Bitmap
 * compositing round-trip), and returns the seq as a 4-byte ack, which the
 * capture uses to gate buffer reuse.
 *
 * Touch handling (added 2026-08-31): instead of relying on a transparent
 * Android Activity window to receive MotionEvents and forward them, this
 * daemon reads the physical touchscreen's evdev node directly (it runs as
 * root) and forwards int32_t[4] packets to kaos-touch-input's Unix socket.
 * That removes the Activity from the critical touch path, bypassing the
 * z-order / untrusted-window occlusion problem that blocked touches when
 * the SF surface sat above PhoshDisplayActivity.
 *
 * Listens on /dev/socket/kaos_display_ctl.sock for single-byte commands
 * from the PhoshDisplayActivity Android app:
 *   CMD_SHOW (0x01) — set surface alpha=1.0, resume rendering, enable touch
 *   CMD_HIDE (0x02) — set surface alpha=0.0, pause rendering, disable touch
 *   CMD_QUIT (0xFF) — destroy surface and exit
 *
 * The surface is created below PhoshDisplayActivity's window layer so the
 * transparent activity sits on top and can consume touches (preventing them
 * from reaching Android UI behind) while this daemon forwards them to Linux.
 *
 * Build (AOSP):
 *   Included in kaos/scripts/Android.mk
 *
 * Usage:
 *   /system/bin/kaos-display-bridge &
 *   (started by kaos-service once phoc/wayland-0 is up)
 */

// Must precede any header that might transitively include log/log.h
// (binder/ProcessState.h does) -- that header defines LOG_TAG as NULL,
// and defining our own after it hits a macro-redefinition error under
// -Werror.
#define LOG_TAG "KaosDisplayBridge"

#include <cstdio>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <cstdint>
#include <unistd.h>
#include <fcntl.h>
#include <glob.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <linux/input.h>
#include <errno.h>
#include <poll.h>
#include <time.h>

static double now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <binder/ProcessState.h>
#include <cutils/properties.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/SurfaceControl.h>
#include <gui/Surface.h>
#include <ui/DisplayMode.h>
#include <ui/PixelFormat.h>
#include <utils/String8.h>

using android::sp;
using android::SurfaceComposerClient;
using android::SurfaceControl;
using android::Surface;
using android::PhysicalDisplayId;
using android::String8;

#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) do { fprintf(stderr, "I/" LOG_TAG ": "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#define LOGD(...) do { fprintf(stderr, "D/" LOG_TAG ": "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#define LOGE(...) do { fprintf(stderr, "E/" LOG_TAG ": "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#endif

#define CTL_SOCKET_PATH    "/dev/socket/kaos_display_ctl.sock"
#define FRAME_SOCKET_PATH  "/dev/socket/kaos_ui.sock"
#define INPUT_SOCKET_PATH  "/dev/socket/kaos_input.sock"
// Persistent SOCK_STREAM session link for PhoshDisplayActivity (KaosDisplayLink.java).
// Distinct from CTL_SOCKET_PATH above, which stays SOCK_DGRAM fire-and-forget
// for the Launcher "-1 screen" plugin (KaosOverlayNative) -- that caller has
// no live-disconnect story yet and doesn't need one. This socket's whole
// point is that connect()/EOF ARE the show/hide-availability events: a
// client connecting means "show", and the client's fd closing for any
// reason (explicit stop, crash, app killed) means "hide", detected by the
// kernel the instant it happens instead of the app polling or guessing.
#define LINK_SOCKET_PATH   "/dev/socket/kaos_display_link.sock"

static const uint8_t CMD_SHOW = 0x01;
static const uint8_t CMD_HIDE = 0x02;
static const uint8_t CMD_QUIT = 0xFF;

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int) {
    g_running = 0;
}

static const char *VERTEX_SHADER_SRC =
    "attribute vec2 aPos;\n"
    "attribute vec2 aTexCoord;\n"
    "varying vec2 vTexCoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "    vTexCoord = aTexCoord;\n"
    "}\n";

static const char *FRAGMENT_SHADER_SRC =
    "precision mediump float;\n"
    "varying vec2 vTexCoord;\n"
    "uniform sampler2D uTexture;\n"
    "void main() {\n"
    // Force alpha=1.0 rather than passing through the captured frame's own
    // alpha channel -- confirmed live 2026-08-28: phoc's headless output
    // doesn't populate a meaningful per-pixel alpha (a screen capture has
    // no reason to), so honoring it made the whole layer composite as
    // fully transparent even with the *layer's* own alpha at 1.0 (CMD_SHOW
    // processed correctly, "Surface SHOWN" logged, screenshot still showed
    // nothing -- SurfaceFlinger blends using the buffer's real per-pixel
    // alpha regardless of the layer-level alpha being set to opaque). A
    // captured screen output should always be treated as fully opaque.
    "    gl_FragColor = vec4(texture2D(uTexture, vTexCoord).rgb, 1.0);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOGE("Shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

/* Renderer state: one texture, one fullscreen-quad program. init() is
 * called lazily (see ensure_egl_ready() in main()) once there's an actual
 * EGL window surface current to compile shaders against. */
struct Renderer {
    GLuint program = 0;
    GLuint texture = 0;
    GLint a_pos = -1;
    GLint a_tex = -1;
    GLint u_tex = -1;
    bool ready = false;
    int tex_w = 0;
    int tex_h = 0;

    bool init() {
        GLuint vs = compile_shader(GL_VERTEX_SHADER, VERTEX_SHADER_SRC);
        GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FRAGMENT_SHADER_SRC);
        if (!vs || !fs) return false;

        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        GLint ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        glDeleteShader(vs);
        glDeleteShader(fs);
        if (!ok) {
            char log[512];
            glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            LOGE("Program link failed: %s", log);
            return false;
        }

        a_pos = glGetAttribLocation(program, "aPos");
        a_tex = glGetAttribLocation(program, "aTexCoord");
        u_tex = glGetUniformLocation(program, "uTexture");

        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        ready = true;
        return true;
    }

    // Upload one RGBA8888 frame and draw it as a fullscreen quad.
    // Texture storage is allocated once per size change and updated with
    // glTexSubImage2D afterwards — glTexImage2D per frame would reallocate
    // GPU storage every frame.
    void draw_frame(const uint8_t *rgba, int w, int h) {
        static const GLfloat verts[] = {
            // x,    y,     u,    v
            -1.0f,  1.0f,  0.0f, 0.0f,
            -1.0f, -1.0f,  0.0f, 1.0f,
             1.0f,  1.0f,  1.0f, 0.0f,
             1.0f, -1.0f,  1.0f, 1.0f,
        };

        glViewport(0, 0, w, h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(program);

        glBindTexture(GL_TEXTURE_2D, texture);
        if (w != tex_w || h != tex_h) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            tex_w = w; tex_h = h;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        }
        glUniform1i(u_tex, 0);

        glVertexAttribPointer(a_pos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), verts);
        glEnableVertexAttribArray(a_pos);
        glVertexAttribPointer(a_tex, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), verts + 2);
        glEnableVertexAttribArray(a_tex);

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        glDisableVertexAttribArray(a_pos);
        glDisableVertexAttribArray(a_tex);
    }
};

/* KFR1 frame protocol (shared with kaos-ui-capture): a 24-byte header
 * {magic "KFR1", seq, width, height, stride} followed by one shm fd via
 * SCM_RIGHTS. The fd is mmap'd, uploaded as a GL texture, and the seq is
 * returned to the capture as a 4-byte ack (which gates buffer reuse). */
struct FrameHdr {
    char magic[4];
    uint32_t seq;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
};

/* TouchForwarder: reads the physical evdev touchscreen and forwards
 * simplified int32_t[4] packets to kaos-touch-input. */
struct TouchForwarder {
    static constexpr int MAX_SLOTS = 10;

    int touch_fd = -1;
    int input_fd = -1;
    bool capture_enabled = false;

    int display_w = 0;
    int display_h = 0;
    int abs_x_min = 0, abs_x_max = 0;
    int abs_y_min = 0, abs_y_max = 0;

    struct Slot {
        int id = -1;
        int x = 0;
        int y = 0;
        // Batched per-frame state, flushed on SYN_REPORT. Sending DOWN/UP
        // immediately on ABS_MT_TRACKING_ID used stale slot coordinates:
        // the standard MT-B event order is SLOT, TRACKING_ID, then
        // POSITION_X/Y within one SYN frame, so the position for the new
        // contact had not arrived yet and the first packet carried the
        // previous gesture's (or 0,0) coordinates.
        bool pending_down = false;
        bool pending_up = false;
        bool pending_move = false;
    };
    Slot slots[MAX_SLOTS];
    int current_slot = 0;

    bool open_touchscreen() {
        glob_t gl;
        memset(&gl, 0, sizeof(gl));
        if (glob("/dev/input/event*", 0, nullptr, &gl) != 0) {
            LOGE("No /dev/input/event* nodes found");
            return false;
        }

        int found = -1;
        for (size_t i = 0; i < gl.gl_pathc; i++) {
            int fd = ::open(gl.gl_pathv[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;

            struct input_absinfo abs_x;
            if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &abs_x) == 0 && abs_x.maximum > 0) {
                // Found a multitouch device. Prefer one that also claims
                // INPUT_PROP_DIRECT (a direct-touchscreen panel) if multiple
                // exist, but fall back to the first MT device we find.
                unsigned long propbits[INPUT_PROP_CNT / (sizeof(unsigned long) * 8) + 1] = {0};
                bool direct = false;
                if (ioctl(fd, EVIOCGPROP(sizeof(propbits)), propbits) >= 0) {
                    direct = propbits[INPUT_PROP_DIRECT / (sizeof(unsigned long) * 8)] &
                             (1UL << (INPUT_PROP_DIRECT % (sizeof(unsigned long) * 8)));
                }
                if (found < 0 || direct) {
                    if (found >= 0) ::close(found);
                    found = fd;
                    abs_x_min = abs_x.minimum;
                    abs_x_max = abs_x.maximum;
                    struct input_absinfo abs_y;
                    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs_y) == 0) {
                        abs_y_min = abs_y.minimum;
                        abs_y_max = abs_y.maximum;
                    }
                    if (direct) break;
                } else {
                    ::close(fd);
                }
            } else {
                ::close(fd);
            }
        }
        globfree(&gl);

        if (found < 0) {
            LOGE("No multitouch evdev device found");
            return false;
        }
        touch_fd = found;
        LOGI("Touchscreen: fd=%d range X=[%d,%d] Y=[%d,%d]",
             touch_fd, abs_x_min, abs_x_max, abs_y_min, abs_y_max);
        return true;
    }

    bool ensure_input_connected() {
        if (input_fd >= 0) return true;
        input_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (input_fd < 0) return false;
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, INPUT_SOCKET_PATH, sizeof(addr.sun_path) - 1);
        if (connect(input_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            ::close(input_fd);
            input_fd = -1;
            return false;
        }
        LOGI("Connected to touch input socket");
        return true;
    }

    void disconnect_input() {
        if (input_fd >= 0) {
            ::close(input_fd);
            input_fd = -1;
        }
    }

    void send_packet(int32_t action, int32_t ptr_id, int32_t x, int32_t y) {
        if (!ensure_input_connected()) return;
        int32_t pkt[4] = {action, ptr_id, x, y};
        ssize_t sent = send(input_fd, pkt, sizeof(pkt), MSG_NOSIGNAL);
        if (sent < 0) {
            LOGE("Touch packet send failed: %s", strerror(errno));
            disconnect_input();
        }
    }

    int scale_x(int v) const {
        if (abs_x_max <= abs_x_min) return v;
        long long num = (long long)(v - abs_x_min) * (display_w - 1);
        return (int)(num / (abs_x_max - abs_x_min));
    }

    int scale_y(int v) const {
        if (abs_y_max <= abs_y_min) return v;
        long long num = (long long)(v - abs_y_min) * (display_h - 1);
        return (int)(num / (abs_y_max - abs_y_min));
    }

    void process_event(const struct input_event &ev) {
        if (ev.type == EV_ABS) {
            switch (ev.code) {
                case ABS_MT_SLOT:
                    if (ev.value >= 0 && ev.value < MAX_SLOTS)
                        current_slot = ev.value;
                    break;
                case ABS_MT_TRACKING_ID: {
                    Slot &s = slots[current_slot];
                    int old_id = s.id;
                    int new_id = ev.value;
                    if (old_id >= 0 && new_id < 0) {
                        // Finger lifted (flush on SYN_REPORT).
                        s.id = -1;
                        s.pending_up = true;
                    } else if (old_id < 0 && new_id >= 0) {
                        // New finger (flush on SYN_REPORT).
                        s.id = new_id;
                        s.pending_down = true;
                    } else if (old_id >= 0 && new_id >= 0 && old_id != new_id) {
                        // Rare: tracking id swapped in the same slot.
                        s.pending_up = true;
                        s.id = new_id;
                        s.pending_down = true;
                    }
                    break;
                }
                case ABS_MT_POSITION_X:
                    slots[current_slot].x = ev.value;
                    if (slots[current_slot].id >= 0)
                        slots[current_slot].pending_move = true;
                    break;
                case ABS_MT_POSITION_Y:
                    slots[current_slot].y = ev.value;
                    if (slots[current_slot].id >= 0)
                        slots[current_slot].pending_move = true;
                    break;
            }
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            // Flush the accumulated frame: UP before DOWN before MOVE, using
            // the final per-slot coordinates seen in this SYN frame.
            for (int i = 0; i < MAX_SLOTS; i++) {
                Slot &s = slots[i];
                if (s.pending_up) {
                    send_packet(1, i, scale_x(s.x), scale_y(s.y));
                    s.pending_up = false;
                    s.pending_move = false;
                }
                if (s.pending_down) {
                    send_packet(0, i, scale_x(s.x), scale_y(s.y));
                    s.pending_down = false;
                }
                if (s.pending_move) {
                    send_packet(2, i, scale_x(s.x), scale_y(s.y));
                    s.pending_move = false;
                }
            }
        }
    }

    void release_all() {
        for (int i = 0; i < MAX_SLOTS; i++) {
            if (slots[i].id >= 0) {
                send_packet(1, i, scale_x(slots[i].x), scale_y(slots[i].y));
                slots[i].id = -1;
            }
        }
    }

    void enable(int w, int h) {
        display_w = w;
        display_h = h;
        if (touch_fd < 0 && !open_touchscreen()) {
            LOGE("Touch capture requested but no touchscreen available");
            return;
        }
        capture_enabled = true;
        LOGI("Touch capture enabled");
    }

    void disable() {
        if (!capture_enabled) return;
        capture_enabled = false;
        release_all();
        disconnect_input();
        LOGI("Touch capture disabled");
    }
};

static int make_listen_socket(const char *path) {
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOGE("socket(%s) failed: %s", path, strerror(errno));
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE("bind(%s) failed: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    chmod(path, 0666);
    if (listen(fd, 1) < 0) {
        LOGE("listen(%s) failed: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    // Optional frame timing stats (KAOS_TIMING=1). Accumulators reset on
    // each new frame-source connection so rates are per-connection.
    const bool timing = getenv("KAOS_TIMING") != nullptr;
    double t_last_rx = 0.0, rx_period_sum = 0.0, blit_sum = 0.0;
    uint64_t rx_timed = 0, blit_timed = 0;

    // Every native process that talks to Binder services needs a thread
    // ready to service *incoming* calls (death notifications, callbacks)
    // from whatever it's linked to, or linkToDeath registration on the
    // other end silently fails ("Thread Pool max thread count is 0" /
    // "Linking to death ... but there are no threads listening"). Ruled
    // out as the cause of the bootanim/zygote issue documented in the
    // header comment (still happened with this in place), but it's
    // correct, standard practice regardless -- keep it, and it must run
    // before any Binder call, so before creating the SurfaceComposerClient.
    android::ProcessState::self()->startThreadPool();

    std::vector<PhysicalDisplayId> display_ids = SurfaceComposerClient::getPhysicalDisplayIds();
    if (display_ids.empty()) {
        LOGE("No physical displays found");
        return 1;
    }
    sp<android::IBinder> display_token = SurfaceComposerClient::getPhysicalDisplayToken(display_ids[0]);
    if (!display_token) {
        LOGE("Cannot get physical display token");
        return 1;
    }
    android::ui::DisplayMode display_mode;
    if (SurfaceComposerClient::getActiveDisplayMode(display_token, &display_mode) != android::NO_ERROR) {
        LOGE("Cannot get active display mode");
        return 1;
    }
    int display_width = display_mode.resolution.width;
    int display_height = display_mode.resolution.height;
    LOGI("Display: %dx%d", display_width, display_height);

    sp<SurfaceComposerClient> client = new SurfaceComposerClient();

    sp<SurfaceControl> surface_control = client->createSurface(
        String8("KaosDesktop"), display_width, display_height,
        android::PIXEL_FORMAT_RGBA_8888, 0 /* flags -- see header comment */);
    if (surface_control == nullptr || !surface_control->isValid()) {
        LOGE("Cannot create SF surface");
        return 1;
    }
    SurfaceComposerClient::Transaction()
        .setLayerStack(surface_control, android::ui::LayerStack::fromValue(0))
        // Keep this surface above all normal Android app/system-UI layers so
        // the Linux desktop is fully visible. Touch is no longer delivered
        // through the Android Activity window stack; kaos-display-bridge reads
        // the physical evdev touchscreen directly and forwards events to
        // kaos-touch-input. The NO_INPUT_CHANNEL surface here blocks touches
        // from reaching Android UI behind it while the bridge routes them to
        // the Linux side.
        .setLayer(surface_control, 1000000)
        .setAlpha(surface_control, 0.0f)    // start hidden
        .setPosition(surface_control, 0, 0)
        .apply();
    LOGI("SF surface created (hidden, waiting for SHOW command)");

    // Standalone EGL setup -- the compat layer this used to go through
    // (see header comment) previously supplied the display/config/context.
    EGLDisplay egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint egl_major, egl_minor;
    if (egl_display == EGL_NO_DISPLAY || !eglInitialize(egl_display, &egl_major, &egl_minor)) {
        LOGE("eglInitialize failed: 0x%x", eglGetError());
        return 1;
    }
    const EGLint config_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig egl_config;
    EGLint num_configs = 0;
    if (!eglChooseConfig(egl_display, config_attribs, &egl_config, 1, &num_configs) || num_configs < 1) {
        LOGE("eglChooseConfig failed: 0x%x", eglGetError());
        return 1;
    }
    const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, context_attribs);
    if (egl_context == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed: 0x%x", eglGetError());
        return 1;
    }

    // eglCreateWindowSurface() connects to the layer's ANativeWindow/
    // BufferQueue right away -- confirmed live 2026-08-28: doing that
    // unconditionally at startup, while this surface is still hidden with
    // nothing to render, reliably made SurfaceFlinger issue ctl.start for
    // bootanim and restart zygote, every single time, before phoc/
    // phosh-session was even stable enough to produce a frame -- true even
    // after ruling out the layer value and the compat-layer flags bug
    // (see header comment), so keep it deferred defensively regardless.
    // There's no need for the window surface to exist until there's an
    // actual frame to draw into it, so create it lazily on first real use
    // instead of at startup.
    sp<Surface> native_window = surface_control->getSurface();
    EGLSurface egl_surface = EGL_NO_SURFACE;
    Renderer renderer;
    bool renderer_ready = false;
    auto ensure_egl_ready = [&]() -> bool {
        if (renderer_ready) return true;
        egl_surface = eglCreateWindowSurface(egl_display, egl_config, native_window.get(), NULL);
        if (egl_surface == EGL_NO_SURFACE) {
            LOGE("eglCreateWindowSurface failed: 0x%x", eglGetError());
            return false;
        }
        eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
        if (!renderer.init()) {
            LOGE("Renderer init failed");
            return false;
        }
        renderer_ready = true;
        return true;
    };


    int ctl_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (ctl_fd < 0) {
        LOGE("Cannot create control socket: %s", strerror(errno));
        return 1;
    }
    unlink(CTL_SOCKET_PATH);
    struct sockaddr_un ctl_addr;
    memset(&ctl_addr, 0, sizeof(ctl_addr));
    ctl_addr.sun_family = AF_UNIX;
    strncpy(ctl_addr.sun_path, CTL_SOCKET_PATH, sizeof(ctl_addr.sun_path) - 1);
    if (bind(ctl_fd, (struct sockaddr *)&ctl_addr, sizeof(ctl_addr)) < 0) {
        LOGE("Cannot bind control socket: %s", strerror(errno));
        return 1;
    }
    chmod(CTL_SOCKET_PATH, 0666);
    LOGI("Listening on %s", CTL_SOCKET_PATH);

    int frame_listen_fd = make_listen_socket(FRAME_SOCKET_PATH);
    if (frame_listen_fd < 0) return 1;
    LOGI("Listening on %s", FRAME_SOCKET_PATH);

    int link_listen_fd = make_listen_socket(LINK_SOCKET_PATH);
    if (link_listen_fd < 0) return 1;
    LOGI("Listening on %s", LINK_SOCKET_PATH);
    int link_client_fd = -1;

    TouchForwarder touch;
    bool visible = false;
    // Define the PhoneWindowManager routing flag up front so the property
    // is always present (default: not shown).
    property_set("sys.kaos.display_shown", "0");
    int frame_conn_fd = -1;

    // Shared by both the legacy ctl_fd (DGRAM) command path and the new
    // link_client_fd (STREAM) path below -- same protocol, same effect,
    // just two different transports for two different callers.
    auto handle_link_cmd = [&](uint8_t cmd) {
        switch (cmd) {
            case CMD_SHOW:
                if (!visible) {
                    if (!ensure_egl_ready()) break;
                    SurfaceComposerClient::Transaction()
                        .setAlpha(surface_control, 1.0f).apply();
                    visible = true;
                    // PhoneWindowManager reads this to route the
                    // hardware AI button (KEYCODE_VOICE_ASSIST) to
                    // the foreground Kaos app instead of launching
                    // the assistant while the display is shown.
                    property_set("sys.kaos.display_shown", "1");
                    touch.enable(display_width, display_height);
                    LOGI("Surface SHOWN");
                }
                break;
            case CMD_HIDE:
                if (visible) {
                    SurfaceComposerClient::Transaction()
                        .setAlpha(surface_control, 0.0f).apply();
                    visible = false;
                    property_set("sys.kaos.display_shown", "0");
                    touch.disable();
                    LOGI("Surface HIDDEN");
                }
                break;
            case CMD_QUIT:
                LOGI("QUIT received, shutting down");
                g_running = 0;
                break;
            default:
                LOGE("Unknown command: 0x%02x", cmd);
                break;
        }
    };

    while (g_running) {
        struct pollfd fds[7];
        int nfds = 0;
        int ctl_idx = nfds; fds[nfds++] = {ctl_fd, POLLIN, 0};
        int listen_idx = nfds; fds[nfds++] = {frame_listen_fd, POLLIN, 0};
        int conn_idx = -1;
        if (frame_conn_fd >= 0) {
            conn_idx = nfds; fds[nfds++] = {frame_conn_fd, POLLIN, 0};
        }
        int touch_idx = -1;
        if (touch.capture_enabled && touch.touch_fd >= 0) {
            touch_idx = nfds; fds[nfds++] = {touch.touch_fd, POLLIN, 0};
        }
        int link_listen_idx = nfds; fds[nfds++] = {link_listen_fd, POLLIN, 0};
        int link_client_idx = -1;
        if (link_client_fd >= 0) {
            link_client_idx = nfds; fds[nfds++] = {link_client_fd, POLLIN, 0};
        }

        int rc = poll(fds, nfds, -1);
        if (rc < 0) {
            if (errno == EINTR) continue;
            LOGE("poll failed: %s", strerror(errno));
            break;
        }

        if (fds[ctl_idx].revents & POLLIN) {
            uint8_t cmd;
            ssize_t n = recv(ctl_fd, &cmd, 1, 0);
            if (n > 0) handle_link_cmd(cmd);
        }

        if (fds[link_listen_idx].revents & POLLIN) {
            int new_fd = accept(link_listen_fd, nullptr, nullptr);
            if (new_fd >= 0) {
                if (link_client_fd >= 0) {
                    LOGI("Replacing existing display-link client");
                    close(link_client_fd);
                }
                link_client_fd = new_fd;
                LOGI("Display-link client connected");
            }
        }

        if (link_client_idx >= 0 && (fds[link_client_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            uint8_t cmd;
            ssize_t n = recv(link_client_fd, &cmd, 1, 0);
            if (n > 0) {
                handle_link_cmd(cmd);
            } else {
                // EOF or error: the client end closed, for any reason
                // (explicit stop, the app crashing, the app being killed).
                // Auto-hide so a dead client never leaves the desktop
                // stuck visible with nothing driving it.
                LOGI("Display-link client disconnected, auto-hiding");
                close(link_client_fd);
                link_client_fd = -1;
                if (visible) handle_link_cmd(CMD_HIDE);
            }
        }

        if (fds[listen_idx].revents & POLLIN) {
            int new_fd = accept(frame_listen_fd, nullptr, nullptr);
            if (new_fd >= 0) {
                if (frame_conn_fd >= 0) {
                    LOGI("Replacing existing frame source connection");
                    close(frame_conn_fd);
                }
                frame_conn_fd = new_fd;
                t_last_rx = 0.0; rx_period_sum = 0.0; blit_sum = 0.0;
                rx_timed = 0; blit_timed = 0;
                LOGI("Frame source connected");
            }
        }

        if (conn_idx >= 0 && (fds[conn_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (fds[conn_idx].revents & POLLIN) {
                // Receive one KFR1 frame: 24-byte header + shm fd via
                // SCM_RIGHTS. The header and fd travel in a single
                // sendmsg/recvmsg pair, so MSG_WAITALL on 24 bytes is
                // effectively atomic here.
                FrameHdr hdr;
                int frame_fd = -1;
                char cbuf[CMSG_SPACE(sizeof(int))];
                struct iovec iov = {&hdr, sizeof(hdr)};
                struct msghdr msg;
                memset(&msg, 0, sizeof(msg));
                msg.msg_iov = &iov;
                msg.msg_iovlen = 1;
                msg.msg_control = cbuf;
                msg.msg_controllen = sizeof(cbuf);
                ssize_t n = recvmsg(frame_conn_fd, &msg, MSG_WAITALL);
                bool hdr_ok = (n == (ssize_t)sizeof(hdr)) && memcmp(hdr.magic, "KFR1", 4) == 0;
                if (hdr_ok) {
                    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
                    if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                        memcpy(&frame_fd, CMSG_DATA(c), sizeof(int));
                    }
                }
                if (n == 0) {
                    // Clean EOF: the capture exited or reconnected. Not an error.
                    LOGI("Frame source disconnected (EOF)");
                    close(frame_conn_fd);
                    frame_conn_fd = -1;
                } else if (!hdr_ok || frame_fd < 0) {
                    LOGE("Malformed frame message (n=%zd hdr_ok=%d fd=%d), disconnecting",
                         n, hdr_ok, frame_fd);
                    if (frame_fd >= 0) close(frame_fd);
                    close(frame_conn_fd);
                    frame_conn_fd = -1;
                } else {
                    double t_rx = now_ms();
                    if (timing) {
                        if (t_last_rx > 0) { rx_period_sum += t_rx - t_last_rx; rx_timed++; }
                        t_last_rx = t_rx;
                    }
                    size_t px_size = (size_t)hdr.stride * hdr.height;
                    void *px = mmap(nullptr, px_size, PROT_READ, MAP_SHARED, frame_fd, 0);
                    if (px != MAP_FAILED) {
                        if (visible && renderer_ready) {
                            renderer.draw_frame((const uint8_t *)px, hdr.width, hdr.height);
                            eglSwapBuffers(egl_display, egl_surface);
                            if (timing) {
                                double t_blit = now_ms();
                                blit_sum += t_blit - t_rx;
                                blit_timed++;
                                if (blit_timed % 60 == 0 && rx_timed > 0) {
                                    LOGI("TIMING avg rx_period=%.1fms draw+swap=%.1fms (n=%llu)",
                                         rx_period_sum / rx_timed, blit_sum / blit_timed,
                                         (unsigned long long)blit_timed);
                                }
                            }
                        }
                        munmap(px, px_size);
                    } else {
                        LOGE("Frame mmap failed: %s", strerror(errno));
                    }
                    close(frame_fd);
                    // Ack the seq so the capture can reuse this buffer.
                    uint32_t ack = hdr.seq;
                    ssize_t snt = send(frame_conn_fd, &ack, sizeof(ack), MSG_NOSIGNAL);
                    if (snt != (ssize_t)sizeof(ack)) {
                        LOGI("Frame source disconnected (ack failed)");
                        close(frame_conn_fd);
                        frame_conn_fd = -1;
                    }
                }
            }
            // recvmsg returning EOF/0 inside the POLLIN branch also lands
            // here via POLLHUP on the next poll cycle.
            if (frame_conn_fd >= 0 && (fds[conn_idx].revents & (POLLHUP | POLLERR))) {
                LOGI("Frame source disconnected");
                close(frame_conn_fd);
                frame_conn_fd = -1;
            }
        }

        if (touch_idx >= 0 && (fds[touch_idx].revents & POLLIN)) {
            struct input_event ev;
            ssize_t n;
            while ((n = read(touch.touch_fd, &ev, sizeof(ev))) == sizeof(ev)) {
                touch.process_event(ev);
            }
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                LOGE("Touch device read error: %s", strerror(errno));
                touch.disable();
            }
        }
    }

    touch.disable();
    if (touch.touch_fd >= 0) close(touch.touch_fd);
    if (frame_conn_fd >= 0) close(frame_conn_fd);
    close(frame_listen_fd);
    unlink(FRAME_SOCKET_PATH);
    if (link_client_fd >= 0) close(link_client_fd);
    close(link_listen_fd);
    unlink(LINK_SOCKET_PATH);
    close(ctl_fd);
    unlink(CTL_SOCKET_PATH);
    LOGI("Exiting");
    return 0;
}
