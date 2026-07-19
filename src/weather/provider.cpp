// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/weather/provider.cpp
// Purpose:       Weather provider implementations (Ambient Weather Network and
//                Weather Underground) over an HTTPS client (httplib + OpenSSL),
//                normalizing each source's units to metric/SI.
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "provider.hpp"

#include <cctype>
#include <cmath>
#include <stdexcept>

#include "httplib.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace nightwatcher::weather {
namespace {

std::string url_encode(const std::string& s) {
    static const char* const kHex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0f];
        }
    }
    return out;
}

// HTTPS GET; returns the response body or throws with a useful message.
std::string https_get(const std::string& host, const std::string& path_and_query) {
    httplib::Client cli("https://" + host);
    cli.set_connection_timeout(10, 0);
    cli.set_read_timeout(15, 0);
    auto res = cli.Get(path_and_query.c_str());
    if (!res) {
        throw std::runtime_error("request to " + host + " failed: " +
                                 httplib::to_string(res.error()));
    }
    if (res->status < 200 || res->status >= 300) {
        throw std::runtime_error("HTTP " + std::to_string(res->status) + " from " + host + ": " +
                                 res->body.substr(0, 200));
    }
    return res->body;
}

std::optional<double> jnum(const json& j, const char* key) {
    if (j.contains(key) && j[key].is_number()) return j[key].get<double>();
    return std::nullopt;
}

// "2026-07-19T03:45:42.000Z" / "...Z" -> "2026-07-19 03:45:42"
std::string iso_to_sql(const json& j, const char* key) {
    if (j.contains(key) && j[key].is_string()) {
        std::string dt = j[key].get<std::string>();
        if (dt.size() >= 19) {
            dt = dt.substr(0, 19);
            dt[10] = ' ';
            return dt;
        }
    }
    return std::string();
}

db::WeatherReadingRow parse_ambient(const json& d) {
    db::WeatherReadingRow r;
    r.raw = d.dump();
    r.ts_utc = iso_to_sql(d, "date");
    const auto f2c = [](double f) { return (f - 32.0) * 5.0 / 9.0; };
    if (auto v = jnum(d, "tempf")) r.temp_c = f2c(*v);
    if (auto v = jnum(d, "humidity")) r.humidity_pct = *v;
    if (auto v = jnum(d, "dewPoint")) r.dew_point_c = f2c(*v);
    if (auto v = jnum(d, "baromrelin")) r.pressure_hpa = *v * 33.8639;
    if (auto v = jnum(d, "baromabsin")) r.pressure_abs_hpa = *v * 33.8639;
    if (auto v = jnum(d, "windspeedmph")) r.wind_speed_ms = *v * 0.44704;
    if (auto v = jnum(d, "windgustmph")) r.wind_gust_ms = *v * 0.44704;
    if (auto v = jnum(d, "winddir")) r.wind_dir_deg = static_cast<int>(std::lround(*v));
    if (auto v = jnum(d, "hourlyrainin")) r.rain_rate_mmh = *v * 25.4;
    if (auto v = jnum(d, "dailyrainin")) r.rain_daily_mm = *v * 25.4;
    if (auto v = jnum(d, "uv")) r.uv_index = *v;
    if (auto v = jnum(d, "solarradiation")) r.solar_wm2 = *v;
    return r;
}

db::WeatherReadingRow parse_wunderground(const json& obs) {
    db::WeatherReadingRow r;
    r.raw = obs.dump();
    r.ts_utc = iso_to_sql(obs, "obsTimeUtc");
    if (auto v = jnum(obs, "humidity")) r.humidity_pct = *v;
    if (auto v = jnum(obs, "winddir")) r.wind_dir_deg = static_cast<int>(std::lround(*v));
    if (auto v = jnum(obs, "uv")) r.uv_index = *v;
    if (auto v = jnum(obs, "solarRadiation")) r.solar_wm2 = *v;
    if (obs.contains("metric") && obs["metric"].is_object()) {
        const json& m = obs["metric"];
        if (auto v = jnum(m, "temp")) r.temp_c = *v;
        if (auto v = jnum(m, "dewpt")) r.dew_point_c = *v;
        if (auto v = jnum(m, "pressure")) r.pressure_hpa = *v;
        if (auto v = jnum(m, "windSpeed")) r.wind_speed_ms = *v / 3.6;  // km/h -> m/s
        if (auto v = jnum(m, "windGust")) r.wind_gust_ms = *v / 3.6;
        if (auto v = jnum(m, "precipRate")) r.rain_rate_mmh = *v;
        if (auto v = jnum(m, "precipTotal")) r.rain_daily_mm = *v;
    }
    return r;
}

class AmbientProvider : public IWeatherProvider {
public:
    AmbientProvider(std::string app_key, std::string api_key, std::string mac)
        : app_key_(std::move(app_key)), api_key_(std::move(api_key)), mac_(std::move(mac)) {}

    db::WeatherReadingRow fetch() override {
        const std::string path = "/v1/devices?applicationKey=" + url_encode(app_key_) +
                                 "&apiKey=" + url_encode(api_key_);
        const json arr = json::parse(https_get("api.ambientweather.net", path));
        if (!arr.is_array() || arr.empty()) throw std::runtime_error("Ambient: no devices returned");
        json dev = arr[0];
        if (!mac_.empty()) {
            bool found = false;
            for (const auto& d : arr) {
                if (d.value("macAddress", std::string()) == mac_) { dev = d; found = true; break; }
            }
            if (!found) throw std::runtime_error("Ambient: device MAC " + mac_ + " not found");
        }
        if (!dev.contains("lastData")) throw std::runtime_error("Ambient: device has no lastData");
        return parse_ambient(dev["lastData"]);
    }

private:
    std::string app_key_, api_key_, mac_;
};

class WundergroundProvider : public IWeatherProvider {
public:
    WundergroundProvider(std::string station_id, std::string api_key)
        : station_id_(std::move(station_id)), api_key_(std::move(api_key)) {}

    db::WeatherReadingRow fetch() override {
        const std::string path = "/v2/pws/observations/current?stationId=" + url_encode(station_id_) +
                                 "&format=json&units=m&apiKey=" + url_encode(api_key_);
        const json j = json::parse(https_get("api.weather.com", path));
        if (!j.contains("observations") || !j["observations"].is_array() ||
            j["observations"].empty()) {
            throw std::runtime_error("Wunderground: no observations returned");
        }
        return parse_wunderground(j["observations"][0]);
    }

private:
    std::string station_id_, api_key_;
};

}  // namespace

std::unique_ptr<IWeatherProvider> make_provider(const std::string& provider,
                                                const std::string& config_json) {
    const json cfg = config_json.empty() ? json::object() : json::parse(config_json);
    if (provider == "ambientweather") {
        const std::string app = cfg.value("applicationKey", std::string());
        const std::string api = cfg.value("apiKey", std::string());
        const std::string mac = cfg.value("macAddress", std::string());
        if (app.empty() || api.empty()) {
            throw std::runtime_error("ambientweather requires applicationKey and apiKey");
        }
        return std::make_unique<AmbientProvider>(app, api, mac);
    }
    if (provider == "wunderground") {
        const std::string sid = cfg.value("stationId", std::string());
        const std::string api = cfg.value("apiKey", std::string());
        if (sid.empty() || api.empty()) {
            throw std::runtime_error("wunderground requires stationId and apiKey");
        }
        return std::make_unique<WundergroundProvider>(sid, api);
    }
    throw std::runtime_error("unknown weather provider: '" + provider + "'");
}

db::WeatherReadingRow parse_ambient_json(const std::string& last_data_json) {
    return parse_ambient(json::parse(last_data_json));
}

db::WeatherReadingRow parse_wunderground_json(const std::string& observation_json) {
    return parse_wunderground(json::parse(observation_json));
}

}  // namespace nightwatcher::weather
