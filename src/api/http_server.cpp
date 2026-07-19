// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/api/http_server.cpp
// Purpose:       Implementation of the embedded HTTP/JSON API (cpp-httplib +
//                nlohmann/json). Reads are open; writes require the API token.
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "http_server.hpp"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include "discovery.hpp"
#include "httplib.h"
#include "logging.hpp"
#include "nightwatcher/version.hpp"
#include "nlohmann/json.hpp"
#include "password.hpp"
#include "sqm_device.hpp"
#include "tcp_transport.hpp"

using json = nlohmann::json;

namespace nightwatcher {

struct HttpServer::Impl {
    ApiConfig cfg;
    httplib::Server srv;
    std::thread thread;
    explicit Impl(ApiConfig c) : cfg(std::move(c)) {}
};

namespace {

// ---- response helpers ------------------------------------------------------
void send(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}
void send_err(httplib::Response& res, int status, const std::string& msg) {
    send(res, status, json{{"error", msg}});
}

json j_opt(const std::optional<double>& v) { return v ? json(*v) : json(nullptr); }
json j_opt(const std::optional<int>& v) { return v ? json(*v) : json(nullptr); }

// ---- serialization ---------------------------------------------------------
json to_json(const db::SensorRow& s) {
    return json{{"id", s.id},
                {"name", s.name},
                {"site", s.site},
                {"serial_number", s.serial_number},
                {"model", s.model},
                {"protocol_ver", j_opt(s.protocol_ver)},
                {"feature_ver", j_opt(s.feature_ver)},
                {"latitude", j_opt(s.latitude)},
                {"longitude", j_opt(s.longitude)},
                {"elevation_m", j_opt(s.elevation_m)},
                {"timezone", s.timezone},
                {"transport", s.transport},
                {"address", s.address},
                {"poll_interval_s", s.poll_interval_s},
                {"status", s.status},
                {"installed_at", s.installed_at},
                {"notes", s.notes},
                {"created_at", s.created_at}};
}
json to_json(const db::ReadingRow& r) {
    return json{{"id", r.id},         {"sensor_id", r.sensor_id}, {"ts_utc", r.ts_utc},
                {"mag_arcsec2", r.mag_arcsec2}, {"freq_hz", r.freq_hz},
                {"period_counts", r.period_counts}, {"period_s", r.period_s},
                {"temp_c", r.temp_c}, {"quality", r.quality}, {"source", r.source},
                {"raw_line", r.raw_line}};
}
json to_json(const db::WeatherStationRow& w) {
    return json{{"id", w.id},
                {"name", w.name},
                {"site", w.site},
                {"model", w.model},
                {"transport", w.transport},
                {"address", w.address},
                {"latitude", j_opt(w.latitude)},
                {"longitude", j_opt(w.longitude)},
                {"elevation_m", j_opt(w.elevation_m)},
                {"timezone", w.timezone},
                {"poll_interval_s", w.poll_interval_s},
                {"status", w.status},
                {"installed_at", w.installed_at},
                {"notes", w.notes},
                {"created_at", w.created_at}};
}
json to_json(const db::TableCount& t) {
    return json{{"table", t.table}, {"present", t.present}, {"rows", t.rows}};
}
json to_json(const db::EventRow& e) {
    return json{{"id", e.id},       {"ts_utc", e.ts_utc}, {"device_id", e.device_id},
                {"source", e.source}, {"level", e.level},  {"event", e.event},
                {"detail", e.detail}};
}

// ---- request-body parsing --------------------------------------------------
db::SensorFields sensor_fields_from_json(const json& b) {
    db::SensorFields f;
    const auto s = [&](const char* k, std::optional<std::string>& o) {
        if (b.contains(k) && !b[k].is_null()) o = b[k].get<std::string>();
    };
    const auto d = [&](const char* k, std::optional<double>& o) {
        if (b.contains(k) && !b[k].is_null()) o = b[k].get<double>();
    };
    const auto i = [&](const char* k, std::optional<int>& o) {
        if (b.contains(k) && !b[k].is_null()) o = b[k].get<int>();
    };
    s("name", f.name); s("site", f.site); s("serial_number", f.serial_number); s("model", f.model);
    i("protocol_ver", f.protocol_ver); i("feature_ver", f.feature_ver);
    d("latitude", f.latitude); d("longitude", f.longitude); d("elevation_m", f.elevation_m);
    s("timezone", f.timezone); s("transport", f.transport); s("address", f.address);
    i("poll_interval_s", f.poll_interval_s); s("status", f.status);
    s("installed_at", f.installed_at); s("notes", f.notes);
    if (b.contains("tcp") && !b["tcp"].is_null()) {
        f.transport = std::string("tcp");
        f.address = b["tcp"].get<std::string>();
    }
    return f;
}
db::WeatherStationFields wx_fields_from_json(const json& b) {
    db::WeatherStationFields f;
    const auto s = [&](const char* k, std::optional<std::string>& o) {
        if (b.contains(k) && !b[k].is_null()) o = b[k].get<std::string>();
    };
    const auto d = [&](const char* k, std::optional<double>& o) {
        if (b.contains(k) && !b[k].is_null()) o = b[k].get<double>();
    };
    const auto i = [&](const char* k, std::optional<int>& o) {
        if (b.contains(k) && !b[k].is_null()) o = b[k].get<int>();
    };
    s("name", f.name); s("site", f.site); s("model", f.model);
    s("transport", f.transport); s("address", f.address);
    d("latitude", f.latitude); d("longitude", f.longitude); d("elevation_m", f.elevation_m);
    s("timezone", f.timezone); i("poll_interval_s", f.poll_interval_s);
    s("status", f.status); s("installed_at", f.installed_at); s("notes", f.notes);
    return f;
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

// Best-effort fill of serial/protocol/feature from the device's ix response.
void probe_fill(db::SensorFields& f) {
    if (!(f.transport && *f.transport == "tcp" && f.address)) return;
    try {
        std::string host;
        uint16_t port;
        split_endpoint(*f.address, host, port);
        auto t = std::make_unique<sqm::TcpTransport>(host, port, 3000);
        sqm::SqmDevice dev(std::move(t));
        const sqm::UnitInfo u = dev.info();
        if (!f.serial_number) f.serial_number = u.serial;
        if (!f.protocol_ver) f.protocol_ver = u.protocol;
        if (!f.feature_ver) f.feature_ver = u.feature;
    } catch (const std::exception&) {
        // leave those fields unset if the device is unreachable
    }
}

std::string get_cookie(const httplib::Request& req, const std::string& name) {
    const std::string c = req.get_header_value("Cookie");
    size_t pos = 0;
    while (pos < c.size()) {
        const size_t eq = c.find('=', pos);
        if (eq == std::string::npos) break;
        size_t ks = c.find_first_not_of(" \t", pos);
        if (ks == std::string::npos) ks = pos;
        const std::string key = c.substr(ks, eq - ks);
        const size_t semi = c.find(';', eq);
        const std::string val =
            c.substr(eq + 1, (semi == std::string::npos ? c.size() : semi) - eq - 1);
        if (key == name) return val;
        if (semi == std::string::npos) break;
        pos = semi + 1;
    }
    return std::string();
}

// Session token from the nw_session cookie or an Authorization: Bearer header.
std::string session_token(const httplib::Request& req) {
    std::string sess = get_cookie(req, "nw_session");
    if (sess.empty()) {
        const std::string h = req.get_header_value("Authorization");
        if (h.rfind("Bearer ", 0) == 0) sess = h.substr(7);
    }
    return sess;
}

// Authorize a write request via the static API token (admin) or a valid login
// session. Sends the error response and returns false when not authorized.
bool require_auth(const httplib::Request& req, httplib::Response& res, const std::string& token,
                  const db::DbConfig& dbc, bool need_admin = false) {
    const std::string h = req.get_header_value("Authorization");
    if (!token.empty() && (h == token || h == "Bearer " + token)) {
        return true;  // the static token acts as admin
    }
    const std::string sess = session_token(req);
    if (!sess.empty()) {
        try {
            db::Database db(dbc);
            if (const auto si = db.validate_session(sess)) {
                if (need_admin && si->role != "admin") {
                    send_err(res, 403, "admin role required");
                    return false;
                }
                return true;
            }
        } catch (const std::exception&) {
        }
    }
    send_err(res, 401, "unauthorized");
    return false;
}

}  // namespace

HttpServer::HttpServer(ApiConfig cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::start() {
    httplib::Server& srv = impl_->srv;
    const db::DbConfig dbc = impl_->cfg.db;
    const std::string token = impl_->cfg.token;
    const std::string schema_file = impl_->cfg.schema_file;

    if (!impl_->cfg.web_root.empty()) {
        srv.set_mount_point("/", impl_->cfg.web_root);
    }

    // ---- read endpoints ----
    srv.Get("/api/v1/version", [](const httplib::Request&, httplib::Response& res) {
        send(res, 200, json{{"version", NIGHTWATCHER_VERSION}});
    });
    srv.Get("/api/v1/health", [dbc](const httplib::Request&, httplib::Response& res) {
        json j{{"status", "ok"}, {"version", NIGHTWATCHER_VERSION}};
        try {
            db::Database db(dbc);
            (void)db;
            j["db"] = "ok";
        } catch (const std::exception& e) {
            j["db"] = "error";
            j["db_error"] = e.what();
        }
        send(res, 200, j);
    });
    srv.Get("/api/v1/sensors", [dbc](const httplib::Request&, httplib::Response& res) {
        try {
            db::Database db(dbc);
            json arr = json::array();
            for (const auto& s : db.sensors()) arr.push_back(to_json(s));
            send(res, 200, arr);
        } catch (const std::exception& e) { send_err(res, 500, e.what()); }
    });
    srv.Get(R"(/api/v1/sensors/([^/]+))", [dbc](const httplib::Request& req, httplib::Response& res) {
        try {
            db::Database db(dbc);
            const auto s = db.find_sensor(req.matches[1]);
            if (!s) { send_err(res, 404, "no such sensor"); return; }
            send(res, 200, to_json(*s));
        } catch (const std::exception& e) { send_err(res, 500, e.what()); }
    });
    srv.Get(R"(/api/v1/sensors/([^/]+)/readings)",
            [dbc](const httplib::Request& req, httplib::Response& res) {
                const int limit = req.has_param("limit") ? std::atoi(req.get_param_value("limit").c_str()) : 100;
                const std::string from = req.has_param("from") ? req.get_param_value("from") : "";
                const std::string to = req.has_param("to") ? req.get_param_value("to") : "";
                try {
                    db::Database db(dbc);
                    json arr = json::array();
                    for (const auto& r : db.readings_between(req.matches[1], from, to, limit)) arr.push_back(to_json(r));
                    send(res, 200, arr);
                } catch (const std::exception& e) { send_err(res, 500, e.what()); }
            });
    srv.Get("/api/v1/events", [dbc](const httplib::Request& req, httplib::Response& res) {
        int limit = 50;
        if (req.has_param("limit")) limit = std::atoi(req.get_param_value("limit").c_str());
        try {
            db::Database db(dbc);
            json arr = json::array();
            for (const auto& e : db.recent_events(limit)) arr.push_back(to_json(e));
            send(res, 200, arr);
        } catch (const std::exception& e) { send_err(res, 500, e.what()); }
    });
    srv.Get("/api/v1/weather-stations", [dbc](const httplib::Request&, httplib::Response& res) {
        try {
            db::Database db(dbc);
            json arr = json::array();
            for (const auto& w : db.weather_stations()) arr.push_back(to_json(w));
            send(res, 200, arr);
        } catch (const std::exception& e) { send_err(res, 500, e.what()); }
    });
    srv.Get(R"(/api/v1/weather-stations/([^/]+))",
            [dbc](const httplib::Request& req, httplib::Response& res) {
                try {
                    db::Database db(dbc);
                    const auto w = db.find_weather_station(req.matches[1]);
                    if (!w) { send_err(res, 404, "no such weather station"); return; }
                    send(res, 200, to_json(*w));
                } catch (const std::exception& e) { send_err(res, 500, e.what()); }
            });
    srv.Get("/api/v1/db/status", [dbc](const httplib::Request&, httplib::Response& res) {
        try {
            db::Database db(dbc);
            json arr = json::array();
            for (const auto& t : db.schema_status()) arr.push_back(to_json(t));
            send(res, 200, json{{"tables", arr}});
        } catch (const std::exception& e) { send_err(res, 500, e.what()); }
    });
    srv.Get("/api/v1/discover", [dbc](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("cidr")) { send_err(res, 400, "missing 'cidr' query parameter"); return; }
        try {
            json arr = json::array();
            for (const auto& d : sqm::discover(req.get_param_value("cidr"))) {
                arr.push_back(json{{"ip", d.ip},
                                   {"serial", d.info.serial},
                                   {"model", d.info.model},
                                   {"feature", d.info.feature},
                                   {"protocol", d.info.protocol}});
            }
            send(res, 200, arr);
        } catch (const std::exception& e) { send_err(res, 400, e.what()); }
    });

    // ---- write endpoints (token required) ----
    srv.Post("/api/v1/sensors", [dbc, token](const httplib::Request& req, httplib::Response& res) {
        if (!require_auth(req, res, token, dbc, true)) return;
        json body;
        try { body = json::parse(req.body); } catch (...) { send_err(res, 400, "invalid JSON"); return; }
        if (!body.contains("id")) { send_err(res, 400, "missing 'id'"); return; }
        const std::string id = body["id"].get<std::string>();
        db::SensorFields f = sensor_fields_from_json(body);
        if (!f.transport || !f.address) {
            send_err(res, 400, "missing transport/address (or 'tcp')");
            return;
        }
        const bool probe = !body.contains("probe") || body["probe"].get<bool>();
        if (probe) probe_fill(f);
        try {
            db::Database db(dbc);
            db.upsert_sensor(id, f);
            const auto s = db.find_sensor(id);
            send(res, 201, s ? to_json(*s) : json{{"id", id}});
        } catch (const std::exception& e) { send_err(res, 500, e.what()); }
    });
    srv.Patch(R"(/api/v1/sensors/([^/]+))", [dbc, token](const httplib::Request& req, httplib::Response& res) {
        if (!require_auth(req, res, token, dbc, true)) return;
        json body;
        try { body = json::parse(req.body); } catch (...) { send_err(res, 400, "invalid JSON"); return; }
        try {
            db::Database db(dbc);
            if (!db.update_sensor(req.matches[1], sensor_fields_from_json(body))) {
                send_err(res, 404, "no such sensor");
                return;
            }
            const auto s = db.find_sensor(req.matches[1]);
            send(res, 200, s ? to_json(*s) : json::object());
        } catch (const std::exception& e) { send_err(res, 500, e.what()); }
    });
    srv.Delete(R"(/api/v1/sensors/([^/]+)/readings)",
               [dbc, token](const httplib::Request& req, httplib::Response& res) {
                   if (!require_auth(req, res, token, dbc, true)) return;
                   if (!req.has_param("before")) {
                       send_err(res, 400, "missing 'before' (YYYY-MM-DD[ HH:MM:SS])");
                       return;
                   }
                   try {
                       db::Database db(dbc);
                       const long long n =
                           db.delete_readings_before(req.matches[1], req.get_param_value("before"));
                       send(res, 200, json{{"deleted", n}});
                   } catch (const std::exception& e) { send_err(res, 500, e.what()); }
               });
    srv.Delete(R"(/api/v1/sensors/([^/]+))", [dbc, token](const httplib::Request& req, httplib::Response& res) {
        if (!require_auth(req, res, token, dbc, true)) return;
        try {
            db::Database db(dbc);
            db.remove_sensor(req.matches[1]);
            send(res, 200, json{{"deleted", std::string(req.matches[1])}});
        } catch (const std::exception& e) { send_err(res, 500, e.what()); }
    });
    srv.Post(R"(/api/v1/sensors/([^/]+)/poll)",
             [dbc, token](const httplib::Request& req, httplib::Response& res) {
                 if (!require_auth(req, res, token, dbc, true)) return;
                 try {
                     db::Database db(dbc);
                     const auto s = db.find_sensor(req.matches[1]);
                     if (!s) { send_err(res, 404, "no such sensor"); return; }
                     if (s->transport != "tcp") { send_err(res, 400, "only tcp transport supported"); return; }
                     std::string host;
                     uint16_t port;
                     split_endpoint(s->address, host, port);
                     auto t = std::make_unique<sqm::TcpTransport>(host, port, 5000);
                     sqm::SqmDevice dev(std::move(t));
                     const sqm::Reading r = dev.read_averaged();
                     const std::string quality = (r.mag_arcsec2 <= 0.0) ? "saturated" : "ok";
                     const long long id = db.insert_reading(s->id, r, "", "poll", quality);
                     send(res, 200,
                          json{{"stored_id", id},
                               {"mag_arcsec2", r.mag_arcsec2},
                               {"temp_c", r.temp_c},
                               {"quality", quality},
                               {"raw", r.raw}});
                 } catch (const std::exception& e) { send_err(res, 502, e.what()); }
             });
    srv.Post("/api/v1/weather-stations", [dbc, token](const httplib::Request& req, httplib::Response& res) {
        if (!require_auth(req, res, token, dbc, true)) return;
        json body;
        try { body = json::parse(req.body); } catch (...) { send_err(res, 400, "invalid JSON"); return; }
        if (!body.contains("id")) { send_err(res, 400, "missing 'id'"); return; }
        try {
            db::Database db(dbc);
            const std::string id = body["id"].get<std::string>();
            db.upsert_weather_station(id, wx_fields_from_json(body));
            const auto w = db.find_weather_station(id);
            send(res, 201, w ? to_json(*w) : json{{"id", id}});
        } catch (const std::exception& e) { send_err(res, 500, e.what()); }
    });
    srv.Patch(R"(/api/v1/weather-stations/([^/]+))",
              [dbc, token](const httplib::Request& req, httplib::Response& res) {
                  if (!require_auth(req, res, token, dbc, true)) return;
                  json body;
                  try { body = json::parse(req.body); } catch (...) { send_err(res, 400, "invalid JSON"); return; }
                  try {
                      db::Database db(dbc);
                      if (!db.update_weather_station(req.matches[1], wx_fields_from_json(body))) {
                          send_err(res, 404, "no such weather station");
                          return;
                      }
                      const auto w = db.find_weather_station(req.matches[1]);
                      send(res, 200, w ? to_json(*w) : json::object());
                  } catch (const std::exception& e) { send_err(res, 500, e.what()); }
              });
    srv.Delete(R"(/api/v1/weather-stations/([^/]+))",
               [dbc, token](const httplib::Request& req, httplib::Response& res) {
                   if (!require_auth(req, res, token, dbc, true)) return;
                   try {
                       db::Database db(dbc);
                       db.remove_weather_station(req.matches[1]);
                       send(res, 200, json{{"deleted", std::string(req.matches[1])}});
                   } catch (const std::exception& e) { send_err(res, 500, e.what()); }
               });
    srv.Post("/api/v1/db/init", [dbc, token, schema_file](const httplib::Request& req, httplib::Response& res) {
        if (!require_auth(req, res, token, dbc, true)) return;
        if (schema_file.empty()) { send_err(res, 503, "schema_file not configured"); return; }
        std::ifstream in(schema_file);
        if (!in) { send_err(res, 500, "cannot open schema file: " + schema_file); return; }
        std::stringstream ss;
        ss << in.rdbuf();
        try {
            db::Database db(dbc);
            db.run_schema_script(ss.str());
            json arr = json::array();
            for (const auto& t : db.schema_status()) arr.push_back(to_json(t));
            send(res, 200, json{{"initialized", true}, {"tables", arr}});
        } catch (const std::exception& e) { send_err(res, 500, e.what()); }
    });

    // ---- authentication ----
    srv.Post("/api/v1/login", [dbc](const httplib::Request& req, httplib::Response& res) {
        json body;
        try { body = json::parse(req.body); } catch (...) { send_err(res, 400, "invalid JSON"); return; }
        if (!body.contains("username") || !body.contains("password")) {
            send_err(res, 400, "username and password required");
            return;
        }
        try {
            db::Database db(dbc);
            const auto u = db.find_user(body["username"].get<std::string>());
            if (!u || !auth::verify_password(body["password"].get<std::string>(), u->password_hash)) {
                send_err(res, 401, "invalid credentials");
                return;
            }
            const std::string sess = auth::random_hex(32);
            const int ttl = 7 * 24 * 3600;
            db.create_session(sess, u->id, ttl);
            res.set_header("Set-Cookie", "nw_session=" + sess +
                                             "; HttpOnly; SameSite=Strict; Path=/; Max-Age=" +
                                             std::to_string(ttl));
            send(res, 200,
                 json{{"token", sess},
                      {"user", {{"username", u->username}, {"role", u->role}}},
                      {"must_change_password", u->must_change_password}});
        } catch (const std::exception& e) { send_err(res, 500, e.what()); }
    });
    srv.Post("/api/v1/logout", [dbc](const httplib::Request& req, httplib::Response& res) {
        const std::string sess = session_token(req);
        try {
            if (!sess.empty()) {
                db::Database db(dbc);
                db.delete_session(sess);
            }
        } catch (const std::exception&) {
        }
        res.set_header("Set-Cookie", "nw_session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");
        send(res, 200, json{{"ok", true}});
    });
    srv.Get("/api/v1/me", [dbc, token](const httplib::Request& req, httplib::Response& res) {
        const std::string h = req.get_header_value("Authorization");
        if (!token.empty() && (h == token || h == "Bearer " + token)) {
            send(res, 200, json{{"username", "(token)"}, {"role", "admin"}, {"via", "token"}});
            return;
        }
        const std::string sess = session_token(req);
        if (!sess.empty()) {
            try {
                db::Database db(dbc);
                if (const auto si = db.validate_session(sess)) {
                    send(res, 200,
                         json{{"username", si->username}, {"role", si->role}, {"via", "session"}});
                    return;
                }
            } catch (const std::exception&) {
            }
        }
        send_err(res, 401, "not logged in");
    });
    srv.Post("/api/v1/me/password", [dbc](const httplib::Request& req, httplib::Response& res) {
        json body;
        try { body = json::parse(req.body); } catch (...) { send_err(res, 400, "invalid JSON"); return; }
        if (!body.contains("current_password") || !body.contains("new_password")) {
            send_err(res, 400, "current_password and new_password required");
            return;
        }
        const std::string sess = session_token(req);
        try {
            db::Database db(dbc);
            const auto si = sess.empty() ? std::optional<db::SessionInfo>{} : db.validate_session(sess);
            if (!si) { send_err(res, 401, "not logged in"); return; }
            const auto u = db.find_user(si->username);
            if (!u ||
                !auth::verify_password(body["current_password"].get<std::string>(), u->password_hash)) {
                send_err(res, 403, "current password is incorrect");
                return;
            }
            db.set_user_password(si->username,
                                 auth::hash_password(body["new_password"].get<std::string>()), false);
            send(res, 200, json{{"ok", true}});
        } catch (const std::exception& e) { send_err(res, 500, e.what()); }
    });
    srv.Get("/api/v1/users", [dbc, token](const httplib::Request& req, httplib::Response& res) {
        if (!require_auth(req, res, token, dbc, true)) return;
        try {
            db::Database db(dbc);
            json arr = json::array();
            for (const auto& u : db.users()) {
                arr.push_back(json{{"id", u.id},
                                   {"username", u.username},
                                   {"role", u.role},
                                   {"must_change_password", u.must_change_password},
                                   {"created_at", u.created_at}});
            }
            send(res, 200, arr);
        } catch (const std::exception& e) { send_err(res, 500, e.what()); }
    });
    srv.Post("/api/v1/users", [dbc, token](const httplib::Request& req, httplib::Response& res) {
        if (!require_auth(req, res, token, dbc, true)) return;
        json body;
        try { body = json::parse(req.body); } catch (...) { send_err(res, 400, "invalid JSON"); return; }
        if (!body.contains("username") || !body.contains("password")) {
            send_err(res, 400, "username and password required");
            return;
        }
        const std::string role = body.contains("role") ? body["role"].get<std::string>() : "viewer";
        try {
            db::Database db(dbc);
            const std::string uname = body["username"].get<std::string>();
            if (db.find_user(uname)) { send_err(res, 409, "user already exists"); return; }
            db.create_user(uname, auth::hash_password(body["password"].get<std::string>()), role, false);
            send(res, 201, json{{"username", uname}, {"role", role}});
        } catch (const std::exception& e) { send_err(res, 500, e.what()); }
    });
    srv.Delete(R"(/api/v1/users/([^/]+))", [dbc, token](const httplib::Request& req, httplib::Response& res) {
        if (!require_auth(req, res, token, dbc, true)) return;
        const std::string uname = req.matches[1];
        try {
            db::Database db(dbc);
            const auto u = db.find_user(uname);
            if (!u) { send_err(res, 404, "no such user"); return; }
            if (u->role == "admin") {
                int admins = 0;
                for (const auto& x : db.users()) {
                    if (x.role == "admin") ++admins;
                }
                if (admins <= 1) { send_err(res, 409, "cannot delete the last admin"); return; }
            }
            db.delete_user(uname);
            send(res, 200, json{{"deleted", uname}});
        } catch (const std::exception& e) { send_err(res, 500, e.what()); }
    });

    if (!srv.bind_to_port(impl_->cfg.bind.c_str(), impl_->cfg.port)) {
        throw std::runtime_error("cannot bind API to " + impl_->cfg.bind + ":" +
                                 std::to_string(impl_->cfg.port));
    }
    impl_->thread = std::thread([&srv]() { srv.listen_after_bind(); });
    log_info("API listening on http://" + impl_->cfg.bind + ":" + std::to_string(impl_->cfg.port) +
             (token.empty() ? " (writes require login)" : " (writes require login or the API token)"));
}

void HttpServer::stop() {
    if (impl_->srv.is_running()) impl_->srv.stop();
    if (impl_->thread.joinable()) impl_->thread.join();
}

}  // namespace nightwatcher
