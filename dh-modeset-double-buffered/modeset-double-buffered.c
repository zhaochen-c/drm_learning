#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

struct modeset_buf;
struct modeset_dev;

static int modeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn, struct modeset_dev *dev);
static int modeset_create_fb(int fd, struct modeset_buf *buf);
static void modeset_destroy_fb(int fd, struct modeset_buf *buf);
static int modeset_setup_dev(int fd, drmModeRes *res, drmModeConnector *conn, struct modeset_dev *dev);
static int modeset_open(int *out, const char *node);
static int modeset_prepare(int fd);
static void modeset_draw(int fd);
static void modeset_cleanup(int fd);

static int modeset_open(int *out, const char *node)
{
    int fd, ret;
    uint64_t has_dumb;

    fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ret = -errno;
        fprintf(stderr, "cannot open '%s' %m\n", node);
        return ret;
    }

    if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb) < 0 || !has_dumb) {
        fprintf(stderr, "drm device '%s' does not support dumb buffer\n", node);
        close(fd);
        return -EOPNOTSUPP;
    }

    *out = fd;
    return 0;
}

struct modeset_buf {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t size;
    uint32_t handle;
    uint8_t *map;
    uint32_t fb;
};

struct modeset_dev {
    struct modeset_dev *next;

    unsigned int front_buf;
    struct modeset_buf bufs[2];

    drmModeModeInfo mode;
    uint32_t conn;
    uint32_t crtc;
    drmModeCrtc *saved_crtc;
};

static struct modeset_dev *modeset_list = NULL;

static int modeset_prepare(int fd)
{
    drmModeRes *res;
    drmModeConnector *conn;
    unsigned int i;
    struct modeset_dev *dev;
    int ret;

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

		dev = malloc(sizeof(*dev));
		memset(dev, 0, sizeof(*dev));
		dev->conn = conn->connector_id;

        ret = modeset_setup_dev(fd, res, conn, dev);
        if (ret) {
            if (ret != -ENOENT) {
                errno = -ret;
                fprintf(stderr, "cannot setup device for connector %u:%u (%d): %m\n", i, res->connectors[i], errno);
            }
            free(dev);
            drmModeFreeConnector(conn);
            continue;
        }

        drmModeFreeConnector(conn);
        dev->next = modeset_list;
        modeset_list = dev;
    }

    drmModeFreeResources(res);
    return 0;
}

static int modeset_setup_dev(int fd, drmModeRes *res, drmModeConnector *conn, struct modeset_dev *dev)
{
    int ret;

    if (conn->connection != DRM_MODE_CONNECTED) {
        fprintf(stderr, "ignoring unused connector %u\n", conn->connector_id);
        return -ENOENT;
    }

    if (conn->count_modes == 0) {
        fprintf(stderr, "no valid mode for connector %u\n", conn->connector_id);
        return -EFAULT;
    }

    memcpy(&dev->mode, &conn->modes[0], sizeof(dev->mode));
    dev->bufs[0].width = conn->modes[0].hdisplay;
    dev->bufs[0].height = conn->modes[0].vdisplay;
    dev->bufs[1].width = conn->modes[0].hdisplay;
    dev->bufs[1].height = conn->modes[0].vdisplay;
    fprintf(stderr, "mode for connector %u is %ux%u\n", conn->connector_id, dev->bufs[0].width, dev->bufs[0].height);

    ret = modeset_find_crtc(fd, res, conn, dev);
    if (ret) {
        fprintf(stderr, "no valid crtc for connector %u\n", conn->connector_id);
        return ret;
    }

    ret = modeset_create_fb(fd, &dev->bufs[0]);
    if (ret) {
        fprintf(stderr, "cannot create framebuffer for connector %u\n", conn->connector_id);
        return ret;
    }

    ret = modeset_create_fb(fd, &dev->bufs[1]);
    if (ret) {
        fprintf(stderr, "cannot create framebuffer for connector %u\n", conn->connector_id);
        modeset_destroy_fb(fd, &dev->bufs[0]);
        return ret;
    }

    return 0;
}

static int modeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn, struct modeset_dev *dev)
{
    drmModeEncoder *enc;
    unsigned int i, j;
    int32_t crtc;
    struct modeset_dev *iter;

    if (conn->encoder_id)
        enc = drmModeGetEncoder(fd, conn->encoder_id);
    else
        enc = NULL;

    if (enc) {
        if (enc->crtc_id) {
            crtc = enc->crtc_id;
            for (iter = modeset_list; iter; iter = iter->next) {
                if (iter->crtc == crtc) {
                    crtc = -1;
                    break;
                }
            }

            if (crtc >= 0) {
                drmModeFreeEncoder(enc);
                dev->crtc = crtc;
                return 0;
            }
        }
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
            for (iter = modeset_list; iter; iter = iter->next) {
                if (iter->crtc == crtc) {
                    crtc = -1;
                    break;
                }
            }

            if (crtc >= 0) {
                drmModeFreeEncoder(enc);
                dev->crtc = crtc;
                return 0;
            }
        }

        drmModeFreeEncoder(enc);
    }

    fprintf(stderr, "cannot find suitable CRTC for connector %u\n", conn->connector_id);
    return -ENOENT;
}

static int modeset_create_fb(int fd, struct modeset_buf *buf)
{
    struct drm_mode_create_dumb creq;
    struct drm_mode_destroy_dumb dreq;
    struct drm_mode_map_dumb mreq;
    int ret;

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

    ret = drmModeAddFB(fd, buf->width, buf->height, 24, 32, buf->stride, buf->handle, &buf->fb);
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

int main(int argc, char **argv)
{
    int ret, fd;
    const char *card;
    struct modeset_dev *iter;
    struct modeset_buf *buf;

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

    for (iter = modeset_list; iter; iter = iter->next) {
        iter->saved_crtc = drmModeGetCrtc(fd, iter->crtc);
        buf = &iter->bufs[iter->front_buf];
        ret = drmModeSetCrtc(fd, iter->crtc, buf->fb, 0, 0, &iter->conn, 1, &iter->mode);
        if (ret)
            fprintf(stderr, "cannot set CRTC for connector %u (%d): %m\n", iter->conn, errno);
    }

    modeset_draw(fd);

    modeset_cleanup(fd);

    ret = 0;

out_close:
    close(fd);
out_return:
    if (ret) {
        errno = -ret;
        fprintf(stderr, "modeset failed with error %d: %m\n", errno);
    } else {
        fprintf(stderr, "exiting\n");
    }
    return ret;
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

static void modeset_draw(int fd)
{
    uint8_t r, g, b;
    bool r_up, g_up, b_up;
    unsigned int i, j, k, off;
    struct modeset_dev *iter;
    struct modeset_buf *buf;
    int ret;

    srand(time(NULL));
    r = rand() % 0xff;
    g = rand() % 0xff;
    b = rand() % 0xff;
    r_up = g_up = b_up = true;

    for (i = 0; i < 50; ++i) {
        r = next_color(&r_up, r, 20);
        g = next_color(&g_up, g, 10);
        b = next_color(&b_up, b, 5);

        for (iter = modeset_list; iter; iter = iter->next) {
            buf = &iter->bufs[iter->front_buf ^ 1];
            for (j = 0; j < buf->height; ++j) {
                for (k = 0; k < buf->width; ++k) {
                    off = buf->stride * j + k * 4;
                    *(uint32_t *)&buf->map[off] = (r << 16) | (g << 8) | b;
                }
            }

            ret = drmModeSetCrtc(fd, iter->crtc, buf->fb, 0, 0, &iter->conn, 1, &iter->mode);
            if (ret)
                fprintf(stderr, "cannot flip CRTC for connector %u (%d): %m\n", iter->conn, errno);
            else
                iter->front_buf ^ 1;
        }

        usleep(100000);
    }
}

static void modeset_cleanup(int fd)
{
    struct modeset_dev *iter;

    while (modeset_list) {
        iter = modeset_list;
        modeset_list = iter->next;

        drmModeSetCrtc(fd, iter->saved_crtc->crtc_id, iter->saved_crtc->buffer_id, iter->saved_crtc->x,
                       iter->saved_crtc->y, &iter->conn, 1, &iter->saved_crtc->mode);
        drmModeFreeCrtc(iter->saved_crtc);

        modeset_destroy_fb(fd, &iter->bufs[1]);
        modeset_destroy_fb(fd, &iter->bufs[0]);

        free(iter);
    }
}
