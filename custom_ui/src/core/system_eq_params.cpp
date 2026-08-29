#include "core/system_eq_params.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "core/log_timing.h"

namespace core {

bool write_system_eq_params(const CarpiEqParams &params) {
    const char *tmp_path = CARPI_EQ_PARAMS_PATH ".tmp";
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        std::fprintf(stderr, "%s [HAL:AUDIO] Failed to open %s for system EQ params: %s\n",
                     core::log_timestamp().c_str(), tmp_path, std::strerror(errno));
        return false;
    }
    ssize_t written = write(fd, &params, sizeof(params));
    close(fd);
    if (written != static_cast<ssize_t>(sizeof(params))) {
        std::fprintf(stderr, "%s [HAL:AUDIO] Short write for system EQ params (%zd/%zu bytes)\n",
                     core::log_timestamp().c_str(), written, sizeof(params));
        return false;
    }
    if (rename(tmp_path, CARPI_EQ_PARAMS_PATH) != 0) {
        std::fprintf(stderr, "%s [HAL:AUDIO] Failed to rename system EQ params into place: %s\n",
                     core::log_timestamp().c_str(), std::strerror(errno));
        return false;
    }
    return true;
}

}  // namespace core
