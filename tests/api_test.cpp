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
#include <optional>
#include <string>

#include <unistd.h>

#include "database.hpp"
#include "http_server.hpp"
#include "httplib.h"
#include "nlohmann/json.hpp"
#include "password.hpp"
#include "tls_cert.hpp"

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
        CHECK(j["status"] == "inactive");  // new sensors start disabled (no DB population)
    }

    // Partial update.
    r = cli.Patch("/api/v1/sensors/APITEST", auth, R"({"elevation_m":1234})", "application/json");
    CHECK(r && r->status == 200);

    // Non-persisting self-test: 127.0.0.1:1 is unreachable, so the connect check
    // fails and overall ok is false, but the endpoint still returns structured
    // results and writes nothing to the readings table.
    r = cli.Post("/api/v1/sensors/APITEST/test", auth, "", "application/json");
    CHECK(r && r->status == 200);
    if (r && r->status == 200) {
        const json j = json::parse(r->body);
        CHECK(j["ok"] == false);
        CHECK(j["checks"].is_array() && !j["checks"].empty());
        CHECK(j["checks"][0]["name"] == "connect");
        CHECK(j["checks"][0]["ok"] == false);
    }
    r = cli.Get("/api/v1/sensors/APITEST/readings?limit=1");
    CHECK(r && r->status == 200);
    if (r && r->status == 200) CHECK(json::parse(r->body).empty());  // test stored nothing

    // Enable/disable flip the status (the daemon reload hook is a no-op in tests).
    r = cli.Post("/api/v1/sensors/APITEST/enable", auth, "", "application/json");
    CHECK(r && r->status == 200);
    if (r && r->status == 200) CHECK(json::parse(r->body)["status"] == "active");
    r = cli.Post("/api/v1/sensors/APITEST/disable", auth, "", "application/json");
    CHECK(r && r->status == 200);
    if (r && r->status == 200) CHECK(json::parse(r->body)["status"] == "inactive");
    // Both require auth.
    CHECK((r = cli.Post("/api/v1/sensors/APITEST/enable", "", "application/json")) && r->status == 401);
    CHECK((r = cli.Post("/api/v1/sensors/APITEST/test", "", "application/json")) && r->status == 401);

    // ---- calibration endpoints (routing/auth/validation; APITEST is unreachable) ----
    // History is a DB read (empty array for a fresh sensor).
    r = cli.Get("/api/v1/sensors/APITEST/calibration/history");
    CHECK(r && r->status == 200);
    if (r && r->status == 200) CHECK(json::parse(r->body).is_array());
    // Arm validates the mode before touching the device.
    CHECK((r = cli.Post("/api/v1/sensors/APITEST/calibration/arm", auth, R"({"mode":"nope"})",
                        "application/json")) && r->status == 400);
    // Reaching the (unreachable) device yields 502.
    CHECK((r = cli.Get("/api/v1/sensors/APITEST/calibration")) && r->status == 502);
    CHECK((r = cli.Post("/api/v1/sensors/APITEST/calibration/set", auth, R"({"light_offset":17.6})",
                        "application/json")) && r->status == 502);
    // Mutating calibration requires auth.
    CHECK((r = cli.Post("/api/v1/sensors/APITEST/calibration/record", "", "application/json")) &&
          r->status == 401);
    CHECK((r = cli.Post("/api/v1/sensors/APITEST/calibration/set", "", "application/json")) &&
          r->status == 401);

    // DSN download: a community-format .dat with an attachment header.
    r = cli.Get("/api/v1/sensors/APITEST/dsn");
    CHECK(r && r->status == 200);
    if (r && r->status == 200) {
        CHECK(r->body.rfind("# Community Standard Skyglow Data Format 1.0", 0) == 0);
        CHECK(r->has_header("Content-Disposition"));
    }
    CHECK((r = cli.Get("/api/v1/sensors/NOPE_APITEST/dsn")) && r->status == 404);

    // ---- export targets (nested-secret masking + run/log) ----
    {
        const char* exbody =
            R"({"id":"APIEXP","sensor_id":"APITEST","target":"dsn","schedule":"manual",)"
            R"("config":{"site_id":"DSN036-S","supplier":"Api","auth":{"mode":"oauth",)"
            R"("client_id":"cid","client_secret":"SECRET","refresh_token":"REFRESH"}}})";
        r = cli.Post("/api/v1/export-targets", auth, exbody, "application/json");
        CHECK(r && r->status == 201);

        r = cli.Get("/api/v1/export-targets/APIEXP");
        CHECK(r && r->status == 200);
        if (r && r->status == 200) {
            const json j = json::parse(r->body);
            CHECK(j["target"] == "dsn");
            CHECK(j["sensor_id"] == "APITEST");
            CHECK(j["config"]["auth"]["refresh_token"] == "***");   // nested secret masked
            CHECK(j["config"]["auth"]["client_secret"] == "***");
            CHECK(j["config"]["auth"]["client_id"] == "cid");       // non-secret visible
            CHECK(j["config"]["site_id"] == "DSN036-S");
        }

        // PATCH a non-secret field leaving the masked secret in place -> secret preserved.
        r = cli.Patch("/api/v1/export-targets/APIEXP", auth,
                      R"({"config":{"supplier":"Api2","auth":{"refresh_token":"***"}}})",
                      "application/json");
        CHECK(r && r->status == 200);
        {
            db::Database d(cfg.db);
            const auto t = d.find_export_target("APIEXP");
            CHECK(t.has_value());
            if (t) {
                const json cf = json::parse(t->config);
                CHECK(cf["auth"]["refresh_token"] == "REFRESH");  // preserved through the edit
                CHECK(cf["supplier"] == "Api2");                  // updated
            }
        }

        r = cli.Get("/api/v1/export-targets/APIEXP/log", auth);
        CHECK(r && r->status == 200);
        // run-now with no readings for this sensor exports nothing (no network).
        r = cli.Post("/api/v1/export-targets/APIEXP/run", auth, "", "application/json");
        CHECK(r && r->status == 200);
        if (r && r->status == 200) CHECK(json::parse(r->body)["files"] == 0);

        r = cli.Delete("/api/v1/export-targets/APIEXP", auth);
        CHECK(r && r->status == 200);
    }

    // Delete, then confirm gone.
    r = cli.Delete("/api/v1/sensors/APITEST", auth);
    CHECK(r && r->status == 200);
    r = cli.Get("/api/v1/sensors/APITEST");
    CHECK(r && r->status == 404);

    // ---- login flow ----
    {
        db::Database d(cfg.db);
        d.delete_user("apiadmin");
        d.create_user("apiadmin", nightwatcher::auth::hash_password("secret"), "admin", false);
    }
    // Bad password is rejected.
    r = cli.Post("/api/v1/login", R"({"username":"apiadmin","password":"wrong"})", "application/json");
    CHECK(r && r->status == 401);
    // Good password logs in and returns a session token.
    r = cli.Post("/api/v1/login", R"({"username":"apiadmin","password":"secret"})", "application/json");
    CHECK(r && r->status == 200);
    std::string sess;
    if (r && r->status == 200) {
        const json j = json::parse(r->body);
        sess = j["token"].get<std::string>();
        CHECK(j["user"]["role"] == "admin");
    }
    const httplib::Headers sauth = {{"Authorization", "Bearer " + sess}};
    // /me reflects the logged-in user.
    r = cli.Get("/api/v1/me", sauth);
    CHECK(r && r->status == 200);
    if (r && r->status == 200) {
        const json j = json::parse(r->body);
        CHECK(j["username"] == "apiadmin");
        CHECK(j["via"] == "session");
    }
    // The session authorizes a write.
    r = cli.Post("/api/v1/sensors", sauth,
                 R"({"id":"APITEST2","tcp":"127.0.0.1:1","probe":false})", "application/json");
    CHECK(r && r->status == 201);
    r = cli.Delete("/api/v1/sensors/APITEST2", sauth);
    CHECK(r && r->status == 200);

    // Snapshot the settings this test mutates so we can restore them exactly.
    // The DB may be shared with a live daemon; clobbering api_bind/api_port/
    // api_tls would silently change where it listens.
    std::optional<std::string> save_bind, save_port, save_tls;
    {
        db::Database d(cfg.db);
        save_bind = d.get_setting("api_bind");
        save_port = d.get_setting("api_port");
        save_tls = d.get_setting("api_tls");
    }

    // Server settings: GET is admin-only; PUT validates and persists.
    CHECK((r = cli.Get("/api/v1/settings")) && r->status == 401);  // no auth
    r = cli.Get("/api/v1/settings", sauth);
    CHECK(r && r->status == 200);
    if (r && r->status == 200) {
        const json j = json::parse(r->body);
        CHECK(j.contains("running") && j.contains("configured"));
    }
    CHECK((r = cli.Put("/api/v1/settings", sauth, R"({"bind":"bad addr!"})", "application/json")) &&
          r->status == 400);
    CHECK((r = cli.Put("/api/v1/settings", sauth, R"({"port":99999})", "application/json")) &&
          r->status == 400);
    // A valid change persists (127.0.0.1:8080 = the daemon's normal values, so this is safe).
    r = cli.Put("/api/v1/settings", sauth, R"({"bind":"127.0.0.1","port":8080})", "application/json");
    CHECK(r && r->status == 200);
    if (r && r->status == 200) {
        const json j = json::parse(r->body);
        CHECK(j["configured"]["bind"] == "127.0.0.1");
        CHECK(j["configured"]["port"] == 8080);
    }
    // Apply endpoint: admin-only, and a no-op here (no on_apply callback set).
    CHECK((r = cli.Post("/api/v1/settings/apply", "", "application/json")) && r->status == 401);
    r = cli.Post("/api/v1/settings/apply", sauth, "", "application/json");
    CHECK(r && r->status == 200);
    if (r && r->status == 200) CHECK(json::parse(r->body)["applying"] == false);

    // Restore the settings snapshot (set back if present, delete if it wasn't).
    {
        db::Database d(cfg.db);
        const auto restore = [&d](const char* k, const std::optional<std::string>& v) {
            if (v) d.set_setting(k, *v);
            else d.delete_setting(k);
        };
        restore("api_bind", save_bind);
        restore("api_port", save_port);
        restore("api_tls", save_tls);
    }

    // Logout invalidates the session.
    r = cli.Post("/api/v1/logout", sauth, "", "application/json");
    CHECK(r && r->status == 200);
    r = cli.Get("/api/v1/me", sauth);
    CHECK(r && r->status == 401);
    {
        db::Database d(cfg.db);
        d.delete_user("apiadmin");
    }

    // ---- read-auth gate (as set when bound off localhost) ----
    // A second server with require_auth_reads forces the gate on even for this
    // local client, so reads now demand a session or token; the sign-in and
    // liveness endpoints stay public.
    {
        ApiConfig rcfg;
        rcfg.bind = "127.0.0.1";
        rcfg.port = 18100;
        rcfg.token = "testtoken";
        rcfg.db = db::DbConfig::from_env();
        rcfg.require_auth_reads = true;
        HttpServer rserver(rcfg);
        try {
            rserver.start();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "api_test: gated server start failed: %s\n", e.what());
            return 1;
        }
        httplib::Client rc("127.0.0.1", 18100);
        rc.set_connection_timeout(2, 0);
        httplib::Result rr;
        for (int i = 0; i < 100; ++i) {
            rr = rc.Get("/api/v1/version");
            if (rr && rr->status == 200) break;
        }
        CHECK(rr && rr->status == 200);                                // version stays public
        CHECK((rr = rc.Get("/api/v1/health")) && rr->status == 200);   // liveness stays public
        CHECK((rr = rc.Get("/api/v1/sensors")) && rr->status == 401);  // read now gated
        const httplib::Headers ra = {{"Authorization", "Bearer testtoken"}};
        CHECK((rr = rc.Get("/api/v1/sensors", ra)) && rr->status == 200);  // token authorizes it
        rserver.stop();
    }

    // ---- HTTPS end to end ----
    // Generate a self-signed cert, serve with it, and reach it over TLS.
    {
        const std::string cert = "nw_api_tls_cert.pem";
        const std::string key = "nw_api_tls_key.pem";
        ::unlink(cert.c_str());
        ::unlink(key.c_str());
        std::string info;
        CHECK(nightwatcher::ensure_self_signed_cert(cert, key, "localhost", info));

        ApiConfig scfg;
        scfg.bind = "127.0.0.1";
        scfg.port = 18101;
        scfg.token = "testtoken";
        scfg.tls_cert = cert;
        scfg.tls_key = key;
        scfg.db = db::DbConfig::from_env();
        HttpServer sserver(scfg);
        try {
            sserver.start();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "api_test: TLS server start failed: %s\n", e.what());
            ++g_failures;
        }
        httplib::SSLClient sc("127.0.0.1", 18101);
        sc.enable_server_certificate_verification(false);  // self-signed
        sc.set_connection_timeout(2, 0);
        httplib::Result sr;
        for (int i = 0; i < 100; ++i) {
            sr = sc.Get("/api/v1/version");
            if (sr && sr->status == 200) break;
        }
        CHECK(sr && sr->status == 200);  // served over HTTPS
        sserver.stop();
        ::unlink(cert.c_str());
        ::unlink(key.c_str());
    }

    server.stop();

    if (g_failures == 0) {
        std::puts("api_test passed");
        return 0;
    }
    std::fprintf(stderr, "api_test FAILED (%d checks)\n", g_failures);
    return 1;
}
