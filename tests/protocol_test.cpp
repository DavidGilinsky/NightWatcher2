// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/protocol_test.cpp
// Purpose:       Unit tests for the SQM response parsers, using the exact
//                example strings from the Unihedron manual plus edge cases.
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

#include "protocol.hpp"

using namespace nightwatcher::sqm;

static int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "CHECK failed at line %d: %s\n",         \
                         __LINE__, #cond);                                \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

static bool close_to(double a, double b) { return std::fabs(a - b) < 1e-6; }

int main() {
    // Averaged reading (manual example).
    {
        const Reading r =
            parse_reading("r, 06.70m,0000022921Hz,0000000020c,0000000.000s, 039.4C");
        CHECK(close_to(r.mag_arcsec2, 6.70));
        CHECK(r.freq_hz == 22921);
        CHECK(r.period_counts == 20);
        CHECK(close_to(r.period_s, 0.0));
        CHECK(close_to(r.temp_c, 39.4));
        CHECK(r.serial.empty());
    }

    // Negative values + interval report with trailing serial.
    {
        const Reading r = parse_reading(
            "r,-01.23m,0000000100Hz,0000000200c,0000000.500s,-005.0C,00000413");
        CHECK(close_to(r.mag_arcsec2, -1.23));
        CHECK(r.freq_hz == 100);
        CHECK(r.period_counts == 200);
        CHECK(close_to(r.period_s, 0.5));
        CHECK(close_to(r.temp_c, -5.0));
        CHECK(r.serial == "00000413");
    }

    // Unaveraged reading (leading 'u').
    {
        const Reading r =
            parse_reading("u, 06.53m,0000022921Hz,0000000020c,0000000.000s, 039.4C");
        CHECK(close_to(r.mag_arcsec2, 6.53));
    }

    // Unit information (manual example).
    {
        const UnitInfo u = parse_unit_info("i,00000002,00000003,00000001,00000413");
        CHECK(u.protocol == 2);
        CHECK(u.model == 3);
        CHECK(u.feature == 1);
        CHECK(u.serial == "00000413");  // leading zeros preserved
    }

    // Calibration information (manual example).
    {
        const Calibration c = parse_calibration(
            "c,00000017.60m,0000000.000s, 039.4C,00000008.71m, 039.4C");
        CHECK(close_to(c.light_cal_offset, 17.60));
        CHECK(close_to(c.dark_cal_period_s, 0.0));
        CHECK(close_to(c.temp_light_c, 39.4));
        CHECK(close_to(c.sensor_offset, 8.71));
        CHECK(close_to(c.temp_dark_c, 39.4));
    }

    // Trailing CR (as delivered before terminator stripping) is tolerated.
    {
        const Reading r =
            parse_reading("r, 06.70m,0000022921Hz,0000000020c,0000000.000s, 039.4C\r");
        CHECK(close_to(r.mag_arcsec2, 6.70));
    }

    // Malformed input must throw.
    {
        bool threw = false;
        try {
            parse_reading("x,garbage");
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
    }

    if (g_failures == 0) {
        std::puts("protocol_test passed");
        return 0;
    }
    std::fprintf(stderr, "protocol_test FAILED (%d checks)\n", g_failures);
    return 1;
}
