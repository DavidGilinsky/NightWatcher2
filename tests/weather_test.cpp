// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/weather_test.cpp
// Purpose:       Unit tests for the weather provider payload parsers + unit
//                conversions (Ambient Weather imperial -> SI, Wunderground
//                metric). No network required.
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <cmath>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>

#include "provider.hpp"

namespace w = nightwatcher::weather;

static int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "CHECK failed at line %d: %s\n",         \
                         __LINE__, #cond);                                \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

static bool close_to(const std::optional<double>& v, double e) {
    return v.has_value() && std::fabs(*v - e) < 0.01;
}

int main() {
    // Ambient Weather (imperial) -> SI.
    {
        const auto r = w::parse_ambient_json(
            R"({"date":"2026-07-19T03:45:42.000Z","tempf":70.0,"humidity":50,)"
            R"("windspeedmph":10.0,"windgustmph":15.0,"winddir":180,"baromrelin":30.0,)"
            R"("hourlyrainin":0.1,"dailyrainin":0.5,"uv":3,"solarradiation":500.0,"dewPoint":50.0})");
        CHECK(r.ts_utc == "2026-07-19 03:45:42");
        CHECK(close_to(r.temp_c, 21.111));       // (70-32)*5/9
        CHECK(close_to(r.humidity_pct, 50));
        CHECK(close_to(r.wind_speed_ms, 4.4704)); // 10 mph
        CHECK(close_to(r.wind_gust_ms, 6.7056));  // 15 mph
        CHECK(r.wind_dir_deg.has_value() && *r.wind_dir_deg == 180);
        CHECK(close_to(r.pressure_hpa, 1015.917)); // 30 inHg
        CHECK(close_to(r.rain_rate_mmh, 2.54));    // 0.1 in
        CHECK(close_to(r.rain_daily_mm, 12.7));    // 0.5 in
        CHECK(close_to(r.uv_index, 3));
        CHECK(close_to(r.solar_wm2, 500));
        CHECK(close_to(r.dew_point_c, 10.0));      // (50-32)*5/9
    }

    // Weather Underground (metric) -> SI.
    {
        const auto r = w::parse_wunderground_json(
            R"({"obsTimeUtc":"2026-07-19T03:45:42Z","humidity":55,"winddir":200,"uv":2.0,)"
            R"("solarRadiation":450.0,"metric":{"temp":18,"dewpt":9,"pressure":1013.2,)"
            R"("windSpeed":18.0,"windGust":25.0,"precipRate":1.5,"precipTotal":3.0}})");
        CHECK(r.ts_utc == "2026-07-19 03:45:42");
        CHECK(close_to(r.temp_c, 18));
        CHECK(close_to(r.dew_point_c, 9));
        CHECK(close_to(r.pressure_hpa, 1013.2));
        CHECK(close_to(r.wind_speed_ms, 5.0));   // 18 km/h
        CHECK(close_to(r.wind_gust_ms, 6.944));  // 25 km/h
        CHECK(close_to(r.rain_rate_mmh, 1.5));
        CHECK(close_to(r.rain_daily_mm, 3.0));
        CHECK(close_to(r.humidity_pct, 55));
        CHECK(r.wind_dir_deg.has_value() && *r.wind_dir_deg == 200);
        CHECK(close_to(r.uv_index, 2));
        CHECK(close_to(r.solar_wm2, 450));
    }

    // make_provider validates the provider name and required config.
    {
        bool threw = false;
        try { w::make_provider("nope", "{}"); } catch (const std::exception&) { threw = true; }
        CHECK(threw);
        threw = false;
        try { w::make_provider("ambientweather", "{}"); } catch (const std::exception&) { threw = true; }
        CHECK(threw);
        threw = false;
        try { w::make_provider("wunderground", R"({"stationId":"X"})"); } catch (const std::exception&) { threw = true; }
        CHECK(threw);
    }

    if (g_failures == 0) {
        std::puts("weather_test passed");
        return 0;
    }
    std::fprintf(stderr, "weather_test FAILED (%d checks)\n", g_failures);
    return 1;
}
