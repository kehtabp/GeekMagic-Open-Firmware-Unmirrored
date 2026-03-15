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

#pragma once

#include <Arduino.h>

struct WeatherData {
    int  tempC       = 0;
    char description[48] = {};
    bool umbrella    = false;
    bool valid       = false;
};

class WeatherClient {
   public:
    void begin(const String& location, const String& apiKey);
    void loop();
    const WeatherData& getData() const;
    uint32_t getSerial() const;

   private:
    void fetch();

    String        _location;
    String        _apiKey;
    WeatherData   _data;
    uint32_t      _serial      = 0;
    unsigned long _lastFetchMs = 0;
    bool          _fetchedOnce = false;

    static constexpr unsigned long FETCH_INTERVAL_MS = 10UL * 60UL * 1000UL;
};
