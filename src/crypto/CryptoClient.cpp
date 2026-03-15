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

#include "crypto/CryptoClient.h"
#include "wireless/WiFiManager.h"
#include <Logger.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino.h>

static constexpr const char* TAG = "CryptoClient";

// Defined in main.cpp — stop/restart the webserver to free heap for BearSSL TLS
extern void cryptoWebserverPause();
extern void cryptoWebserverResume();

// ---------------------------------------------------------------------------
// parseCoinsCfg — fills slots 0..(CRYPTO_MAX_COINS-2) from "id:SYM,..." cfg
// ---------------------------------------------------------------------------
void CryptoClient::parseCoinsCfg(const String& cfg) {
    _count = 0;

    if (cfg.isEmpty()) {
        // Defaults: BTC and ETH
        strncpy(_tickers[0].id,     "bitcoin",  sizeof(_tickers[0].id)     - 1);
        strncpy(_tickers[0].symbol, "BTC",      sizeof(_tickers[0].symbol) - 1);
        strncpy(_tickers[1].id,     "ethereum", sizeof(_tickers[1].id)     - 1);
        strncpy(_tickers[1].symbol, "ETH",      sizeof(_tickers[1].symbol) - 1);
        _count = 2;
        return;
    }

    String remaining = cfg;
    while (!remaining.isEmpty() && _count < (CRYPTO_MAX_COINS - 1)) {
        int comma = remaining.indexOf(',');
        String pair = (comma >= 0) ? remaining.substring(0, comma) : remaining;
        remaining   = (comma >= 0) ? remaining.substring(comma + 1) : String("");

        int colon = pair.indexOf(':');
        if (colon <= 0) continue;

        String id  = pair.substring(0, colon);
        String sym = pair.substring(colon + 1);
        id.trim();
        sym.trim();
        if (id.isEmpty() || sym.isEmpty()) continue;

        int i = _count++;
        strncpy(_tickers[i].id,     id.c_str(),  sizeof(_tickers[i].id)     - 1);
        strncpy(_tickers[i].symbol, sym.c_str(), sizeof(_tickers[i].symbol) - 1);
        _tickers[i].id[sizeof(_tickers[i].id) - 1]         = '\0';
        _tickers[i].symbol[sizeof(_tickers[i].symbol) - 1] = '\0';
    }
}

// ---------------------------------------------------------------------------
// begin — parse config, always add FWRG (stock ETF) as last slot
// ---------------------------------------------------------------------------
void CryptoClient::begin(const String& coins) {
    parseCoinsCfg(coins);

    // Slot 2: FWRG stock ETF (always present, fetched from Yahoo Finance)
    strncpy(_tickers[2].id,     "FWRG", sizeof(_tickers[2].id)     - 1);
    strncpy(_tickers[2].symbol, "FWRG", sizeof(_tickers[2].symbol) - 1);
    _tickers[2].id[sizeof(_tickers[2].id) - 1]         = '\0';
    _tickers[2].symbol[sizeof(_tickers[2].symbol) - 1] = '\0';
    _count = 3;

    Logger::info(("CryptoClient: " + String(_count) + " tickers").c_str(), TAG);
}

// ---------------------------------------------------------------------------
// loop — trigger fetch at the configured interval
// ---------------------------------------------------------------------------
void CryptoClient::loop() {
    unsigned long now = millis();
    if (!_fetchedOnce && now < FIRST_FETCH_DELAY_MS) return;
    if (_lastFetchMs > 0 && (now - _lastFetchMs) < FETCH_INTERVAL_MS) return;

    fetch();
    _lastFetchMs = now;
}

const CryptoTicker* CryptoClient::getTickers() const { return _tickers; }
uint32_t            CryptoClient::getSerial()   const { return _serial;  }

// ---------------------------------------------------------------------------
// fetchFromCoinGecko — GET prices + 7d change for crypto slots 0..(n-1)
// ---------------------------------------------------------------------------
static bool fetchFromCoinGecko(CryptoTicker* tickers, int cryptoCount) {
    // Build comma-separated ids string
    String ids;
    for (int i = 0; i < cryptoCount; i++) {
        if (ids.length()) ids += ',';
        ids += tickers[i].id;
    }
    if (ids.isEmpty()) return false;

    String url = String("https://api.coingecko.com/api/v3/simple/price?ids=") + ids +
                 "&vs_currencies=usd&include_7d_change=true";

    BearSSL::WiFiClientSecure wcs;
    wcs.setInsecure();
    wcs.setBufferSizes(4096, 512);

    HTTPClient https;
    if (!https.begin(wcs, url)) {
        Logger::warn("CoinGecko begin() failed", TAG);
        return false;
    }
    https.addHeader("User-Agent", "ESP8266/1.0");

    int code = https.GET();
    if (code != 200) {
        Logger::warn(("CoinGecko HTTP " + String(code)).c_str(), TAG);
        https.end();
        return false;
    }

    JsonDocument filter;
    for (int i = 0; i < cryptoCount; i++) {
        filter[tickers[i].id]["usd"]          = true;
        filter[tickers[i].id]["usd_7d_change"] = true;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(
        doc, https.getStream(), DeserializationOption::Filter(filter));
    https.end();

    if (err) {
        Logger::warn(("CoinGecko parse: " + String(err.c_str())).c_str(), TAG);
        return false;
    }

    for (int i = 0; i < cryptoCount; i++) {
        JsonObject coin = doc[tickers[i].id].as<JsonObject>();
        if (!coin.isNull()) {
            tickers[i].price    = coin["usd"]          | 0.0f;
            tickers[i].change7d = coin["usd_7d_change"] | 0.0f;
            tickers[i].valid    = true;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// fetchFwrgFromYahoo — GET FWRG stock quote (1-day change) from Yahoo Finance
// ---------------------------------------------------------------------------
static bool fetchFwrgFromYahoo(CryptoTicker& ticker) {
    const String url = "https://query1.finance.yahoo.com/v7/finance/quote?symbols=FWRG";

    BearSSL::WiFiClientSecure wcs;
    wcs.setInsecure();
    wcs.setBufferSizes(4096, 512);

    HTTPClient https;
    if (!https.begin(wcs, url)) {
        Logger::warn("Yahoo begin() failed", TAG);
        return false;
    }
    https.addHeader("User-Agent", "Mozilla/5.0");
    https.addHeader("Accept",     "application/json");

    int code = https.GET();
    if (code != 200) {
        Logger::warn(("Yahoo HTTP " + String(code)).c_str(), TAG);
        https.end();
        return false;
    }

    JsonDocument filter;
    filter["quoteResponse"]["result"][0]["regularMarketPrice"]         = true;
    filter["quoteResponse"]["result"][0]["regularMarketChangePercent"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(
        doc, https.getStream(), DeserializationOption::Filter(filter));
    https.end();

    if (err) {
        Logger::warn(("FWRG parse: " + String(err.c_str())).c_str(), TAG);
        return false;
    }

    JsonObject result = doc["quoteResponse"]["result"][0].as<JsonObject>();
    if (result.isNull()) {
        Logger::warn("FWRG: no result in response", TAG);
        return false;
    }

    ticker.price    = result["regularMarketPrice"]         | 0.0f;
    ticker.change7d = result["regularMarketChangePercent"] | 0.0f;
    ticker.valid    = true;
    return true;
}

// ---------------------------------------------------------------------------
// fetch — stop webserver, run both HTTPS fetches, restart webserver
// ---------------------------------------------------------------------------
void CryptoClient::fetch() {
    if (!WiFiManager::isConnected()) return;

    Logger::info("Fetching prices...", TAG);

    // Stop webserver to free ~8 KB of heap for BearSSL TLS buffers
    cryptoWebserverPause();

    // Disable hardware watchdog: TLS handshake can take > 2 s on ESP8266
    EspClass::wdtDisable();

    // Fetch crypto (BTC + ETH) — slots 0..1
    if (_count > 1) {
        fetchFromCoinGecko(_tickers, _count - 1);
    }

    // Fetch FWRG stock ETF — slot 2
    fetchFwrgFromYahoo(_tickers[_count - 1]);

    // Re-enable watchdog and restart webserver
    EspClass::wdtEnable(WDTO_2S);
    EspClass::wdtFeed();

    cryptoWebserverResume();

    _serial++;
    _fetchedOnce = true;

    Logger::info("Prices fetched.", TAG);
}
