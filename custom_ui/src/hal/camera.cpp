#include "hal/camera.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace hal {

namespace {

// Local copy of the ioctl numbers this HAL needs, taken directly from
// the real driver sources in the sibling linux-arkmicro repo (not
// vendored wholesale -- these two headers pull in a large chunk of
// kernel-only type machinery neither driver's userspace ABI actually
// needs). Kept minimal on purpose, same reasoning as
// third_party/bluez_uapi/bluetooth/bluetooth.h's header comment.
//
//   linux/drivers/soc/arkmicro/itu656/ark1668_itu656.h  (ARK_DVR_*)
//   linux/drivers/soc/arkmicro/ark-carback.c             (CARBACK_IOCTL_*)
//
// _IO/_IOR/_IOWR encodings copied by hand to match those headers
// exactly -- if the kernel driver's ioctl numbers ever change, this
// silently goes stale; there's no automated cross-check against the
// other repo.
#define ARK_DVR_IOC_MAGIC 'n'
#define ARK_DVR_START _IO(ARK_DVR_IOC_MAGIC, 0x10)
#define ARK_DVR_STOP _IO(ARK_DVR_IOC_MAGIC, 0x20)

#define CARBACK_IOCTL_BASE 0x9A
#define CARBACK_IOCTL_SET_APP_READY _IO(CARBACK_IOCTL_BASE, 0)
#define CARBACK_IOCTL_APP_ENTER_DONE _IO(CARBACK_IOCTL_BASE, 1)
#define CARBACK_IOCTL_APP_EXIT_DONE _IO(CARBACK_IOCTL_BASE, 2)
#define CARBACK_IOCTL_GET_STATUS _IOR(CARBACK_IOCTL_BASE, 3, int)

}  // namespace

bool init_camera(CameraHandle & out, const char * dvr_path, const char * carback_path) {
    out.dvr_fd = open(dvr_path, O_RDWR);
    if (out.dvr_fd < 0) {
        std::fprintf(stderr, "hal::camera::init_camera: warning: %s unavailable (%s)\n",
                     dvr_path, std::strerror(errno));
    }

    out.carback_fd = open(carback_path, O_RDWR);
    if (out.carback_fd < 0) {
        std::fprintf(stderr, "hal::camera::init_camera: warning: %s unavailable (%s)\n",
                     carback_path, std::strerror(errno));
    }

    if (out.dvr_fd < 0 && out.carback_fd < 0) {
        return false;
    }
    return true;
}

void start_camera_stream(CameraHandle & h) {
    if (h.dvr_fd < 0) return;
    if (ioctl(h.dvr_fd, ARK_DVR_START) < 0) {
        std::fprintf(stderr, "hal::camera::start_camera_stream: ARK_DVR_START failed (%s)\n",
                     std::strerror(errno));
    }
}

void stop_camera_stream(CameraHandle & h) {
    if (h.dvr_fd < 0) return;
    if (ioctl(h.dvr_fd, ARK_DVR_STOP) < 0) {
        std::fprintf(stderr, "hal::camera::stop_camera_stream: ARK_DVR_STOP failed (%s)\n",
                     std::strerror(errno));
    }
}

ReverseGearState get_reverse_gear_state(CameraHandle & h) {
    if (h.carback_fd < 0) return ReverseGearState::Unknown;
    int status = 0;
    if (ioctl(h.carback_fd, CARBACK_IOCTL_GET_STATUS, &status) < 0) {
        return ReverseGearState::Unknown;
    }
    return status ? ReverseGearState::Engaged : ReverseGearState::Disengaged;
}

void set_app_ready(CameraHandle & h) {
    if (h.carback_fd < 0) return;
    ioctl(h.carback_fd, CARBACK_IOCTL_SET_APP_READY);
}

void ack_enter_done(CameraHandle & h) {
    if (h.carback_fd < 0) return;
    ioctl(h.carback_fd, CARBACK_IOCTL_APP_ENTER_DONE);
}

void ack_exit_done(CameraHandle & h) {
    if (h.carback_fd < 0) return;
    ioctl(h.carback_fd, CARBACK_IOCTL_APP_EXIT_DONE);
}

ReverseGearState wait_reverse_gear_change(CameraHandle & h) {
    if (h.carback_fd < 0) return ReverseGearState::Unknown;
    unsigned char status = 0;
    // Driver's own read() contract (ark_carback_read in ark-carback.c):
    // blocks on carback_waiq until carback_changed, requires size==1.
    ssize_t n = read(h.carback_fd, &status, 1);
    if (n != 1) return ReverseGearState::Unknown;
    return status ? ReverseGearState::Engaged : ReverseGearState::Disengaged;
}

void close_camera(CameraHandle & h) {
    if (h.dvr_fd >= 0) {
        close(h.dvr_fd);
        h.dvr_fd = -1;
    }
    if (h.carback_fd >= 0) {
        close(h.carback_fd);
        h.carback_fd = -1;
    }
}

}  // namespace hal
