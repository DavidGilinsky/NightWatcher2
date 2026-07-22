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
#include <sstream>
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
        sf.latitude = 31.9500;   // obfuscated placeholder (not a real site)
        sf.longitude = -111.6000;
        sf.elevation_m = 1200.0;
        sf.timezone = "America/Phoenix";
        dbh.upsert_sensor(kId, sf);

        const auto found = dbh.find_sensor(kId);
        CHECK(found.has_value());
        if (found) {
            CHECK(found->address == "127.0.0.1:10001");
            CHECK(found->name == "Integration Test");
            CHECK(found->timezone == "America/Phoenix");
            CHECK(found->latitude.has_value());
            if (found->latitude) CHECK(std::fabs(*found->latitude - 31.9500) < 1e-4);
        }

        // Partial update: change only elevation; other fields must be preserved.
        db::SensorFields upd;
        upd.elevation_m = 1205.0;
        CHECK(dbh.update_sensor(kId, upd));
        const auto found2 = dbh.find_sensor(kId);
        if (found2) {
            CHECK(found2->name == "Integration Test");  // preserved by partial update
            CHECK(found2->elevation_m.has_value());
            if (found2->elevation_m) CHECK(std::fabs(*found2->elevation_m - 1205.0) < 1e-3);
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
        wf.latitude = 31.95;
        wf.longitude = -111.60;
        wf.provider = "ambientweather";
        wf.config = R"({"applicationKey":"app","apiKey":"key"})";
        dbh.upsert_weather_station("WXTEST", wf);
        const auto wfound = dbh.find_weather_station("WXTEST");
        CHECK(wfound.has_value());
        if (wfound) {
            CHECK(wfound->model == "Ambient Weather WS-2000");
            CHECK(wfound->latitude.has_value());
            CHECK(wfound->provider == "ambientweather");
        }
        db::WeatherStationFields wupd;
        wupd.elevation_m = 810.0;
        CHECK(dbh.update_weather_station("WXTEST", wupd));
        CHECK(!dbh.update_weather_station("NOPE_WX", wupd));

        // Weather readings insert + query.
        db::WeatherReadingRow wr;
        wr.station_id = "WXTEST";
        wr.ts_utc = "2026-07-19 04:00:00";
        wr.temp_c = 21.5;
        wr.humidity_pct = 48.0;
        wr.wind_speed_ms = 3.2;
        CHECK(dbh.insert_weather_reading(wr) > 0);
        const auto wrows = dbh.weather_readings("WXTEST", 10);
        bool wr_found = false;
        for (const auto& x : wrows) {
            if (x.ts_utc.rfind("2026-07-19 04:00:00", 0) == 0) {
                wr_found = true;
                CHECK(x.temp_c.has_value() && std::fabs(*x.temp_c - 21.5) < 0.01);
            }
        }
        CHECK(wr_found);
        CHECK(dbh.weather_readings_between("WXTEST", "2000-01-01 00:00:00", "2000-01-02 00:00:00", 10).empty());

        dbh.remove_weather_station("WXTEST");
        CHECK(!dbh.find_weather_station("WXTEST").has_value());

        // Export-target CRUD + export log (FK to the test sensor).
        dbh.remove_export_target("EXTEST");
        db::ExportTargetFields ef;
        ef.sensor_id = kId;
        ef.name = "DSN export test";
        ef.target = "dsn";
        ef.config = R"({"site_id":"DSN036-S","outbox_dir":"/tmp"})";
        ef.schedule = "nightly";
        ef.schedule_time = "06:00";
        dbh.upsert_export_target("EXTEST", ef);
        const auto efound = dbh.find_export_target("EXTEST");
        CHECK(efound.has_value());
        if (efound) {
            CHECK(efound->target == "dsn");
            CHECK(efound->sensor_id == kId);
            CHECK(efound->schedule == "nightly");
        }
        db::ExportTargetFields eupd;
        eupd.status = "inactive";
        CHECK(dbh.update_export_target("EXTEST", eupd));
        CHECK(!dbh.update_export_target("NOPE_EX", eupd));
        // Inactive target must not appear in active_export_targets().
        for (const auto& t : dbh.active_export_targets()) CHECK(t.id != "EXTEST");
        dbh.set_export_watermark("EXTEST", kTs);
        CHECK(dbh.find_export_target("EXTEST")->last_export_ts.rfind(kTs, 0) == 0);

        db::ExportLogRow el;
        el.target_id = "EXTEST";
        el.from_ts = "2026-07-19 00:00:00";
        el.to_ts = "2026-07-19 23:59:59";
        el.row_count = 42;
        el.file_name = "DSN036-S_20260719.dat";
        el.status = "ok";
        CHECK(dbh.insert_export_log(el) > 0);
        const auto elog = dbh.export_log("EXTEST", 10);
        CHECK(!elog.empty());
        if (!elog.empty()) {
            CHECK(elog.front().row_count == 42);
            CHECK(elog.front().status == "ok");
        }
        dbh.remove_export_target("EXTEST");
        CHECK(!dbh.find_export_target("EXTEST").has_value());

        // Backup: the dump carries the schema + the test sensor's data.
        {
            std::ostringstream dump;
            dbh.dump_sql(dump);
            const std::string s = dump.str();
            CHECK(s.find("SET FOREIGN_KEY_CHECKS=0") != std::string::npos);
            CHECK(s.find("CREATE TABLE `sensors`") != std::string::npos);
            CHECK(s.find("INSERT INTO `sensors`") != std::string::npos);
            CHECK(s.find(kId) != std::string::npos);  // NWTEST present in the backup

            // Restore round-trips a throwaway table (incl. a value with a ';').
            dbh.run_schema_script("DROP TABLE IF EXISTS nwtest_backup; "
                                  "CREATE TABLE nwtest_backup (id INT, note VARCHAR(64));");
            std::istringstream rin("-- comment\nINSERT INTO nwtest_backup VALUES (42,'he;llo');\n");
            CHECK(dbh.restore_sql(rin) == 1);
            std::ostringstream chk;
            dbh.dump_sql(chk);
            CHECK(chk.str().find("nwtest_backup") != std::string::npos);
            CHECK(chk.str().find("he;llo") != std::string::npos);  // survived restore + re-dump
            dbh.run_schema_script("DROP TABLE IF EXISTS nwtest_backup;");
        }

        // Settings key/value round-trip (upsert overwrites; missing -> nullopt;
        // delete removes). Uses a throwaway key so a live daemon's real
        // settings (api_bind/api_port/api_tls) are never disturbed.
        dbh.set_setting("nwtest_setting", "0.0.0.0");
        CHECK(dbh.get_setting("nwtest_setting").value_or("") == "0.0.0.0");
        dbh.set_setting("nwtest_setting", "127.0.0.1");
        CHECK(dbh.get_setting("nwtest_setting").value_or("") == "127.0.0.1");
        CHECK(!dbh.get_setting("nwtest_missing_setting").has_value());
        dbh.delete_setting("nwtest_setting");
        CHECK(!dbh.get_setting("nwtest_setting").has_value());

        // Schema status reports the twelve known tables, sensors present.
        const auto st = dbh.schema_status();
        CHECK(st.size() == 12);
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
