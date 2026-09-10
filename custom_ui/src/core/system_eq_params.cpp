#include "core/system_eq_params.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "core/log_timing.h"

namespace core {

static bool write_to(const char *path, const char *tmp_path, const CarpiEqParams &params) {
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }
    ssize_t written = write(fd, &params, sizeof(params));
    close(fd);
    if (written != static_cast<ssize_t>(sizeof(params))) {
        unlink(tmp_path);
        return false;
    }
    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return false;
    }
    return true;
}

bool write_system_eq_params(const CarpiEqParams &params) {
    // 1. Attempt primary write to persistent ext4 partition 3 (/data)
    if (write_to(CARPI_EQ_PARAMS_PRIMARY_PATH, CARPI_EQ_PARAMS_PRIMARY_PATH ".tmp", params)) {
        return true;
    }

    // 2. Fallback to /tmp if /data is not mounted or writable (e.g. host dev build or early boot)
    if (write_to(CARPI_EQ_PARAMS_FALLBACK_PATH, CARPI_EQ_PARAMS_FALLBACK_PATH ".tmp", params)) {
        return true;
    }

    std::fprintf(stderr, "%s [HAL:AUDIO] Failed to write system EQ params to both /data and /tmp: %s\n",
                 core::log_timestamp().c_str(), std::strerror(errno));
    return false;
}

}  // namespace core
