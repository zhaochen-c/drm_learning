#define __GNU_SOURCE
#include <stdint.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

struct buffer_object {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t handle;
    uint32_t size;
    uint8_t *vaddr;
    uint32_t fb_id;
};

struct buffer_object buf;

static int modeset_create_fb(int fd, struct buffer_object *bo)
{
    struct drm_mode_create_dumb create = {};
    struct drm_mode_map_dumb map = {};

    create.width = bo->width;
    create.height = bo->height;
    create.bpp = 32;
    drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);

    bo->pitch = create.pitch;
    bo->size = create.size;
    bo->handle = create.handle;
    drmModeAddFB(fd, bo->width, bo->height, 24, 32, bo->pitch, bo->handle, &bo->fb_id);

    map.handle = create.handle;
    drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);

    bo->vaddr = mmap(0, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);

    memset(bo->vaddr, 0xff, bo->size);

    return 0;
}

static void modeset_destroy_fb(int fd, struct buffer_object *bo)
{
    struct drm_mode_destroy_dumb destroy = {};

    drmModeRmFB(fd, bo->fb_id);

    munmap(bo->vaddr, bo->size);

    destroy.handle = bo->handle;

    drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
}

static uint32_t get_property_id(int fd, drmModeObjectProperties *props, const char *name)
{
    drmModePropertyPtr property;
    uint32_t i, id = 0;
    for (i = 0; i < props->count_props; ++i) {
        property = drmModeGetProperty(fd, props->props[i]);
        if (!strcmp(property->name, name))
            id = property->prop_id;
        drmModeFreeProperty(property);

        if (id)
            break;
    }

    return id;
}

int main(int argc, char **argv)
{
    int fd;
    drmModeConnector *conn;
    drmModeRes *res;
    drmModePlaneRes *plane_res;
    drmModeObjectProperties *props;
    drmModeAtomicReq *req;
    uint32_t conn_id;
    uint32_t crtc_id;
    uint32_t plane_id;
    uint32_t blob_id;
    uint32_t property_crtc_id;
    uint32_t property_mode_id;
    uint32_t property_active;

    fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);

    res = drmModeGetResources(fd);
    crtc_id = res->crtcs[0];
    conn_id = res->connectors[0];

    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    plane_res = drmModeGetPlaneResources(fd);
    plane_id = plane_res->planes[0];

    conn = drmModeGetConnector(fd, conn_id);
    buf.width = conn->modes[0].hdisplay;
    buf.height = conn->modes[0].vdisplay;

    modeset_create_fb(fd, &buf);

    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);

    props = drmModeObjectGetProperties(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR);
    property_crtc_id = get_property_id(fd, props, "CRTC ID");
    drmModeFreeObjectProperties(props);

    props = drmModeObjectGetProperties(fd, crtc_id, DRM_MODE_OBJECT_CRTC);
    property_active = get_property_id(fd, props, "ACTIVE");
    property_mode_id = get_property_id(fd, props, "MODE_ID");
    drmModeFreeObjectProperties(props);

    drmModeCreatePropertyBlob(fd, &conn->modes[0], sizeof(conn->modes[0]), &blob_id);

    req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, crtc_id, property_active, 1);
    drmModeAtomicAddProperty(req, crtc_id, property_mode_id, blob_id);
    drmModeAtomicAddProperty(req, conn_id, property_crtc_id, crtc_id);
    drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    drmModeAtomicFree(req);

    printf("drmModeAtomicCommit SetCrtc\n");
    getchar();

    drmModeSetPlane(fd, plane_id, crtc_id, buf.fb_id, 0, 50, 50, 320, 320, 0, 0, 320 << 16, 320 << 16);

    printf("drmModeSetPlane\n");
    getchar();

    modeset_destroy_fb(fd, &buf);

    drmModeFreeConnector(conn);
    drmModeFreePlaneResources(plane_res);
    drmModeFreeResources(res);

    close(fd);

    return 0;
}
