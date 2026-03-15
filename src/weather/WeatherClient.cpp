// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * GeekMagic Open Firmware
 * Copyright (C) 2026 Times-Z
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "weather/WeatherClient.h"

#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <Logger.h>
#include <WiFiClient.h>
#include <wireless/WiFiManager.h>

static constexpr const char* TAG               = "WeatherClient";
static constexpr unsigned long HTTP_TIMEOUT_MS = 10000UL;

// OpenWeatherMap weather IDs 2xx-6xx = precipitation (thunderstorm / drizzle / rain / snow)
static constexpr int PRECIP_ID_MIN = 200;
static constexpr int PRECIP_ID_MAX = 699;

void WeatherClient::begin(const String& location, const String& apiKey) {
    _location    = location;
    _apiKey      = apiKey;
    _fetchedOnce = false;
    _serial      = 0;
}

void WeatherClient::loop() {
    if (_location.isEmpty() || _apiKey.isEmpty()) return;
    if (!WiFiManager::isConnected()) return;

    unsigned long now = millis();
    if (_fetchedOnce && (now - _lastFetchMs < FETCH_INTERVAL_MS)) return;

    _lastFetchMs  = now;
    _fetchedOnce  = true;
    fetch();
}

void WeatherClient::fetch() {
    String loc = _location;
    loc.replace(" ", "+");
    // Plain HTTP — no TLS, works within ESP8266's available heap
    String url = "http://api.openweathermap.org/data/2.5/weather?q=" + loc +
                 "&units=metric&appid=" + _apiKey;

    WiFiClient client;
    HTTPClient http;

    if (!http.begin(client, url)) {
        Logger::warn("HTTP begin failed", TAG);
        return;
    }

    http.setTimeout(HTTP_TIMEOUT_MS);
    int code = http.GET();

    if (code != 200) {
        Logger::warn(("HTTP " + String(code)).c_str(), TAG);
        http.end();
        _data.valid = false;
        _serial++;
        return;
    }

    JsonDocument filter;
    filter["weather"][0]["id"]          = true;
    filter["weather"][0]["description"] = true;
    filter["main"]["temp"]              = true;

    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();

    if (err) {
        Logger::warn(("JSON: " + String(err.c_str())).c_str(), TAG);
        _data.valid = false;
        _serial++;
        return;
    }

    float tempF   = doc["main"]["temp"] | 0.0f;
    _data.tempC   = static_cast<int>(tempF + 0.5f);

    const char* desc = doc["weather"][0]["description"] | "Unknown";
    strncpy(_data.description, desc, sizeof(_data.description) - 1);
    _data.description[sizeof(_data.description) - 1] = '\0';

    int weatherId  = doc["weather"][0]["id"] | 800;
    _data.umbrella = (weatherId >= PRECIP_ID_MIN && weatherId <= PRECIP_ID_MAX);

    _data.valid = true;
    _serial++;

    Logger::info(
        ("Weather: " + String(_data.tempC) + "C, " + String(_data.description) +
         (_data.umbrella ? ", umbrella!" : ""))
            .c_str(),
        TAG);
}

auto WeatherClient::getData() const -> const WeatherData& { return _data; }
auto WeatherClient::getSerial() const -> uint32_t { return _serial; }
