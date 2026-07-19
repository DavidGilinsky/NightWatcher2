// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/api/http_server.hpp
// Purpose:       Embedded HTTP/JSON API served inside nightwatcherd: sensor and
//                weather-station CRUD, readings/events queries, live device
//                poll/discover, and schema setup/maintenance. Token-authed writes.
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "database.hpp"

namespace nightwatcher {

struct ApiConfig {
    std::string bind = "127.0.0.1";
    int port = 8080;
    std::string token;        // NW_API_TOKEN; empty disables write endpoints
    std::string schema_file;  // path to schema.sql for POST /db/init
    std::string web_root;     // static files directory (optional)
    // When both are set, the server listens with TLS (HTTPS) using these PEM
    // files; empty = plain HTTP. The daemon generates a self-signed pair here
    // when the operator enables TLS and none exists yet.
    std::string tls_cert;
    std::string tls_key;
    db::DbConfig db;
    // When true, read endpoints also require a valid session or API token, not
    // just writes. The daemon sets this whenever the server is bound off
    // localhost, so exposing the UI to the LAN never leaves readings world-open.
    // Localhost binds leave reads open for convenience.
    bool require_auth_reads = false;
    // Invoked (after the response) when POST /settings/apply is called, so the
    // daemon can restart the server on a changed bind. Empty = the endpoint is a
    // no-op (e.g. in tests).
    std::function<void()> on_apply;
    // Invoked (after the response) when a sensor's participation in polling
    // changes (enable/disable/delete), so the daemon can reload its device list
    // and start/stop populating the database immediately. Empty = no-op.
    std::function<void()> on_reload;
};

// Runs an httplib server on a background thread. Each request handler opens its
// own database connection (Database is not thread-safe).
class HttpServer {
public:
    explicit HttpServer(ApiConfig cfg);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Bind and start serving on a background thread. Throws if the bind fails.
    void start();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nightwatcher
