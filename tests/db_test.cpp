// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/db_test.cpp
// Purpose:       Integration test for the database layer. Skips (passes) unless
//                NW_DB_TEST=1 is set with NW_DB_* connection variables, so the
//                default test run needs no database server.
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include "database.hpp"
#include "protocol.hpp"

namespace db = nightwatcher::db;
namespace sqm = nightwatcher::sqm;

static int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "CHECK failed at line %d: %s\n",         \
                         __LINE__, #cond);                                \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

int main() {
    const char* flag = std::getenv("NW_DB_TEST");
    if (flag == nullptr || std::string(flag) != "1") {
        std::puts("db_test skipped (set NW_DB_TEST=1 with NW_DB_* to run)");
        return 0;
    }

    const std::string kId = "NWTEST";
    const std::string kTs = "2026-07-18 04:05:06";

    try {
        db::Database dbh(db::DbConfig::from_env());

        dbh.remove_sensor(kId);  // start from a clean slate

        db::SensorFields sf;
        sf.name = "Integration Test";
        sf.transport = "tcp";
        sf.address = "127.0.0.1:10001";
        sf.latitude = 32.4188;
        sf.longitude = -110.7345;
        sf.elevation_m = 2791.0;
        sf.timezone = "America/Phoenix";
        dbh.upsert_sensor(kId, sf);

        const auto found = dbh.find_sensor(kId);
        CHECK(found.has_value());
        if (found) {
            CHECK(found->address == "127.0.0.1:10001");
            CHECK(found->name == "Integration Test");
            CHECK(found->timezone == "America/Phoenix");
            CHECK(found->latitude.has_value());
            if (found->latitude) CHECK(std::fabs(*found->latitude - 32.4188) < 1e-4);
        }

        // Partial update: change only elevation; other fields must be preserved.
        db::SensorFields upd;
        upd.elevation_m = 2795.0;
        CHECK(dbh.update_sensor(kId, upd));
        const auto found2 = dbh.find_sensor(kId);
        if (found2) {
            CHECK(found2->name == "Integration Test");  // preserved by partial update
            CHECK(found2->elevation_m.has_value());
            if (found2->elevation_m) CHECK(std::fabs(*found2->elevation_m - 2795.0) < 1e-3);
        }
        // Updating a nonexistent sensor returns false and creates nothing.
        CHECK(!dbh.update_sensor("NOPE_NWTEST", upd));
        CHECK(!dbh.find_sensor("NOPE_NWTEST").has_value());

        // active_sensors() includes our (active by default) test sensor.
        bool in_active = false;
        for (const auto& a : dbh.active_sensors()) {
            if (a.id == kId) in_active = true;
        }
        CHECK(in_active);

        // insert_event must succeed (used by the daemon for its event log).
        dbh.insert_event("test", "info", "unit-test", "db_test event", kId);

        sqm::Reading r;
        r.mag_arcsec2 = 20.13;
        r.freq_hz = 123;
        r.period_counts = 456;
        r.period_s = 2.5;
        r.temp_c = 15.5;
        r.raw = "r, 20.13m,0000000123Hz,0000000456c,0000002.500s, 015.5C";
        const long long id = dbh.insert_reading(kId, r, kTs, "poll", "ok");
        CHECK(id > 0);

        // Re-inserting the same (sensor, ts) is ignored (unique key).
        const long long dup = dbh.insert_reading(kId, r, kTs, "poll", "ok");
        CHECK(dup == 0);

        // A saturated (daytime) reading carrying an explicit quality flag.
        sqm::Reading r2 = r;
        r2.mag_arcsec2 = 0.0;
        r2.raw = "r, 00.00m,0000555820Hz,0000000000c,0000000.000s, 061.5C";
        const std::string kTs2 = "2026-07-18 04:10:06";
        const long long id2 = dbh.insert_reading(kId, r2, kTs2, "poll", "saturated");
        CHECK(id2 > 0);

        const auto rows = dbh.readings(kId, 10);
        CHECK(rows.size() >= 2);
        bool matched_ok = false;
        bool matched_sat = false;
        for (const auto& rr : rows) {
            if (rr.ts_utc.rfind(kTs, 0) == 0) {
                matched_ok = true;
                CHECK(std::fabs(rr.mag_arcsec2 - 20.13) < 0.01);
                CHECK(rr.freq_hz == 123);
                CHECK(rr.period_counts == 456);
                CHECK(std::fabs(rr.temp_c - 15.5) < 0.01);
                CHECK(rr.quality == "ok");
            }
            if (rr.ts_utc.rfind(kTs2, 0) == 0) {
                matched_sat = true;
                CHECK(rr.quality == "saturated");
            }
        }
        CHECK(matched_ok);
        CHECK(matched_sat);

        // Range query returns only readings inside the window.
        const auto in_window = dbh.readings_between(kId, "2026-07-18 00:00:00", "2026-07-18 23:59:59", 100);
        bool found_in_window = false;
        for (const auto& rr : in_window) {
            if (rr.ts_utc.rfind(kTs, 0) == 0) found_in_window = true;
        }
        CHECK(found_in_window);
        CHECK(dbh.readings_between(kId, "2000-01-01 00:00:00", "2000-01-02 00:00:00", 100).empty());

        sqm::Calibration c;
        c.light_cal_offset = 19.92;
        c.dark_cal_period_s = 300.0;
        c.temp_light_c = 22.2;
        c.sensor_offset = 8.71;
        c.temp_dark_c = 31.2;
        c.raw = "c,00000019.92m,0000300.000s, 022.2C,00000008.71m, 031.2C";
        dbh.insert_calibration(kId, c, kTs);  // must not throw

        // Weather-station CRUD.
        dbh.remove_weather_station("WXTEST");
        db::WeatherStationFields wf;
        wf.name = "Weather Test";
        wf.model = "Ambient Weather WS-2000";
        wf.transport = "http";
        wf.address = "192.168.1.60";
        wf.latitude = 32.42;
        wf.longitude = -110.73;
        dbh.upsert_weather_station("WXTEST", wf);
        const auto wfound = dbh.find_weather_station("WXTEST");
        CHECK(wfound.has_value());
        if (wfound) {
            CHECK(wfound->model == "Ambient Weather WS-2000");
            CHECK(wfound->latitude.has_value());
        }
        db::WeatherStationFields wupd;
        wupd.elevation_m = 810.0;
        CHECK(dbh.update_weather_station("WXTEST", wupd));
        CHECK(!dbh.update_weather_station("NOPE_WX", wupd));
        dbh.remove_weather_station("WXTEST");
        CHECK(!dbh.find_weather_station("WXTEST").has_value());

        // Schema status reports the six known tables, sensors present.
        const auto st = dbh.schema_status();
        CHECK(st.size() == 8);
        bool sensors_present = false;
        for (const auto& tc : st) {
            if (tc.table == "sensors") sensors_present = tc.present;
        }
        CHECK(sensors_present);

        // Pruning removes the test readings (all are before this far-future cutoff).
        const long long pruned = dbh.delete_readings_before(kId, "2999-01-01 00:00:00");
        CHECK(pruned >= 2);

        dbh.remove_sensor(kId);  // cleanup
        CHECK(!dbh.find_sensor(kId).has_value());

        if (g_failures == 0) {
            std::puts("db_test passed");
            return 0;
        }
        std::fprintf(stderr, "db_test FAILED (%d checks)\n", g_failures);
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "db_test ERROR: %s\n", e.what());
        return 1;
    }
}
