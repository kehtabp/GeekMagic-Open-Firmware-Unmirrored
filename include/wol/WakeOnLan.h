// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAKE_ON_LAN_H
#define WAKE_ON_LAN_H

#include <Arduino.h>

class WakeOnLan {
   public:
    void begin(const String& pollUrl, const String& mac);
    void loop();

   private:
    bool poll();
    void sendMagicPacket();

    String _pollUrl;
    uint8_t _mac[6] = {};
    bool _macValid = false;
    unsigned long _lastPoll = 0;
    static constexpr unsigned long POLL_INTERVAL_MS = 10000UL;
};

#endif  // WAKE_ON_LAN_H
