#define _GNU_SOURCE
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <drm_fourcc.h>

struct drm_object {
    drmModeObjectProperties *props;
    drmModePropertyRes **props_info;
    uint32_t id;
};

struct modeset_buf {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t size;
    uint32_t handle;
    uint8_t *map;
    uint32_t fb;
};

struct modeset_output {
    struct modeset_output *next;

    unsigned int front_buf;
    struct modeset_buf bufs[2];

    struct drm_object connector;
    struct drm_object crtc;
    struct drm_object plane;

    drmModeModeInfo mode;
    uint32_t mode_blob_id;
    uint32_t crtc_index;

    bool pflip_pending;
    bool cleanup;

    uint8_t r, g, b;
    bool r_up, g_up, b_up;
};

static struct modeset_output *output_list = NULL;

static int modeset_open(int *out, const char *node)
{
    int fd, ret;
    uint64_t cap;

    fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ret = -errno;
        fprintf(stderr, "cannot open '%s', %m\n", node);
        return ret;
    }

    ret = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    if (ret) {
        fprintf(stderr, "failed to set universal planes cap, %d\n", ret);
        return ret;
    }

    ret = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
    if (ret) {
        fprintf(stderr, "failed to set atomic cap, %d\n", ret);
        return ret;
    }

    if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &cap) < 0 || !cap) {
        fprintf(stderr, "drm device '%s' does not support dumb buffers\n", node);
        close(fd);
        return -EOPNOTSUPP;
    }

    if (drmGetCap(fd, DRM_CAP_CRTC_IN_VBLANK_EVENT, &cap) < 0 && !cap) {
        fprintf(stderr, "drm device '%s' does not support atomic KMS\n", node);
        close(fd);
        return -EOPNOTSUPP;
    }

    *out = fd;
    return 0;
}

static int64_t get_property_value(int fd, drmModeObjectPropertiesPtr props, const char *name)
{
    drmModePropertyPtr prop;
    uint64_t value;
    bool found;
    int j;

    found = false;
    for (j = 0; j < props->count_props && !found; ++j) {
        prop = drmModeGetProperty(fd, props->props[j]);
        if (!strcmp(prop->name, name)) {
            value = props->prop_values[j];
            found = true;
        }
        drmModeFreeProperty(prop);
    }

    if (!found)
        return -1;
    return value;
}

static void modeset_get_object_properties(int fd, struct drm_object *obj, uint32_t type)
{
    const char *type_str;
    unsigned int i;

    obj->props = drmModeObjectGetProperties(fd, obj->id, type);
    if (!obj->props) {
        switch (type) {
            case DRM_MODE_OBJECT_CONNECTOR:
                type_str = "connector";
                break;
            case DRM_MODE_OBJECT_PLANE:
                type_str = "plane";
                break;
            case DRM_MODE_OBJECT_CRTC:
                type_str = "CRTC";
                break;
            default:
                type_str = "unknown type";
                break;
        }
        fprintf(stderr, "cannot get %s %d properties: %s\n", type_str, obj->id, strerror(errno));
        return;
    }

    obj->props_info = calloc(obj->props->count_props, sizeof(obj->props_info));
    for (i = 0; i < obj->props->count_props; ++i)
        obj->props_info[i] = drmModeGetProperty(fd, obj->props->props[i]);
}

static int set_drm_object_property(drmModeAtomicReq *req, struct drm_object *obj, const char *name, uint64_t value)
{
    int i;
    uint32_t prop_id = 0;

    for (i = 0; i < obj->props->count_props; ++i) {
        if (!strcmp(obj->props_info[i]->name, name)) {
            prop_id = obj->props_info[i]->prop_id;
            break;
        }
    }

    if (prop_id == 0) {
        fprintf(stderr, "no object property: %s\n", name);
        return -EINVAL;
    }

    return drmModeAtomicAddProperty(req, obj->id, prop_id, value);
}

static int modeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn, struct modeset_output *out)
{
    drmModeEncoder *enc;
    unsigned int i, j;
    uint32_t crtc;
    struct modeset_output *iter;

    if (conn->encoder_id)
        enc = drmModeGetEncoder(fd, conn->encoder_id);
    else
        enc = NULL;

    if (enc) {
        if (enc->crtc_id) {
            crtc = enc->crtc_id;
            for (iter = output_list; iter; iter = iter->next) {
                if (iter->crtc.id == crtc) {
                    crtc = 0;
                    break;
                }
            }

            if (crtc > 0) {
                drmModeFreeEncoder(enc);
                out->crtc.id = crtc;
                for (i = 0; i < res->count_crtcs; ++i) {
                    if (res->crtcs[i] == crtc) {
                        out->crtc_index = i;
                        break;
                    }
                }
                return 0;
            }
        }
        drmModeFreeEncoder(enc);
    }

    for (i = 0; i < conn->count_encoders; ++i) {
        enc = drmModeGetEncoder(fd, conn->encoders[i]);
        if (!enc) {
            fprintf(stderr, "cannot retrieve encoder %u:%u (%d): %m\n", i, conn->encoders[i], errno);
            continue;
        }

        for (j = 0; j < res->count_crtcs; ++j) {
            if (!(enc->possible_crtcs & (1 << j)))
                continue;

            crtc = res->crtcs[j];
            for (iter = output_list; iter; iter = iter->next) {
                if (iter->crtc.id == crtc) {
                    crtc = 0;
                    break;
                }
            }

            if (crtc > 0) {
                fprintf(stderr, "crtc %u found for encoder %u, will need full modeset\n", crtc, conn->encoders[i]);
                drmModeFreeEncoder(enc);
                out->crtc.id = crtc;
                out->crtc_index = j;
                return 0;
            }
        }

        drmModeFreeEncoder(enc);
    }

    fprintf(stderr, "cannot find suitable crtc for connector %u\n", conn->connector_id);
    return -ENOENT;
}

static int modeset_find_plane(int fd, struct modeset_output *out)
{
    drmModePlaneResPtr plane_res;
    bool found_primary = false;
    int i, ret = -EINVAL;

    plane_res = drmModeGetPlaneResources(fd);
    if (!plane_res) {
        fprintf(stderr, "drmModeGetPlaneResources failed: %s\n", strerror(errno));
        return -ENOENT;
    }

    for (i = 0; i < plane_res->count_planes && !found_primary; ++i) {
        int plane_id = plane_res->planes[i];

        drmModePlanePtr plane = drmModeGetPlane(fd, plane_id);
        if (!plane) {
            fprintf(stderr, "drmModeGetPlane(%u) failed: %s\n", plane_id, strerror(errno));
            continue;
        }

        if (plane->possible_crtcs & (1 << out->crtc_index)) {
            drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);

            if (get_property_value(fd, props, "type") == DRM_PLANE_TYPE_PRIMARY) {
                found_primary = true;
                out->plane.id = plane_id;
                ret = 0;
            }

            drmModeFreeObjectProperties(props);
        }

        drmModeFreePlane(plane);
    }

    drmModeFreePlaneResources(plane_res);

    if (found_primary)
        fprintf(stderr, "found primary plane, id: %d\n", out->plane.id);
    else
        fprintf(stderr, "couldn't find a primary plane\n");
    return ret;
}

static void modeset_drm_object_fini(struct drm_object *obj)
{
    for (int i = 0; i < obj->props->count_props; ++i)
        drmModeFreeProperty(obj->props_info[i]);
    free(obj->props_info);
    drmModeFreeObjectProperties(obj->props);
}

static int modeset_setup_objects(int fd, struct modeset_output *out)
{
    struct drm_object *connector = &out->connector;
    struct drm_object *crtc = &out->crtc;
    struct drm_object *plane = &out->plane;

    modeset_get_object_properties(fd, connector, DRM_MODE_OBJECT_CONNECTOR);
    if (!connector->props)
        goto out_conn;

    modeset_get_object_properties(fd, crtc, DRM_MODE_OBJECT_CRTC);
    if (!crtc->props)
        goto out_crtc;

    modeset_get_object_properties(fd, plane, DRM_MODE_OBJECT_PLANE);
    if (!plane->props)
        goto out_plane;

    return 0;

out_plane:
    modeset_drm_object_fini(crtc);
out_crtc:
    modeset_drm_object_fini(connector);
out_conn:
    return -ENOMEM;
}

static void modeset_destroy_objects(int fd, struct modeset_output *out)
{
    modeset_drm_object_fini(&out->connector);
    modeset_drm_object_fini(&out->crtc);
    modeset_drm_object_fini(&out->plane);
}

static int modeset_create_fb(int fd, struct modeset_buf *buf)
{
    struct drm_mode_create_dumb creq;
    struct drm_mode_destroy_dumb dreq;
    struct drm_mode_map_dumb mreq;
    int ret;
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};

    memset(&creq, 0, sizeof(creq));
    creq.width = buf->width;
    creq.height = buf->height;
    creq.bpp = 32;
    ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
    if (ret < 0) {
        fprintf(stderr, "cannot create dumb buffer (%d): %m\n", errno);
        return -errno;
    }
    buf->stride = creq.pitch;
    buf->size = creq.size;
    buf->handle = creq.handle;

    handles[0] = buf->handle;
    pitches[0] = buf->stride;
    ret = drmModeAddFB2(fd, buf->width, buf->height, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &buf->fb, 0);
    if (ret) {
        fprintf(stderr, "cannot create framebuffer (%d): %m\n", errno);
        ret = -errno;
        goto err_destroy;
    }

    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = buf->handle;
    ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
    if (ret) {
        fprintf(stderr, "cannot map dumb buffer (%d): %m\n", errno);
        ret = -errno;
        goto err_fb;
    }

    buf->map = mmap(0, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
    if (buf->map == MAP_FAILED) {
        fprintf(stderr, "cannot mmap dumb buffer (%d): %m\n", errno);
        ret = -errno;
        goto err_fb;
    }

    memset(buf->map, 0, buf->size);

    return 0;

err_fb:
    drmModeRmFB(fd, buf->fb);
err_destroy:
    memset(&dreq, 0, sizeof(dreq));
    dreq.handle = buf->handle;
    drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    return ret;
}

static void modeset_destroy_fb(int fd, struct modeset_buf *buf)
{
    struct drm_mode_destroy_dumb dreq;

    munmap(buf->map, buf->size);

    drmModeRmFB(fd, buf->fb);

    memset(&dreq, 0, sizeof(dreq));
    dreq.handle = buf->handle;
    drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
}

static int modeset_setup_framebuffers(int fd, drmModeConnector *conn, struct modeset_output *out)
{
    int i, ret;

    for (i = 0; i < 2; ++i) {
        out->bufs[i].width = conn->modes[0].hdisplay;
        out->bufs[i].height = conn->modes[0].vdisplay;

        ret = modeset_create_fb(fd, &out->bufs[i]);
        if (ret) {
            if (i == 1)
                modeset_destroy_fb(fd, &out->bufs[0]);
            return ret;
        }
    }

    return 0;
}

static void modeset_output_destroy(int fd, struct modeset_output *out)
{
    modeset_destroy_objects(fd, out);

    modeset_destroy_fb(fd, &out->bufs[0]);
    modeset_destroy_fb(fd, &out->bufs[1]);

    drmModeDestroyPropertyBlob(fd, out->mode_blob_id);

    free(out);
}

static struct modeset_output* modeset_output_create(int fd, drmModeRes *res, drmModeConnector *conn)
{
    int ret;
    struct modeset_output *out;

    out = malloc(sizeof(*out));
    memset(out, 0, sizeof(*out));
    out->connector.id = conn->connector_id;

    if (conn->connection != DRM_MODE_CONNECTED) {
        fprintf(stderr, "ignoring unused connector %u\n", conn->connector_id);
        goto out_error;
    }

    memcpy(&out->mode, &conn->modes[0], sizeof(out->mode));
    if (drmModeCreatePropertyBlob(fd, &out->mode, sizeof(out->mode), &out->mode_blob_id) != 0) {
        fprintf(stderr, "couldn't create a blob property\n");
        goto out_error;
    }
    fprintf(stderr, "mode for connector %u is %ux%u\n", conn->connector_id, out->bufs[0].width, out->bufs[0].height);

    ret = modeset_find_crtc(fd, res, conn, out);
    if (ret) {
        fprintf(stderr, "no valid crtc for connector %u\n", conn->connector_id);
        goto out_blob;
    }

    ret = modeset_find_plane(fd, out);
    if (ret) {
        fprintf(stderr, "no valid plane for crtc %u\n", out->crtc.id);
        goto out_blob;
    }

    ret = modeset_setup_objects(fd, out);
    if (ret) {
        fprintf(stderr, "cannot get plane properties\n");
        goto out_blob;
    }

    ret = modeset_setup_framebuffers(fd, conn, out);
    if (ret) {
        fprintf(stderr, "cannot create framebuffer for connector %u\n", conn->connector_id);
        goto out_obj;
    }

    return out;

out_obj:
    modeset_destroy_objects(fd, out);
out_blob:
    drmModeDestroyPropertyBlob(fd, out->mode_blob_id);
out_error:
    free(out);
    return NULL;
}

static int modeset_prepare(int fd)
{
    drmModeRes *res;
    drmModeConnector *conn;
    unsigned int i;
    struct modeset_output *out;

    res = drmModeGetResources(fd);
    if (!res) {
        fprintf(stderr, "cannot retrieve DRM resources (%d): %m\n", errno);
        return -errno;
    }

    for (i = 0; i < res->count_connectors; ++i) {
        conn = drmModeGetConnector(fd, res->connectors[i]);
        if (!conn) {
            fprintf(stderr, "cannot retrieve DRM connector %u:%u (%d): %m\n", i, res->connectors[i], errno);
            continue;
        }

        out = modeset_output_create(fd, res, conn);
        drmModeFreeConnector(conn);
        if (!out)
            continue;

        out->next = output_list;
        output_list = out;
    }
    if (!output_list) {
        fprintf(stderr, "couldn't create any outputs\n");
        return -1;
    }

    drmModeFreeResources(res);
    return 0;
}

static int modeset_atomic_prepare_commit(int fd, struct modeset_output *out, drmModeAtomicReq *req)
{
    struct drm_object *plane = &out->plane;
    struct modeset_buf *buf = &out->bufs[out->front_buf ^ 1];

    if (set_drm_object_property(req, &out->connector, "CRTC_ID", out->crtc.id) < 0)
        return -1;

    if (set_drm_object_property(req, &out->crtc, "MODE_ID", out->mode_blob_id) < 0)
        return -1;

    if (set_drm_object_property(req, &out->crtc, "ACTIVE", 1) < 0)
        return -1;

    if (set_drm_object_property(req, plane, "FB_ID", buf->fb) < 0)
        return -1;
    if (set_drm_object_property(req, plane, "CRTC_ID", out->crtc.id) < 0)
        return -1;
    if (set_drm_object_property(req, plane, "SRC_X", 0) < 0)
        return -1;
    if (set_drm_object_property(req, plane, "SRC_Y", 0) < 0)
        return -1;
    if (set_drm_object_property(req, plane, "SRC_W", buf->width << 16) < 0)
        return -1;
    if (set_drm_object_property(req, plane, "SRC_H", buf->height << 16) < 0)
        return -1;
    if (set_drm_object_property(req, plane, "CRTC_X", 0) < 0)
        return -1;
    if (set_drm_object_property(req, plane, "CRTC_Y", 0) < 0)
        return -1;
    if (set_drm_object_property(req, plane, "CRTC_W", buf->width) < 0)
        return -1;
    if (set_drm_object_property(req, plane, "CRTC_H", buf->height) < 0)
        return -1;

    return 0;
}

static uint8_t next_color(bool *up, uint8_t cur, unsigned int mod)
{
    uint8_t next;
    next = cur + (*up ? 1 : -1) * (rand() % mod);
    if ((*up && next < cur) || (!*up && next > cur)) {
        *up = !*up;
        next = cur;
    }

    return next;
}

static void modeset_paint_framebuffer(struct modeset_output *out)
{
    struct modeset_buf *buf;
    unsigned int j, k, off;

    out->r = next_color(&out->r_up, out->r, 5);
    out->g = next_color(&out->g_up, out->g, 5);
    out->b = next_color(&out->b_up, out->b, 5);
    buf = &out->bufs[out->front_buf ^ 1];
    for (j = 0; j < buf->height; ++j) {
        for (k = 0; k < buf->width; ++k) {
            off = buf->stride * j + k * 4;
            *(uint32_t*)&buf->map[off] = (out->r << 16) | (out->g << 8) | out->b;
        }
    }
}

static void modeset_draw_out(int fd, struct modeset_output *out)
{
    drmModeAtomicReq *req;
    int ret, flags;

    modeset_paint_framebuffer(out);

    req = drmModeAtomicAlloc();
    ret = modeset_atomic_prepare_commit(fd, out, req);
    if (ret < 0) {
        fprintf(stderr, "prepare atomic commit failed, %d\n", errno);
        return;
    }

    flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;
    ret = drmModeAtomicCommit(fd, req, flags, NULL);
    drmModeAtomicFree(req);

    if (ret < 0) {
        fprintf(stderr, "atomic commit failed, %d\n", errno);
        return;
    }

    out->front_buf ^= 1;
    out->pflip_pending = true;
}

static void modeset_page_flip_event(int fd, unsigned int frame, unsigned int sec, unsigned int usec, unsigned int crtc_id, void *data)
{
    struct modeset_output *out, *iter;

    out = NULL;
    for (iter = output_list; iter; iter = iter->next) {
        if (iter->crtc.id == crtc_id) {
            out = iter;
            break;
        }
    }

    if (out == NULL)
        return;

    out->pflip_pending = false;
    if (!out->cleanup)
        modeset_draw_out(fd, out);
}

static int modeset_perform_modeset(int fd)
{
    int ret, flags;
    struct modeset_output *iter;
    drmModeAtomicReq *req;

    req = drmModeAtomicAlloc();
    for (iter = output_list; iter; iter = iter->next) {
        ret = modeset_atomic_prepare_commit(fd, output_list, req);
        if (ret < 0)
            break;
    }
    if (ret < 0) {
        fprintf(stderr, "prepare atomic commit failed, %d\n", errno);
        return ret;
    }

    flags = DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET;
    ret = drmModeAtomicCommit(fd, req, flags, NULL);
    if (ret < 0) {
        fprintf(stderr, "test-only atomic failed, %d\n", errno);
        drmModeAtomicFree(req);
        return ret;
    }

    for (iter = output_list; iter; iter = iter->next) {
        iter->r = rand() % 0xff;
        iter->g = rand() % 0xff;
        iter->b = rand() % 0xff;
        iter->r_up = iter->g_up = iter->b_up = true;

        modeset_paint_framebuffer(iter);
    }

    flags = DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT;
    ret = drmModeAtomicCommit(fd, req, flags, NULL);
    if (ret < 0)
        fprintf(stderr, "modeset aomic commit failed, %d\n", errno);

    drmModeAtomicFree(req);

    return ret;
}

static void modeset_draw(int fd)
{
    int ret;
    fd_set fds;
    time_t start, cur;
    struct timeval v;
    drmEventContext ev;

    srand(time(&start));
    FD_ZERO(&fds);
    memset(&v, 0, sizeof(v));
    memset(&ev, 0 ,sizeof(ev));

    ev.version = 3;
    ev.page_flip_handler2 = modeset_page_flip_event;

    modeset_perform_modeset(fd);

    while (time(&cur) < start + 5) {
        FD_SET(0, &fds);
        FD_SET(fd, &fds);
        v.tv_sec = start + 5 - cur;

        ret = select(fd + 1, &fds, NULL, NULL, &v);
        if (ret < 0) {
            fprintf(stderr, "select() failed with %d: %m\n", errno);
            break;
        }
        else if (FD_ISSET(0, &fds)) {
            fprintf(stderr, "exit due to user-input\n");
            break;
        }
        else if (FD_ISSET(fd, &fds)) {
            drmHandleEvent(fd, &ev);
        }
    }
}

static void modeset_cleanup(int fd)
{
    struct modeset_output *iter;
    drmEventContext ev;
    int ret;

    memset(&ev, 0, sizeof(ev));
    ev.version = 3;
    ev.page_flip_handler2 = modeset_page_flip_event;

    while (output_list) {
        iter = output_list;

        iter->cleanup = true;
        fprintf(stderr, "wait for pending page-flip to complete...\n");
        while (iter->pflip_pending) {
            ret = drmHandleEvent(fd, &ev);
            if (ret)
                break;
        }

        output_list = iter->next;

        modeset_output_destroy(fd, iter);
    }
}

int main(int argc, char **argv)
{
    int ret, fd;
    const char *card;

    if (argc > 1)
        card = argv[1];
    else
        card = "/dev/dri/card0";

    fprintf(stderr, "using card '%s'\n", card);

    ret = modeset_open(&fd, card);
    if (ret)
        goto out_return;

    ret = modeset_prepare(fd);
    if (ret)
        goto out_close;

    modeset_draw(fd);

    modeset_cleanup(fd);

    ret = 0;

out_close:
    close(fd);
out_return:
    if (ret) {
        errno = -ret;
        fprintf(stderr, "modeset failed with error %d: %m\n", errno);
    }
    else {
        fprintf(stderr, "exiting\n");
    }
    return ret;
}
