// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/config_test.cpp
// Purpose:       Unit test for nightwatcher::Config::load (plain CTest exit code,
//                no framework).
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// ---------------------------------------------------------------------------
#include <cstdio>
#include <fstream>
#include <string>

#include "config.hpp"

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
    const std::string path = "test_nightwatcher.conf";
    {
        std::ofstream out(path);
        out << "# NightWatcher2 test config\n"
            << "[daemon]\n"
            << "interval = 120\n"
            << "\n"
            << "[database]\n"
            << "host = db.example.org\n"
            << "name = nw\n"
            << "user = nwuser\n"
            << "\n"
            << "[api]\n"
            << "port = 9090\n"
            << "tls = on\n"
            << "\n"
            << "[sensor:DSN003]\n"
            << "transport = tcp\n"
            << "address = 192.168.1.50:10001\n"
            << "name = Sugarloaf\n"
            << "\n"
            << "[sensor:DSN004]\n"
            << "transport = serial\n"
            << "address = /dev/ttyUSB0\n";
    }

    const nightwatcher::Config cfg = nightwatcher::Config::load(path);
    std::remove(path.c_str());

    CHECK(cfg.poll_interval_s == 120);
    CHECK(cfg.db_host == "db.example.org");
    CHECK(cfg.db_name == "nw");
    CHECK(cfg.db_user == "nwuser");
    CHECK(cfg.api_port == 9090);
    CHECK(cfg.api_tls == true);
    // With no explicit paths, the cert/key default to a tls/ dir beside the config.
    CHECK(cfg.api_tls_cert == "./tls/nightwatcher-cert.pem");
    CHECK(cfg.api_tls_key == "./tls/nightwatcher-key.pem");
    CHECK(cfg.sensors.size() == 2);
    if (cfg.sensors.size() == 2) {
        CHECK(cfg.sensors[0].id == "DSN003");
        CHECK(cfg.sensors[0].transport == "tcp");
        CHECK(cfg.sensors[0].address == "192.168.1.50:10001");
        CHECK(cfg.sensors[0].name == "Sugarloaf");
        CHECK(cfg.sensors[1].id == "DSN004");
        CHECK(cfg.sensors[1].transport == "serial");
        CHECK(cfg.sensors[1].address == "/dev/ttyUSB0");
    }

    if (g_failures == 0) {
        std::puts("config_test passed");
        return 0;
    }
    std::fprintf(stderr, "config_test FAILED (%d checks)\n", g_failures);
    return 1;
}
