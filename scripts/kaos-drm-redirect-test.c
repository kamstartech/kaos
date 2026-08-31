/*
 * Quick test for kaos-drm-redirect.so: prints DRM resources and the
 * connector/encoder/crtc chain. Run normally and with LD_PRELOAD.
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drmMode.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/dri/card0";
    int fd = open(dev, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    drmModeResPtr res = drmModeGetResources(fd);
    if (!res) {
        fprintf(stderr, "drmModeGetResources failed\n");
        close(fd);
        return 1;
    }

    printf("CRTCs:     %d\n", res->count_crtcs);
    for (int i = 0; i < res->count_crtcs; i++)
        printf("  crtc[%d] = %u\n", i, res->crtcs[i]);

    printf("Encoders:  %d\n", res->count_encoders);
    for (int i = 0; i < res->count_encoders; i++)
        printf("  encoder[%d] = %u\n", i, res->encoders[i]);

    printf("Connectors:%d\n", res->count_connectors);
    for (int i = 0; i < res->count_connectors; i++) {
        uint32_t id = res->connectors[i];
        drmModeConnectorPtr c = drmModeGetConnector(fd, id);
        const char *state = c ? (c->connection == DRM_MODE_CONNECTED ? "connected" : "disconnected") : "???";
        printf("  connector[%d] = %u (%s)\n", i, id, state);
        if (c) {
            printf("    type=%u type_id=%u encoder_id=%u modes=%d\n",
                   c->connector_type, c->connector_type_id, c->encoder_id, c->count_modes);
            drmModeFreeConnector(c);
        }
    }

    printf("Planes:\n");
    drmModePlaneResPtr pres = drmModeGetPlaneResources(fd);
    if (pres) {
        for (int i = 0; i < pres->count_planes; i++) {
            drmModePlanePtr p = drmModeGetPlane(fd, pres->planes[i]);
            if (p) {
                printf("  plane[%d] = %u possible_crtcs=0x%x crtc_id=%u fb_id=%u\n",
                       i, p->plane_id, p->possible_crtcs, p->crtc_id, p->fb_id);
                drmModeFreePlane(p);
            }
        }
        drmModeFreePlaneResources(pres);
    }

    drmModeFreeResources(res);
    close(fd);
    return 0;
}
