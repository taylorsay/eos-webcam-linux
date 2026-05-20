/*
 * eos-webcam: streams Canon EOS live view to a v4l2loopback device.
 *
 * Uses libgphoto2 for camera communication and libturbojpeg to decode
 * the camera's MJPEG live view into YUYV for v4l2loopback.
 *
 * Usage: eos-webcam [--device /dev/videoX]
 */

#include <gphoto2/gphoto2.h>
#include <turbojpeg.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static std::atomic<bool> g_stop{false};

static void on_signal(int) { g_stop = true; }

static void gp_log_error(GPContext *, const char *msg, void *)
{
    fprintf(stderr, "[gphoto2] %s\n", msg);
}

static void rgb_to_yuyv(const uint8_t *rgb, uint8_t *yuyv, int width, int height)
{
    for (int i = 0; i < width * height; i += 2) {
        int r0 = rgb[i*3+0], g0 = rgb[i*3+1], b0 = rgb[i*3+2];
        int r1 = rgb[(i+1)*3+0], g1 = rgb[(i+1)*3+1], b1 = rgb[(i+1)*3+2];
        yuyv[i*2+0] = (uint8_t)((  66*r0 + 129*g0 +  25*b0 + 128) >> 8) + 16;
        yuyv[i*2+1] = (uint8_t)(( -38*r0 -  74*g0 + 112*b0 + 128) >> 8) + 128;
        yuyv[i*2+2] = (uint8_t)((  66*r1 + 129*g1 +  25*b1 + 128) >> 8) + 16;
        yuyv[i*2+3] = (uint8_t)(( 112*r0 -  94*g0 -  18*b0 + 128) >> 8) + 128;
    }
}

static int configure_loopback(int fd, int width, int height)
{
    struct v4l2_format fmt = {};
    fmt.type                 = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width        = (uint32_t)width;
    fmt.fmt.pix.height       = (uint32_t)height;
    fmt.fmt.pix.pixelformat  = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field        = V4L2_FIELD_NONE;
    fmt.fmt.pix.bytesperline = (uint32_t)width * 2;
    fmt.fmt.pix.sizeimage    = (uint32_t)width * (uint32_t)height * 2;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "VIDIOC_S_FMT (%dx%d): %s\n", width, height, strerror(errno));
        return -1;
    }
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s [--device /dev/videoX]\n"
        "  --device PATH    v4l2loopback device (default: /dev/video10)\n",
        argv0);
}

int main(int argc, char *argv[])
{
    std::string device = "/dev/video10";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--device") && i + 1 < argc) {
            device = argv[++i];
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    /* Open loopback device first so we fail fast if it's missing. */
    int loopfd = open(device.c_str(), O_RDWR);
    if (loopfd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", device.c_str(), strerror(errno));
        return 1;
    }
    /* Placeholder size; reconfigured from the first JPEG frame header. */
    configure_loopback(loopfd, 1056, 704);

    /* Set up gphoto2. */
    GPContext *ctx = gp_context_new();
    gp_context_set_error_func(ctx, gp_log_error, nullptr);

    Camera *camera = nullptr;
    int rc = gp_camera_new(&camera);
    if (rc < GP_OK) {
        fprintf(stderr, "gp_camera_new: %s\n", gp_result_as_string(rc));
        gp_context_unref(ctx);
        close(loopfd);
        return 1;
    }

    fprintf(stderr, "Detecting Canon EOS camera...\n");
    rc = gp_camera_init(camera, ctx);
    if (rc < GP_OK) {
        fprintf(stderr, "No camera found: %s\n", gp_result_as_string(rc));
        gp_camera_free(camera);
        gp_context_unref(ctx);
        close(loopfd);
        return 1;
    }

    CameraAbilities abilities;
    if (gp_camera_get_abilities(camera, &abilities) == GP_OK)
        fprintf(stderr, "Camera: %s\n", abilities.model);

    tjhandle tjh = tjInitDecompress();
    if (!tjh) {
        fprintf(stderr, "Failed to init turbojpeg\n");
        gp_camera_exit(camera, ctx);
        gp_camera_free(camera);
        gp_context_unref(ctx);
        close(loopfd);
        return 1;
    }

    CameraFile *file = nullptr;
    rc = gp_file_new(&file);
    if (rc < GP_OK) {
        fprintf(stderr, "gp_file_new: %s\n", gp_result_as_string(rc));
        tjDestroy(tjh);
        gp_camera_exit(camera, ctx);
        gp_camera_free(camera);
        gp_context_unref(ctx);
        close(loopfd);
        return 1;
    }

    fprintf(stderr, "Streaming to %s — Ctrl-C or SIGTERM to stop\n", device.c_str());

    int actualW = 0, actualH = 0;
    std::vector<uint8_t> rgbBuf, yuyvBuf;
    uint64_t frameCount = 0;

    while (!g_stop) {
        rc = gp_camera_capture_preview(camera, file, ctx);
        if (rc < GP_OK) {
            fprintf(stderr, "\ngp_camera_capture_preview: %s\n", gp_result_as_string(rc));
            break;
        }

        const char *data = nullptr;
        unsigned long size = 0;
        rc = gp_file_get_data_and_size(file, &data, &size);
        if (rc < GP_OK) {
            fprintf(stderr, "\ngp_file_get_data_and_size: %s\n", gp_result_as_string(rc));
            break;
        }

        /* First frame: detect actual JPEG dimensions and reconfigure loopback. */
        if (actualW == 0) {
            int w, h, subsamp, cs;
            if (tjDecompressHeader3(tjh, (const uint8_t *)data, size,
                                    &w, &h, &subsamp, &cs) != 0) {
                fprintf(stderr, "\nFailed to read JPEG header, skipping frame: %s\n",
                        tjGetErrorStr2(tjh));
                continue;
            }
            if (w < 320 || w > 8192 || h < 240 || h > 8192) {
                fprintf(stderr, "\nRejecting implausible JPEG dimensions %dx%d\n", w, h);
                continue;
            }
            actualW = w;
            actualH = h;
            fprintf(stderr, "Live view resolution: %dx%d\n", actualW, actualH);
            if (configure_loopback(loopfd, actualW, actualH) < 0)
                break;
            rgbBuf.resize((size_t)actualW * actualH * 3);
            yuyvBuf.resize((size_t)actualW * actualH * 2);
        }

        if (tjDecompress2(tjh, (const uint8_t *)data, size,
                          rgbBuf.data(), actualW, 0, actualH,
                          TJPF_RGB, TJFLAG_FASTDCT) != 0) {
            fprintf(stderr, "\nMJPEG decode error: %s\n", tjGetErrorStr2(tjh));
        } else {
            rgb_to_yuyv(rgbBuf.data(), yuyvBuf.data(), actualW, actualH);
            if (write(loopfd, yuyvBuf.data(), yuyvBuf.size()) < 0)
                fprintf(stderr, "\nwrite to loopback: %s\n", strerror(errno));
            else
                fprintf(stderr, "\rFrame %" PRIu64, ++frameCount);
        }
    }

    fprintf(stderr, "\nStopping...\n");

    gp_file_free(file);
    tjDestroy(tjh);
    gp_camera_exit(camera, ctx);
    gp_camera_free(camera);
    gp_context_unref(ctx);
    close(loopfd);

    return 0;
}
