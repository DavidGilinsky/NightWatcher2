// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/weather/provider.hpp
// Purpose:       Pluggable weather-data provider interface (mirrors the SQM
//                ITransport pattern): each provider fetches the latest
//                observation from a source (Ambient Weather, Weather Underground).
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <memory>
#include <string>

#include "database.hpp"  // db::WeatherReadingRow

namespace nightwatcher::weather {

// A weather-data provider. fetch() returns the latest observation in metric/SI
// units (station_id is left for the caller to fill). Throws on any error.
class IWeatherProvider {
public:
    virtual ~IWeatherProvider() = default;
    virtual db::WeatherReadingRow fetch() = 0;
};

// Build a provider by name from its JSON config string. Throws std::runtime_error
// on an unknown provider, malformed config, or missing required settings.
//   "ambientweather" : { applicationKey, apiKey, macAddress? }
//   "wunderground"   : { stationId, apiKey }
std::unique_ptr<IWeatherProvider> make_provider(const std::string& provider,
                                                const std::string& config_json);

// Payload parsers exposed for unit testing (JSON string -> normalized reading).
db::WeatherReadingRow parse_ambient_json(const std::string& last_data_json);
db::WeatherReadingRow parse_wunderground_json(const std::string& observation_json);

}  // namespace nightwatcher::weather
