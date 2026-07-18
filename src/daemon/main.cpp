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
#include <iostream>
#include <string>
#include <vector>

#include <pthread.h>

#include "config.hpp"
#include "database.hpp"
#include "logging.hpp"
#include "nightwatcher/version.hpp"
#include "scheduler.hpp"

using nightwatcher::Config;
using nightwatcher::Scheduler;
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

    int exit_code = 0;
    bool reload = true;
    while (reload) {
        reload = false;

        std::vector<db::SensorRow> sensors;
        try {
            db::Database dbh(db_cfg);
            sensors = dbh.active_sensors();
        } catch (const std::exception& e) {
            log_error(std::string("cannot read sensor list: ") + e.what());
            exit_code = 1;
            break;
        }
        log_info("polling " + std::to_string(sensors.size()) + " active sensor(s)");

        Scheduler sched(db_cfg);
        sched.start(sensors);
        if (sched.worker_count() == 0) {
            log_warn("no pollable (tcp) sensors registered; waiting for a signal");
        }

        int sig = 0;
        while (true) {
            if (sigwait(&sigs, &sig) != 0) continue;
            if (sig == SIGINT || sig == SIGTERM) {
                log_info("signal " + std::to_string(sig) + " received; shutting down");
                sched.stop();
                break;
            }
            if (sig == SIGHUP) {
                log_info("SIGHUP received; reloading sensor list");
                sched.stop();
                reload = true;
                break;
            }
        }
        sched.join();
    }

    try {
        db::Database dbh(db_cfg);
        dbh.insert_event("daemon", "info", "stopped");
    } catch (const std::exception&) {
        // best effort
    }
    log_info("nightwatcherd stopped");
    return exit_code;
}
