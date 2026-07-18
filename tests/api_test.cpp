// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/api_test.cpp
// Purpose:       Integration test for the HTTP API: starts HttpServer and drives
//                it with the httplib client (auth, sensor CRUD, db/status). Skips
//                unless NW_DB_TEST=1 with NW_DB_* set.
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstdlib>
#include <string>

#include "database.hpp"
#include "http_server.hpp"
#include "httplib.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;
using nightwatcher::ApiConfig;
using nightwatcher::HttpServer;
namespace db = nightwatcher::db;

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
        std::puts("api_test skipped (set NW_DB_TEST=1 with NW_DB_* to run)");
        return 0;
    }

    ApiConfig cfg;
    cfg.bind = "127.0.0.1";
    cfg.port = 18099;
    cfg.token = "testtoken";
    cfg.db = db::DbConfig::from_env();

    // Clean up any leftover test sensor before starting.
    try {
        db::Database d(cfg.db);
        d.remove_sensor("APITEST");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "api_test: DB not reachable: %s\n", e.what());
        return 1;
    }

    HttpServer server(cfg);
    try {
        server.start();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "api_test: server start failed: %s\n", e.what());
        return 1;
    }

    httplib::Client cli("127.0.0.1", 18099);
    cli.set_connection_timeout(2, 0);

    // Version (retry until the listener is up).
    httplib::Result r;
    for (int i = 0; i < 100; ++i) {
        r = cli.Get("/api/v1/version");
        if (r && r->status == 200) break;
    }
    CHECK(r && r->status == 200);

    CHECK((r = cli.Get("/api/v1/health")) && r->status == 200);
    CHECK((r = cli.Get("/api/v1/sensors")) && r->status == 200);
    CHECK((r = cli.Get("/api/v1/db/status")) && r->status == 200);

    const char* body = R"({"id":"APITEST","tcp":"127.0.0.1:1","probe":false,"name":"Api Test","lat":32.0})";

    // Write without the token is rejected.
    r = cli.Post("/api/v1/sensors", body, "application/json");
    CHECK(r && r->status == 401);

    // Write with the token succeeds.
    const httplib::Headers auth = {{"Authorization", "Bearer testtoken"}};
    r = cli.Post("/api/v1/sensors", auth, body, "application/json");
    CHECK(r && r->status == 201);

    r = cli.Get("/api/v1/sensors/APITEST");
    CHECK(r && r->status == 200);
    if (r && r->status == 200) {
        const json j = json::parse(r->body);
        CHECK(j["name"] == "Api Test");
        CHECK(j["transport"] == "tcp");
    }

    // Partial update.
    r = cli.Patch("/api/v1/sensors/APITEST", auth, R"({"elevation_m":1234})", "application/json");
    CHECK(r && r->status == 200);

    // Delete, then confirm gone.
    r = cli.Delete("/api/v1/sensors/APITEST", auth);
    CHECK(r && r->status == 200);
    r = cli.Get("/api/v1/sensors/APITEST");
    CHECK(r && r->status == 404);

    server.stop();

    if (g_failures == 0) {
        std::puts("api_test passed");
        return 0;
    }
    std::fprintf(stderr, "api_test FAILED (%d checks)\n", g_failures);
    return 1;
}
