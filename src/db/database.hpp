// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/db/database.hpp
// Purpose:       RAII wrapper over MariaDB Connector/C (libmariadb) for storing
//                and querying SQM readings, sensors, and calibration history.
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "protocol.hpp"  // nightwatcher::sqm::Reading, Calibration

namespace nightwatcher::db {

// Connection parameters. Password is never defaulted in source; supply it via
// the environment (NW_DB_PASSWORD).
struct DbConfig {
    std::string host = "localhost";
    uint16_t port = 3306;
    std::string user = "nightwatcher";
    std::string password;
    std::string database = "nightwatcher";

    // Build from NW_DB_HOST/PORT/USER/PASSWORD/NAME, falling back to defaults.
    static DbConfig from_env();
};

// Full sensor/station record as read from the database.
struct SensorRow {
    std::string id;
    std::string name;
    std::string site;                    // grouping label for co-located instruments
    std::string serial_number;
    std::string model;
    std::optional<int> protocol_ver;
    std::optional<int> feature_ver;
    std::optional<double> latitude;
    std::optional<double> longitude;
    std::optional<double> elevation_m;
    std::string timezone;                // IANA tz, e.g. "America/Phoenix"
    std::string transport;               // "tcp" | "serial"
    std::string address;                 // "host:port" | "/dev/ttyUSB0"
    int poll_interval_s = 300;
    std::string status;                  // "active" | "inactive" | "retired"
    std::string installed_at;            // "YYYY-MM-DD" or empty
    std::string notes;
    std::string created_at;
};

// Editable sensor fields. Only members that are set are written, so the same
// struct drives both creation (add-sensor) and partial edits (set-sensor).
struct SensorFields {
    std::optional<std::string> name;
    std::optional<std::string> site;
    std::optional<std::string> serial_number;
    std::optional<std::string> model;
    std::optional<int>         protocol_ver;
    std::optional<int>         feature_ver;
    std::optional<double>      latitude;
    std::optional<double>      longitude;
    std::optional<double>      elevation_m;
    std::optional<std::string> timezone;
    std::optional<std::string> transport;    // "tcp" | "serial"
    std::optional<std::string> address;
    std::optional<int>         poll_interval_s;
    std::optional<std::string> status;       // "active" | "inactive" | "retired"
    std::optional<std::string> installed_at; // "YYYY-MM-DD"
    std::optional<std::string> notes;
};

// Full weather-station record (e.g. an Ambient Weather WS-2000).
struct WeatherStationRow {
    std::string id;
    std::string name;
    std::string site;
    std::string model;
    std::string provider;   // "ambientweather" | "wunderground"
    std::string config;     // JSON provider settings (secrets masked when served)
    std::string transport;  // legacy
    std::string address;    // legacy
    std::optional<double> latitude;
    std::optional<double> longitude;
    std::optional<double> elevation_m;
    std::string timezone;
    int poll_interval_s = 300;
    std::string status;
    std::string installed_at;
    std::string notes;
    std::string created_at;
};

// Editable weather-station fields (only set members are written).
struct WeatherStationFields {
    std::optional<std::string> name;
    std::optional<std::string> site;
    std::optional<std::string> model;
    std::optional<std::string> provider;
    std::optional<std::string> config;
    std::optional<std::string> transport;
    std::optional<std::string> address;
    std::optional<double>      latitude;
    std::optional<double>      longitude;
    std::optional<double>      elevation_m;
    std::optional<std::string> timezone;
    std::optional<int>         poll_interval_s;
    std::optional<std::string> status;
    std::optional<std::string> installed_at;
    std::optional<std::string> notes;
};

// One weather observation (metric/SI units) as stored in weather_readings.
struct WeatherReadingRow {
    long long id = 0;
    std::string station_id;
    std::string ts_utc;
    std::optional<double> temp_c;
    std::optional<double> humidity_pct;
    std::optional<double> dew_point_c;
    std::optional<double> pressure_hpa;
    std::optional<double> pressure_abs_hpa;
    std::optional<double> wind_speed_ms;
    std::optional<double> wind_gust_ms;
    std::optional<int>    wind_dir_deg;
    std::optional<double> rain_rate_mmh;
    std::optional<double> rain_daily_mm;
    std::optional<double> uv_index;
    std::optional<double> solar_wm2;
    std::optional<double> cloud_cover_pct;
    std::string source;
    std::string raw;
};

// One row of schema_status(): a known table and its row count.
struct TableCount {
    std::string table;
    long long rows = 0;
    bool present = false;
};

// One row of recent_events().
struct EventRow {
    long long id = 0;
    std::string ts_utc;
    std::string device_id;
    std::string source;
    std::string level;
    std::string event;
    std::string detail;
};

// A web-UI / API user.
struct UserRow {
    long long id = 0;
    std::string username;
    std::string password_hash;        // pbkdf2_sha256$...
    std::string role;                 // "admin" | "viewer"
    bool must_change_password = false;
    std::string created_at;
};

// The user behind a valid session.
struct SessionInfo {
    long long user_id = 0;
    std::string username;
    std::string role;
};

struct ReadingRow {
    long long id = 0;
    std::string sensor_id;
    std::string ts_utc;  // "YYYY-MM-DD HH:MM:SS"
    double mag_arcsec2 = 0.0;
    long long freq_hz = 0;
    long long period_counts = 0;
    double period_s = 0.0;
    double temp_c = 0.0;
    std::string quality;  // "ok" | "saturated" | "suspect"
    std::string source;
    std::string raw_line;
};

// A single MariaDB connection with domain helpers. Not thread-safe; use one
// Database per thread.
class Database {
public:
    explicit Database(const DbConfig& cfg);  // connects; throws on failure
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Create a sensor or update the provided fields (INSERT ... ON DUPLICATE KEY
    // UPDATE). transport + address must be present in `f` for a brand-new sensor.
    void upsert_sensor(const std::string& id, const SensorFields& f);
    // Update only the provided fields of an existing sensor; returns false (and
    // creates nothing) if no sensor with `id` exists.
    bool update_sensor(const std::string& id, const SensorFields& f);
    std::vector<SensorRow> sensors();
    std::optional<SensorRow> find_sensor(const std::string& id);
    void remove_sensor(const std::string& id);  // also removes its readings + config (cascade)

    // Sensors with status = 'active'.
    std::vector<SensorRow> active_sensors();

    // Append an operational event (device up/down, errors, daemon lifecycle).
    // Empty device_id or detail are stored as SQL NULL.
    void insert_event(const std::string& source, const std::string& level,
                      const std::string& event, const std::string& detail = "",
                      const std::string& device_id = "");

    // Most-recent events, newest first.
    std::vector<EventRow> recent_events(int limit = 50);

    // --- Weather stations ---
    void upsert_weather_station(const std::string& id, const WeatherStationFields& f);
    bool update_weather_station(const std::string& id, const WeatherStationFields& f);
    std::vector<WeatherStationRow> weather_stations();
    std::vector<WeatherStationRow> active_weather_stations();
    std::optional<WeatherStationRow> find_weather_station(const std::string& id);
    void remove_weather_station(const std::string& id);

    long long insert_weather_reading(const WeatherReadingRow& r);
    std::vector<WeatherReadingRow> weather_readings(const std::string& station_id, int limit = 20);
    std::vector<WeatherReadingRow> weather_readings_between(const std::string& station_id,
                                                           const std::string& from,
                                                           const std::string& to, int limit = 5000);

    // --- Maintenance ---
    // Delete readings for a sensor with ts_utc < the given timestamp; returns the
    // number of rows removed.
    long long delete_readings_before(const std::string& sensor_id, const std::string& ts_utc);

    // --- Schema ---
    std::vector<TableCount> schema_status();          // known tables + row counts
    void run_schema_script(const std::string& sql);   // execute a multi-statement SQL script

    // --- Users & sessions (web-UI / API authentication) ---
    long long count_users();
    void create_user(const std::string& username, const std::string& password_hash,
                     const std::string& role, bool must_change_password);
    std::optional<UserRow> find_user(const std::string& username);
    std::vector<UserRow> users();
    bool set_user_password(const std::string& username, const std::string& password_hash,
                          bool must_change_password);
    bool delete_user(const std::string& username);

    void create_session(const std::string& token, long long user_id, int ttl_seconds);
    std::optional<SessionInfo> validate_session(const std::string& token);
    void delete_session(const std::string& token);
    void delete_expired_sessions();

    // Insert a reading. If ts_utc is empty the database clock (UTC) is used.
    // Returns the new row id, or 0 if a row for (sensor_id, ts_utc) already
    // existed (INSERT IGNORE on the unique key).
    long long insert_reading(const std::string& sensor_id, const sqm::Reading& r,
                             const std::string& ts_utc = "",
                             const std::string& source = "poll",
                             const std::string& quality = "ok");

    // Most-recent readings for a sensor, newest first.
    std::vector<ReadingRow> readings(const std::string& sensor_id, int limit = 20);
    // Readings within [from, to] (an empty bound is unbounded), newest first.
    std::vector<ReadingRow> readings_between(const std::string& sensor_id, const std::string& from,
                                             const std::string& to, int limit = 5000);

    // Record a calibration snapshot in config_log.
    void insert_calibration(const std::string& sensor_id, const sqm::Calibration& c,
                            const std::string& ts_utc = "");

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    void exec(const std::string& sql);
    std::string esc(const std::string& s);
};

}  // namespace nightwatcher::db
