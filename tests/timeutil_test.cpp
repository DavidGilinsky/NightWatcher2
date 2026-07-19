// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/timeutil_test.cpp
// Purpose:       Unit tests for the exporter time helpers (UTC<->local, local
//                month bounds, next-local-HH:MM). Uses America/Phoenix, which is
//                a fixed UTC-7 (no DST), so results are deterministic.
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <cstdio>
#include <string>

#include "timeutil.hpp"

namespace tu = nightwatcher::exporter::timeutil;

static int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

static std::string next_hhmm(const std::string& now_utc, int h, int m) {
    time_t now = 0;
    tu::parse_sql_utc(now_utc, now);
    return tu::fmt_sql_utc(tu::next_local_hhmm_utc(h, m, "America/Phoenix", now));
}

int main() {
    const std::string PHX = "America/Phoenix";  // fixed UTC-7

    // local_ym: the sample's last reading (2025-01-01 06:59:31 UTC) is local
    // 2024-12-31 23:59:31 -> bucketed as December 2024.
    CHECK(tu::local_ym("2026-07-19 04:00:00", PHX) == "2026-07");  // local 07-18 21:00
    CHECK(tu::local_ym("2025-01-01 06:59:31", PHX) == "2024-12");
    CHECK(tu::local_ym("2025-01-01 07:00:00", PHX) == "2025-01");  // exactly local 00:00 Jan 1

    // local month bounds (UTC, inclusive).
    {
        const auto [from, to] = tu::local_month_bounds_utc("2026-07-19 04:00:00", PHX);
        CHECK(from == "2026-07-01 07:00:00");  // local 07-01 00:00 = 07:00 UTC
        CHECK(to == "2026-08-01 06:59:59");    // last second before local 08-01 00:00
    }
    {
        // December 2024: the sample's whole span must fall inside these bounds.
        const auto [from, to] = tu::local_month_bounds_utc("2024-12-15 10:00:00", PHX);
        CHECK(from == "2024-12-01 07:00:00");
        CHECK(to == "2025-01-01 06:59:59");
        CHECK(std::string("2024-12-14 03:45:00") >= from);  // sample first row (UTC)
        CHECK(std::string("2025-01-01 06:59:31") <= to);    // sample last row (UTC)
    }

    // next nightly run at 06:00 local.
    CHECK(next_hhmm("2026-07-18 10:00:00", 6, 0) == "2026-07-18 13:00:00");  // 03:00 local -> today 06:00
    CHECK(next_hhmm("2026-07-19 04:00:00", 6, 0) == "2026-07-19 13:00:00");  // 21:00 local -> tomorrow 06:00

    if (g_failures == 0) {
        std::puts("timeutil_test passed");
        return 0;
    }
    std::fprintf(stderr, "timeutil_test FAILED (%d checks)\n", g_failures);
    return 1;
}
