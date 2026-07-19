// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/daemon/main.cpp
// Purpose:       Entry point for nightwatcherd: load config, connect to the
//                database, poll every active sensor on its interval, and handle
//                signals (SIGTERM/SIGINT shut down, SIGHUP reloads the sensor list).
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <pthread.h>

#include "config.hpp"
#include "database.hpp"
#include "http_server.hpp"
#include "logging.hpp"
#include "nightwatcher/version.hpp"
#include "password.hpp"
#include "scheduler.hpp"
#include "weather_scheduler.hpp"

using nightwatcher::ApiConfig;
using nightwatcher::Config;
using nightwatcher::HttpServer;
using nightwatcher::Scheduler;
using nightwatcher::weather::WeatherScheduler;
using nightwatcher::log_error;
using nightwatcher::log_info;
using nightwatcher::log_warn;
namespace db = nightwatcher::db;

namespace {

void print_version() { std::cout << "nightwatcherd " << NIGHTWATCHER_VERSION << "\n"; }

void print_help(const char* argv0) {
    std::cout
        << "nightwatcherd " << NIGHTWATCHER_VERSION
        << " \xe2\x80\x94 NightWatcher2 SQM daemon\n\n"
        << "Usage: " << argv0 << " [options]\n\n"
        << "Options:\n"
        << "  -c, --config <path>   Path to the INI-style config file\n"
        << "  -v, --version         Print version and exit\n"
        << "  -h, --help            Print this help and exit\n\n"
        << "The daemon polls every active sensor from the database (register them\n"
        << "with nwdb) at each sensor's poll_interval_s, stores readings, and logs\n"
        << "to the events table. Send SIGHUP to reload the sensor list.\n";
}

db::DbConfig make_db_config(const Config& cfg) {
    db::DbConfig dc;
    dc.host = cfg.db_host;
    dc.port = static_cast<uint16_t>(cfg.db_port);
    dc.user = cfg.db_user;
    dc.database = cfg.db_name;
    if (const char* p = std::getenv("NW_DB_PASSWORD")) dc.password = p;
    return dc;
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-v" || a == "--version") {
            print_version();
            return 0;
        } else if (a == "-h" || a == "--help") {
            print_help(argv[0]);
            return 0;
        } else if (a == "-c" || a == "--config") {
            if (i + 1 >= argc) { std::cerr << "error: " << a << " requires a path\n"; return 2; }
            config_path = argv[++i];
        } else {
            std::cerr << "error: unknown argument '" << a << "' (try --help)\n";
            return 2;
        }
    }
    if (config_path.empty()) {
        std::cerr << "error: no config file given (use --config <path>)\n";
        return 2;
    }

    Config cfg;
    try {
        cfg = Config::load(config_path);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    const db::DbConfig db_cfg = make_db_config(cfg);

    // Block the signals we handle so all threads inherit the mask; the main
    // thread consumes them synchronously with sigwait().
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    sigaddset(&sigs, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

    // Verify the database is reachable and record startup.
    try {
        db::Database dbh(db_cfg);
        dbh.insert_event("daemon", "info", "started",
                         std::string("nightwatcherd ") + NIGHTWATCHER_VERSION);
    } catch (const std::exception& e) {
        log_error(std::string("cannot connect to database: ") + e.what());
        return 1;
    }
    log_info(std::string("nightwatcherd ") + NIGHTWATCHER_VERSION + " starting (db " +
             db_cfg.user + "@" + db_cfg.host + "/" + db_cfg.database + ")");

    // Ensure the schema exists (idempotent) if a schema file is configured.
    if (!cfg.schema_file.empty()) {
        try {
            std::ifstream in(cfg.schema_file);
            if (in) {
                std::stringstream ss;
                ss << in.rdbuf();
                db::Database dbh(db_cfg);
                dbh.run_schema_script(ss.str());
            }
        } catch (const std::exception& e) {
            log_warn(std::string("schema ensure failed: ") + e.what());
        }
    }
    // Seed the initial admin user if there are none yet.
    try {
        db::Database dbh(db_cfg);
        if (dbh.count_users() == 0) {
            dbh.create_user("admin", nightwatcher::auth::hash_password("admin"), "admin", true);
            log_warn("seeded default admin user 'admin' / 'admin' \xe2\x80\x94 change the password "
                     "after first login");
        }
    } catch (const std::exception& e) {
        log_warn(std::string("could not ensure admin user (run schema.sql or POST /db/init): ") +
                 e.what());
    }

    // Start the HTTP/JSON API (shared with the web UI). It reads the database per
    // request, so it keeps running across sensor-list reloads.
    std::unique_ptr<HttpServer> api;
    if (cfg.api_port > 0) {
        ApiConfig api_cfg;
        api_cfg.bind = cfg.api_bind;
        api_cfg.port = cfg.api_port;
        if (const char* tok = std::getenv("NW_API_TOKEN")) api_cfg.token = tok;
        api_cfg.schema_file = cfg.schema_file;
        api_cfg.web_root = cfg.web_root;
        api_cfg.db = db_cfg;
        api = std::make_unique<HttpServer>(std::move(api_cfg));
        try {
            api->start();
        } catch (const std::exception& e) {
            log_error(std::string("API failed to start: ") + e.what());
            api.reset();
        }
    }

    int exit_code = 0;
    bool reload = true;
    while (reload) {
        reload = false;

        std::vector<db::SensorRow> sensors;
        std::vector<db::WeatherStationRow> stations;
        try {
            db::Database dbh(db_cfg);
            sensors = dbh.active_sensors();
            stations = dbh.active_weather_stations();
        } catch (const std::exception& e) {
            log_error(std::string("cannot read device list: ") + e.what());
            exit_code = 1;
            break;
        }
        log_info("polling " + std::to_string(sensors.size()) + " sensor(s), " +
                 std::to_string(stations.size()) + " weather station(s)");

        Scheduler sched(db_cfg);
        sched.start(sensors);
        WeatherScheduler wsched(db_cfg);
        wsched.start(stations);
        if (sched.worker_count() == 0 && wsched.worker_count() == 0) {
            log_warn("no pollable devices registered; waiting for a signal");
        }

        int sig = 0;
        while (true) {
            if (sigwait(&sigs, &sig) != 0) continue;
            if (sig == SIGINT || sig == SIGTERM) {
                log_info("signal " + std::to_string(sig) + " received; shutting down");
                sched.stop();
                wsched.stop();
                break;
            }
            if (sig == SIGHUP) {
                log_info("SIGHUP received; reloading device lists");
                sched.stop();
                wsched.stop();
                reload = true;
                break;
            }
        }
        sched.join();
        wsched.join();
    }

    if (api) api->stop();

    try {
        db::Database dbh(db_cfg);
        dbh.insert_event("daemon", "info", "stopped");
    } catch (const std::exception&) {
        // best effort
    }
    log_info("nightwatcherd stopped");
    return exit_code;
}
