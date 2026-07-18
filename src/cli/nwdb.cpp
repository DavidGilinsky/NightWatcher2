// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/cli/nwdb.cpp
// Purpose:       CLI for the NightWatcher2 database: register and edit sensor /
//                site metadata, poll a sensor's SQM into the database, and query
//                stored readings.
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
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
using nightwatcher::db::SensorFields;
using nightwatcher::db::SensorRow;
namespace sqm = nightwatcher::sqm;

void usage(const char* argv0) {
    std::cout
        << "nwdb " << NIGHTWATCHER_VERSION << " \xe2\x80\x94 NightWatcher2 database tool\n\n"
        << "Usage:\n"
        << "  " << argv0 << " sensors\n"
        << "  " << argv0 << " show <ID>\n"
        << "  " << argv0 << " add-sensor <ID> --tcp HOST[:PORT] [metadata...]\n"
        << "  " << argv0 << " set-sensor <ID> [metadata...]      (partial edit)\n"
        << "  " << argv0 << " poll <ID>                          (read SQM -> store)\n"
        << "  " << argv0 << " cal <ID>                           (read + store calibration)\n"
        << "  " << argv0 << " readings <ID> [--limit N]\n\n"
        << "Metadata flags (add-sensor / set-sensor):\n"
        << "  --tcp HOST[:PORT]   --name NAME       --site SITE\n"
        << "  --lat DEG           --lon DEG         --elev METERS\n"
        << "  --timezone TZ       --model MODEL     --serial SERIAL\n"
        << "  --status S          --installed DATE  --poll-interval SECONDS\n"
        << "  --notes TEXT        --no-probe        (skip reading ix from the device)\n\n"
        << "add-sensor auto-fills serial/protocol/feature from the device (ix) unless\n"
        << "--no-probe is given. Connection comes from NW_DB_HOST/PORT/USER/PASSWORD/NAME.\n";
}

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

// Fill serial/protocol/feature from the device's ix response (best effort).
void probe_and_fill(SensorFields& sf) {
    if (!(sf.transport && *sf.transport == "tcp" && sf.address)) return;
    try {
        std::string host;
        uint16_t port;
        split_endpoint(*sf.address, host, port);
        auto transport = std::make_unique<sqm::TcpTransport>(host, port, 3000);
        sqm::SqmDevice dev(std::move(transport));
        const sqm::UnitInfo u = dev.info();
        if (!sf.serial_number) sf.serial_number = u.serial;
        if (!sf.protocol_ver) sf.protocol_ver = u.protocol;
        if (!sf.feature_ver) sf.feature_ver = u.feature;
        std::cerr << "probed device: serial=" << u.serial << " protocol=" << u.protocol
                  << " feature=" << u.feature << "\n";
    } catch (const std::exception& e) {
        std::cerr << "note: could not probe device (" << e.what()
                  << "); continuing without serial/protocol/feature\n";
    }
}

std::string dash(const std::string& s) { return s.empty() ? std::string("-") : s; }

template <class T>
std::string dash_opt(const std::optional<T>& v) {
    return v ? std::to_string(*v) : std::string("-");
}

void print_sensor(const SensorRow& s) {
    std::cout << "id            : " << s.id << "\n"
              << "name          : " << dash(s.name) << "\n"
              << "site          : " << dash(s.site) << "\n"
              << "model         : " << dash(s.model) << "\n"
              << "serial        : " << dash(s.serial_number) << "\n"
              << "protocol/feat : " << dash_opt(s.protocol_ver) << " / " << dash_opt(s.feature_ver)
              << "\n"
              << "latitude      : " << dash_opt(s.latitude) << "\n"
              << "longitude     : " << dash_opt(s.longitude) << "\n"
              << "elevation_m   : " << dash_opt(s.elevation_m) << "\n"
              << "timezone      : " << dash(s.timezone) << "\n"
              << "transport     : " << dash(s.transport) << "\n"
              << "address       : " << dash(s.address) << "\n"
              << "poll_interval : " << s.poll_interval_s << " s\n"
              << "status        : " << dash(s.status) << "\n"
              << "installed_at  : " << dash(s.installed_at) << "\n"
              << "notes         : " << dash(s.notes) << "\n"
              << "created_at    : " << dash(s.created_at) << "\n";
}

int cmd_sensors(Database& db) {
    const auto rows = db.sensors();
    if (rows.empty()) {
        std::cout << "(no sensors registered)\n";
        return 0;
    }
    for (const auto& s : rows) {
        std::cout << s.id << "  [" << s.transport << "] " << s.address;
        if (!s.name.empty()) std::cout << "  (" << s.name << ")";
        if (!s.site.empty()) std::cout << "  site=" << s.site;
        if (s.latitude && s.longitude) std::cout << "  @ " << *s.latitude << "," << *s.longitude;
        std::cout << "\n";
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
              << "  dark_period=" << c.dark_cal_period_s << " s  sensor_offset=" << c.sensor_offset
              << "\nraw: " << c.raw << "\n";
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

// Read the next argument for a flag, or report an error. Returns false on error.
bool next_arg(int& i, int argc, char** argv, std::string& out) {
    if (i + 1 >= argc) {
        std::cerr << "error: " << argv[i] << " requires a value\n";
        return false;
    }
    out = argv[++i];
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> pos;
    SensorFields sf;
    int limit = 20;
    bool no_probe = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        std::string v;
        if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else if (a == "--version") { std::cout << "nwdb " << NIGHTWATCHER_VERSION << "\n"; return 0; }
        else if (a == "--no-probe") { no_probe = true; }
        else if (a == "--tcp") { if (!next_arg(i, argc, argv, v)) return 2; sf.transport = "tcp"; sf.address = v; }
        else if (a == "--name") { if (!next_arg(i, argc, argv, v)) return 2; sf.name = v; }
        else if (a == "--site") { if (!next_arg(i, argc, argv, v)) return 2; sf.site = v; }
        else if (a == "--timezone") { if (!next_arg(i, argc, argv, v)) return 2; sf.timezone = v; }
        else if (a == "--model") { if (!next_arg(i, argc, argv, v)) return 2; sf.model = v; }
        else if (a == "--serial") { if (!next_arg(i, argc, argv, v)) return 2; sf.serial_number = v; }
        else if (a == "--status") { if (!next_arg(i, argc, argv, v)) return 2; sf.status = v; }
        else if (a == "--installed") { if (!next_arg(i, argc, argv, v)) return 2; sf.installed_at = v; }
        else if (a == "--notes") { if (!next_arg(i, argc, argv, v)) return 2; sf.notes = v; }
        else if (a == "--lat") { if (!next_arg(i, argc, argv, v)) return 2; try { sf.latitude = std::stod(v); } catch (...) { std::cerr << "error: --lat expects a number\n"; return 2; } }
        else if (a == "--lon") { if (!next_arg(i, argc, argv, v)) return 2; try { sf.longitude = std::stod(v); } catch (...) { std::cerr << "error: --lon expects a number\n"; return 2; } }
        else if (a == "--elev") { if (!next_arg(i, argc, argv, v)) return 2; try { sf.elevation_m = std::stod(v); } catch (...) { std::cerr << "error: --elev expects a number\n"; return 2; } }
        else if (a == "--poll-interval") { if (!next_arg(i, argc, argv, v)) return 2; try { sf.poll_interval_s = std::stoi(v); } catch (...) { std::cerr << "error: --poll-interval expects an integer\n"; return 2; } }
        else if (a == "--protocol") { if (!next_arg(i, argc, argv, v)) return 2; try { sf.protocol_ver = std::stoi(v); } catch (...) { std::cerr << "error: --protocol expects an integer\n"; return 2; } }
        else if (a == "--feature") { if (!next_arg(i, argc, argv, v)) return 2; try { sf.feature_ver = std::stoi(v); } catch (...) { std::cerr << "error: --feature expects an integer\n"; return 2; } }
        else if (a == "--limit") { if (!next_arg(i, argc, argv, v)) return 2; try { limit = std::stoi(v); } catch (...) { std::cerr << "error: --limit expects an integer\n"; return 2; } }
        else if (!a.empty() && a[0] == '-') { std::cerr << "error: unknown option '" << a << "'\n"; return 2; }
        else { pos.push_back(a); }
    }

    if (pos.empty()) { usage(argv[0]); return 2; }
    const std::string& cmd = pos[0];

    try {
        Database db(DbConfig::from_env());

        if (cmd == "sensors") {
            return cmd_sensors(db);
        } else if (cmd == "show") {
            if (pos.size() < 2) { std::cerr << "error: show requires a sensor ID\n"; return 2; }
            const auto s = db.find_sensor(pos[1]);
            if (!s) { std::cerr << "error: no such sensor '" << pos[1] << "'\n"; return 1; }
            print_sensor(*s);
            return 0;
        } else if (cmd == "add-sensor") {
            if (pos.size() < 2) { std::cerr << "error: add-sensor requires an ID\n"; return 2; }
            if (!sf.transport || !sf.address) {
                std::cerr << "error: add-sensor requires --tcp HOST[:PORT]\n";
                return 2;
            }
            if (!no_probe) probe_and_fill(sf);
            db.upsert_sensor(pos[1], sf);
            std::cout << "sensor '" << pos[1] << "' saved.\n";
            if (const auto s = db.find_sensor(pos[1])) print_sensor(*s);
            return 0;
        } else if (cmd == "set-sensor") {
            if (pos.size() < 2) { std::cerr << "error: set-sensor requires an ID\n"; return 2; }
            if (!db.update_sensor(pos[1], sf)) {
                std::cerr << "error: no such sensor '" << pos[1] << "' (use add-sensor to create it)\n";
                return 1;
            }
            std::cout << "sensor '" << pos[1] << "' updated.\n";
            if (const auto s = db.find_sensor(pos[1])) print_sensor(*s);
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
