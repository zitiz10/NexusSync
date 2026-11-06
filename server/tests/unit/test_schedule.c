/*
 * test_schedule.c — plat_next_local_time, the clock behind the nightly
 * cleanup at 02:00 local time (SPEC §10, D-048). The time zone is set
 * through TZ, so the expected instants are exact. POSIX only: Windows
 * reads its zone from the system, so CI there checks just the bounds.
 */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L /* setenv, tzset under -std=c11 */
#endif

#include "nx_test.h"
#include "platform/platform.h"

#include <stdlib.h>
#include <time.h>

#if !defined(_WIN32)
static void use_tz(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();
}
#endif

static void test_next_two_am(void)
{
#if !defined(_WIN32)
    use_tz("UTC0");
    /* 2026-11-06 00:30 UTC -> 02:00 the same day. */
    CHECK_EQ_U64(plat_next_local_time(1793925000u, 2, 0), 1793930400u);
    /* Exactly 02:00 is not "after now": the next one is tomorrow. */
    CHECK_EQ_U64(plat_next_local_time(1793930400u, 2, 0), 1794016800u);
    /* 03:00 -> tomorrow 02:00. */
    CHECK_EQ_U64(plat_next_local_time(1793934000u, 2, 0), 1794016800u);

    /* India, UTC+5:30: 20:00 UTC is 01:30 local, so 02:00 local is 20:30 UTC. */
    use_tz("IST-5:30");
    CHECK_EQ_U64(plat_next_local_time(1793908800u, 2, 0), 1793910600u);

    /* New York on 2026-03-08: clocks jump from 02:00 to 03:00, so 02:00
     * does not exist that night. 06:30 UTC is 01:30 EST; the run happens
     * at the first valid time after the gap, 03:00 EDT = 07:00 UTC. */
    use_tz("EST5EDT,M3.2.0,M11.1.0");
    uint64_t t = plat_next_local_time(1772951400u, 2, 0);
    CHECK(t >= 1772951400u + 1800u && t <= 1772953200u);
    unsetenv("TZ");
    tzset();
#endif
    /* Any zone: the next run is in the future and at most a day away
     * (plus an hour for a daylight-saving change). */
    uint64_t now = plat_now_unix_s();
    uint64_t next = plat_next_local_time(now, 2, 0);
    CHECK(next > now && next <= now + 25u * 3600u);
}

static const nx_test_case TESTS[] = {
    NX_CASE(test_next_two_am),
};

NX_TEST_MAIN(TESTS)
