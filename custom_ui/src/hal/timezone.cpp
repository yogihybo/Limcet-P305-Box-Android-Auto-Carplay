#include "hal/timezone.h"
#include "core/config_store.h"
#include "core/log_timing.h"

#include <cstdlib>
#include <cstdio>
#include <ctime>

namespace hal {

namespace {

const std::vector<TimezoneEntry> s_timezones = {
    { "UTC (UTC+0)", "UTC0" },
    { "Sydney / Melb / Hobart (UTC+10 / DST +11)", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Brisbane / QLD (UTC+10)", "AEST-10" },
    { "Adelaide / SA (UTC+9:30 / DST +10:30)", "ACST-9:30ACDT,M10.1.0,M4.1.0/3" },
    { "Darwin / NT (UTC+9:30)", "ACST-9:30" },
    { "Perth / WA (UTC+8)", "AWST-8" },
    { "Auckland / NZ (UTC+12 / DST +13)", "NZST-12NZDT,M9.5.0,M4.1.0/3" },
    { "Tokyo / Japan (UTC+9)", "JST-9" },
    { "Singapore / HK / Beijing (UTC+8)", "CST-8" },
    { "Bangkok / Jakarta (UTC+7)", "WIB-7" },
    { "India / New Delhi (UTC+5:30)", "IST-5:30" },
    { "Dubai / UAE (UTC+4)", "GST-4" },
    { "London / GMT (UTC+0 / DST +1)", "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Paris / Berlin / Rome (UTC+1 / DST +2)", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Athens / Helsinki (UTC+2 / DST +3)", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "New York / Toronto (UTC-5 / DST -4)", "EST5EDT,M3.2.0,M11.1.0" },
    { "Chicago / Central (UTC-6 / DST -5)", "CST6CDT,M3.2.0,M11.1.0" },
    { "Denver / Mountain (UTC-7 / DST -6)", "MST7MDT,M3.2.0,M11.1.0" },
    { "Los Angeles / PST (UTC-8 / DST -7)", "PST8PDT,M3.2.0,M11.1.0" },
    { "Hawaii (UTC-10)", "HST10" },
};

} // namespace

const std::vector<TimezoneEntry> & get_timezones() {
    return s_timezones;
}

void apply_timezone(int index) {
    if (index < 0 || index >= static_cast<int>(s_timezones.size())) {
        index = 0; // Default to UTC
    }

    const auto & tz = s_timezones[index];
    setenv("TZ", tz.tz_posix, 1);
    tzset();

    FILE * f = fopen("/etc/TZ", "w");
    if (f) {
        fprintf(f, "%s\n", tz.tz_posix);
        fclose(f);
    }

    core::default_store().set_int("TimezoneIndex", index, "General");
    core::default_store().save();

    std::printf("%s [TIMEZONE] Set timezone to index %d: %s (%s)\n",
                core::log_timestamp().c_str(), index, tz.label, tz.tz_posix);
}

int get_current_timezone_index() {
    int idx = core::default_store().get_int("TimezoneIndex", 0, "General");
    if (idx < 0 || idx >= static_cast<int>(s_timezones.size())) {
        idx = 0;
    }
    return idx;
}

} // namespace hal
