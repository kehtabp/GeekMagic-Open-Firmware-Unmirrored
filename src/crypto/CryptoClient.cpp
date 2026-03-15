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
#include <WiFiClient.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino.h>

static constexpr const char* TAG = "CryptoClient";

// ---------------------------------------------------------------------------
// CoinLore ID lookup — maps common CoinGecko-style names to CoinLore numeric IDs
// ---------------------------------------------------------------------------
static const struct { const char* name; const char* id; } COINLORE_IDS[] = {
    {"bitcoin",   "90"},
    {"ethereum",  "80"},
    {"litecoin",  "1"},
    {"dogecoin",  "2"},
    {"xrp",       "58"},
    {"solana",    "48543"},
    {"cardano",   "257679"},
    {nullptr,     nullptr}
};

static const char* coinLoreId(const char* name) {
    for (int i = 0; COINLORE_IDS[i].name != nullptr; i++) {
        if (strcmp(name, COINLORE_IDS[i].name) == 0) return COINLORE_IDS[i].id;
    }
    return name;  // fall back: treat as a raw CoinLore numeric ID
}

// ---------------------------------------------------------------------------
// parseCoinsCfg — fills up to CRYPTO_MAX_COINS slots from "id:SYM,..." config
// ---------------------------------------------------------------------------
void CryptoClient::parseCoinsCfg(const String& cfg) {
    _count = 0;

    if (cfg.isEmpty()) {
        strncpy(_tickers[0].id,     "bitcoin",  sizeof(_tickers[0].id)     - 1);
        strncpy(_tickers[0].symbol, "BTC",      sizeof(_tickers[0].symbol) - 1);
        strncpy(_tickers[1].id,     "ethereum", sizeof(_tickers[1].id)     - 1);
        strncpy(_tickers[1].symbol, "ETH",      sizeof(_tickers[1].symbol) - 1);
        strncpy(_tickers[2].id,     "litecoin", sizeof(_tickers[2].id)     - 1);
        strncpy(_tickers[2].symbol, "LTC",      sizeof(_tickers[2].symbol) - 1);
        _count = 3;
        return;
    }

    String remaining = cfg;
    while (!remaining.isEmpty() && _count < CRYPTO_MAX_COINS) {
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
// begin — parse config
// ---------------------------------------------------------------------------
void CryptoClient::begin(const String& coins) {
    parseCoinsCfg(coins);
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
// fetchFromCoinLore — GET prices + 7d change over plain HTTP
// ---------------------------------------------------------------------------
static bool fetchFromCoinLore(CryptoTicker* tickers, int count) {
    String ids;
    for (int i = 0; i < count; i++) {
        if (tickers[i].id[0] == '\0') continue;
        if (ids.length()) ids += ',';
        ids += coinLoreId(tickers[i].id);
    }
    if (ids.isEmpty()) return false;

    String url = String("http://api.coinlore.net/api/ticker/?id=") + ids;

    WiFiClient client;
    HTTPClient http;
    if (!http.begin(client, url)) {
        Logger::warn("CoinLore begin() failed", TAG);
        return false;
    }
    http.addHeader("User-Agent", "ESP8266/1.0");

    int code = http.GET();
    if (code != 200) {
        Logger::warn(("CoinLore HTTP " + String(code)).c_str(), TAG);
        http.end();
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();

    if (err) {
        Logger::warn(("CoinLore parse: " + String(err.c_str())).c_str(), TAG);
        return false;
    }

    JsonArray arr = doc.as<JsonArray>();
    for (JsonObject coin : arr) {
        const char* nameid = coin["nameid"] | "";
        const char* sym    = coin["symbol"]  | "";
        for (int i = 0; i < count; i++) {
            if (strcmp(nameid, tickers[i].id) == 0 ||
                strcasecmp(sym, tickers[i].symbol) == 0) {
                tickers[i].price    = atof(coin["price_usd"]        | "0");
                tickers[i].change7d = atof(coin["percent_change_7d"] | "0");
                tickers[i].valid    = true;
                break;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// fetch — fetch all tickers from CoinLore over plain HTTP
// ---------------------------------------------------------------------------
void CryptoClient::fetch() {
    if (!WiFiManager::isConnected()) return;

    Logger::info("Fetching prices...", TAG);
    fetchFromCoinLore(_tickers, _count);
    _serial++;
    _fetchedOnce = true;
    Logger::info("Prices fetched.", TAG);
}
