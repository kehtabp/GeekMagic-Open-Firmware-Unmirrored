// SPDX-License-Identifier: GPL-3.0-or-later
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <Logger.h>

#include "wol/WakeOnLan.h"

static constexpr uint16_t WOL_UDP_PORT = 9;
static constexpr size_t   WOL_PACKET_SIZE = 102;  // 6 + 16*6
static constexpr int      WOL_MAC_BYTES = 6;
static constexpr int      WOL_MAC_REPEAT = 16;

void WakeOnLan::begin(const String& pollUrl, const String& mac) {
    _pollUrl = pollUrl;
    _macValid = false;

    if (mac.length() == 0 || pollUrl.length() == 0) {
        return;
    }

    // Parse "AA:BB:CC:DD:EE:FF" or "AA-BB-CC-DD-EE-FF"
    int parsed = sscanf(mac.c_str(), "%hhx%*c%hhx%*c%hhx%*c%hhx%*c%hhx%*c%hhx",
                        &_mac[0], &_mac[1], &_mac[2], &_mac[3], &_mac[4], &_mac[5]);
    _macValid = (parsed == WOL_MAC_BYTES);

    if (_macValid) {
        Logger::info(("WoL ready, polling: " + pollUrl).c_str(), "WakeOnLan");
    } else {
        Logger::warn("WoL: invalid MAC address", "WakeOnLan");
    }
}

bool WakeOnLan::poll() {
    WiFiClient client;
    HTTPClient http;

    if (!http.begin(client, _pollUrl)) {
        return false;
    }

    int code = http.GET();
    String body = (code == 200) ? http.getString() : "";
    http.end();

    body.trim();
    return body == "1";
}

void WakeOnLan::sendMagicPacket() {
    uint8_t packet[WOL_PACKET_SIZE];
    memset(packet, 0xFF, WOL_MAC_BYTES);
    for (int i = 0; i < WOL_MAC_REPEAT; i++) {
        memcpy(packet + WOL_MAC_BYTES + i * WOL_MAC_BYTES, _mac, WOL_MAC_BYTES);
    }

    WiFiUDP udp;
    udp.begin(0);
    udp.beginPacket(IPAddress(255, 255, 255, 255), WOL_UDP_PORT);
    udp.write(packet, WOL_PACKET_SIZE);
    udp.endPacket();
    udp.stop();

    Logger::info("Magic packet sent", "WakeOnLan");
}

void WakeOnLan::loop() {
    if (!_macValid || _pollUrl.length() == 0) {
        return;
    }

    unsigned long now = millis();
    if (now - _lastPoll < POLL_INTERVAL_MS) {
        return;
    }
    _lastPoll = now;

    if (poll()) {
        sendMagicPacket();
    }
}
