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

struct SensorRow {
    std::string id;
    std::string name;
    std::string transport;  // "tcp" | "serial"
    std::string address;    // "host:port" | "/dev/ttyUSB0"
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

    // Insert or update a sensor row (keyed on id).
    void upsert_sensor(const SensorRow& s);
    std::vector<SensorRow> sensors();
    std::optional<SensorRow> find_sensor(const std::string& id);
    void remove_sensor(const std::string& id);  // also removes its readings + config (cascade)

    // Insert a reading. If ts_utc is empty the database clock (UTC) is used.
    // Returns the new row id, or 0 if a row for (sensor_id, ts_utc) already
    // existed (INSERT IGNORE on the unique key).
    long long insert_reading(const std::string& sensor_id, const sqm::Reading& r,
                             const std::string& ts_utc = "",
                             const std::string& source = "poll",
                             const std::string& quality = "ok");

    // Most-recent readings for a sensor, newest first.
    std::vector<ReadingRow> readings(const std::string& sensor_id, int limit = 20);

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
