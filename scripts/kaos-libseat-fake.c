/*
 * kaos-libseat-fake.so -- minimal LD_PRELOAD shim that makes libseat succeed
 * in a Kaos namespace where logind has no real graphics device.
 *
 * Intercepts just enough of libseat so phoc/wlroots can start in headless
 * mode. It returns a fake seat handle, a fake/passthrough device fd, and a
 * silent event pipe. No DRM/KMS emulation is performed -- the headless
 * backend renders to memory and does not need a real display device.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>
#include <libseat.h>

static int debug = 0;

static void log_dbg(const char *fmt, ...)
{
    if (!debug) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[kaos-libseat-fake] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

struct fake_seat {
    struct libseat_seat_listener listener;
    void *userdata;
    int event_pipe_rd;
    int event_pipe_wr;
    int next_dev_id;
    int last_fd;
};

static struct fake_seat *g_seat = NULL;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int init_pipe(struct fake_seat *seat)
{
    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;
    if (fcntl(pipefd[0], F_SETFL, O_NONBLOCK) < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    seat->event_pipe_rd = pipefd[0];
    seat->event_pipe_wr = pipefd[1];
    return 0;
}

struct libseat *libseat_open_seat(const struct libseat_seat_listener *listener,
                                   void *userdata)
{
    debug = getenv("KAOS_LIBSEAT_FAKE_DEBUG") != NULL;
    log_dbg("libseat_open_seat");

    pthread_mutex_lock(&g_lock);
    if (g_seat) {
        pthread_mutex_unlock(&g_lock);
        return (struct libseat *)g_seat;
    }

    g_seat = calloc(1, sizeof(*g_seat));
    if (!g_seat) {
        pthread_mutex_unlock(&g_lock);
        errno = ENOMEM;
        return NULL;
    }
    if (listener) g_seat->listener = *listener;
    g_seat->userdata = userdata;
    g_seat->last_fd = -1;
    if (init_pipe(g_seat) < 0) {
        free(g_seat);
        g_seat = NULL;
        pthread_mutex_unlock(&g_lock);
        errno = ENOMEM;
        return NULL;
    }

    if (listener && listener->enable_seat) {
        listener->enable_seat((struct libseat *)g_seat, userdata);
    }

    pthread_mutex_unlock(&g_lock);
    return (struct libseat *)g_seat;
}

int libseat_close_seat(struct libseat *seat)
{
    log_dbg("libseat_close_seat");
    struct fake_seat *fs = (struct fake_seat *)seat;
    pthread_mutex_lock(&g_lock);
    if (fs && fs == g_seat) {
        if (fs->event_pipe_rd >= 0) close(fs->event_pipe_rd);
        if (fs->event_pipe_wr >= 0) close(fs->event_pipe_wr);
        free(fs);
        g_seat = NULL;
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int (*real_open)(const char *pathname, int flags, ...) = NULL;

static void load_real_open(void)
{
    if (real_open) return;
    real_open = dlsym(RTLD_NEXT, "open");
}

int libseat_open_device(struct libseat *seat, const char *path, int *fd)
{
    log_dbg("libseat_open_device path=%s", path ? path : "(null)");
    struct fake_seat *fs = (struct fake_seat *)seat;
    if (!fs || fs != g_seat || !fd) {
        errno = EINVAL;
        return -1;
    }

    load_real_open();

    int dev_fd;
    if (path && strncmp(path, "/dev/dri/card", 13) == 0) {
        /* Headless mode doesn't need a real KMS device. Give it /dev/null
         * so wlroots has a valid fd to poll, but it won't actually issue
         * KMS ioctls against it. */
        dev_fd = real_open ? real_open("/dev/null", O_RDWR | O_CLOEXEC) : open("/dev/null", O_RDWR | O_CLOEXEC);
    } else if (path) {
        dev_fd = real_open ? real_open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK) : open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    } else {
        errno = EINVAL;
        return -1;
    }

    if (dev_fd < 0) return -1;

    *fd = dev_fd;
    fs->last_fd = dev_fd;
    return ++fs->next_dev_id;
}

int libseat_close_device(struct libseat *seat, int device_id)
{
    log_dbg("libseat_close_device device_id=%d", device_id);
    struct fake_seat *fs = (struct fake_seat *)seat;
    if (!fs || fs != g_seat) {
        errno = EINVAL;
        return -1;
    }
    if (fs->last_fd >= 0) {
        close(fs->last_fd);
        fs->last_fd = -1;
    }
    return 0;
}

int libseat_get_fd(struct libseat *seat)
{
    struct fake_seat *fs = (struct fake_seat *)seat;
    if (fs && fs == g_seat) return fs->event_pipe_rd;
    return -1;
}

int libseat_dispatch(struct libseat *seat, int timeout)
{
    (void)seat;
    (void)timeout;
    return 0;
}

const char *libseat_seat_name(struct libseat *seat)
{
    (void)seat;
    return "seat0";
}

int libseat_switch_session(struct libseat *seat, int session)
{
    log_dbg("libseat_switch_session %d", session);
    (void)seat;
    (void)session;
    return 0;
}

int libseat_disable_seat(struct libseat *seat)
{
    log_dbg("libseat_disable_seat");
    (void)seat;
    return 0;
}
