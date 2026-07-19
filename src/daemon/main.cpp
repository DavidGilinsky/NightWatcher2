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
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>
#include <unistd.h>

#include "config.hpp"
#include "database.hpp"
#include "http_server.hpp"
#include "logging.hpp"
#include "nightwatcher/version.hpp"
#include "password.hpp"
#include "scheduler.hpp"
#include "export_scheduler.hpp"
#include "weather_scheduler.hpp"

using nightwatcher::ApiConfig;
using nightwatcher::Config;
using nightwatcher::HttpServer;
using nightwatcher::Scheduler;
using nightwatcher::exporter::ExportScheduler;
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
    sigaddset(&sigs, SIGUSR1);  // "Restart & apply": rebind the API server
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
    // (Re)start the HTTP API on the effective bind/port: config-file defaults,
    // overridden by DB settings that the web UI edits. Re-callable, so the
    // "Restart & apply" button (which raises SIGUSR1) can rebind without a full
    // process restart.
    const auto start_api_server = [&]() {
        api.reset();  // stop any running server first (~HttpServer stops + joins)
        if (cfg.api_port <= 0) return;
        std::string bind = cfg.api_bind;
        int port = cfg.api_port;
        try {
            db::Database sdb(db_cfg);
            if (auto v = sdb.get_setting("api_bind"); v && !v->empty()) bind = *v;
            if (auto v = sdb.get_setting("api_port"); v && !v->empty()) {
                const int p = std::atoi(v->c_str());
                if (p > 0 && p < 65536) port = p;
            }
        } catch (const std::exception&) { /* settings unavailable; use config-file values */ }

        std::string tok;
        if (const char* t = std::getenv("NW_API_TOKEN")) tok = t;
        const auto make = [&](const std::string& b) {
            ApiConfig ac;
            ac.bind = b;
            ac.port = port;
            ac.token = tok;
            ac.schema_file = cfg.schema_file;
            ac.web_root = cfg.web_root;
            ac.db = db_cfg;
            // Off localhost, require auth for reads too — never expose readings
            // world-open on the LAN. The localhost fallback below re-enables open
            // reads because it passes "127.0.0.1" here.
            ac.require_auth_reads = (b != "127.0.0.1" && b != "localhost" && b != "::1");
            ac.on_apply = [] { kill(getpid(), SIGUSR1); };  // raised by POST /settings/apply
            auto srv = std::make_unique<HttpServer>(std::move(ac));
            srv->start();  // throws on bind failure
            return srv;
        };
        // Try the configured bind, retrying briefly: on a rebind the previous
        // listener may still be releasing the port for a moment.
        bool bound = false;
        for (int i = 0; i < 10 && !bound; ++i) {
            try {
                api = make(bind);
                bound = true;
            } catch (const std::exception& e) {
                if (i == 0)
                    log_warn(bind + ":" + std::to_string(port) + " not bindable yet (" + e.what() +
                             "); retrying");
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }
        }
        // Anti-lockout: only after retries fail, fall back to localhost so the
        // UI stays reachable to fix the setting.
        if (!bound) {
            log_error("could not bind API to " + bind + ":" + std::to_string(port) + " after retries");
            if (bind != "127.0.0.1" && bind != "localhost") {
                log_warn("falling back to 127.0.0.1:" + std::to_string(port) +
                         " so the web UI stays reachable");
                try {
                    api = make("127.0.0.1");
                } catch (const std::exception& e2) {
                    log_error(std::string("localhost fallback also failed: ") + e2.what());
                    api.reset();
                }
            } else {
                api.reset();
            }
        }
    };
    start_api_server();

    int exit_code = 0;
    bool reload = true;
    while (reload) {
        reload = false;

        std::vector<db::SensorRow> sensors;
        std::vector<db::WeatherStationRow> stations;
        std::vector<db::ExportTargetRow> exports;
        try {
            db::Database dbh(db_cfg);
            sensors = dbh.active_sensors();
            stations = dbh.active_weather_stations();
            exports = dbh.active_export_targets();
        } catch (const std::exception& e) {
            log_error(std::string("cannot read device list: ") + e.what());
            exit_code = 1;
            break;
        }
        log_info("polling " + std::to_string(sensors.size()) + " sensor(s), " +
                 std::to_string(stations.size()) + " weather station(s); " +
                 std::to_string(exports.size()) + " export target(s)");

        Scheduler sched(db_cfg);
        sched.start(sensors);
        WeatherScheduler wsched(db_cfg);
        wsched.start(stations);
        ExportScheduler esched(db_cfg);
        esched.start(exports);
        if (sched.worker_count() == 0 && wsched.worker_count() == 0 && esched.worker_count() == 0) {
            log_warn("no pollable devices registered; waiting for a signal");
        }

        int sig = 0;
        while (true) {
            if (sigwait(&sigs, &sig) != 0) continue;
            if (sig == SIGINT || sig == SIGTERM) {
                log_info("signal " + std::to_string(sig) + " received; shutting down");
                sched.stop();
                wsched.stop();
                esched.stop();
                break;
            }
            if (sig == SIGHUP) {
                log_info("SIGHUP received; reloading device lists");
                sched.stop();
                wsched.stop();
                esched.stop();
                reload = true;
                break;
            }
            if (sig == SIGUSR1) {
                log_info("SIGUSR1 received; restarting the API server to apply settings");
                std::this_thread::sleep_for(std::chrono::milliseconds(250));  // let the response flush
                start_api_server();  // reads fresh settings; anti-lockout falls back to localhost
                continue;  // keep polling; no device reload
            }
        }
        sched.join();
        wsched.join();
        esched.join();
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
