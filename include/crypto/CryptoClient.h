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

static constexpr uint8_t CRYPTO_MAX_COINS = 3;

struct CryptoTicker {
    char  symbol[8]  = {};   // display symbol, e.g. "BTC"
    char  id[32]     = {};   // CoinGecko ID, e.g. "bitcoin"
    float price      = 0.f;
    float change7d   = 0.f;  // percentage, positive = green
    bool  valid      = false;
};

class CryptoClient {
   public:
    // coins: comma-separated "id:SYMBOL" pairs, e.g. "bitcoin:BTC,ethereum:ETH"
    void begin(const String& coins);
    void loop();

    const CryptoTicker* getTickers() const;
    uint32_t getSerial() const;

   private:
    void fetch();
    void parseCoinsCfg(const String& cfg);

    CryptoTicker      _tickers[CRYPTO_MAX_COINS];
    uint8_t           _count       = 0;
    uint32_t          _serial      = 0;
    unsigned long     _lastFetchMs = 0;
    bool              _fetchedOnce = false;

    static constexpr unsigned long FETCH_INTERVAL_MS  = 5UL * 60UL * 1000UL;
    static constexpr unsigned long FIRST_FETCH_DELAY_MS = 30000UL; // wait 30s after boot
};
