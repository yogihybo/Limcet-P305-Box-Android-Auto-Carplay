#pragma once

#include <string>
#include <vector>

namespace hal {

struct TimezoneEntry {
    const char * label;
    const char * tz_posix;
};

const std::vector<TimezoneEntry> & get_timezones();
void apply_timezone(int index);
int get_current_timezone_index();

} // namespace hal
