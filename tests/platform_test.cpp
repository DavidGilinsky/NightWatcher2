// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/platform_test.cpp
// Purpose:       Integration test for the platform helper proxy: stands up a
//                stub helper on a unix socket, starts HttpServer, and checks
//                that /api/v1/platform forwards, authorizes, and disappears
//                cleanly when no helper is present. Needs no database (the
//                static API token authorizes without one).
// Created:       2026-07-27
// Last Modified: 2026-07-27
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

#include "http_server.hpp"
#include "httplib.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;
using nightwatcher::ApiConfig;
using nightwatcher::HttpServer;

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
    const std::string sock = "/tmp/nw-platform-test-" + std::to_string(::getpid()) + ".sock";
    ::unlink(sock.c_str());
    ::setenv("NW_PLATFORM_SOCKET", sock.c_str(), 1);

    // ---- stub helper: what an appliance's privileged agent would serve ----
    httplib::Server helper;
    helper.set_address_family(AF_UNIX);
    helper.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(json{{"name", "test"}, {"label", "Test"}, {"ui", "ui.js"}}.dump(),
                        "application/json");
    });
    helper.Get("/ui.js", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("export function render() {}", "application/javascript");
    });
    helper.Get("/echo", [](const httplib::Request& req, httplib::Response& res) {
        res.set_content(json{{"q", req.get_param_value("q")}}.dump(), "application/json");
    });
    helper.Post("/act", [](const httplib::Request& req, httplib::Response& res) {
        res.status = 202;
        res.set_content(json{{"got", json::parse(req.body)}}.dump(), "application/json");
    });
    std::thread helper_thread([&] { helper.listen(sock, 80); });
    for (int i = 0; i < 100 && !helper.is_running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // ---- the daemon's HTTP server in front of it ----
    ApiConfig cfg;
    cfg.bind = "127.0.0.1";
    cfg.port = 18098;
    cfg.token = "testtoken";
    HttpServer server(cfg);
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("127.0.0.1", cfg.port);
    cli.set_connection_timeout(5, 0);
    const httplib::Headers auth{{"Authorization", "Bearer testtoken"}};

    // The probe the web UI uses to decide whether to offer the tab.
    if (auto r = cli.Get("/api/v1/platform", auth)) {
        CHECK(r->status == 200);
        const auto j = json::parse(r->body, nullptr, false);
        CHECK(!j.is_discarded() && j.value("ui", "") == "ui.js");
    } else {
        CHECK(false);
    }

    // The helper's own UI module, served through the daemon's origin so it
    // rides the same session and certificate.
    if (auto r = cli.Get("/api/v1/platform/ui.js", auth)) {
        CHECK(r->status == 200);
        CHECK(r->body.find("export function render") != std::string::npos);
        CHECK(r->get_header_value("Content-Type").find("javascript") != std::string::npos);
    } else {
        CHECK(false);
    }

    // Query strings and POST bodies survive the hop.
    if (auto r = cli.Get("/api/v1/platform/echo?q=hello", auth)) {
        CHECK(r->status == 200);
        CHECK(json::parse(r->body, nullptr, false).value("q", "") == "hello");
    } else {
        CHECK(false);
    }
    if (auto r = cli.Post("/api/v1/platform/act", auth, R"({"ssid":"x"})", "application/json")) {
        CHECK(r->status == 202);  // the helper's status is passed through, not invented
        CHECK(r->body.find("ssid") != std::string::npos);
    } else {
        CHECK(false);
    }

    // Unauthenticated callers never reach the helper.
    if (auto r = cli.Get("/api/v1/platform")) {
        CHECK(r->status == 401);
    } else {
        CHECK(false);
    }

    // A path cannot climb out of the helper's namespace.
    if (auto r = cli.Get("/api/v1/platform/../secret", auth)) {
        CHECK(r->status == 400 || r->status == 404);
    } else {
        CHECK(false);
    }

    // With no helper socket the whole surface is gone, which is the state every
    // non-appliance install is in.
    helper.stop();
    helper_thread.join();
    ::unlink(sock.c_str());
    if (auto r = cli.Get("/api/v1/platform", auth)) {
        CHECK(r->status == 404);
    } else {
        CHECK(false);
    }

    server.stop();
    if (g_failures == 0) std::puts("platform_test passed");
    return g_failures == 0 ? 0 : 1;
}
