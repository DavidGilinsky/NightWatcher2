// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/cli/nwdb.cpp
// Purpose:       CLI for the NightWatcher2 database: register sensors, poll a
//                sensor's SQM and store the reading, and query stored readings.
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "database.hpp"
#include "nightwatcher/version.hpp"
#include "protocol.hpp"
#include "sqm_device.hpp"
#include "tcp_transport.hpp"

namespace {

using nightwatcher::db::Database;
using nightwatcher::db::DbConfig;
using nightwatcher::db::ReadingRow;
using nightwatcher::db::SensorRow;
namespace sqm = nightwatcher::sqm;

void usage(const char* argv0) {
    std::cout
        << "nwdb " << NIGHTWATCHER_VERSION << " \xe2\x80\x94 NightWatcher2 database tool\n\n"
        << "Usage:\n"
        << "  " << argv0 << " sensors\n"
        << "  " << argv0 << " add-sensor <ID> --tcp HOST[:PORT] [--name NAME]\n"
        << "  " << argv0 << " poll <ID>              (read the SQM and store a reading)\n"
        << "  " << argv0 << " cal <ID>               (read + store calibration)\n"
        << "  " << argv0 << " readings <ID> [--limit N]\n\n"
        << "Connection comes from the environment: NW_DB_HOST, NW_DB_PORT, NW_DB_USER,\n"
        << "NW_DB_PASSWORD, NW_DB_NAME.\n";
}

// Split "host:port" (default port 10001) for a tcp sensor address.
void split_endpoint(const std::string& addr, std::string& host, uint16_t& port) {
    host = addr;
    port = 10001;
    const auto colon = addr.rfind(':');
    if (colon != std::string::npos) {
        host = addr.substr(0, colon);
        port = static_cast<uint16_t>(std::atoi(addr.substr(colon + 1).c_str()));
    }
}

std::unique_ptr<sqm::SqmDevice> open_device(const SensorRow& s) {
    if (s.transport != "tcp") {
        throw std::runtime_error("sensor '" + s.id + "' uses unsupported transport '" +
                                 s.transport + "' (only tcp is implemented)");
    }
    std::string host;
    uint16_t port;
    split_endpoint(s.address, host, port);
    auto transport = std::make_unique<sqm::TcpTransport>(host, port, 5000);
    return std::make_unique<sqm::SqmDevice>(std::move(transport));
}

int cmd_sensors(Database& db) {
    const auto rows = db.sensors();
    if (rows.empty()) {
        std::cout << "(no sensors registered)\n";
        return 0;
    }
    for (const auto& s : rows) {
        std::cout << s.id << "  [" << s.transport << "] " << s.address
                  << (s.name.empty() ? "" : "  (" + s.name + ")") << "\n";
    }
    return 0;
}

int cmd_poll(Database& db, const std::string& id) {
    const auto s = db.find_sensor(id);
    if (!s) { std::cerr << "error: no such sensor '" << id << "'\n"; return 1; }
    auto dev = open_device(*s);
    const sqm::Reading r = dev->read_averaged();
    // 0.00 mag/arcsec^2 means the sensor is at its upper brightness limit (daytime).
    const std::string quality = (r.mag_arcsec2 <= 0.0) ? "saturated" : "ok";
    const long long rid = db.insert_reading(id, r, "", "poll", quality);
    std::cout << "stored reading id=" << rid << "  mag=" << r.mag_arcsec2
              << "  temp=" << r.temp_c << " C  (" << quality << ")\n"
              << "raw: " << r.raw << "\n";
    if (rid == 0) std::cout << "(a reading with this timestamp already existed; ignored)\n";
    return 0;
}

int cmd_cal(Database& db, const std::string& id) {
    const auto s = db.find_sensor(id);
    if (!s) { std::cerr << "error: no such sensor '" << id << "'\n"; return 1; }
    auto dev = open_device(*s);
    const sqm::Calibration c = dev->calibration();
    db.insert_calibration(id, c);
    std::cout << "stored calibration  light_offset=" << c.light_cal_offset
              << "  dark_period=" << c.dark_cal_period_s << " s\n"
              << "raw: " << c.raw << "\n";
    return 0;
}

int cmd_readings(Database& db, const std::string& id, int limit) {
    const auto rows = db.readings(id, limit);
    if (rows.empty()) {
        std::cout << "(no readings for '" << id << "')\n";
        return 0;
    }
    for (const auto& r : rows) {
        std::cout << r.ts_utc << "  " << r.mag_arcsec2 << " mag/arcsec^2  " << r.temp_c
                  << " C  [" << r.source << "/" << r.quality << "]\n";
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> pos;
    std::string tcp_addr;
    std::string name;
    int limit = 20;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else if (a == "--version") { std::cout << "nwdb " << NIGHTWATCHER_VERSION << "\n"; return 0; }
        else if (a == "--tcp") { if (++i >= argc) { std::cerr << "error: --tcp requires HOST[:PORT]\n"; return 2; } tcp_addr = argv[i]; }
        else if (a == "--name") { if (++i >= argc) { std::cerr << "error: --name requires a value\n"; return 2; } name = argv[i]; }
        else if (a == "--limit") { if (++i >= argc) { std::cerr << "error: --limit requires N\n"; return 2; } limit = std::atoi(argv[i]); }
        else if (!a.empty() && a[0] == '-') { std::cerr << "error: unknown option '" << a << "'\n"; return 2; }
        else pos.push_back(a);
    }

    if (pos.empty()) { usage(argv[0]); return 2; }
    const std::string& cmd = pos[0];

    try {
        Database db(DbConfig::from_env());

        if (cmd == "sensors") {
            return cmd_sensors(db);
        } else if (cmd == "add-sensor") {
            if (pos.size() < 2) { std::cerr << "error: add-sensor requires an ID\n"; return 2; }
            if (tcp_addr.empty()) { std::cerr << "error: add-sensor requires --tcp HOST[:PORT]\n"; return 2; }
            SensorRow s;
            s.id = pos[1];
            s.name = name;
            s.transport = "tcp";
            s.address = tcp_addr;
            db.upsert_sensor(s);
            std::cout << "sensor '" << s.id << "' saved: [tcp] " << s.address << "\n";
            return 0;
        } else if (cmd == "poll") {
            if (pos.size() < 2) { std::cerr << "error: poll requires a sensor ID\n"; return 2; }
            return cmd_poll(db, pos[1]);
        } else if (cmd == "cal") {
            if (pos.size() < 2) { std::cerr << "error: cal requires a sensor ID\n"; return 2; }
            return cmd_cal(db, pos[1]);
        } else if (cmd == "readings") {
            if (pos.size() < 2) { std::cerr << "error: readings requires a sensor ID\n"; return 2; }
            return cmd_readings(db, pos[1], limit);
        }
        std::cerr << "error: unknown command '" << cmd << "'\n";
        usage(argv[0]);
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
