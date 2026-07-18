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

bool require_auth(const httplib::Request& req, httplib::Response& res, const std::string& token) {
    if (token.empty()) {
        send_err(res, 503, "write operations disabled: set NW_API_TOKEN");
        return false;
    }
    const std::string h = req.get_header_value("Authorization");
    if (h == token || h == "Bearer " + token) return true;
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
                int limit = 100;
                if (req.has_param("limit")) limit = std::atoi(req.get_param_value("limit").c_str());
                try {
                    db::Database db(dbc);
                    json arr = json::array();
                    for (const auto& r : db.readings(req.matches[1], limit)) arr.push_back(to_json(r));
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
        if (!require_auth(req, res, token)) return;
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
        if (!require_auth(req, res, token)) return;
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
                   if (!require_auth(req, res, token)) return;
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
        if (!require_auth(req, res, token)) return;
        try {
            db::Database db(dbc);
            db.remove_sensor(req.matches[1]);
            send(res, 200, json{{"deleted", std::string(req.matches[1])}});
        } catch (const std::exception& e) { send_err(res, 500, e.what()); }
    });
    srv.Post(R"(/api/v1/sensors/([^/]+)/poll)",
             [dbc, token](const httplib::Request& req, httplib::Response& res) {
                 if (!require_auth(req, res, token)) return;
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
        if (!require_auth(req, res, token)) return;
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
                  if (!require_auth(req, res, token)) return;
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
                   if (!require_auth(req, res, token)) return;
                   try {
                       db::Database db(dbc);
                       db.remove_weather_station(req.matches[1]);
                       send(res, 200, json{{"deleted", std::string(req.matches[1])}});
                   } catch (const std::exception& e) { send_err(res, 500, e.what()); }
               });
    srv.Post("/api/v1/db/init", [dbc, token, schema_file](const httplib::Request& req, httplib::Response& res) {
        if (!require_auth(req, res, token)) return;
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

    if (!srv.bind_to_port(impl_->cfg.bind.c_str(), impl_->cfg.port)) {
        throw std::runtime_error("cannot bind API to " + impl_->cfg.bind + ":" +
                                 std::to_string(impl_->cfg.port));
    }
    impl_->thread = std::thread([&srv]() { srv.listen_after_bind(); });
    log_info("API listening on http://" + impl_->cfg.bind + ":" + std::to_string(impl_->cfg.port) +
             (token.empty() ? " (writes disabled: NW_API_TOKEN unset)" : " (token auth on writes)"));
}

void HttpServer::stop() {
    if (impl_->srv.is_running()) impl_->srv.stop();
    if (impl_->thread.joinable()) impl_->thread.join();
}

}  // namespace nightwatcher
