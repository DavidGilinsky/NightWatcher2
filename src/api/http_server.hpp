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
    db::DbConfig db;
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
