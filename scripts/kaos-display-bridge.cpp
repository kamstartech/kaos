/* kaos-display-bridge: SurfaceFlinger surface + compositor daemon
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
 * SurfaceFlinger-aware backend. Instead, kaos-service's existing kaos-init
 * pipeline (wf-recorder, a wlr-screencopy client) captures phoc's headless
 * output and relays raw RGBA8888 frames — no video codec — over
 * /dev/socket/kaos_ui.sock (already bind-mounted, host-and-chroot-visible)
 * via ffmpeg's rawvideo unix-socket muxer. This daemon is the listener on
 * that socket: it uploads each frame as a GL texture and draws it into the
 * SF surface with a single textured-quad blit, avoiding any Android
 * Activity/View/Bitmap compositing round-trip.
 *
 * Listens on /dev/socket/kaos_display_ctl.sock for single-byte commands
 * from the PhoshDisplayActivity Android app:
 *   CMD_SHOW (0x01) — set surface alpha=1.0, resume rendering
 *   CMD_HIDE (0x02) — set surface alpha=0.0, pause rendering (frames from
 *                      kaos_ui.sock are still drained, not rendered, so
 *                      wf-recorder/ffmpeg never blocks on backpressure)
 *   CMD_QUIT (0xFF) — destroy surface and exit
 *
 * The surface is created at the highest z-order so it appears above all
 * Android windows. When PhoshDisplayActivity is in the foreground
 * (transparent overlay), the user sees the Linux desktop composited
 * directly by SurfaceFlinger behind the transparent activity window.
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
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <errno.h>
#include <poll.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <binder/ProcessState.h>
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
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) do { fprintf(stderr, "I/" LOG_TAG ": "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#define LOGE(...) do { fprintf(stderr, "E/" LOG_TAG ": "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#endif

#define CTL_SOCKET_PATH "/dev/socket/kaos_display_ctl.sock"
#define FRAME_SOCKET_PATH "/dev/socket/kaos_ui.sock"

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
    "    gl_FragColor = texture2D(uTexture, vTexCoord);\n"
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
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
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

/* Frame-socket connection state: a partially-received RGBA8888 frame
 * being accumulated across possibly-many read()s (SOCK_STREAM gives no
 * message boundaries — ffmpeg's rawvideo unix muxer just writes a
 * continuous byte stream, one frame's worth of bytes at a time). */
struct FrameReader {
    size_t frame_bytes = 0;
    uint8_t *buf = nullptr;
    size_t have = 0;

    void reset(int w, int h) {
        frame_bytes = (size_t)w * (size_t)h * 4;
        free(buf);
        buf = (uint8_t *)malloc(frame_bytes);
        have = 0;
    }

    // Returns true when a full frame has been accumulated into buf.
    bool feed(int fd) {
        ssize_t n = read(fd, buf + have, frame_bytes - have);
        if (n <= 0) return false; // caller checks errno/EOF
        have += (size_t)n;
        if (have == frame_bytes) {
            have = 0;
            return true;
        }
        return false;
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
        .setLayer(surface_control, 1000000) // comfortably above any normal
                                             // app/system-UI layer (those
                                             // stay in the low thousands)
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

    FrameReader frames;
    frames.reset(display_width, display_height);

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

    bool visible = false;
    int frame_conn_fd = -1;

    while (g_running) {
        struct pollfd fds[3];
        int nfds = 0;
        int ctl_idx = nfds; fds[nfds++] = {ctl_fd, POLLIN, 0};
        int listen_idx = nfds; fds[nfds++] = {frame_listen_fd, POLLIN, 0};
        int conn_idx = -1;
        if (frame_conn_fd >= 0) {
            conn_idx = nfds; fds[nfds++] = {frame_conn_fd, POLLIN, 0};
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
            if (n > 0) {
                switch (cmd) {
                    case CMD_SHOW:
                        if (!visible) {
                            if (!ensure_egl_ready()) break;
                            SurfaceComposerClient::Transaction()
                                .setAlpha(surface_control, 1.0f).apply();
                            visible = true;
                            LOGI("Surface SHOWN");
                        }
                        break;
                    case CMD_HIDE:
                        if (visible) {
                            SurfaceComposerClient::Transaction()
                                .setAlpha(surface_control, 0.0f).apply();
                            visible = false;
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
                frames.reset(display_width, display_height);
                LOGI("Frame source connected");
            }
        }

        if (conn_idx >= 0 && (fds[conn_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (fds[conn_idx].revents & POLLIN) {
                bool got_frame = frames.feed(frame_conn_fd);
                if (got_frame && visible && renderer_ready) {
                    renderer.draw_frame(frames.buf, display_width, display_height);
                    eglSwapBuffers(egl_display, egl_surface);
                }
            }
            // Treat a read()-reported error/EOF (feed() returns false without
            // completing a frame while POLLHUP/POLLERR is set) as disconnect.
            if (fds[conn_idx].revents & (POLLHUP | POLLERR)) {
                LOGI("Frame source disconnected");
                close(frame_conn_fd);
                frame_conn_fd = -1;
            }
        }
    }

    if (frame_conn_fd >= 0) close(frame_conn_fd);
    close(frame_listen_fd);
    unlink(FRAME_SOCKET_PATH);
    close(ctl_fd);
    unlink(CTL_SOCKET_PATH);
    LOGI("Exiting");
    return 0;
}
