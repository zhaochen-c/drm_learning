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

struct modeset_dev;

static int modeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn, struct modeset_dev *dev);
static int modeset_create_fb(int fd, struct modeset_dev *dev);
static int modeset_setup_dev(int fd, drmModeRes *res, drmModeConnector *conn, struct modeset_dev *dev);
static int modeset_open(int *out, const char *node);
static int modeset_prepare(int fd);
static void modeset_draw(void);
static void modeset_cleanup(int fd);

static int modeset_open(int *out, const char *node)
{
    int fd, ret;
    uint64_t has_dumb;

    fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ret = -errno;
        fprintf(stderr, "cannot open '%s': %m\n", node);
        return ret;
    }

    if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb) < 0 || !has_dumb) {
        fprintf(stderr, "drm device '%s' does not support dumb buffers\n", node);
        close(fd);
        return -EOPNOTSUPP;
    }

    *out = fd;
    return 0;
}

struct modeset_dev {
    struct modeset_dev *next;

    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t size;
    uint32_t handle;
    uint8_t *map;

    drmModeModeInfo mode;
    uint32_t fb;
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
            fprintf(stderr, "cannot retrieve DRM connector %u:%u (%d): %m", i, res->connectors[i], errno);
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
    dev->width = conn->modes[0].hdisplay;
    dev->height = conn->modes[0].vdisplay;
    fprintf(stderr, "mode for connector %u is %ux%u\n", conn->connector_id, dev->width, dev->height);

    ret = modeset_find_crtc(fd, res, conn, dev);
    if (ret) {
        fprintf(stderr, "no valid crtc for connector %u\n", conn->connector_id);
        return ret;
    }

    ret = modeset_create_fb(fd, dev);
    if (ret) {
        fprintf(stderr, "cannot create framebuffer for connector %u\n", conn->connector_id);
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

                if (crtc >= 0) {
                    drmModeFreeEncoder(enc);
                    dev->crtc = crtc;
                    return 0;
                }
            }
        }

        drmModeFreeEncoder(enc);
    }

    fprintf(stderr, "cannot find suitable CRTC for connector %u\n", conn->connector_id);
    return -ENOENT;
}

static int modeset_create_fb(int fd, struct modeset_dev *dev)
{
    struct drm_mode_create_dumb creq;
    struct drm_mode_destroy_dumb dreq;
    struct drm_mode_map_dumb mreq;
    int ret;

    memset(&creq, 0, sizeof(creq));
    creq.width = dev->width;
    creq.height = dev->height;
    creq.bpp = 32;
    ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
    if (ret < 0) {
        fprintf(stderr, "cannot create dumb buffer (%d): %m\n", errno);
        return -errno;
    }
    dev->stride = creq.pitch;
    dev->size = creq.size;
    dev->handle = creq.handle;

    ret = drmModeAddFB(fd, dev->width, dev->height, 24, 32, dev->stride, dev->handle, &dev->fb);
    if (ret) {
        fprintf(stderr, "cannot create framebuffer (%d): %m", errno);
        ret = -errno;
        goto err_destroy;
    }

    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = dev->handle;
    ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
    if (ret) {
        fprintf(stderr, "cannot map dumb buffer (%d): %m", errno);
        ret = -errno;
        goto err_fb;
    }

    dev->map = mmap(0, dev->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
    if (dev->map == MAP_FAILED) {
        fprintf(stderr, "cannot mmap dumb buffer (%d): %m\n", errno);
        ret = -errno;
        goto err_fb;
    }

    memset(dev->map, 0, dev->size);
    return 0;

err_fb:
    drmModeRmFB(fd, dev->fb);
err_destroy:
    memset(&dreq, 0, sizeof(dreq));
    dreq.handle = dev->handle;
    drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    return ret;
}

int main(int argc, char **argv)
{
    int ret, fd;
    const char *card;
    struct modeset_dev *iter;

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
        ret = drmModeSetCrtc(fd, iter->crtc, iter->fb, 0, 0, &iter->conn, 1, &iter->mode);

        if (ret)
            fprintf(stderr, "cannot set CRTC for connector %u (%d): %m\n", iter->conn, errno);
    }

    modeset_draw();

    modeset_cleanup(fd);

    ret = 0;

out_close:
    close(fd);
out_return:
    if (ret) {
        errno = -ret;
        fprintf(stderr, "modeset failed width error %d: %m\n", errno);
    } else {
        fprintf(stderr, "exiting\n");
    }

    return ret;
}

static uint8_t next_color(bool *up, uint8_t cur, unsigned int mod)
{
    uint8_t next;

    next = cur + (*up ? 1 : -1) * (rand() % mod);
    if ((*up && next < cur) || !*up && next > cur) {
        *up = !*up;
        next = cur;
    }

    return next;
}

static void modeset_draw(void)
{
    uint8_t r, g, b;
    bool r_up, g_up, b_up;
    unsigned int i, j, k, off;
    struct modeset_dev *iter;

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
            for (j = 0; j < iter->height; ++j) {
                for (k = 0; k < iter->width; ++k) {
                    off = iter->stride * j + k * 4;
                    *(uint32_t *)&iter->map[off] = (r << 16) | (g << 8) | b;
                }
            }
        }

        usleep(100000);
    }
}

static void modeset_cleanup(int fd)
{
    struct modeset_dev *iter;
    struct drm_mode_destroy_dumb dreq;

    while (modeset_list) {
        iter = modeset_list;
        modeset_list = iter->next;

        drmModeSetCrtc(fd, iter->saved_crtc->crtc_id, iter->saved_crtc->buffer_id, iter->saved_crtc->x,
                       iter->saved_crtc->y, &iter->conn, 1, &iter->saved_crtc->mode);
        drmModeFreeCrtc(iter->saved_crtc);

        munmap(iter->map, iter->size);

        drmModeRmFB(fd, iter->fb);

        memset(&dreq, 0, sizeof(dreq));
        dreq.handle = iter->handle;
        drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);

        free(iter);
    }
}
