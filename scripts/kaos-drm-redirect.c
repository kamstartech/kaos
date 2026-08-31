/*
 * kaos-drm-redirect.so -- LD_PRELOAD shim that redirects phoc's DRM probing
 * away from the real DSI-1 panel and onto the unused Virtual-1 connector.
 *
 * Background:
 *   - DSI-1  (connector 26, encoder 25, CRTC 113) is the physical panel
 *     actively driven by Android's SurfaceFlinger. phoc must NEVER commit
 *     a KMS mode to it, or it will fight SurfaceFlinger for the real screen.
 *   - Virtual-1 (connector 45, encoder 44, CRTC 166) is an unused virtual
 *     connector on the same msm_drm card. It is safe for phoc to use.
 *
 * This shim intercepts libdrm mode-getter functions and filters the returned
 * IDs so phoc only sees Virtual-1. All other DRM ioctl paths pass through
 * unchanged.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <xf86drmMode.h>

/* IDs measured on perseus (sdm845-tavil-snd-card, msm_drm 1.2.0). */
#define KEEP_CONNECTOR 45
#define KEEP_ENCODER   44
#define KEEP_CRTC      166
#define HIDE_CONNECTOR 26
#define HIDE_ENCODER   25
#define HIDE_CRTC      113

static int debug = 0;

static void __attribute__((constructor)) init(void)
{
    debug = getenv("KAOS_DRM_REDIRECT_DEBUG") != NULL;
}

static void log_dbg(const char *fmt, ...)
{
    if (!debug) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[kaos-drm-redirect] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* Real libdrm function pointers. */
static drmModeResPtr (*real_drmModeGetResources)(int fd) = NULL;
static drmModeConnectorPtr (*real_drmModeGetConnector)(int fd, uint32_t connectorId) = NULL;
static drmModeConnectorPtr (*real_drmModeGetConnectorCurrent)(int fd, uint32_t connectorId) = NULL;
static drmModeEncoderPtr (*real_drmModeGetEncoder)(int fd, uint32_t encoderId) = NULL;
static drmModeCrtcPtr (*real_drmModeGetCrtc)(int fd, uint32_t crtcId) = NULL;
static drmModePlaneResPtr (*real_drmModeGetPlaneResources)(int fd) = NULL;
static drmModePlanePtr (*real_drmModeGetPlane)(int fd, uint32_t planeId) = NULL;

static void load_real(void)
{
    static int loaded = 0;
    if (loaded) return;
    loaded = 1;

#define LOAD(fn) do { \
        if (!real_##fn) { \
            real_##fn = dlsym(RTLD_NEXT, #fn); \
            if (!real_##fn) { \
                fprintf(stderr, "[kaos-drm-redirect] failed to resolve " #fn "\n"); \
            } \
        } \
    } while (0)

    LOAD(drmModeGetResources);
    LOAD(drmModeGetConnector);
    LOAD(drmModeGetConnectorCurrent);
    LOAD(drmModeGetEncoder);
    LOAD(drmModeGetCrtc);
    LOAD(drmModeGetPlaneResources);
    LOAD(drmModeGetPlane);
#undef LOAD
}

/* Return the index of KEEP_CRTC in the real resources array, -1 if absent. */
static int real_crtc_index(drmModeResPtr res, uint32_t crtc_id)
{
    for (int i = 0; i < res->count_crtcs; i++) {
        if (res->crtcs[i] == crtc_id)
            return i;
    }
    return -1;
}

/* Rewrite a possible_crtcs bitmask so only KEEP_CRTC's bit is set.
 * The caller already knows KEEP_CRTC exists in the real array. */
static uint32_t rewrite_possible_crtcs(uint32_t orig, drmModeResPtr res)
{
    int idx = real_crtc_index(res, KEEP_CRTC);
    if (idx < 0)
        return 0;
    if (!(orig & (1u << idx)))
        return 0;
    return 1u; /* KEEP_CRTC is the only CRTC we expose, so bit 0 */
}

/* drmModeGetResources: expose only the Virtual-1 pipeline. */
drmModeResPtr drmModeGetResources(int fd)
{
    load_real();
    if (!real_drmModeGetResources)
        return NULL;

    drmModeResPtr real = real_drmModeGetResources(fd);
    if (!real)
        return NULL;

    int has_crtc = real_crtc_index(real, KEEP_CRTC) >= 0;
    int has_connector = 0;
    for (int i = 0; i < real->count_connectors; i++) {
        if (real->connectors[i] == KEEP_CONNECTOR) {
            has_connector = 1;
            break;
        }
    }
    int has_encoder = 0;
    for (int i = 0; i < real->count_encoders; i++) {
        if (real->encoders[i] == KEEP_ENCODER) {
            has_encoder = 1;
            break;
        }
    }

    if (!has_crtc || !has_connector || !has_encoder) {
        log_dbg("Virtual-1 pipeline missing, passing resources through");
        return real;
    }

    drmModeResPtr fake = calloc(1, sizeof(*fake));
    if (!fake) {
        drmModeFreeResources(real);
        return NULL;
    }

    fake->count_fbs = real->count_fbs;
    if (fake->count_fbs > 0) {
        fake->fbs = calloc(fake->count_fbs, sizeof(uint32_t));
        if (fake->fbs)
            memcpy(fake->fbs, real->fbs, fake->count_fbs * sizeof(uint32_t));
    }

    fake->count_crtcs = 1;
    fake->crtcs = calloc(1, sizeof(uint32_t));
    if (fake->crtcs)
        *fake->crtcs = KEEP_CRTC;

    fake->count_connectors = 1;
    fake->connectors = calloc(1, sizeof(uint32_t));
    if (fake->connectors)
        *fake->connectors = KEEP_CONNECTOR;

    fake->count_encoders = 1;
    fake->encoders = calloc(1, sizeof(uint32_t));
    if (fake->encoders)
        *fake->encoders = KEEP_ENCODER;

    /* Use Virtual-1's reported mode limits; fall back to real limits. */
    drmModeConnectorPtr conn = real_drmModeGetConnector(fd, KEEP_CONNECTOR);
    if (conn && conn->count_modes > 0) {
        uint32_t min_w = ~0u, min_h = ~0u, max_w = 0, max_h = 0;
        for (int i = 0; i < conn->count_modes; i++) {
            if (conn->modes[i].hdisplay < min_w) min_w = conn->modes[i].hdisplay;
            if (conn->modes[i].vdisplay < min_h) min_h = conn->modes[i].vdisplay;
            if (conn->modes[i].hdisplay > max_w) max_w = conn->modes[i].hdisplay;
            if (conn->modes[i].vdisplay > max_h) max_h = conn->modes[i].vdisplay;
        }
        fake->min_width = min_w;
        fake->min_height = min_h;
        fake->max_width = max_w;
        fake->max_height = max_h;
        drmModeFreeConnector(conn);
    } else {
        fake->min_width = real->min_width;
        fake->min_height = real->min_height;
        fake->max_width = real->max_width;
        fake->max_height = real->max_height;
        if (conn) drmModeFreeConnector(conn);
    }

    log_dbg("filtered resources: %d crtcs, %d connectors, %d encoders",
            fake->count_crtcs, fake->count_connectors, fake->count_encoders);

    drmModeFreeResources(real);
    return fake;
}

/* Hide DSI-1; pass Virtual-1 through unchanged. */
drmModeConnectorPtr drmModeGetConnector(int fd, uint32_t connectorId)
{
    load_real();
    if (!real_drmModeGetConnector)
        return NULL;

    if (connectorId == HIDE_CONNECTOR) {
        log_dbg("hiding connector %u (DSI-1)", connectorId);
        return NULL;
    }

    drmModeConnectorPtr conn = real_drmModeGetConnector(fd, connectorId);
    if (connectorId == KEEP_CONNECTOR && conn) {
        log_dbg("connector %u (Virtual-1) modes=%d", connectorId, conn->count_modes);
    }
    return conn;
}

drmModeConnectorPtr drmModeGetConnectorCurrent(int fd, uint32_t connectorId)
{
    load_real();
    if (!real_drmModeGetConnectorCurrent)
        return NULL;

    if (connectorId == HIDE_CONNECTOR) {
        log_dbg("hiding connector current %u (DSI-1)", connectorId);
        return NULL;
    }

    return real_drmModeGetConnectorCurrent(fd, connectorId);
}

/* Hide DSI-1 encoder; rewrite possible_crtcs on Virtual-1 encoder. */
drmModeEncoderPtr drmModeGetEncoder(int fd, uint32_t encoderId)
{
    load_real();
    if (!real_drmModeGetEncoder)
        return NULL;

    if (encoderId == HIDE_ENCODER) {
        log_dbg("hiding encoder %u (DSI-1)", encoderId);
        return NULL;
    }

    drmModeEncoderPtr enc = real_drmModeGetEncoder(fd, encoderId);
    if (encoderId == KEEP_ENCODER && enc) {
        drmModeResPtr res = real_drmModeGetResources(fd);
        if (res) {
            enc->possible_crtcs = rewrite_possible_crtcs(enc->possible_crtcs, res);
            log_dbg("encoder %u possible_crtcs rewritten to 0x%x", encoderId, enc->possible_crtcs);
            drmModeFreeResources(res);
        }
    }
    return enc;
}

/* Hide DSI-1 CRTC; pass Virtual-1 through. */
drmModeCrtcPtr drmModeGetCrtc(int fd, uint32_t crtcId)
{
    load_real();
    if (!real_drmModeGetCrtc)
        return NULL;

    if (crtcId == HIDE_CRTC) {
        log_dbg("hiding CRTC %u (DSI-1)", crtcId);
        return NULL;
    }

    return real_drmModeGetCrtc(fd, crtcId);
}

/* Pass plane resources through; phoc will filter by possible_crtcs itself,
 * and we rewrite those in drmModeGetPlane. */
drmModePlaneResPtr drmModeGetPlaneResources(int fd)
{
    load_real();
    if (!real_drmModeGetPlaneResources)
        return NULL;
    return real_drmModeGetPlaneResources(fd);
}

/* Rewrite possible_crtcs so planes only claim the exposed Virtual-1 CRTC. */
drmModePlanePtr drmModeGetPlane(int fd, uint32_t planeId)
{
    load_real();
    if (!real_drmModeGetPlane)
        return NULL;

    drmModePlanePtr plane = real_drmModeGetPlane(fd, planeId);
    if (plane) {
        drmModeResPtr res = real_drmModeGetResources(fd);
        if (res) {
            plane->possible_crtcs = rewrite_possible_crtcs(plane->possible_crtcs, res);
            log_dbg("plane %u possible_crtcs rewritten to 0x%x", planeId, plane->possible_crtcs);
            drmModeFreeResources(res);
        }
    }
    return plane;
}
