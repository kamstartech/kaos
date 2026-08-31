/*
 * kaos-drm-fake.so -- LD_PRELOAD userspace KMS driver for overlay mode.
 *
 * Intercepts open/ioctl/mmap/close on /dev/dri/card0 and implements a minimal
 * but sufficient DRM/KMS device so phoc/wlroots can become DRM-master, enumerate
 * a Virtual-1 output, allocate dumb buffers, commit framebuffers, and receive
 * page-flip events. The resulting scanout buffer is forwarded as raw RGBA8888
 * to /dev/socket/kaos_ui.sock for kaos-display-bridge on the Android side.
 *
 * Built inside the target chroot against glibc + libdrm.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <linux/memfd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libdrm/drm_fourcc.h>

#ifndef DRM_MAJOR
#define DRM_MAJOR 226
#endif

#ifndef DRM_IOCTL_BASE
#define DRM_IOCTL_BASE 'd'
#endif

#ifndef DRM_EVENT_FLIP_COMPLETE
#define DRM_EVENT_FLIP_COMPLETE 0x02
struct drm_event_vblank_abi {
    struct drm_event base;
    uint64_t user_data;
    uint32_t tv_sec;
    uint32_t tv_usec;
    uint32_t crtc_id;
    uint32_t reserved;
};
#else
#define drm_event_vblank_abi drm_event_vblank
#endif

/* ------------------------------------------------------------------------- */
/* Logging                                                                   */
/* ------------------------------------------------------------------------- */

static int debug = 0;
static FILE *log_fp = NULL;

static void log_init(void)
{
    static int initialized = 0;
    if (initialized) return;
    initialized = 1;
    debug = getenv("KAOS_DRM_FAKE_DEBUG") != NULL;
    if (debug) {
        log_fp = fopen("/var/log/kaos-drm-fake.log", "a");
        if (!log_fp) log_fp = stderr;
    }
}

static void log_dbg(const char *fmt, ...)
{
    if (!debug || !log_fp) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(log_fp, "[kaos-drm-fake] ");
    vfprintf(log_fp, fmt, ap);
    fprintf(log_fp, "\n");
    fflush(log_fp);
    va_end(ap);
}

static void log_err(const char *fmt, ...)
{
    log_init();
    FILE *fp = log_fp ? log_fp : stderr;
    va_list ap;
    va_start(ap, fmt);
    fprintf(fp, "[kaos-drm-fake] ERROR: ");
    vfprintf(fp, fmt, ap);
    fprintf(fp, "\n");
    fflush(fp);
    va_end(ap);
}

/* ------------------------------------------------------------------------- */
/* Real libc function pointers                                               */
/* ------------------------------------------------------------------------- */

static int (*real_open)(const char *pathname, int flags, ...);
static int (*real_open64)(const char *pathname, int flags, ...);
static int (*real_openat)(int dirfd, const char *pathname, int flags, ...);
static int (*real_close)(int fd);
static int (*real_ioctl)(int fd, unsigned long request, ...);
static void *(*real_mmap)(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
static int (*real_munmap)(void *addr, size_t length);

static void load_reals(void)
{
    static int loaded = 0;
    if (loaded) return;
    loaded = 1;

#define LOAD(name) do { \
        if (!real_##name) { \
            real_##name = dlsym(RTLD_NEXT, #name); \
            if (!real_##name) { \
                log_err("failed to resolve " #name); \
            } \
        } \
    } while (0)

    LOAD(open);
    LOAD(open64);
    LOAD(openat);
    LOAD(close);
    LOAD(ioctl);
    LOAD(mmap);
    LOAD(munmap);
#undef LOAD
}

/* glibc open variadic wrapper for internal use */
static int do_real_open(const char *path, int flags, mode_t mode)
{
    load_reals();
    if (real_open) return real_open(path, flags, mode);
    errno = ENOSYS;
    return -1;
}

static int do_real_close(int fd)
{
    load_reals();
    if (real_close) return real_close(fd);
    errno = ENOSYS;
    return -1;
}

static int do_real_ioctl(int fd, unsigned long req, void *arg)
{
    load_reals();
    if (real_ioctl) {
        int (*fn)(int, unsigned long, ...) = real_ioctl;
        return fn(fd, req, arg);
    }
    errno = ENOSYS;
    return -1;
}

/* ------------------------------------------------------------------------- */
/* memfd helper (glibc wrapper may be missing on older systems)              */
/* ------------------------------------------------------------------------- */

static int memfd_create_compat(const char *name, unsigned int flags)
{
#ifdef __NR_memfd_create
    return syscall(__NR_memfd_create, name, flags);
#else
    errno = ENOSYS;
    return -1;
#endif
}

/* ------------------------------------------------------------------------- */
/* Fake DRM state                                                            */
/* ------------------------------------------------------------------------- */

#define MAX_FAKE_FDS 16
#define MAX_BUFS 64
#define MAX_FBS 64
#define MAX_EVENTS 64

#define FAKE_CONNECTOR_ID 1
#define FAKE_ENCODER_ID   2
#define FAKE_CRTC_ID      3
#define FAKE_PLANE_ID     4

#define DISPLAY_WIDTH  1080
#define DISPLAY_HEIGHT 2340

struct dumb_buf {
    uint32_t handle;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t pitch;
    uint64_t size;
    int memfd;
    void *mem;
    int map_count;
    uint32_t fb_id; /* 0 if not attached to any fb */
};

struct framebuffer {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t handles[4];
    uint32_t pitches[4];
    uint32_t offsets[4];
    uint32_t pixel_format; /* DRM_FORMAT_* */
};

struct drm_event_queue {
    uint8_t buf[4096];
    size_t head;
    size_t tail;
    pthread_mutex_t lock;
};

struct fake_drm {
    int used;
    int fd;       /* read end of event pipe; fd returned to caller */
    int pipe_wr;  /* write end of event pipe */

    uint32_t mode_width;
    uint32_t mode_height;

    struct dumb_buf bufs[MAX_BUFS];
    uint32_t next_handle;

    struct framebuffer fbs[MAX_FBS];
    uint32_t next_fb_id;

    uint32_t crtc_fb_id;
    uint32_t crtc_enabled;

    struct drm_event_queue events;
    pthread_t event_thread;
    int event_thread_running;

    int socket_fd;
};

static struct fake_drm fake_fds[MAX_FAKE_FDS];
static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;

static struct fake_drm *fd_to_fake(int fd)
{
    if (fd < 0) return NULL;
    pthread_mutex_lock(&fake_lock);
    for (int i = 0; i < MAX_FAKE_FDS; i++) {
        if (fake_fds[i].used && fake_fds[i].fd == fd) {
            pthread_mutex_unlock(&fake_lock);
            return &fake_fds[i];
        }
    }
    pthread_mutex_unlock(&fake_lock);
    return NULL;
}

static struct fake_drm *alloc_fake(void)
{
    int pipefd[2];
    if (pipe(pipefd) < 0) return NULL;
    if (fcntl(pipefd[0], F_SETFL, O_NONBLOCK) < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }

    pthread_mutex_lock(&fake_lock);
    for (int i = 0; i < MAX_FAKE_FDS; i++) {
        if (!fake_fds[i].used) {
            memset(&fake_fds[i], 0, sizeof(fake_fds[i]));
            fake_fds[i].used = 1;
            fake_fds[i].fd = pipefd[0];
            fake_fds[i].pipe_wr = pipefd[1];
            fake_fds[i].next_handle = 0x1000;
            fake_fds[i].next_fb_id = 0x100;
            fake_fds[i].mode_width = DISPLAY_WIDTH;
            fake_fds[i].mode_height = DISPLAY_HEIGHT;
            fake_fds[i].socket_fd = -1;
            pthread_mutex_init(&fake_fds[i].events.lock, NULL);
            pthread_mutex_unlock(&fake_lock);
            return &fake_fds[i];
        }
    }
    pthread_mutex_unlock(&fake_lock);
    close(pipefd[0]);
    close(pipefd[1]);
    return NULL;
}

static void free_fake(struct fake_drm *fake)
{
    if (!fake) return;
    if (fake->socket_fd >= 0) do_real_close(fake->socket_fd);
    if (fake->pipe_wr >= 0) do_real_close(fake->pipe_wr);
    for (int i = 0; i < MAX_BUFS; i++) {
        if (fake->bufs[i].mem) {
            munmap(fake->bufs[i].mem, fake->bufs[i].size);
            fake->bufs[i].mem = NULL;
        }
        if (fake->bufs[i].memfd >= 0) do_real_close(fake->bufs[i].memfd);
    }
    pthread_mutex_destroy(&fake->events.lock);
    fake->used = 0;
}

static struct dumb_buf *buf_by_handle(struct fake_drm *fake, uint32_t handle)
{
    for (int i = 0; i < MAX_BUFS; i++) {
        if (fake->bufs[i].handle == handle) return &fake->bufs[i];
    }
    return NULL;
}

static struct framebuffer *fb_by_id(struct fake_drm *fake, uint32_t fb_id)
{
    for (int i = 0; i < MAX_FBS; i++) {
        if (fake->fbs[i].fb_id == fb_id) return &fake->fbs[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Frame socket output                                                       */
/* ------------------------------------------------------------------------- */

#define FRAME_SOCKET_PATH "/dev/socket/kaos_ui.sock"

static int connect_frame_socket_once(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FRAME_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        do_real_close(fd);
        return -1;
    }
    return fd;
}

static int ensure_socket(struct fake_drm *fake)
{
    if (fake->socket_fd >= 0) return fake->socket_fd;
    fake->socket_fd = connect_frame_socket_once();
    return fake->socket_fd;
}

static bool write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        done += (size_t)n;
    }
    return true;
}

static void convert_to_rgba(const struct framebuffer *fb,
                            const struct dumb_buf *buf,
                            uint8_t *dst)
{
    const uint8_t *src = buf->mem;
    const uint32_t w = fb->width;
    const uint32_t h = fb->height;
    const uint32_t src_stride = fb->pitches[0];
    const uint32_t dst_stride = w * 4;

    if (fb->pixel_format == DRM_FORMAT_XRGB8888 ||
        fb->pixel_format == DRM_FORMAT_ARGB8888) {
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *s = src + y * src_stride;
            uint8_t *d = dst + y * dst_stride;
            for (uint32_t x = 0; x < w; x++) {
                d[0] = s[2];
                d[1] = s[1];
                d[2] = s[0];
                d[3] = 0xFF;
                s += 4; d += 4;
            }
        }
    } else if (fb->pixel_format == DRM_FORMAT_XBGR8888 ||
               fb->pixel_format == DRM_FORMAT_ABGR8888) {
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *s = src + y * src_stride;
            uint8_t *d = dst + y * dst_stride;
            for (uint32_t x = 0; x < w; x++) {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] = 0xFF;
                s += 4; d += 4;
            }
        }
    } else {
        /* Unknown: copy as-is and hope it's RGBA-ish */
        for (uint32_t y = 0; y < h; y++) {
            memcpy(dst + y * dst_stride, src + y * src_stride,
                   y == h - 1 ? w * 4 : src_stride);
        }
    }
}

static void emit_frame(struct fake_drm *fake)
{
    struct framebuffer *fb = fb_by_id(fake, fake->crtc_fb_id);
    if (!fb) return;
    struct dumb_buf *buf = buf_by_handle(fake, fb->handles[0]);
    if (!buf || !buf->mem) return;

    int sock = ensure_socket(fake);
    if (sock < 0) return;

    uint32_t w = fb->width;
    uint32_t h = fb->height;
    size_t frame_size = (size_t)w * h * 4;
    uint8_t *rgba = malloc(frame_size);
    if (!rgba) return;

    convert_to_rgba(fb, buf, rgba);

    if (!write_all(sock, rgba, frame_size)) {
        do_real_close(fake->socket_fd);
        fake->socket_fd = -1;
    }

    free(rgba);
}

/* ------------------------------------------------------------------------- */
/* DRM event queue                                                           */
/* ------------------------------------------------------------------------- */

static void event_push(struct fake_drm *fake, const void *data, size_t len)
{
    struct drm_event_queue *q = &fake->events;
    pthread_mutex_lock(&q->lock);
    if (q->tail + len <= sizeof(q->buf)) {
        memcpy(q->buf + q->tail, data, len);
        q->tail += len;
    }
    pthread_mutex_unlock(&q->lock);
}

static size_t event_pop(struct fake_drm *fake, void *data, size_t max_len)
{
    struct drm_event_queue *q = &fake->events;
    pthread_mutex_lock(&q->lock);
    size_t avail = q->tail - q->head;
    size_t copy = avail < max_len ? avail : max_len;
    if (copy) {
        memcpy(data, q->buf + q->head, copy);
        q->head += copy;
        if (q->head == q->tail) q->head = q->tail = 0;
    }
    pthread_mutex_unlock(&q->lock);
    return copy;
}

static void queue_page_flip(struct fake_drm *fake, uint64_t user_data)
{
    struct drm_event_vblank_abi ev;
    memset(&ev, 0, sizeof(ev));
    ev.base.type = DRM_EVENT_FLIP_COMPLETE;
    ev.base.length = sizeof(ev);
    ev.user_data = user_data;
    ev.crtc_id = FAKE_CRTC_ID;
    /* Fake timestamp */
    ev.tv_sec = 0;
    ev.tv_usec = 0;

    /* Write event bytes to the pipe so select/poll/read all work normally. */
    if (fake->pipe_wr >= 0) {
        ssize_t n = write(fake->pipe_wr, &ev, sizeof(ev));
        (void)n;
    }
}

/* ------------------------------------------------------------------------- */
/* IOCTL handlers                                                            */
/* ------------------------------------------------------------------------- */

static inline void copy_to_user(void *dst, const void *src, size_t len)
{
    if (dst && len) memcpy(dst, src, len);
}

static int handle_version(struct fake_drm *fake, struct drm_version *v)
{
    static const char name[] = "kaos-drm-fake";
    static const char desc[] = "Kaos userspace fake KMS";
    static const char date[] = "20260830";

    if (v->name_len) {
        size_t n = sizeof(name) < v->name_len ? sizeof(name) : v->name_len;
        memcpy(v->name, name, n);
    }
    if (v->desc_len) {
        size_t n = sizeof(desc) < v->desc_len ? sizeof(desc) : v->desc_len;
        memcpy(v->desc, desc, n);
    }
    if (v->date_len) {
        size_t n = sizeof(date) < v->date_len ? sizeof(date) : v->date_len;
        memcpy(v->date, date, n);
    }
    v->version_major = 1;
    v->version_minor = 0;
    v->version_patchlevel = 0;
    v->name_len = sizeof(name) - 1;
    v->desc_len = sizeof(desc) - 1;
    v->date_len = sizeof(date) - 1;
    return 0;
}

static int handle_set_client_cap(struct fake_drm *fake, struct drm_set_client_cap *cap)
{
    log_dbg("SET_CLIENT_CAP cap=%u value=%llu", cap->capability, (unsigned long long)cap->value);
    switch (cap->capability) {
    case DRM_CLIENT_CAP_UNIVERSAL_PLANES:
        return 0; /* ok */
    case DRM_CLIENT_CAP_ATOMIC:
        return 0; /* accept; actual atomic commits will be rejected later */
    default:
        return -EINVAL;
    }
}

static int handle_get_cap(struct fake_drm *fake, struct drm_get_cap *cap)
{
    switch (cap->capability) {
    case DRM_CAP_DUMB_BUFFER:
        cap->value = 1;
        break;
    case DRM_CAP_DUMB_PREFERRED_DEPTH:
        cap->value = 24;
        break;
    case DRM_CAP_DUMB_PREFER_SHADOW:
        cap->value = 0;
        break;
    case DRM_CAP_CRTC_IN_VBLANK_EVENT:
        cap->value = 1;
        break;
    case DRM_CAP_ADDFB2_MODIFIERS:
        cap->value = 0;
        break;
    case DRM_CAP_PAGE_FLIP_TARGET:
        cap->value = 0;
        break;
    case DRM_CAP_ASYNC_PAGE_FLIP:
        cap->value = 0;
        break;
    case DRM_CAP_PRIME:
        cap->value = 3; /* PRIME_IMPORT | PRIME_EXPORT */
        break;
    case DRM_CAP_TIMESTAMP_MONOTONIC:
        cap->value = 1;
        break;
    case DRM_CAP_SYNCOBJ:
    case DRM_CAP_SYNCOBJ_TIMELINE:
        cap->value = 0;
        break;
    default:
        cap->value = 0;
        break;
    }
    return 0;
}

static void fill_mode(struct drm_mode_modeinfo *m, uint32_t w, uint32_t h)
{
    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%dx%d", w, h);
    m->clock = (w * h * 60) / 1000; /* kHz-ish */
    m->hdisplay = w;
    m->hsync_start = w + 10;
    m->hsync_end = w + 20;
    m->htotal = w + 30;
    m->vdisplay = h;
    m->vsync_start = h + 5;
    m->vsync_end = h + 10;
    m->vtotal = h + 15;
    m->vrefresh = 60;
    m->flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
    m->type = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
}

static int handle_get_resources(struct fake_drm *fake, struct drm_mode_card_res *res)
{
    uint32_t crtc_ids[1] = { FAKE_CRTC_ID };
    uint32_t conn_ids[1] = { FAKE_CONNECTOR_ID };
    uint32_t enc_ids[1] = { FAKE_ENCODER_ID };
    uint32_t fb_ids[4];
    int nfb = 0;
    for (int i = 0; i < MAX_FBS && nfb < 4; i++) {
        if (fake->fbs[i].fb_id) fb_ids[nfb++] = fake->fbs[i].fb_id;
    }

    res->count_crtcs = 1;
    res->count_connectors = 1;
    res->count_encoders = 1;
    res->count_fbs = nfb;
    res->min_width = fake->mode_width;
    res->min_height = fake->mode_height;
    res->max_width = fake->mode_width;
    res->max_height = fake->mode_height;

    if (res->crtc_id_ptr) {
        copy_to_user((void *)(uintptr_t)res->crtc_id_ptr, crtc_ids, sizeof(crtc_ids));
    }
    if (res->connector_id_ptr) {
        copy_to_user((void *)(uintptr_t)res->connector_id_ptr, conn_ids, sizeof(conn_ids));
    }
    if (res->encoder_id_ptr) {
        copy_to_user((void *)(uintptr_t)res->encoder_id_ptr, enc_ids, sizeof(enc_ids));
    }
    if (res->fb_id_ptr) {
        copy_to_user((void *)(uintptr_t)res->fb_id_ptr, fb_ids, nfb * sizeof(uint32_t));
    }
    return 0;
}

static int handle_get_connector(struct fake_drm *fake, struct drm_mode_get_connector *conn)
{
    struct drm_mode_modeinfo mode;
    fill_mode(&mode, fake->mode_width, fake->mode_height);

    uint32_t enc_ids[1] = { FAKE_ENCODER_ID };
    uint64_t props[1] = { 0 };
    uint64_t propvals[1] = { 0 };

    conn->encoder_id = FAKE_ENCODER_ID;
    conn->connection = 1; /* DRM_MODE_CONNECTED */
    conn->connector_type = DRM_MODE_CONNECTOR_VIRTUAL;
    conn->connector_type_id = 1;
    conn->mm_width = 68;  /* fake physical size mm */
    conn->mm_height = 147;
    conn->subpixel = DRM_MODE_SUBPIXEL_UNKNOWN;
    conn->count_modes = 1;
    conn->count_encoders = 1;
    conn->count_props = 0;

    if (conn->modes_ptr) {
        copy_to_user((void *)(uintptr_t)conn->modes_ptr, &mode, sizeof(mode));
    }
    if (conn->encoders_ptr) {
        copy_to_user((void *)(uintptr_t)conn->encoders_ptr, enc_ids, sizeof(enc_ids));
    }
    if (conn->props_ptr) {
        copy_to_user((void *)(uintptr_t)conn->props_ptr, props, 0);
    }
    if (conn->prop_values_ptr) {
        copy_to_user((void *)(uintptr_t)conn->prop_values_ptr, propvals, 0);
    }
    return 0;
}

static int handle_get_encoder(struct fake_drm *fake, struct drm_mode_get_encoder *enc)
{
    enc->encoder_id = FAKE_ENCODER_ID;
    enc->encoder_type = DRM_MODE_ENCODER_VIRTUAL;
    enc->crtc_id = FAKE_CRTC_ID;
    enc->possible_crtcs = 1;
    enc->possible_clones = 0;
    return 0;
}

static int handle_get_crtc(struct fake_drm *fake, struct drm_mode_crtc *crtc)
{
    crtc->crtc_id = FAKE_CRTC_ID;
    crtc->fb_id = fake->crtc_fb_id;
    crtc->x = 0;
    crtc->y = 0;
    crtc->gamma_size = 0;
    crtc->mode_valid = fake->crtc_enabled;
    if (fake->crtc_enabled) {
        fill_mode(&crtc->mode, fake->mode_width, fake->mode_height);
    }
    return 0;
}

static int handle_set_crtc(struct fake_drm *fake, struct drm_mode_crtc *crtc)
{
    fake->crtc_fb_id = crtc->fb_id;
    fake->crtc_enabled = (crtc->fb_id != 0);
    if (fake->crtc_enabled) {
        log_dbg("SET_CRTC fb=%u enabled", crtc->fb_id);
        emit_frame(fake);
        queue_page_flip(fake, 0);
    } else {
        log_dbg("SET_CRTC disabled");
    }
    return 0;
}

static int handle_create_dumb(struct fake_drm *fake, struct drm_mode_create_dumb *dumb)
{
    int idx = -1;
    for (int i = 0; i < MAX_BUFS; i++) {
        if (!fake->bufs[i].handle) { idx = i; break; }
    }
    if (idx < 0) return -ENOMEM;

    uint32_t pitch = dumb->width * ((dumb->bpp + 7) / 8);
    uint64_t size = (uint64_t)pitch * dumb->height;
    if (size == 0) return -EINVAL;
    /* align to page */
    size = (size + 4095) & ~4095ULL;

    int memfd = memfd_create_compat("kaos-drm-dumb", MFD_CLOEXEC);
    if (memfd < 0) {
        log_err("memfd_create failed: %s", strerror(errno));
        return -errno;
    }
    if (ftruncate(memfd, size) < 0) {
        do_real_close(memfd);
        return -errno;
    }

    uint32_t handle = fake->next_handle++;
    fake->bufs[idx].handle = handle;
    fake->bufs[idx].width = dumb->width;
    fake->bufs[idx].height = dumb->height;
    fake->bufs[idx].bpp = dumb->bpp;
    fake->bufs[idx].pitch = pitch;
    fake->bufs[idx].size = size;
    fake->bufs[idx].memfd = memfd;
    fake->bufs[idx].mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    if (fake->bufs[idx].mem == MAP_FAILED) {
        fake->bufs[idx].mem = NULL;
        do_real_close(memfd);
        memset(&fake->bufs[idx], 0, sizeof(fake->bufs[idx]));
        return -errno;
    }

    dumb->handle = handle;
    dumb->pitch = pitch;
    dumb->size = size;
    log_dbg("CREATE_DUMB handle=%u %ux%u bpp=%u pitch=%u size=%llu",
            handle, dumb->width, dumb->height, dumb->bpp, pitch,
            (unsigned long long)size);
    return 0;
}

static int handle_map_dumb(struct fake_drm *fake, struct drm_mode_map_dumb *map)
{
    struct dumb_buf *buf = buf_by_handle(fake, map->handle);
    if (!buf) return -ENOENT;
    /* Use handle as the fake offset; mmap intercept will resolve it. */
    map->offset = (uint64_t)map->handle;
    log_dbg("MAP_DUMB handle=%u offset=%llu", map->handle, (unsigned long long)map->offset);
    return 0;
}

static int handle_destroy_dumb(struct fake_drm *fake, struct drm_mode_destroy_dumb *dumb)
{
    struct dumb_buf *buf = buf_by_handle(fake, dumb->handle);
    if (!buf) return -ENOENT;
    if (buf->mem) munmap(buf->mem, buf->size);
    if (buf->memfd >= 0) do_real_close(buf->memfd);
    memset(buf, 0, sizeof(*buf));
    buf->memfd = -1;
    return 0;
}

static uint32_t drm_fourcc_from_wl(uint32_t format)
{
    switch (format) {
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XBGR8888:
    case DRM_FORMAT_ABGR8888:
        return format;
    default:
        return DRM_FORMAT_XRGB8888;
    }
}

static int handle_addfb2(struct fake_drm *fake, struct drm_mode_fb_cmd2 *fb)
{
    int idx = -1;
    for (int i = 0; i < MAX_FBS; i++) {
        if (!fake->fbs[i].fb_id) { idx = i; break; }
    }
    if (idx < 0) return -ENOMEM;

    uint32_t fb_id = fake->next_fb_id++;
    fake->fbs[idx].fb_id = fb_id;
    fake->fbs[idx].width = fb->width;
    fake->fbs[idx].height = fb->height;
    fake->fbs[idx].pixel_format = drm_fourcc_from_wl(fb->pixel_format);
    for (int i = 0; i < 4; i++) {
        fake->fbs[idx].handles[i] = fb->handles[i];
        fake->fbs[idx].pitches[i] = fb->pitches[i];
        fake->fbs[idx].offsets[i] = fb->offsets[i];
    }

    struct dumb_buf *buf = buf_by_handle(fake, fb->handles[0]);
    if (buf) buf->fb_id = fb_id;

    fb->fb_id = fb_id;
    log_dbg("ADDFB2 fb_id=%u %ux%u fourcc=%.4s handle=%u",
            fb_id, fb->width, fb->height, (char *)&fb->pixel_format, fb->handles[0]);
    return 0;
}

static int handle_rmfb(struct fake_drm *fake, unsigned int *fb_id_ptr)
{
    uint32_t fb_id = *fb_id_ptr;
    struct framebuffer *fb = fb_by_id(fake, fb_id);
    if (!fb) return -ENOENT;
    struct dumb_buf *buf = buf_by_handle(fake, fb->handles[0]);
    if (buf) buf->fb_id = 0;
    memset(fb, 0, sizeof(*fb));
    log_dbg("RMFB fb_id=%u", fb_id);
    return 0;
}

static int handle_page_flip(struct fake_drm *fake, struct drm_mode_crtc_page_flip *flip)
{
    fake->crtc_fb_id = flip->fb_id;
    fake->crtc_enabled = (flip->fb_id != 0);
    if (fake->crtc_enabled) {
        log_dbg("PAGE_FLIP fb=%u", flip->fb_id);
        emit_frame(fake);
        queue_page_flip(fake, flip->user_data);
    }
    return 0;
}

static int handle_gem_close(struct fake_drm *fake, struct drm_gem_close *close)
{
    /* Buffer lifecycle is handled by destroy_dumb; ignore gem close. */
    log_dbg("GEM_CLOSE handle=%u (ignored)", close->handle);
    return 0;
}

static int handle_obj_getproperties(struct fake_drm *fake, struct drm_mode_obj_get_properties *props)
{
    (void)fake;
    props->count_props = 0;
    log_dbg("OBJ_GETPROPERTIES obj_id=%u type=0x%x", props->obj_id, props->obj_type);
    return 0;
}

static int handle_prime_handle_to_fd(struct fake_drm *fake, struct drm_prime_handle *prime)
{
    struct dumb_buf *buf = buf_by_handle(fake, prime->handle);
    if (!buf || buf->memfd < 0) return -ENOENT;
    /* Duplicate the memfd for export. */
    int fd = dup(buf->memfd);
    if (fd < 0) return -errno;
    prime->fd = fd;
    log_dbg("PRIME_HANDLE_TO_FD handle=%u fd=%d", prime->handle, fd);
    return 0;
}

static int handle_prime_fd_to_handle(struct fake_drm *fake, struct drm_prime_handle *prime)
{
    int fd = prime->fd;
    struct stat st;
    if (fstat(fd, &st) < 0) return -errno;
    uint64_t size = st.st_size;
    if (size == 0) size = 4096 * 1024; /* fallback 4 MB */

    void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) return -errno;

    int idx = -1;
    for (int i = 0; i < MAX_BUFS; i++) {
        if (!fake->bufs[i].handle) { idx = i; break; }
    }
    if (idx < 0) {
        munmap(mem, size);
        return -ENOMEM;
    }

    uint32_t handle = fake->next_handle++;
    fake->bufs[idx].handle = handle;
    fake->bufs[idx].width = 0;  /* unknown until ADDFB2 */
    fake->bufs[idx].height = 0;
    fake->bufs[idx].bpp = 0;
    fake->bufs[idx].pitch = 0;
    fake->bufs[idx].size = size;
    fake->bufs[idx].memfd = dup(fd);
    fake->bufs[idx].mem = mem;

    prime->handle = handle;
    log_dbg("PRIME_FD_TO_HANDLE fd=%d size=%llu handle=%u", fd, (unsigned long long)size, handle);
    return 0;
}

static int handle_get_plane_resources(struct fake_drm *fake, struct drm_mode_get_plane_res *res)
{
    uint32_t plane_ids[1] = { FAKE_PLANE_ID };
    res->count_planes = 1;
    if (res->plane_id_ptr) {
        copy_to_user((void *)(uintptr_t)res->plane_id_ptr, plane_ids, sizeof(plane_ids));
    }
    return 0;
}

static int handle_get_plane(struct fake_drm *fake, struct drm_mode_get_plane *plane)
{
    plane->plane_id = FAKE_PLANE_ID;
    plane->possible_crtcs = 1;
    plane->gamma_size = 0;
    plane->count_format_types = 1;
    uint32_t formats[1] = { DRM_FORMAT_XRGB8888 };
    if (plane->format_type_ptr) {
        copy_to_user((void *)(uintptr_t)plane->format_type_ptr, formats, sizeof(formats));
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Main ioctl dispatcher                                                     */
/* ------------------------------------------------------------------------- */

static int fake_ioctl(struct fake_drm *fake, unsigned long request, void *arg)
{
    unsigned int dir  = _IOC_DIR(request);
    unsigned int type = _IOC_TYPE(request);
    unsigned int nr   = _IOC_NR(request);

    (void)dir; (void)type;

    if (type != DRM_IOCTL_BASE) {
        log_dbg("non-DRM ioctl 0x%lx type=%u", request, type);
        return -EINVAL;
    }

    switch (nr) {
    case DRM_IOCTL_NR(DRM_IOCTL_VERSION):
        return handle_version(fake, (struct drm_version *)arg);
    case DRM_IOCTL_NR(DRM_IOCTL_GET_CAP):
        return handle_get_cap(fake, (struct drm_get_cap *)arg);
    case DRM_IOCTL_NR(DRM_IOCTL_MODE_GETRESOURCES):
        return handle_get_resources(fake, (struct drm_mode_card_res *)arg);
    case DRM_IOCTL_NR(DRM_IOCTL_MODE_GETCONNECTOR):
        return handle_get_connector(fake, (struct drm_mode_get_connector *)arg);
    case DRM_IOCTL_NR(DRM_IOCTL_MODE_GETENCODER):
        return handle_get_encoder(fake, (struct drm_mode_get_encoder *)arg);
    case DRM_IOCTL_NR(DRM_IOCTL_MODE_GETCRTC):
        return handle_get_crtc(fake, (struct drm_mode_crtc *)arg);
    case DRM_IOCTL_NR(DRM_IOCTL_MODE_SETCRTC):
        return handle_set_crtc(fake, (struct drm_mode_crtc *)arg);
    case DRM_IOCTL_NR(DRM_IOCTL_MODE_GETPLANERESOURCES):
        return handle_get_plane_resources(fake, (struct drm_mode_get_plane_res *)arg);
    case DRM_IOCTL_NR(DRM_IOCTL_MODE_GETPLANE):
        return handle_get_plane(fake, (struct drm_mode_get_plane *)arg);
    case DRM_IOCTL_NR(DRM_IOCTL_MODE_CREATE_DUMB):
        return handle_create_dumb(fake, (struct drm_mode_create_dumb *)arg);
    case DRM_IOCTL_NR(DRM_IOCTL_MODE_MAP_DUMB):
        return handle_map_dumb(fake, (struct drm_mode_map_dumb *)arg);
    case DRM_IOCTL_NR(DRM_IOCTL_MODE_DESTROY_DUMB):
        return handle_destroy_dumb(fake, (struct drm_mode_destroy_dumb *)arg);
    case DRM_IOCTL_NR(DRM_IOCTL_MODE_ADDFB2):
        return handle_addfb2(fake, (struct drm_mode_fb_cmd2 *)arg);
    case DRM_IOCTL_NR(DRM_IOCTL_MODE_RMFB):
        return handle_rmfb(fake, (unsigned int *)arg);
    case DRM_IOCTL_NR(DRM_IOCTL_SET_CLIENT_CAP):
        return handle_set_client_cap(fake, (struct drm_set_client_cap *)arg);
    case DRM_IOCTL_NR(DRM_IOCTL_MODE_PAGE_FLIP):
        return handle_page_flip(fake, (struct drm_mode_crtc_page_flip *)arg);
    case DRM_IOCTL_NR(DRM_IOCTL_GEM_CLOSE):
        return handle_gem_close(fake, (struct drm_gem_close *)arg);
    case DRM_IOCTL_NR(DRM_IOCTL_MODE_OBJ_GETPROPERTIES):
        return handle_obj_getproperties(fake, (struct drm_mode_obj_get_properties *)arg);
    case DRM_IOCTL_NR(DRM_IOCTL_PRIME_HANDLE_TO_FD):
        return handle_prime_handle_to_fd(fake, (struct drm_prime_handle *)arg);
    case DRM_IOCTL_NR(DRM_IOCTL_PRIME_FD_TO_HANDLE):
        return handle_prime_fd_to_handle(fake, (struct drm_prime_handle *)arg);
    case DRM_IOCTL_NR(DRM_IOCTL_SET_MASTER):
    case DRM_IOCTL_NR(DRM_IOCTL_DROP_MASTER):
        log_dbg("SET/DROP_MASTER noop");
        return 0;
    default:
        log_dbg("unhandled ioctl 0x%lx (nr=%u)", request, nr);
        return -EINVAL;
    }
}

/* ------------------------------------------------------------------------- */
/* Interposed libc functions                                                 */
/* ------------------------------------------------------------------------- */

int open(const char *pathname, int flags, ...)
{
    load_reals();
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    if (pathname && strcmp(pathname, "/dev/dri/card0") == 0) {
        log_init();
        log_dbg("intercepted open(%s)", pathname);
        struct fake_drm *fake = alloc_fake();
        if (!fake) {
            errno = EMFILE;
            return -1;
        }
        return fake->fd;
    }

    if (real_open) return real_open(pathname, flags, mode);
    errno = ENOSYS;
    return -1;
}

int open64(const char *pathname, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    return open(pathname, flags | O_LARGEFILE, mode);
}

int openat(int dirfd, const char *pathname, int flags, ...)
{
    load_reals();
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    if (pathname && strcmp(pathname, "/dev/dri/card0") == 0) {
        return open(pathname, flags, mode);
    }
    if (dirfd == AT_FDCWD && pathname && strncmp(pathname, "/dev/dri/card0", 14) == 0) {
        return open(pathname, flags, mode);
    }

    if (real_openat) return real_openat(dirfd, pathname, flags, mode);
    errno = ENOSYS;
    return -1;
}

int close(int fd)
{
    load_reals();
    struct fake_drm *fake = fd_to_fake(fd);
    if (fake) {
        log_dbg("close fake fd %d", fd);
        free_fake(fake);
        return 0;
    }
    if (real_close) return real_close(fd);
    errno = ENOSYS;
    return -1;
}

int ioctl(int fd, unsigned long request, ...)
{
    load_reals();
    struct fake_drm *fake = fd_to_fake(fd);
    if (fake) {
        va_list ap;
        va_start(ap, request);
        void *arg = va_arg(ap, void *);
        va_end(ap);
        log_dbg("ioctl fd=%d req=0x%lx", fd, request);
        int ret = fake_ioctl(fake, request, arg);
        if (ret < 0) {
            errno = -ret;
            return -1;
        }
        return 0;
    }

    if (real_ioctl) {
        va_list ap;
        va_start(ap, request);
        void *arg = va_arg(ap, void *);
        va_end(ap);
        int (*fn)(int, unsigned long, ...) = real_ioctl;
        return fn(fd, request, arg);
    }
    errno = ENOSYS;
    return -1;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    load_reals();
    struct fake_drm *fake = fd_to_fake(fd);
    if (fake) {
        /* offset is the fake handle from MAP_DUMB */
        struct dumb_buf *buf = buf_by_handle(fake, (uint32_t)offset);
        if (buf && buf->memfd >= 0) {
            log_dbg("mmap fake fd=%d handle=%u length=%zu", fd, (uint32_t)offset, length);
            void *p = mmap(NULL, length, prot, flags, buf->memfd, 0);
            if (p != MAP_FAILED) buf->map_count++;
            return p;
        }
        log_err("mmap on fake fd with unknown offset %lu", (unsigned long)offset);
        errno = EINVAL;
        return MAP_FAILED;
    }

    if (real_mmap) return real_mmap(addr, length, prot, flags, fd, offset);
    errno = ENOSYS;
    return MAP_FAILED;
}

int munmap(void *addr, size_t length)
{
    load_reals();
    if (real_munmap) return real_munmap(addr, length);
    errno = ENOSYS;
    return -1;
}

/* ------------------------------------------------------------------------- */
/* libseat interception -- bypass seatd/logind and hand phoc a fake DRM fd. */
/* ------------------------------------------------------------------------- */

#include <libseat.h>

struct fake_seat {
    struct libseat_seat_listener listener;
    void *userdata;
    int event_pipe_rd;
    int event_pipe_wr;
    int next_dev_id;
    int card_fd;
};

static struct fake_seat *seat_handle = NULL;

static int init_seat_pipe(struct fake_seat *seat)
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
    log_init();
    log_dbg("libseat_open_seat intercepted");
    if (seat_handle) {
        return (struct libseat *)seat_handle;
    }
    seat_handle = calloc(1, sizeof(*seat_handle));
    if (!seat_handle) {
        errno = ENOMEM;
        return NULL;
    }
    if (listener) seat_handle->listener = *listener;
    seat_handle->userdata = userdata;
    seat_handle->card_fd = -1;
    seat_handle->next_dev_id = 0;
    if (init_seat_pipe(seat_handle) < 0) {
        free(seat_handle);
        seat_handle = NULL;
        errno = ENOMEM;
        return NULL;
    }
    if (listener && listener->enable_seat) {
        listener->enable_seat((struct libseat *)seat_handle, userdata);
    }
    return (struct libseat *)seat_handle;
}

int libseat_close_seat(struct libseat *seat)
{
    log_dbg("libseat_close_seat");
    struct fake_seat *fs = (struct fake_seat *)seat;
    if (fs && fs == seat_handle) {
        if (fs->event_pipe_rd >= 0) close(fs->event_pipe_rd);
        if (fs->event_pipe_wr >= 0) close(fs->event_pipe_wr);
        free(fs);
        seat_handle = NULL;
    }
    return 0;
}

int libseat_open_device(struct libseat *seat, const char *path, int *fd)
{
    log_dbg("libseat_open_device path=%s", path ? path : "(null)");
    struct fake_seat *fs = (struct fake_seat *)seat;
    if (!fs || fs != seat_handle) {
        errno = EINVAL;
        return -1;
    }

    if (!path || !fd) {
        errno = EINVAL;
        return -1;
    }

    /* If wlroots asks for card0, give it a fake fd. */
    if (strstr(path, "/dev/dri/card") == path) {
        struct fake_drm *fake = alloc_fake();
        if (!fake) {
            errno = EMFILE;
            return -1;
        }
        *fd = fake->fd;
        fs->card_fd = fake->fd;
        return ++fs->next_dev_id;
    }

    /* For any other device (e.g. /dev/input/event*), pass through. */
    int real_fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (real_fd < 0) return -1;
    *fd = real_fd;
    return ++fs->next_dev_id;
}

int libseat_close_device(struct libseat *seat, int device_id)
{
    log_dbg("libseat_close_device device_id=%d", device_id);
    struct fake_seat *fs = (struct fake_seat *)seat;
    if (!fs || fs != seat_handle) {
        errno = EINVAL;
        return -1;
    }
    if (fs->card_fd >= 0) {
        struct fake_drm *fake = fd_to_fake(fs->card_fd);
        if (fake) free_fake(fake);
        fs->card_fd = -1;
    }
    return 0;
}

int libseat_get_fd(struct libseat *seat)
{
    struct fake_seat *fs = (struct fake_seat *)seat;
    if (fs && fs == seat_handle) {
        return fs->event_pipe_rd;
    }
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
