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

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include "discovery.hpp"
#include "export_runner.hpp"
#include "exporter.hpp"
#include "httplib.h"
#include "logging.hpp"
#include "nightwatcher/version.hpp"
#include "nlohmann/json.hpp"
#include "password.hpp"
#include "provider.hpp"
#include "device_factory.hpp"
#include "sqm_device.hpp"

using json = nlohmann::json;

namespace nightwatcher {

struct HttpServer::Impl {
    ApiConfig cfg;
    // Either a plain httplib::Server or an SSLServer, chosen at start() from the
    // TLS config. Held by base pointer so the route setup below is identical.
    std::unique_ptr<httplib::Server> srv;
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
json cal_json(const sqm::Calibration& c) {
    return json{{"light_cal_offset", c.light_cal_offset},
                {"dark_cal_period_s", c.dark_cal_period_s},
                {"temp_light_c", c.temp_light_c},
                {"sensor_offset", c.sensor_offset},
                {"temp_dark_c", c.temp_dark_c},
                {"raw", c.raw}};
}
json calstatus_json(const sqm::CalStatus& s) {
    return json{{"mode", std::string(1, s.mode)}, {"armed", s.armed},
                {"locked", s.locked}, {"raw", s.raw}};
}
json configlog_json(const db::ConfigLogRow& r) {
    return json{{"id", r.id}, {"ts_utc", r.ts_utc}, {"event_type", r.event_type},
                {"light_cal_offset", j_opt(r.light_cal_offset)},
                {"dark_cal_period_s", j_opt(r.dark_cal_period_s)},
                {"temp_light_c", j_opt(r.temp_light_c)}, {"sensor_offset", j_opt(r.sensor_offset)},
                {"temp_dark_c", j_opt(r.temp_dark_c)}, {"interval_s", j_opt(r.interval_s)},
                {"threshold", j_opt(r.threshold)}, {"raw", r.raw},
                {"changed_by", r.changed_by}, {"note", r.note}};
}
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
bool is_secret_key(const std::string& k) {
    std::string lk;
    for (const char c : k) lk += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lk.find("key") != std::string::npos || lk.find("secret") != std::string::npos ||
           lk.find("password") != std::string::npos || lk.find("token") != std::string::npos;
}

// A syntactically acceptable listen address (IPv4/IPv6/hostname, or 0.0.0.0/::).
// A valid-but-unbindable value is caught at startup, which falls back to localhost.
bool valid_bind(const std::string& b) {
    if (b.empty() || b.size() > 64) return false;
    for (const char c : b)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == ':' || c == '-' ||
              c == '_'))
            return false;
    return true;
}

// Recursively redact secret string values (matched by key name), descending into
// nested objects (e.g. an export target's config.auth.private_key).
json mask_config_json(const json& c) {
    if (!c.is_object()) return c;
    json out = json::object();
    for (auto& item : c.items()) {
        if (item.value().is_object()) {
            out[item.key()] = mask_config_json(item.value());
        } else if (is_secret_key(item.key()) && item.value().is_string() &&
                   !item.value().get<std::string>().empty()) {
            out[item.key()] = "***";
        } else {
            out[item.key()] = item.value();
        }
    }
    return out;
}

// Config with secret values redacted, for serving to clients.
json mask_config(const std::string& config_json) {
    if (config_json.empty()) return json::object();
    json c;
    try { c = json::parse(config_json); } catch (...) { return json::object(); }
    if (!c.is_object()) return json::object();
    return mask_config_json(c);
}

// Recursively merge incoming into existing, ignoring blank/masked ("***") secret
// values so secrets survive edits, and descending into nested objects.
json merge_config_json(json existing, const json& incoming) {
    if (!existing.is_object()) existing = json::object();
    if (!incoming.is_object()) return existing;
    for (const auto& item : incoming.items()) {
        const std::string& k = item.key();
        const json& v = item.value();
        if (v.is_object()) {
            existing[k] = merge_config_json(existing.contains(k) ? existing[k] : json::object(), v);
        } else if (is_secret_key(k) && v.is_string()) {
            const std::string s = v.get<std::string>();
            if (s.empty() || s == "***") continue;  // keep the existing secret
            existing[k] = v;
        } else {
            existing[k] = v;
        }
    }
    return existing;
}

// Merge incoming config into existing, ignoring blank/masked secret values so
// secrets aren't wiped when only other fields are edited.
std::string merge_config(const std::string& existing_json, const json& incoming) {
    json c;
    try { c = existing_json.empty() ? json::object() : json::parse(existing_json); }
    catch (...) { c = json::object(); }
    if (!c.is_object()) c = json::object();
    return merge_config_json(c, incoming).dump();
}

// Download file name for an ad-hoc DSN export over a date range:
// "<site|id>_<from-date>_<to-date>.dat", sanitized for a Content-Disposition header.
std::string dsn_download_name(const std::string& id, const std::string& config,
                              const std::string& from, const std::string& to) {
    std::string site = id;
    if (!config.empty()) {
        try {
            const json c = json::parse(config);
            if (c.contains("site_id") && c["site_id"].is_string() &&
                !c["site_id"].get<std::string>().empty())
                site = c["site_id"].get<std::string>();
        } catch (...) { /* fall back to id */ }
    }
    const std::string fd = from.size() >= 10 ? from.substr(0, 10) : "start";
    const std::string td = to.size() >= 10 ? to.substr(0, 10) : "end";
    std::string out;
    for (const char ch : site + "_" + fd + "_" + td + ".dat")
        out += (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' || ch == '.')
                   ? ch : '_';
    return out;
}

json to_json(const db::WeatherStationRow& w) {
    return json{{"id", w.id},
                {"name", w.name},
                {"site", w.site},
                {"model", w.model},
                {"provider", w.provider},
                {"config", mask_config(w.config)},
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

json to_json(const db::WeatherReadingRow& r) {
    return json{{"id", r.id}, {"station_id", r.station_id}, {"ts_utc", r.ts_utc},
                {"temp_c", j_opt(r.temp_c)}, {"humidity_pct", j_opt(r.humidity_pct)},
                {"dew_point_c", j_opt(r.dew_point_c)}, {"pressure_hpa", j_opt(r.pressure_hpa)},
                {"pressure_abs_hpa", j_opt(r.pressure_abs_hpa)},
                {"wind_speed_ms", j_opt(r.wind_speed_ms)}, {"wind_gust_ms", j_opt(r.wind_gust_ms)},
                {"wind_dir_deg", j_opt(r.wind_dir_deg)}, {"rain_rate_mmh", j_opt(r.rain_rate_mmh)},
                {"rain_daily_mm", j_opt(r.rain_daily_mm)}, {"uv_index", j_opt(r.uv_index)},
                {"solar_wm2", j_opt(r.solar_wm2)}, {"cloud_cover_pct", j_opt(r.cloud_cover_pct)},
                {"source", r.source}};
}
json to_json(const db::TableCount& t) {
    return json{{"table", t.table}, {"present", t.present}, {"rows", t.rows}};
}
json to_json(const db::EventRow& e) {
    return json{{"id", e.id},       {"ts_utc", e.ts_utc}, {"device_id", e.device_id},
                {"source", e.source}, {"level", e.level},  {"event", e.event},
                {"detail", e.detail}};
}
json to_json(const db::ExportTargetRow& t) {
    return json{{"id", t.id},
                {"sensor_id", t.sensor_id},
                {"name", t.name},
                {"target", t.target},
                {"config", mask_config(t.config)},
                {"schedule", t.schedule},
                {"schedule_time", t.schedule_time},
                {"interval_s", j_opt(t.interval_s)},
                {"last_export_ts", t.last_export_ts},
                {"status", t.status},
                {"notes", t.notes},
                {"created_at", t.created_at}};
}
json to_json(const db::ExportLogRow& l) {
    return json{{"id", l.id},          {"target_id", l.target_id}, {"ts_utc", l.ts_utc},
                {"from_ts", l.from_ts}, {"to_ts", l.to_ts},         {"row_count", l.row_count},
                {"file_name", l.file_name}, {"remote_id", l.remote_id}, {"status", l.status},
                {"detail", l.detail}};
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
    if (b.contains("serial") && !b["serial"].is_null()) {
        f.transport = std::string("serial");
        f.address = b["serial"].get<std::string>();
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
    s("name", f.name); s("site", f.site); s("model", f.model); s("provider", f.provider);
    s("transport", f.transport); s("address", f.address);
    d("latitude", f.latitude); d("longitude", f.longitude); d("elevation_m", f.elevation_m);
    s("timezone", f.timezone); i("poll_interval_s", f.poll_interval_s);
    s("status", f.status); s("installed_at", f.installed_at); s("notes", f.notes);
    return f;
}

db::ExportTargetFields export_fields_from_json(const json& b) {
    db::ExportTargetFields f;
    const auto s = [&](const char* k, std::optional<std::string>& o) {
        if (b.contains(k) && !b[k].is_null()) o = b[k].get<std::string>();
    };
    const auto i = [&](const char* k, std::optional<int>& o) {
        if (b.contains(k) && !b[k].is_null()) o = b[k].get<int>();
    };
    s("sensor_id", f.sensor_id); s("name", f.name); s("target", f.target);
    s("schedule", f.schedule); s("schedule_time", f.schedule_time);
    i("interval_s", f.interval_s); s("status", f.status); s("notes", f.notes);
    return f;
}

// Best-effort fill of serial/protocol/feature from the device's ix response.
void probe_fill(db::SensorFields& f) {
    if (!(f.transport && f.address)) return;
    try {
        auto dev = sqm::open_device(*f.transport, *f.address, 3000);
        const sqm::UnitInfo u = dev->info();
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

// Best-effort identity of the caller, for audit fields (changed_by).
std::string auth_user(const httplib::Request& req, const std::string& token,
                      const db::DbConfig& dbc) {
    const std::string h = req.get_header_value("Authorization");
    if (!token.empty() && (h == token || h == "Bearer " + token)) return "api-token";
    const std::string sess = session_token(req);
    if (!sess.empty()) {
        try {
            db::Database db(dbc);
            if (const auto si = db.validate_session(sess)) return si->username;
        } catch (const std::exception&) {
        }
    }
    return "";
}

}  // namespace

HttpServer::HttpServer(ApiConfig cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::start() {
    const bool run_tls = !impl_->cfg.tls_cert.empty() && !impl_->cfg.tls_key.empty();
    if (run_tls) {
        auto ssl = std::make_unique<httplib::SSLServer>(impl_->cfg.tls_cert.c_str(),
                                                        impl_->cfg.tls_key.c_str());
        if (!ssl->is_valid()) {
            throw std::runtime_error("TLS certificate/key invalid or unreadable (" +
                                     impl_->cfg.tls_cert + ", " + impl_->cfg.tls_key + ")");
        }
        impl_->srv = std::move(ssl);
    } else {
        impl_->srv = std::make_unique<httplib::Server>();
    }
    httplib::Server& srv = *impl_->srv;
    const db::DbConfig dbc = impl_->cfg.db;
    const std::string token = impl_->cfg.token;
    const std::string schema_file = impl_->cfg.schema_file;
    const std::string run_bind = impl_->cfg.bind;  // what this process actually bound
    const int run_port = impl_->cfg.port;
    const std::function<void()> on_apply = impl_->cfg.on_apply;
    const std::function<void()> on_reload = impl_->cfg.on_reload;

    if (!impl_->cfg.web_root.empty()) {
        srv.set_mount_point("/", impl_->cfg.web_root);
    }

    // Security gate: when the server is exposed off localhost the daemon sets
    // require_auth_reads, so every API request must carry a session or token —
    // not just the mutating ones (those stay gated in their own handlers). The
    // static web shell and the sign-in / liveness endpoints stay public so the
    // login page can load and users can authenticate.
    if (impl_->cfg.require_auth_reads) {
        srv.set_pre_routing_handler(
            [dbc, token](const httplib::Request& req, httplib::Response& res) {
                using HR = httplib::Server::HandlerResponse;
                if (req.path.rfind("/api/v1/", 0) != 0) return HR::Unhandled;  // static UI shell
                if (req.path == "/api/v1/login" || req.path == "/api/v1/logout" ||
                    req.path == "/api/v1/me" || req.path == "/api/v1/version" ||
                    req.path == "/api/v1/health")
                    return HR::Unhandled;  // public: sign-in + liveness
                if (!require_auth(req, res, token, dbc, false)) return HR::Handled;  // 401 already sent
                return HR::Unhandled;
            });
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

    // Web-server settings (bind/port). Admin-only. Changes are stored and applied
    // when the daemon restarts; the response reports both the running and the
    // configured values so the UI can prompt for a restart.
    const auto settings_json = [run_bind, run_port, run_tls](db::Database& db) {
        std::string cbind = run_bind;
        int cport = run_port;
        bool ctls = run_tls;
        if (auto v = db.get_setting("api_bind"); v && !v->empty()) cbind = *v;
        if (auto v = db.get_setting("api_port"); v && !v->empty()) cport = std::atoi(v->c_str());
        if (auto v = db.get_setting("api_tls"); v) ctls = (*v == "on");
        return json{{"running", {{"bind", run_bind}, {"port", run_port}, {"tls", run_tls}}},
                    {"configured", {{"bind", cbind}, {"port", cport}, {"tls", ctls}}},
                    {"restart_required", (cbind != run_bind || cport != run_port || ctls != run_tls)}};
    };
    srv.Get("/api/v1/settings",
            [dbc, token, settings_json](const httplib::Request& req, httplib::Response& res) {
                if (!require_auth(req, res, token, dbc, true)) return;
                try {
                    db::Database db(dbc);
                    send(res, 200, settings_json(db));
                } catch (const std::exception& e) { send_err(res, 500, e.what()); }
            });
    srv.Put("/api/v1/settings",
            [dbc, token, settings_json](const httplib::Request& req, httplib::Response& res) {
                if (!require_auth(req, res, token, dbc, true)) return;
                json body;
                try { body = json::parse(req.body); } catch (...) { send_err(res, 400, "invalid JSON"); return; }
                try {
                    db::Database db(dbc);
                    if (body.contains("bind") && body["bind"].is_string()) {
                        const std::string b = body["bind"].get<std::string>();
                        if (!valid_bind(b)) { send_err(res, 400, "invalid bind address"); return; }
                        db.set_setting("api_bind", b);
                    }
                    if (body.contains("port")) {
                        int p = 0;
                        if (body["port"].is_number()) p = body["port"].get<int>();
                        else if (body["port"].is_string()) p = std::atoi(body["port"].get<std::string>().c_str());
                        if (p < 1 || p > 65535) { send_err(res, 400, "port out of range (1-65535)"); return; }
                        db.set_setting("api_port", std::to_string(p));
                    }
                    if (body.contains("tls")) {
                        bool on = false;
                        if (body["tls"].is_boolean()) on = body["tls"].get<bool>();
                        else if (body["tls"].is_string()) {
                            const std::string s = body["tls"].get<std::string>();
                            on = (s == "on" || s == "true" || s == "1" || s == "yes");
                        }
                        db.set_setting("api_tls", on ? "on" : "off");
                    }
                    send(res, 200, settings_json(db));
                } catch (const std::exception& e) { send_err(res, 500, e.what()); }
            });
    // Apply the configured bind/port now by asking the daemon to restart the
    // server (the response is sent first; on_apply is a no-op if unset).
    srv.Post("/api/v1/settings/apply",
             [dbc, token, settings_json, on_apply](const httplib::Request& req, httplib::Response& res) {
                 if (!require_auth(req, res, token, dbc, true)) return;
                 try {
                     db::Database db(dbc);
                     json j = settings_json(db);
                     j["applying"] = static_cast<bool>(on_apply);
                     send(res, 200, j);
                 } catch (const std::exception& e) { send_err(res, 500, e.what()); return; }
                 if (on_apply) on_apply();
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
    // Download a DSN community-format .dat for a sensor over a date range.
    srv.Get(R"(/api/v1/sensors/([^/]+)/dsn)",
            [dbc](const httplib::Request& req, httplib::Response& res) {
                const std::string from = req.has_param("from") ? req.get_param_value("from") : "";
                const std::string to = req.has_param("to") ? req.get_param_value("to") : "";
                try {
                    db::Database db(dbc);
                    const std::string id = req.matches[1];
                    const auto s = db.find_sensor(id);
                    if (!s) { send_err(res, 404, "no such sensor"); return; }
                    auto rows = db.readings_between(id, from, to, 500000);
                    std::reverse(rows.begin(), rows.end());  // the .dat is oldest-first
                    // Reuse a DSN export target's config (site_id + header) for this sensor, if any.
                    std::string config;
                    for (const auto& t : db.export_targets())
                        if (t.target == "dsn" && t.sensor_id == id) { config = t.config; break; }
                    exporter::ExportContext ctx;
                    ctx.sensor = *s;
                    ctx.readings = std::move(rows);
                    ctx.from_ts = from;
                    ctx.to_ts = to;
                    const std::string content = exporter::format_dsn_dat(ctx, config);
                    res.set_header("Content-Disposition", "attachment; filename=\"" +
                                                              dsn_download_name(id, config, from, to) + "\"");
                    res.set_content(content, "text/plain");
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
    // Discover SQM-LU units on the local serial/USB bus (probes serial ports).
    srv.Get("/api/v1/discover/usb", [](const httplib::Request&, httplib::Response& res) {
        try {
            json arr = json::array();
            for (const auto& d : sqm::discover_serial()) {
                arr.push_back(json{{"device", d.device},
                                   {"serial", d.info.serial},
                                   {"model", d.info.model},
                                   {"feature", d.info.feature},
                                   {"protocol", d.info.protocol}});
            }
            send(res, 200, arr);
        } catch (const std::exception& e) { send_err(res, 500, e.what()); }
    });

    // ---- write endpoints (token required) ----
    srv.Post("/api/v1/sensors", [dbc, token](const httplib::Request& req, httplib::Response& res) {
        if (!require_auth(req, res, token, dbc, true)) return;
        json body;
        try { body = json::parse(req.body); } catch (...) { send_err(res, 400, "invalid JSON"); return; }
        if (!body.contains("id")) { send_err(res, 400, "missing 'id'"); return; }
        const std::string id = body["id"].get<std::string>();
        db::SensorFields f = sensor_fields_from_json(body);
        // New sensors start disabled (not polled, no rows written) so they can be
        // verified before database population is enabled. An explicit status in
        // the request still wins.
        if (!f.status) f.status = "inactive";
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
    srv.Delete(R"(/api/v1/sensors/([^/]+))",
               [dbc, token, on_reload](const httplib::Request& req, httplib::Response& res) {
                   if (!require_auth(req, res, token, dbc, true)) return;
                   try {
                       db::Database db(dbc);
                       db.remove_sensor(req.matches[1]);
                       send(res, 200, json{{"deleted", std::string(req.matches[1])}});
                   } catch (const std::exception& e) { send_err(res, 500, e.what()); return; }
                   if (on_reload) on_reload();  // stop polling a now-deleted sensor
               });
    srv.Post(R"(/api/v1/sensors/([^/]+)/poll)",
             [dbc, token](const httplib::Request& req, httplib::Response& res) {
                 if (!require_auth(req, res, token, dbc, true)) return;
                 try {
                     db::Database db(dbc);
                     const auto s = db.find_sensor(req.matches[1]);
                     if (!s) { send_err(res, 404, "no such sensor"); return; }
                     auto dev = sqm::open_device(s->transport, s->address, 5000);
                     const sqm::Reading r = dev->read_averaged();
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
    // Non-persisting self-test: exercise every required device function
    // (connect -> identify/ix -> reading/rx -> calibration/cx) and report each
    // result WITHOUT writing to the readings table. This lets an operator verify
    // a sensor works before enabling database population from it.
    srv.Post(R"(/api/v1/sensors/([^/]+)/test)",
             [dbc, token](const httplib::Request& req, httplib::Response& res) {
                 if (!require_auth(req, res, token, dbc, true)) return;
                 std::optional<db::SensorRow> s;
                 try {
                     db::Database db(dbc);
                     s = db.find_sensor(req.matches[1]);
                 } catch (const std::exception& e) { send_err(res, 500, e.what()); return; }
                 if (!s) { send_err(res, 404, "no such sensor"); return; }

                 json checks = json::array();
                 bool overall = true;

                 // 1) Connect/open the transport (throws if unreachable or busy).
                 std::unique_ptr<sqm::SqmDevice> dev;
                 try {
                     dev = sqm::open_device(s->transport, s->address, 5000);
                     checks.push_back({{"name", "connect"}, {"ok", true},
                                       {"detail", s->transport + " " + s->address}});
                 } catch (const std::exception& e) {
                     checks.push_back({{"name", "connect"}, {"ok", false}, {"detail", e.what()}});
                     send(res, 200,
                          json{{"id", s->id}, {"address", s->address}, {"ok", false}, {"checks", checks}});
                     return;  // nothing else is possible without a connection
                 }

                 // 2) Identify (ix): confirm it speaks the SQM protocol; flag a
                 //    serial mismatch against the registered unit if we have one.
                 try {
                     const sqm::UnitInfo u = dev->info();
                     json c{{"name", "identify"}, {"ok", true}, {"protocol", u.protocol},
                            {"model", u.model},   {"feature", u.feature}, {"serial", u.serial},
                            {"raw", u.raw}};
                     if (!s->serial_number.empty()) c["serial_match"] = (u.serial == s->serial_number);
                     checks.push_back(c);
                 } catch (const std::exception& e) {
                     checks.push_back({{"name", "identify"}, {"ok", false}, {"detail", e.what()}});
                     overall = false;
                 }

                 // 3) Reading (rx): a live measurement + quality (not persisted).
                 try {
                     const sqm::Reading r = dev->read_averaged();
                     const std::string quality = (r.mag_arcsec2 <= 0.0) ? "saturated" : "ok";
                     checks.push_back({{"name", "reading"}, {"ok", true}, {"mag_arcsec2", r.mag_arcsec2},
                                       {"freq_hz", r.freq_hz}, {"temp_c", r.temp_c},
                                       {"quality", quality}, {"raw", r.raw}});
                 } catch (const std::exception& e) {
                     checks.push_back({{"name", "reading"}, {"ok", false}, {"detail", e.what()}});
                     overall = false;
                 }

                 // 4) Calibration (cx): confirm the cal read-out responds.
                 try {
                     const sqm::Calibration cal = dev->calibration();
                     checks.push_back({{"name", "calibration"}, {"ok", true},
                                       {"light_cal_offset", cal.light_cal_offset},
                                       {"dark_cal_period_s", cal.dark_cal_period_s},
                                       {"temp_light_c", cal.temp_light_c},
                                       {"sensor_offset", cal.sensor_offset},
                                       {"temp_dark_c", cal.temp_dark_c}, {"raw", cal.raw}});
                 } catch (const std::exception& e) {
                     checks.push_back({{"name", "calibration"}, {"ok", false}, {"detail", e.what()}});
                     overall = false;
                 }

                 send(res, 200,
                      json{{"id", s->id}, {"address", s->address}, {"ok", overall}, {"checks", checks}});
             });
    // Enable/disable database population for a sensor: flip status active<->
    // inactive and ask the daemon to reload so the change takes effect at once.
    const auto set_status = [dbc, token, on_reload](const httplib::Request& req,
                                                    httplib::Response& res, const char* status) {
        if (!require_auth(req, res, token, dbc, true)) return;
        try {
            db::Database db(dbc);
            db::SensorFields f;
            f.status = status;
            if (!db.update_sensor(req.matches[1], f)) { send_err(res, 404, "no such sensor"); return; }
            const auto s = db.find_sensor(req.matches[1]);
            send(res, 200, s ? to_json(*s) : json::object());
        } catch (const std::exception& e) { send_err(res, 500, e.what()); return; }
        if (on_reload) on_reload();  // daemon re-reads active_sensors() and starts/stops polling
    };
    srv.Post(R"(/api/v1/sensors/([^/]+)/enable)",
             [set_status](const httplib::Request& req, httplib::Response& res) {
                 set_status(req, res, "active");
             });
    srv.Post(R"(/api/v1/sensors/([^/]+)/disable)",
             [set_status](const httplib::Request& req, httplib::Response& res) {
                 set_status(req, res, "inactive");
             });

    // ---- calibration ----
    // Live calibration read (cx). Read-only on the device.
    srv.Get(R"(/api/v1/sensors/([^/]+)/calibration)",
            [dbc](const httplib::Request& req, httplib::Response& res) {
                try {
                    db::Database db(dbc);
                    const auto s = db.find_sensor(req.matches[1]);
                    if (!s) { send_err(res, 404, "no such sensor"); return; }
                    auto dev = sqm::open_device(s->transport, s->address, 5000);
                    send(res, 200, cal_json(dev->calibration()));
                } catch (const std::exception& e) { send_err(res, 502, e.what()); }
            });
    // Calibration/config history from config_log.
    srv.Get(R"(/api/v1/sensors/([^/]+)/calibration/history)",
            [dbc](const httplib::Request& req, httplib::Response& res) {
                try {
                    db::Database db(dbc);
                    const int limit =
                        req.has_param("limit") ? std::atoi(req.get_param_value("limit").c_str()) : 50;
                    json arr = json::array();
                    for (const auto& r : db.config_log(req.matches[1], limit))
                        arr.push_back(configlog_json(r));
                    send(res, 200, arr);
                } catch (const std::exception& e) { send_err(res, 500, e.what()); }
            });
    // Read cx and store a snapshot to config_log (audit).
    srv.Post(R"(/api/v1/sensors/([^/]+)/calibration/record)",
             [dbc, token](const httplib::Request& req, httplib::Response& res) {
                 if (!require_auth(req, res, token, dbc, true)) return;
                 try {
                     db::Database db(dbc);
                     const auto s = db.find_sensor(req.matches[1]);
                     if (!s) { send_err(res, 404, "no such sensor"); return; }
                     auto dev = sqm::open_device(s->transport, s->address, 5000);
                     const sqm::Calibration c = dev->calibration();
                     db.insert_calibration(s->id, c, "", "calibration", auth_user(req, token, dbc),
                                           "recorded snapshot");
                     send(res, 200, cal_json(c));
                 } catch (const std::exception& e) { send_err(res, 502, e.what()); }
             });
    // Arm a calibration mode {mode: "light"|"dark"}. The physical unlock switch
    // then triggers the actual measurement.
    srv.Post(R"(/api/v1/sensors/([^/]+)/calibration/arm)",
             [dbc, token](const httplib::Request& req, httplib::Response& res) {
                 if (!require_auth(req, res, token, dbc, true)) return;
                 json body;
                 try { body = json::parse(req.body); } catch (...) { send_err(res, 400, "invalid JSON"); return; }
                 const std::string mode = body.value("mode", "");
                 if (mode != "light" && mode != "dark") {
                     send_err(res, 400, "mode must be 'light' or 'dark'");
                     return;
                 }
                 try {
                     db::Database db(dbc);
                     const auto s = db.find_sensor(req.matches[1]);
                     if (!s) { send_err(res, 404, "no such sensor"); return; }
                     auto dev = sqm::open_device(s->transport, s->address, 5000);
                     const sqm::CalStatus st =
                         (mode == "light") ? dev->arm_light_calibration() : dev->arm_dark_calibration();
                     db.insert_event("sqm", "info", "calibration_arm", mode, s->id);
                     send(res, 200, calstatus_json(st));
                 } catch (const std::exception& e) { send_err(res, 502, e.what()); }
             });
    // Disarm all calibration modes; reports the lock-switch status.
    srv.Post(R"(/api/v1/sensors/([^/]+)/calibration/disarm)",
             [dbc, token](const httplib::Request& req, httplib::Response& res) {
                 if (!require_auth(req, res, token, dbc, true)) return;
                 try {
                     db::Database db(dbc);
                     const auto s = db.find_sensor(req.matches[1]);
                     if (!s) { send_err(res, 404, "no such sensor"); return; }
                     auto dev = sqm::open_device(s->transport, s->address, 5000);
                     send(res, 200, calstatus_json(dev->disarm_calibration()));
                 } catch (const std::exception& e) { send_err(res, 502, e.what()); }
             });
    // Manually write calibration values {light_offset?, light_temp?, dark_period?,
    // dark_temp?}, then re-read cx and record the change. This modifies the unit.
    srv.Post(R"(/api/v1/sensors/([^/]+)/calibration/set)",
             [dbc, token](const httplib::Request& req, httplib::Response& res) {
                 if (!require_auth(req, res, token, dbc, true)) return;
                 json body;
                 try { body = json::parse(req.body); } catch (...) { send_err(res, 400, "invalid JSON"); return; }
                 try {
                     db::Database db(dbc);
                     const auto s = db.find_sensor(req.matches[1]);
                     if (!s) { send_err(res, 404, "no such sensor"); return; }
                     auto dev = sqm::open_device(s->transport, s->address, 5000);
                     std::string note = "manual set:";
                     const auto apply = [&](const char* key, auto setter) {
                         if (body.contains(key) && body[key].is_number()) {
                             (dev.get()->*setter)(body[key].get<double>());
                             note += std::string(" ") + key;
                         }
                     };
                     apply("light_offset", &sqm::SqmDevice::set_light_offset);
                     apply("light_temp", &sqm::SqmDevice::set_light_temp);
                     apply("dark_period", &sqm::SqmDevice::set_dark_period);
                     apply("dark_temp", &sqm::SqmDevice::set_dark_temp);
                     if (note == "manual set:") { send_err(res, 400, "no calibration fields to set"); return; }
                     const sqm::Calibration c = dev->calibration();  // updated state
                     db.insert_calibration(s->id, c, "", "config_change", auth_user(req, token, dbc), note);
                     send(res, 200, cal_json(c));
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
            db::WeatherStationFields f = wx_fields_from_json(body);
            if (body.contains("config") && body["config"].is_object()) f.config = body["config"].dump();
            db.upsert_weather_station(id, f);
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
                      db::WeatherStationFields f = wx_fields_from_json(body);
                      if (body.contains("config")) {
                          const auto existing = db.find_weather_station(req.matches[1]);
                          if (!existing) { send_err(res, 404, "no such weather station"); return; }
                          f.config = merge_config(existing->config, body["config"]);
                      }
                      if (!db.update_weather_station(req.matches[1], f)) {
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
    srv.Get(R"(/api/v1/weather-stations/([^/]+)/readings)",
            [dbc](const httplib::Request& req, httplib::Response& res) {
                const int limit = req.has_param("limit") ? std::atoi(req.get_param_value("limit").c_str()) : 100;
                const std::string from = req.has_param("from") ? req.get_param_value("from") : "";
                const std::string to = req.has_param("to") ? req.get_param_value("to") : "";
                try {
                    db::Database db(dbc);
                    json arr = json::array();
                    for (const auto& r : db.weather_readings_between(req.matches[1], from, to, limit))
                        arr.push_back(to_json(r));
                    send(res, 200, arr);
                } catch (const std::exception& e) { send_err(res, 500, e.what()); }
            });
    srv.Post(R"(/api/v1/weather-stations/([^/]+)/poll)",
             [dbc, token](const httplib::Request& req, httplib::Response& res) {
                 if (!require_auth(req, res, token, dbc, true)) return;
                 try {
                     db::Database db(dbc);
                     const auto w = db.find_weather_station(req.matches[1]);
                     if (!w) { send_err(res, 404, "no such weather station"); return; }
                     if (w->provider.empty()) { send_err(res, 400, "no provider configured"); return; }
                     auto provider = weather::make_provider(w->provider, w->config);
                     db::WeatherReadingRow r = provider->fetch();
                     r.station_id = w->id;
                     const long long id = db.insert_weather_reading(r);
                     json j = to_json(r);
                     j["stored_id"] = id;
                     send(res, 200, j);
                 } catch (const std::exception& e) { send_err(res, 502, e.what()); }
             });
    // ---- export targets (DSN / Globe at Night) ----
    srv.Get("/api/v1/export-targets", [dbc](const httplib::Request&, httplib::Response& res) {
        try {
            db::Database db(dbc);
            json arr = json::array();
            for (const auto& t : db.export_targets()) arr.push_back(to_json(t));
            send(res, 200, arr);
        } catch (const std::exception& e) { send_err(res, 500, e.what()); }
    });
    srv.Get(R"(/api/v1/export-targets/([^/]+)/log)",
            [dbc](const httplib::Request& req, httplib::Response& res) {
                const int limit = req.has_param("limit") ? std::atoi(req.get_param_value("limit").c_str()) : 50;
                try {
                    db::Database db(dbc);
                    json arr = json::array();
                    for (const auto& l : db.export_log(req.matches[1], limit)) arr.push_back(to_json(l));
                    send(res, 200, arr);
                } catch (const std::exception& e) { send_err(res, 500, e.what()); }
            });
    srv.Get(R"(/api/v1/export-targets/([^/]+))",
            [dbc](const httplib::Request& req, httplib::Response& res) {
                try {
                    db::Database db(dbc);
                    const auto t = db.find_export_target(req.matches[1]);
                    if (!t) { send_err(res, 404, "no such export target"); return; }
                    send(res, 200, to_json(*t));
                } catch (const std::exception& e) { send_err(res, 500, e.what()); }
            });
    srv.Post("/api/v1/export-targets", [dbc, token, on_reload](const httplib::Request& req, httplib::Response& res) {
        if (!require_auth(req, res, token, dbc, true)) return;
        json body;
        try { body = json::parse(req.body); } catch (...) { send_err(res, 400, "invalid JSON"); return; }
        if (!body.contains("id")) { send_err(res, 400, "missing 'id'"); return; }
        try {
            db::Database db(dbc);
            const std::string id = body["id"].get<std::string>();
            db::ExportTargetFields f = export_fields_from_json(body);
            if (!f.sensor_id) { send_err(res, 400, "missing 'sensor_id'"); return; }
            if (!f.target) { send_err(res, 400, "missing 'target'"); return; }
            if (body.contains("config") && body["config"].is_object()) f.config = body["config"].dump();
            db.upsert_export_target(id, f);
            if (on_reload) on_reload();  // (de)schedule the target at once, like sensor changes
            const auto t = db.find_export_target(id);
            send(res, 201, t ? to_json(*t) : json{{"id", id}});
        } catch (const std::exception& e) { send_err(res, 500, e.what()); }
    });
    srv.Patch(R"(/api/v1/export-targets/([^/]+))",
              [dbc, token, on_reload](const httplib::Request& req, httplib::Response& res) {
                  if (!require_auth(req, res, token, dbc, true)) return;
                  json body;
                  try { body = json::parse(req.body); } catch (...) { send_err(res, 400, "invalid JSON"); return; }
                  try {
                      db::Database db(dbc);
                      db::ExportTargetFields f = export_fields_from_json(body);
                      if (body.contains("config")) {
                          const auto existing = db.find_export_target(req.matches[1]);
                          if (!existing) { send_err(res, 404, "no such export target"); return; }
                          f.config = merge_config(existing->config, body["config"]);
                      }
                      if (!db.update_export_target(req.matches[1], f)) {
                          send_err(res, 404, "no such export target");
                          return;
                      }
                      if (on_reload) on_reload();  // apply schedule/config/status change at once
                      const auto t = db.find_export_target(req.matches[1]);
                      send(res, 200, t ? to_json(*t) : json::object());
                  } catch (const std::exception& e) { send_err(res, 500, e.what()); }
              });
    srv.Delete(R"(/api/v1/export-targets/([^/]+))",
               [dbc, token, on_reload](const httplib::Request& req, httplib::Response& res) {
                   if (!require_auth(req, res, token, dbc, true)) return;
                   try {
                       db::Database db(dbc);
                       db.remove_export_target(req.matches[1]);
                       if (on_reload) on_reload();  // stop the worker for a now-deleted target
                       send(res, 200, json{{"deleted", std::string(req.matches[1])}});
                   } catch (const std::exception& e) { send_err(res, 500, e.what()); }
               });
    srv.Post(R"(/api/v1/export-targets/([^/]+)/run)",
             [dbc, token](const httplib::Request& req, httplib::Response& res) {
                 if (!require_auth(req, res, token, dbc, true)) return;
                 try {
                     db::Database db(dbc);
                     const auto t = db.find_export_target(req.matches[1]);
                     if (!t) { send_err(res, 404, "no such export target"); return; }
                     const auto r = exporter::run_export(db, *t);
                     send(res, 200,
                          json{{"files", r.files}, {"rows", r.rows}, {"last_ts", r.last_ts},
                               {"file_names", r.file_names}});
                 } catch (const std::exception& e) { send_err(res, 502, e.what()); }
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
    log_info("API listening on " + std::string(run_tls ? "https://" : "http://") + impl_->cfg.bind +
             ":" + std::to_string(impl_->cfg.port) +
             (token.empty() ? " (writes require login)" : " (writes require login or the API token)"));
}

void HttpServer::stop() {
    if (impl_->srv && impl_->srv->is_running()) impl_->srv->stop();
    if (impl_->thread.joinable()) impl_->thread.join();
}

}  // namespace nightwatcher
