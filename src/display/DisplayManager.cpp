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

#include <SPI.h>
#include <Logger.h>
#include <array>
#include <algorithm>
#include <ctime>
#include <Arduino.h>

#include "project_version.h"
#include "display/DisplayManager.h"
#include "config/ConfigManager.h"
#include "display/Gif.h"
#include "crypto/CryptoClient.h"

static Gif  s_gif;
static bool s_wasGifPlaying  = false;   // set when gif is/was playing; triggers clock full-redraw
static bool s_clockFirstDraw = true;    // forces full clear on first clock frame

extern ConfigManager configManager;

static Arduino_HWSPI g_lcdBus = Arduino_HWSPI(LCD_DC_GPIO, -1, &SPI, true);

// MADCTL (0x36) has no effect on this panel variant — gate scan direction is
// hardware-locked. Mirror X in software:
//   1. writeAddrWindow maps logical (x,y,w,h) → physical (_width-x-w, y, w, h)
//   2. writePixels reverses every row of _currentW pixels so pixel order matches
//      the reversed physical scan direction.
class MirroredST7789 : public Arduino_ST7789 {
public:
    using Arduino_ST7789::Arduino_ST7789;

    void writeAddrWindow(int16_t x, int16_t y, uint16_t w, uint16_t h) override {
        Arduino_ST7789::writeAddrWindow((int16_t)_width - x - (int16_t)w, y, w, h);
    }

    void writePixels(uint16_t *data, uint32_t len) override {
        uint32_t w = _currentW;
        if (w <= 1) {
            Arduino_ST7789::writePixels(data, len);
            return;
        }
        uint16_t rowBuf[LCD_W];
        uint32_t offset = 0;
        while (offset < len) {
            uint32_t rowLen = (len - offset >= w) ? w : (len - offset);
            for (uint32_t i = 0; i < rowLen; i++) {
                rowBuf[i] = data[offset + rowLen - 1 - i];
            }
            Arduino_ST7789::writePixels(rowBuf, rowLen);
            offset += rowLen;
        }
    }
};
static MirroredST7789 g_lcd = MirroredST7789(&g_lcdBus, -1, 0, true, LCD_W, LCD_H);

static constexpr uint32_t LCD_HARDWARE_RESET_DELAY_MS = 120;
static constexpr uint32_t LCD_BEGIN_DELAY_MS = 10;
static constexpr int16_t DISPLAY_PADDING = 10;
static constexpr int16_t DISPLAY_INFO_Y = 100;

static constexpr int WRAP_MAX_CHARS = 128;
static constexpr int WRAP_MAX_LINE_SLOTS = 10;

/**
 * @brief Push the current line buffer into the output lines array
 *
 * @param outLines The output lines array
 * @param lineBuf The current line buffer
 * @param lineLen The current line length
 * @param lineCount The current line count
 * @param maxLines The maximum number of lines allowed
 *
 * @return void
 */
static void wrapPushLine(std::array<std::array<char, WRAP_MAX_CHARS>, WRAP_MAX_LINE_SLOTS>& outLines,
                         std::array<char, WRAP_MAX_CHARS>& lineBuf, int& lineLen, int& lineCount, int maxLines) {
    if (lineCount >= maxLines) {
        return;
    }

    lineBuf[lineLen] = '\0';
    strncpy(outLines[lineCount].data(), lineBuf.data(), WRAP_MAX_CHARS - 1);
    outLines[lineCount][WRAP_MAX_CHARS - 1] = '\0';
    ++lineCount;

    lineLen = 0;
    lineBuf[0] = '\0';
}

/**
 * @brief Append a word to the current line buffer, wrapping if necessary
 *
 * @param outLines The output lines array
 * @param lineBuf The current line buffer
 * @param lineLen The current line length
 * @param wordBuf The word buffer to append
 * @param wordLen The word length
 * @param maxCharsPerLine The maximum characters per line
 * @param lineCount The current line count
 * @param maxLines The maximum number of lines allowed
 *
 * @return void
 */
static void wrapAppendWord(std::array<std::array<char, WRAP_MAX_CHARS>, WRAP_MAX_LINE_SLOTS>& outLines,
                           std::array<char, WRAP_MAX_CHARS>& lineBuf, int& lineLen,
                           std::array<char, WRAP_MAX_CHARS>& wordBuf, int& wordLen, int maxCharsPerLine, int& lineCount,
                           int maxLines) {
    if (wordLen == 0) {
        return;
    }

    if (wordLen > maxCharsPerLine) {
        if (lineLen != 0) {
            wrapPushLine(outLines, lineBuf, lineLen, lineCount, maxLines);
            if (lineCount >= maxLines) {
                wordLen = 0;

                return;
            }
        }
        int copyLen = (wordLen > maxCharsPerLine) ? maxCharsPerLine : wordLen;
        memcpy(lineBuf.data(), wordBuf.data(), static_cast<size_t>(copyLen));
        lineLen = copyLen;
        wordLen = 0;
        wordBuf[0] = '\0';

        return;
    }
    if (lineLen == 0) {
        memcpy(lineBuf.data(), wordBuf.data(), static_cast<size_t>(wordLen));
        lineLen = wordLen;
        wordLen = 0;
        wordBuf[0] = '\0';

        return;
    }
    if ((lineLen + 1 + wordLen) <= maxCharsPerLine) {
        lineBuf[lineLen] = ' ';
        memcpy(lineBuf.data() + lineLen + 1, wordBuf.data(), static_cast<size_t>(wordLen));
        lineLen += 1 + wordLen;
        wordLen = 0;
        wordBuf[0] = '\0';

        return;
    }
    wrapPushLine(outLines, lineBuf, lineLen, lineCount, maxLines);
    if (lineCount >= maxLines) {
        wordLen = 0;

        return;
    }

    memcpy(lineBuf.data(), wordBuf.data(), static_cast<size_t>(wordLen));
    lineLen = wordLen;
    wordLen = 0;
    wordBuf[0] = '\0';
}

// Screen cmd
static constexpr uint8_t ST7789_SLEEP_DELAY_MS = 120;
static constexpr uint8_t ST7789_SLEEP_OUT = 0x11;
static constexpr uint8_t ST7789_PORCH = 0xB2;
static constexpr uint8_t ST7789_PORCH_SETTINGS = 0x1F;
static constexpr uint8_t ST7789_SW_RESET = 0x01;

static constexpr uint8_t ST7789_TEARING_EFFECT = 0x35;
static constexpr uint8_t ST7789_MEMORY_ACCESS_CONTROL = 0x36;
static constexpr uint8_t ST7789_COLORMODE = 0x3A;
static constexpr uint8_t ST7789_COLORMODE_RGB565 = 0x05;

static constexpr uint8_t ST7789_POWER_B7 = 0xB7;
static constexpr uint8_t ST7789_POWER_BB = 0xBB;
static constexpr uint8_t ST7789_POWER_C0 = 0xC0;
static constexpr uint8_t ST7789_POWER_C2 = 0xC2;
static constexpr uint8_t ST7789_POWER_C3 = 0xC3;
static constexpr uint8_t ST7789_POWER_C4 = 0xC4;
static constexpr uint8_t ST7789_POWER_C6 = 0xC6;
static constexpr uint8_t ST7789_POWER_D0 = 0xD0;
static constexpr uint8_t ST7789_POWER_D6 = 0xD6;

static constexpr uint8_t ST7789_GAMMA_POS = 0xE0;
static constexpr uint8_t ST7789_GAMMA_NEG = 0xE1;
static constexpr uint8_t ST7789_GAMMA_CTRL = 0xE4;

static constexpr uint8_t ST7789_INVERSION_ON = 0x21;
static constexpr uint8_t ST7789_DISPLAY_ON = 0x29;
static constexpr uint8_t ST7789_NORMAL_DISPLAY_MODE = 0x13;

// Porch parameters used in sequence
static constexpr uint8_t ST7789_PORCH_PARAM_HS = 0x1F;
static constexpr uint8_t ST7789_PORCH_PARAM_VS = 0x1F;
static constexpr uint8_t ST7789_PORCH_PARAM_DUMMY = 0x00;
static constexpr uint8_t ST7789_PORCH_PARAM_HBP = 0x33;
static constexpr uint8_t ST7789_PORCH_PARAM_VBP = 0x33;

// Simple params for commands
static constexpr uint8_t ST7789_TEARING_PARAM_OFF = 0x00;
static constexpr uint8_t ST7789_MADCTL_PARAM_DEFAULT = 0x00;
static constexpr uint8_t ST7789_B7_PARAM_DEFAULT = 0x00;
static constexpr uint8_t ST7789_BB_PARAM_VOLTAGE = 0x36;
static constexpr uint8_t ST7789_C0_PARAM_1 = 0x2C;
static constexpr uint8_t ST7789_C2_PARAM_1 = 0x01;
static constexpr uint8_t ST7789_C3_PARAM_1 = 0x13;
static constexpr uint8_t ST7789_C4_PARAM_1 = 0x20;
static constexpr uint8_t ST7789_C6_PARAM_1 = 0x13;
static constexpr uint8_t ST7789_D6_PARAM_1 = 0xA1;
static constexpr uint8_t ST7789_D0_PARAM_1 = 0xA4;
static constexpr uint8_t ST7789_D0_PARAM_2 = 0xA1;

// Gamma parameter blocks
static constexpr std::array<uint8_t, 14> ST7789_GAMMA_POS_DATA = {0xF0, 0x08, 0x0E, 0x09, 0x08, 0x04, 0x2F,
                                                                  0x33, 0x45, 0x36, 0x13, 0x12, 0x2A, 0x2D};
static constexpr std::array<uint8_t, 14> ST7789_GAMMA_NEG_DATA = {0xF0, 0x0E, 0x12, 0x0C, 0x0A, 0x15, 0x2E,
                                                                  0x32, 0x44, 0x39, 0x17, 0x18, 0x2B, 0x2F};
static constexpr std::array<uint8_t, 3> ST7789_GAMMA_CTRL_DATA = {0x1D, 0x00, 0x00};

// Column/row address parameters
static constexpr uint8_t ST7789_ADDR_START_HIGH = 0x00;
static constexpr uint8_t ST7789_ADDR_START_LOW = 0x00;
static constexpr uint8_t ST7789_ADDR_END_HIGH = 0x00;
static constexpr uint8_t ST7789_ADDR_END_LOW = 0xEF;

/**
 * @brief Get the Arduino_GFX instance used for the LCD
 *
 * @return Pointer to the Arduino_GFX instance
 */
auto DisplayManager::getGfx() -> Arduino_GFX* { return &g_lcd; }

/**
 * @brief Turn the LCD backlight on
 *
 * @return void
 */
static inline void lcdBacklightOn() {
    pinMode((uint8_t)LCD_BACKLIGHT_GPIO, OUTPUT);
    digitalWrite((uint8_t)LCD_BACKLIGHT_GPIO, LCD_BACKLIGHT_ACTIVE_LOW ? LOW : HIGH);
}

/**
 * @brief Set backlight brightness via PWM (0 = off, 100 = full)
 */
void DisplayManager::setBrightness(uint8_t percent) {
    if (percent > 100) percent = 100;
    // analogWrite range 0-1023; backlight is active-low so invert
    int pwmVal = LCD_BACKLIGHT_ACTIVE_LOW
                     ? ((100 - percent) * 1023 / 100)
                     : (percent * 1023 / 100);
    pinMode((uint8_t)LCD_BACKLIGHT_GPIO, OUTPUT);
    analogWrite((uint8_t)LCD_BACKLIGHT_GPIO, pwmVal);
}

/**
 * @brief Write a single command byte to the ST7789 via the data bus
 *
 * @return void
 */
static inline void ST7789_WriteCommand(uint8_t cmd) { g_lcdBus.writeCommand(cmd); }

/**
 * @brief Write a single data byte to the ST7789 via the data bus
 *
 * @return void
 */
static inline void ST7789_WriteData(uint8_t data) { g_lcdBus.write(data); }

/**
 * @brief Run a vendor-specific initialization sequence for the ST7789 panel
 *
 *  - Sleep out (0x11)
 *
 *  - Porch settings (0xB2)
 *
 *  - Tearing effect on (0x35)
 *
 *  - Memory access control/MADCTL (0x36)
 *
 *  - Color mode to 16-bit RGB565 (0x3A)
 *
 *  - Various power control settings (0xB7, 0xBB, 0xC0-0xC6, 0xD0, 0xD6)
 *
 *  - Gamma correction settings (0xE0, 0xE1, 0xE4)
 *
 *  - Display inversion on (0x21)
 *
 *  - Display on (0x29)
 *
 *  - Full window setup and RAMWR command (0x2A, 0x2B, 0x2C)
 *
 * @return void
 */
static void lcdRunVendorInit() {
    g_lcdBus.beginWrite();

    ST7789_WriteCommand(ST7789_SLEEP_OUT);
    delay(ST7789_SLEEP_DELAY_MS);

    ST7789_WriteCommand(ST7789_PORCH);
    ST7789_WriteData(ST7789_PORCH_PARAM_HS);
    ST7789_WriteData(ST7789_PORCH_PARAM_VS);
    ST7789_WriteData(ST7789_PORCH_PARAM_DUMMY);
    ST7789_WriteData(ST7789_PORCH_PARAM_HBP);
    ST7789_WriteData(ST7789_PORCH_PARAM_VBP);

    ST7789_WriteCommand(ST7789_TEARING_EFFECT);
    ST7789_WriteData(ST7789_TEARING_PARAM_OFF);

    ST7789_WriteCommand(ST7789_MEMORY_ACCESS_CONTROL);
    ST7789_WriteData(ST7789_MADCTL_PARAM_DEFAULT);

    ST7789_WriteCommand(ST7789_COLORMODE);
    ST7789_WriteData(ST7789_COLORMODE_RGB565);

    ST7789_WriteCommand(ST7789_POWER_B7);
    ST7789_WriteData(ST7789_B7_PARAM_DEFAULT);

    ST7789_WriteCommand(ST7789_POWER_BB);
    ST7789_WriteData(ST7789_BB_PARAM_VOLTAGE);

    ST7789_WriteCommand(ST7789_POWER_C0);
    ST7789_WriteData(ST7789_C0_PARAM_1);

    ST7789_WriteCommand(ST7789_POWER_C2);
    ST7789_WriteData(ST7789_C2_PARAM_1);

    ST7789_WriteCommand(ST7789_POWER_C3);
    ST7789_WriteData(ST7789_C3_PARAM_1);

    ST7789_WriteCommand(ST7789_POWER_C4);
    ST7789_WriteData(ST7789_C4_PARAM_1);

    ST7789_WriteCommand(ST7789_POWER_C6);
    ST7789_WriteData(ST7789_C6_PARAM_1);

    ST7789_WriteCommand(ST7789_POWER_D6);
    ST7789_WriteData(ST7789_D6_PARAM_1);

    ST7789_WriteCommand(ST7789_POWER_D0);
    ST7789_WriteData(ST7789_D0_PARAM_1);
    ST7789_WriteData(ST7789_D0_PARAM_2);

    ST7789_WriteCommand(ST7789_POWER_D6);
    ST7789_WriteData(ST7789_D6_PARAM_1);

    ST7789_WriteCommand(ST7789_GAMMA_POS);
    for (uint8_t v : ST7789_GAMMA_POS_DATA) {
        ST7789_WriteData(v);
    }

    ST7789_WriteCommand(ST7789_GAMMA_NEG);
    for (uint8_t v : ST7789_GAMMA_NEG_DATA) {
        ST7789_WriteData(v);
    }

    ST7789_WriteCommand(ST7789_GAMMA_CTRL);
    for (uint8_t v : ST7789_GAMMA_CTRL_DATA) {
        ST7789_WriteData(v);
    }

    ST7789_WriteCommand(ST7789_INVERSION_ON);

    ST7789_WriteCommand(ST7789_DISPLAY_ON);

    ST7789_WriteCommand(ST7789_CASET);
    ST7789_WriteData(ST7789_ADDR_START_HIGH);
    ST7789_WriteData(ST7789_ADDR_START_LOW);
    ST7789_WriteData(ST7789_ADDR_END_HIGH);
    ST7789_WriteData(ST7789_ADDR_END_LOW);

    ST7789_WriteCommand(ST7789_RASET);
    ST7789_WriteData(ST7789_ADDR_START_HIGH);
    ST7789_WriteData(ST7789_ADDR_START_LOW);
    ST7789_WriteData(ST7789_ADDR_END_HIGH);
    ST7789_WriteData(ST7789_ADDR_END_LOW);

    ST7789_WriteCommand(ST7789_RAMWR);

    g_lcdBus.endWrite();
}

/**
 * @brief Perform a hardware reset of the LCD panel
 *
 * Toggles the RST GPIO if defined, with appropriate delays
 *
 * @return void
 */
static void lcdHardReset() {
    pinMode((uint8_t)LCD_RST_GPIO, OUTPUT);
    digitalWrite((uint8_t)LCD_RST_GPIO, HIGH);
    delay(LCD_HARDWARE_RESET_DELAY_MS);
    digitalWrite((uint8_t)LCD_RST_GPIO, LOW);
    delay(LCD_HARDWARE_RESET_DELAY_MS);
    digitalWrite((uint8_t)LCD_RST_GPIO, HIGH);
    delay(LCD_HARDWARE_RESET_DELAY_MS);
}

/**
 * @brief Ensure the LCD is initialized and ready for drawing
 *
 * @return void
 */
static void lcdEnsureInit() {
    Logger::info("Initialization started", "DisplayManager");

    lcdBacklightOn();

    uint8_t rotation = configManager.getLCDRotationSafe();

    // SPI mode 3 is required. This toggles the pin from LOW to HIGH after reset, which my guess
    // is after reset "initializes" the SPI interface of the display, as CS is tied to GND?
    // ...strange that SPI_MODE0 will not work as the IC doesn't care about CLK's polarity
    g_lcdBus.begin((int32_t)LCD_SPI_HZ, (int8_t)LCD_SPI_MODE);
    lcdHardReset();
    lcdRunVendorInit();
    delay(LCD_BEGIN_DELAY_MS);

    g_lcd.setRotation(rotation);

    Logger::info(("Width=" + String(g_lcd.width()) + " height=" + String(g_lcd.height())).c_str(), "DisplayManager");

    g_lcd.fillScreen(LCD_BLACK);
    g_lcd.setTextColor(LCD_WHITE, LCD_BLACK);

    Logger::info("Initialization completed", "DisplayManager");
}

/**
 * @brief Wrap text into lines fitting within max characters and lines
 *
 * @param text The input text to wrap
 * @param maxCharsPerLine Maximum characters allowed per line
 * @param maxLines Maximum number of lines allowed
 * @param outLines Output array to hold the wrapped lines
 *
 * @return The number of lines used
 */
static auto lcdWrapTextToBuffer(const String& text, int maxCharsPerLine, int maxLines,
                                std::array<std::array<char, WRAP_MAX_CHARS>, WRAP_MAX_LINE_SLOTS>& outLines) -> int {
    int lineCount = 0;

    for (auto& row : outLines) {
        row[0] = '\0';
    }

    std::array<char, WRAP_MAX_CHARS> lineBuf{};
    std::array<char, WRAP_MAX_CHARS> wordBuf{};
    int lineLen = 0;
    int wordLen = 0;

    for (uint32_t i = 0; i < text.length(); ++i) {
        char chr = text.charAt(i);

        if (chr == '\r') {
            continue;
        }

        if (chr == '\n') {
            wrapAppendWord(outLines, lineBuf, lineLen, wordBuf, wordLen, maxCharsPerLine, lineCount, maxLines);
            wrapPushLine(outLines, lineBuf, lineLen, lineCount, maxLines);

            if (lineCount >= maxLines) {
                break;
            }

            continue;
        }

        if (chr == ' ' || chr == '\t') {
            wrapAppendWord(outLines, lineBuf, lineLen, wordBuf, wordLen, maxCharsPerLine, lineCount, maxLines);

            if (lineCount >= maxLines) {
                break;
            }

            continue;
        }

        if (wordLen + 1 < WRAP_MAX_CHARS) {
            wordBuf[wordLen++] = chr;
            wordBuf[wordLen] = '\0';
        }
    }

    wrapAppendWord(outLines, lineBuf, lineLen, wordBuf, wordLen, maxCharsPerLine, lineCount, maxLines);

    if (lineLen != 0 && lineCount < maxLines) {
        wrapPushLine(outLines, lineBuf, lineLen, lineCount, maxLines);
    }

    if (lineCount == 0) {
        outLines[0][0] = '\0';
        lineCount = 1;
    }

    return lineCount;
}

/**
 * @brief Draw text on the display with simple word-wrapping
 *
 * @param startX Starting X coordinate in pixels
 * @param startY Starting Y coordinate in pixels
 * @param text The text to draw (can contain newlines)
 * @param textSize Font size multiplier (integer)
 * @param fgColor Foreground color (16-bit RGB565)
 * @param bgColor Background color (16-bit RGB565)
 * @param clearBg If true, clears the background rectangle before drawing
 *
 * @return void
 */
static void lcdDrawTextWrapped(int16_t startX, int16_t startY, const String& text, uint8_t textSize, uint16_t fgColor,
                               uint16_t bgColor, bool clearBg) {
    const auto screenW = static_cast<int16_t>(g_lcd.width());
    const auto screenH = static_cast<int16_t>(g_lcd.height());

    if (startX < 0) {
        startX = 0;
    }

    if (startY < 0) {
        startY = 0;
    }

    if (startX >= screenW || startY >= screenH) {
        Logger::warn("Text start position out of bounds", "DisplayManager");

        return;
    }

    const auto charW = static_cast<int16_t>(6 * textSize);
    const auto charH = static_cast<int16_t>(8 * textSize);
    if (charW <= 0 || charH <= 0) {
        Logger::warn("Invalid character dimensions", "DisplayManager");

        return;
    }

    int maxCharsPerLine = (screenW - startX) / charW;
    int maxLines = (screenH - startY) / charH;
    if (maxCharsPerLine <= 0 || maxLines <= 0) {
        Logger::warn("No space for text", "DisplayManager");

        return;
    }

    if (maxLines > WRAP_MAX_LINE_SLOTS) {
        maxLines = WRAP_MAX_LINE_SLOTS;
    }

    std::array<std::array<char, WRAP_MAX_CHARS>, WRAP_MAX_LINE_SLOTS> lines{};
    int lineCount = lcdWrapTextToBuffer(text, maxCharsPerLine, maxLines, lines);

    if (clearBg) {
        const auto heightPixels = static_cast<int16_t>(static_cast<int>(lineCount) * static_cast<int>(charH));
        g_lcd.fillRect(startX, startY, static_cast<int16_t>(screenW - startX), static_cast<int16_t>(heightPixels),
                       bgColor);
    }

    g_lcd.setTextSize(textSize);
    g_lcd.setTextColor(fgColor, bgColor);

    for (int li = 0; li < lineCount; ++li) {
        g_lcd.setCursor(startX, static_cast<int16_t>(startY + li * charH));
        g_lcd.print(lines[li].data());
    }
}

/**
 * @brief Initialize the DisplayManager and LCD
 *
 * Ensures the LCD is initialized and ready for drawing
 *
 * @return void
 */
auto DisplayManager::begin() -> void { lcdEnsureInit(); }

/**
 * @brief Draw the startup screen on the LCD
 *
 * @return void
 */
auto DisplayManager::drawStartup(String currentIP) -> void {
    int constexpr rgbDelayMs = 1000;

    g_lcd.fillScreen(LCD_RED);
    delay(rgbDelayMs);
    g_lcd.fillScreen(LCD_GREEN);
    delay(rgbDelayMs);
    g_lcd.fillScreen(LCD_BLUE);
    delay(rgbDelayMs);

    g_lcd.fillScreen(LCD_BLACK);

    int constexpr titleY = 10;
    int constexpr fontSize = 2;

    DisplayManager::drawTextWrapped(DISPLAY_PADDING, titleY, "GeekMagic Open Firmware", fontSize, LCD_WHITE, LCD_BLACK,
                                    false);
    DisplayManager::drawTextWrapped(DISPLAY_PADDING, titleY + THREE_LINES_SPACE, String(PROJECT_VER_STR), fontSize,
                                    LCD_WHITE, LCD_BLACK, false);
    DisplayManager::drawTextWrapped(DISPLAY_PADDING, (titleY + THREE_LINES_SPACE + TWO_LINES_SPACE), "IP: " + currentIP,
                                    fontSize, LCD_WHITE, LCD_BLACK, false);

    const int16_t box = 40;
    const int16_t gap = 20;
    const int16_t boxY = titleY + (THREE_LINES_SPACE * 2) + ONE_LINE_SPACE;

    g_lcd.fillRect(DISPLAY_PADDING, boxY, box, box, LCD_RED);
    g_lcd.fillRect((int16_t)(DISPLAY_PADDING + box + gap), boxY, box, box, LCD_GREEN);
    g_lcd.fillRect((int16_t)(DISPLAY_PADDING + (box + gap) * 2), boxY, box, box, LCD_BLUE);

    yield();

    Logger::info("Startup screen drawn", "DisplayManager");
}

/**
 * @brief Draw text on the display with simple word-wrapping
 *
 * @param x Starting X coordinate in pixels
 * @param y Starting Y coordinate in pixels
 * @param text The text to draw (can contain newlines)
 * @param textSize Font size multiplier (integer)
 * @param fg Foreground color (16-bit RGB565)
 * @param bg Background color (16-bit RGB565)
 * @param clearBg If true, clears the background rectangle before drawing
 *
 * @return void
 */
void DisplayManager::drawTextWrapped(int16_t xPos, int16_t yPos, const String& text, uint8_t textSize, uint16_t fgColor,
                                     uint16_t bgColor, bool clearBg) {
    lcdDrawTextWrapped(xPos, yPos, text, textSize, fgColor, bgColor, clearBg);
}

/**
 * @brief Draw a loading bar on the display
 *
 * @param progress Progress value between 0.0 (empty) and 1.0 (full)
 * @param yPos Y coordinate of the top of the loading bar
 * @param barWidth Width of the loading bar in pixels
 * @param barHeight Height of the loading bar in pixels
 * @param fgColor Foreground color (16-bit RGB565)
 * @param bgColor Background color (16-bit RGB565)
 */
void DisplayManager::drawLoadingBar(float progress, int yPos, int barWidth, int barHeight, uint16_t fgColor,
                                    uint16_t bgColor) {
    auto barXPos = (static_cast<int32_t>(LCD_W) - static_cast<int32_t>(barWidth)) / 2;
    auto barXPos16 = static_cast<int16_t>(barXPos);
    auto yPos16 = static_cast<int16_t>(yPos);
    auto barWidth16 = static_cast<int16_t>(barWidth);
    auto barHeight16 = static_cast<int16_t>(barHeight);

    g_lcd.fillRect(barXPos16, yPos16, barWidth16, barHeight16, bgColor);

    auto fillWidthF = static_cast<float>(barWidth) * progress;
    auto fillWidth16 = static_cast<int16_t>(fillWidthF);
    if (fillWidth16 > 0) {
        g_lcd.fillRect(barXPos16, yPos16, fillWidth16, barHeight16, fgColor);
    }

    yield();
}

/**
 * @brief Play a single GIF file in full screen mode (blocking)
 *
 * @param path Path to the GIF file on LittleFS
 * @param timeMs Duration to play the GIF in milliseconds (0 = play full GIF)
 * @return true if played successfully, false on error
 */
auto DisplayManager::playGifFullScreen(const String& path, uint32_t timeMs) -> bool {
    // Ensure any currently playing GIF is stopped so we can start a new one
    s_gif.stop();

    if (!s_gif.begin()) {
        return false;
    }

    DisplayManager::clearScreen();

    s_gif.setLoopEnabled(timeMs == 0);

    const bool started = s_gif.playOne(path);
    if (!started) {
        return false;
    }

    s_wasGifPlaying = true;

    if (timeMs == 0) {
        return true;
    }

    const uint32_t startMs = millis();
    const uint32_t endMs = startMs + timeMs;

    while (s_gif.isPlaying() && static_cast<int32_t>(millis() - endMs) < 0) {
        s_gif.update();
        yield();
    }

    if (s_gif.isPlaying()) {
        s_gif.stop();
    }

    while (s_gif.isPlaying()) {
        s_gif.update();
        yield();
    }

    s_gif.setLoopEnabled(false);

    return true;
}

/**
 * @brief Stop GIF playback if playing
 *
 * @return true
 */
auto DisplayManager::stopGif() -> bool {
    s_gif.stop();

    DisplayManager::clearScreen();

    return true;
}

// ---------------------------------------------------------------------------
// Clock + weather screen
// ---------------------------------------------------------------------------

// Layout constants (240×240 panel)
static constexpr int16_t CW_DATE_Y        = 5;
static constexpr uint8_t CW_DATE_SIZE     = 2;
static constexpr int16_t CW_TIME_Y        = 28;
static constexpr uint8_t CW_TIME_SIZE     = 4;
static constexpr int16_t CW_TEMP_Y        = 72;
static constexpr uint8_t CW_TEMP_SIZE     = 4;
static constexpr int16_t CW_DESC_Y        = 116;
static constexpr uint8_t CW_DESC_SIZE     = 2;
static constexpr int16_t CW_UMBRELLA_Y    = 156;
static constexpr uint8_t CW_UMBRELLA_SIZE = 2;
static constexpr int16_t CW_LOC_Y         = 184;
static constexpr uint8_t CW_LOC_SIZE      = 1;
static constexpr time_t  CW_REASONABLE_EPOCH = 1600000000UL;

// Weather state (written by setWeatherData, read by lcdDrawClockWeather)
static int      s_cwTempC       = 0;
static char     s_cwDesc[48]    = {};
static bool     s_cwUmbrella    = false;
static bool     s_cwValid       = false;
static char     s_cwLocation[48]= {};
static uint32_t s_cwSerial      = 0;
static unsigned long s_cwUpdatedMs = 0;

// Clock-screen draw state
static time_t   s_cwLastSecond     = -1;
static uint32_t s_cwLastSerial     = 0xFFFFFFFFU;

// ---------------------------------------------------------------------------
// Crypto ticker state (written by setCryptoPrices, read by lcdDrawCryptoBubbles)
// ---------------------------------------------------------------------------
static CryptoTicker s_cryptoTickers[CRYPTO_MAX_COINS];
static uint8_t      s_cryptoCount      = 0;
static uint32_t     s_cryptoSerial     = 0;
static uint32_t     s_cryptoLastSerial = 0xFFFFFFFFU;

// Bubble layout constants (240×240 panel)
static constexpr int16_t  CW_CRYPTO_Y  = 198;
static constexpr int16_t  CW_CRYPTO_H  = 40;
static constexpr int16_t  CW_CRYPTO_W  = 76;
static constexpr int16_t  CW_CRYPTO_R  = 5;     // corner radius
static constexpr int16_t  CW_CRYPTO_BX[3] = {3, 81, 159};

// Bubble background colors
static constexpr uint16_t COLOR_BUBBLE_UP   = 0x0340;  // dark green  ~(0,104,0)
static constexpr uint16_t COLOR_BUBBLE_DOWN = 0x9000;  // dark red    ~(144,0,0)
static constexpr uint16_t COLOR_BUBBLE_FLAT = 0x3186;  // dark grey   ~(48,48,48)

/**
 * @brief Format a crypto/stock price for compact display (≤6 chars).
 */
static void formatCryptoPrice(float price, char* buf, size_t sz) {
    if (price >= 10000.f)      snprintf(buf, sz, "%.0fK", price / 1000.f);
    else if (price >= 1000.f)  snprintf(buf, sz, "%.1fK", price / 1000.f);
    else if (price >= 100.f)   snprintf(buf, sz, "%.0f",  price);
    else if (price >= 10.f)    snprintf(buf, sz, "%.1f",  price);
    else if (price >= 1.f)     snprintf(buf, sz, "%.2f",  price);
    else                       snprintf(buf, sz, "%.3f",  price);
}

/**
 * @brief Draw Bitcoin logo: gold circle with white "B" inside.
 */
static void drawBtcLogo(int16_t bx, int16_t by) {
    constexpr uint16_t GOLD = 0xFEA0;  // ~RGB(255, 212, 0)
    int16_t cx = (int16_t)(bx + 13);
    int16_t cy = (int16_t)(by + CW_CRYPTO_H / 2);
    g_lcd.fillCircle(cx, cy, 10, GOLD);
    g_lcd.setTextSize(2);
    g_lcd.setTextColor(LCD_WHITE, GOLD);
    g_lcd.setCursor((int16_t)(cx - 6), (int16_t)(cy - 8));
    g_lcd.print("B");
}

/**
 * @brief Draw Ethereum logo: two-triangle diamond shape.
 */
static void drawEthLogo(int16_t bx, int16_t by) {
    constexpr uint16_t LIGHT = 0xBDF7;  // light grey ~(189,188,189)
    constexpr uint16_t DARK  = 0x8410;  // medium grey ~(130,128,130)
    int16_t cx = (int16_t)(bx + 13);
    int16_t cy = (int16_t)(by + CW_CRYPTO_H / 2);
    // Upper triangle (pointing up)
    g_lcd.fillTriangle(cx, (int16_t)(cy - 11),
                       (int16_t)(cx - 9), cy,
                       (int16_t)(cx + 9), cy, LIGHT);
    // Lower triangle (pointing down)
    g_lcd.fillTriangle(cx, (int16_t)(cy + 11),
                       (int16_t)(cx - 9), cy,
                       (int16_t)(cx + 9), cy, DARK);
    // Thin white mid-line for definition
    g_lcd.drawFastHLine((int16_t)(cx - 9), cy, 18, LCD_WHITE);
}

/**
 * @brief Draw FWRG (ETF) logo: white "$" symbol.
 */
static void drawFwrgLogo(int16_t bx, int16_t by, uint16_t bubbleBg) {
    int16_t tx = (int16_t)(bx + 3);
    int16_t ty = (int16_t)(by + CW_CRYPTO_H / 2 - 8);
    g_lcd.setTextSize(2);
    g_lcd.setTextColor(LCD_WHITE, bubbleBg);
    g_lcd.setCursor(tx, ty);
    g_lcd.print("$");
}

/**
 * @brief Draw all three crypto price bubbles at the bottom of the clock screen.
 */
static void lcdDrawCryptoBubbles() {
    for (int i = 0; i < 3; i++) {
        int16_t bx = CW_CRYPTO_BX[i];
        int16_t by = CW_CRYPTO_Y;

        if (i >= (int)s_cryptoCount) {
            // No data for this slot — erase
            g_lcd.fillRect(bx, by, CW_CRYPTO_W, CW_CRYPTO_H, LCD_BLACK);
            continue;
        }

        const CryptoTicker& t = s_cryptoTickers[i];

        // Bubble background
        uint16_t bg = COLOR_BUBBLE_FLAT;
        if (t.valid) {
            if (t.change7d > 0.05f)       bg = COLOR_BUBBLE_UP;
            else if (t.change7d < -0.05f) bg = COLOR_BUBBLE_DOWN;
        }
        g_lcd.fillRoundRect(bx, by, CW_CRYPTO_W, CW_CRYPTO_H, CW_CRYPTO_R, bg);

        // Logo (left 25 px of bubble)
        if (i == 0)      drawBtcLogo(bx, by);
        else if (i == 1) drawEthLogo(bx, by);
        else             drawFwrgLogo(bx, by, bg);

        // Text area: x+26 .. x+74
        int16_t tx = (int16_t)(bx + 26);

        // Line 1: ticker symbol (padded to 4 chars so redraw clears previous content)
        g_lcd.setTextSize(1);
        g_lcd.setTextColor(LCD_WHITE, bg);
        g_lcd.setCursor(tx, (int16_t)(by + 4));
        char symBuf[5];
        snprintf(symBuf, sizeof(symBuf), "%-4s", t.symbol);
        g_lcd.print(symBuf);

        if (t.valid) {
            // Line 2: price
            char priceBuf[8];
            formatCryptoPrice(t.price, priceBuf, sizeof(priceBuf));
            char line2[9];
            snprintf(line2, sizeof(line2), "$%-6s", priceBuf);
            g_lcd.setCursor(tx, (int16_t)(by + 14));
            g_lcd.print(line2);

            // Line 3: 7d change (or 1d for stocks)
            char changeBuf[9];
            snprintf(changeBuf, sizeof(changeBuf), "%+.1f%%  ", t.change7d);
            changeBuf[7] = '\0';
            g_lcd.setCursor(tx, (int16_t)(by + 25));
            g_lcd.print(changeBuf);
        } else {
            g_lcd.setCursor(tx, (int16_t)(by + 14));
            g_lcd.print("...    ");
            g_lcd.setCursor(tx, (int16_t)(by + 25));
            g_lcd.print("       ");
        }
    }
}

/**
 * @brief Draw a single centered row using text-background overdraw (no fillRect = no flicker).
 *        Safe only when text length never changes between refreshes (fixed-format strings like
 *        "HH:MM:SS"). For variable-length text, call g_lcd.fillRect() on the row first.
 */
static void lcdDrawCenteredRow(int16_t y, uint8_t size, const String& text, uint16_t fg) {
    int16_t textW = (int16_t)(text.length() * 6 * size);
    int16_t x     = (int16_t)(((int16_t)LCD_W - textW) / 2);
    if (x < 0) x  = 0;
    g_lcd.setTextSize(size);
    g_lcd.setTextColor(fg, LCD_BLACK);  // bg=BLACK draws character background without fillRect
    g_lcd.setCursor(x, y);
    g_lcd.print(text);
}

/**
 * @brief Render the clock+weather screen; called every loop when no GIF is playing
 */
static void lcdDrawClockWeather() {
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);

    bool forceFullRedraw = s_wasGifPlaying || s_clockFirstDraw;
    bool secondChanged   = (now != s_cwLastSecond);
    bool weatherChanged  = (s_cwSerial != s_cwLastSerial);
    bool cryptoChanged   = (s_cryptoSerial != s_cryptoLastSerial);

    s_wasGifPlaying  = false;
    s_clockFirstDraw = false;

    if (!forceFullRedraw && !secondChanged && !weatherChanged && !cryptoChanged) {
        return;
    }

    if (forceFullRedraw) {
        g_lcd.fillScreen(LCD_BLACK);
    }

    // --- Time + date (every second) ---
    if (forceFullRedraw || secondChanged) {
        if (now > CW_REASONABLE_EPOCH) {
            char dateBuf[20];
            strftime(dateBuf, sizeof(dateBuf), "%a %d %b", tm_info);
            lcdDrawCenteredRow(CW_DATE_Y, CW_DATE_SIZE, String(dateBuf), LCD_GREY);

            char timeBuf[12];
            snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d",
                     tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
            lcdDrawCenteredRow(CW_TIME_Y, CW_TIME_SIZE, String(timeBuf), LCD_WHITE);
        } else {
            lcdDrawCenteredRow(CW_DATE_Y, CW_DATE_SIZE, "Syncing NTP...", LCD_GREY);
            lcdDrawCenteredRow(CW_TIME_Y, CW_TIME_SIZE, "--:--:--", LCD_GREY);
        }
        s_cwLastSecond = now;
    }

    // --- Weather section (only when data changes or on full redraw) ---
    if ((forceFullRedraw || weatherChanged) && s_cwLocation[0] != '\0') {
        // Temperature — variable length, clear row first
        g_lcd.fillRect(0, CW_TEMP_Y, (int16_t)LCD_W, (int16_t)(8 * CW_TEMP_SIZE), LCD_BLACK);
        if (s_cwValid) {
            char tempBuf[10];
            snprintf(tempBuf, sizeof(tempBuf), "%d'C", s_cwTempC);
            lcdDrawCenteredRow(CW_TEMP_Y, CW_TEMP_SIZE, String(tempBuf), LCD_YELLOW);
        } else {
            lcdDrawCenteredRow(CW_TEMP_Y, CW_TEMP_SIZE, "---", LCD_GREY);
        }

        // Description — reserve 2 lines, clear them first
        g_lcd.fillRect(0, CW_DESC_Y, (int16_t)LCD_W,
                       (int16_t)(2 * 8 * CW_DESC_SIZE), LCD_BLACK);
        if (s_cwValid && s_cwDesc[0] != '\0') {
            lcdDrawTextWrapped(DISPLAY_PADDING, CW_DESC_Y, String(s_cwDesc),
                               CW_DESC_SIZE, LCD_GREY, LCD_BLACK, false);
        } else {
            lcdDrawTextWrapped(DISPLAY_PADDING, CW_DESC_Y, "Fetching weather...",
                               CW_DESC_SIZE, LCD_DARK_GREY, LCD_BLACK, false);
        }

        // Umbrella recommendation — variable length, clear row first
        g_lcd.fillRect(0, CW_UMBRELLA_Y, (int16_t)LCD_W, (int16_t)(8 * CW_UMBRELLA_SIZE), LCD_BLACK);
        if (s_cwValid) {
            if (s_cwUmbrella) {
                lcdDrawCenteredRow(CW_UMBRELLA_Y, CW_UMBRELLA_SIZE, "Bring umbrella!", LCD_CYAN);
            } else {
                lcdDrawCenteredRow(CW_UMBRELLA_Y, CW_UMBRELLA_SIZE, "No umbrella :)", LCD_GREEN);
            }
        }

        // Location + last update time on one line
        if (s_cwUpdatedMs > 0 && s_cwValid) {
            unsigned long elapsedMs = millis() - s_cwUpdatedMs;
            time_t updEpoch = now - static_cast<time_t>(elapsedMs / 1000UL);
            struct tm* u = localtime(&updEpoch);
            char locBuf[48];
            snprintf(locBuf, sizeof(locBuf), "%s  upd %02d:%02d",
                     s_cwLocation, u->tm_hour, u->tm_min);
            lcdDrawCenteredRow(CW_LOC_Y, CW_LOC_SIZE, String(locBuf), LCD_DARK_GREY);
        } else {
            lcdDrawCenteredRow(CW_LOC_Y, CW_LOC_SIZE, String(s_cwLocation), LCD_DARK_GREY);
        }

        s_cwLastSerial = s_cwSerial;
    }

    // --- Crypto bubbles (bottom of screen) ---
    if (forceFullRedraw || cryptoChanged) {
        lcdDrawCryptoBubbles();
        s_cryptoLastSerial = s_cryptoSerial;
    }
}

auto DisplayManager::update() -> void {
    if (s_gif.isPlaying()) {
        s_gif.update();
        s_wasGifPlaying = true;
    } else {
        lcdDrawClockWeather();
    }
}

/**
 * @brief Clear the entire display to black
 *
 * @return void
 */
auto DisplayManager::clearScreen() -> void {
    g_lcd.fillScreen(LCD_BLACK);
    s_wasGifPlaying = true;  // force clock redraw after any explicit clear
}

/**
 * @brief Update the weather data shown on the clock screen
 *
 * @param tempC      Temperature in Celsius
 * @param desc       Short weather description (e.g. "Partly cloudy")
 * @param umbrella   true if rain is expected in the next few hours
 * @param valid      false while still fetching / on HTTP error
 * @param location   City name shown on screen; empty string disables weather display
 */
void DisplayManager::setWeatherData(int tempC, const char* desc, bool umbrella, bool valid,
                                    const char* location) {
    s_cwTempC    = tempC;
    s_cwUmbrella = umbrella;
    s_cwValid    = valid;
    s_cwUpdatedMs = valid ? millis() : s_cwUpdatedMs;
    if (desc)     { strncpy(s_cwDesc,     desc,     sizeof(s_cwDesc)     - 1);
                    s_cwDesc[sizeof(s_cwDesc) - 1]     = '\0'; }
    if (location) { strncpy(s_cwLocation, location, sizeof(s_cwLocation) - 1);
                    s_cwLocation[sizeof(s_cwLocation) - 1] = '\0'; }
    s_cwSerial++;
}

/**
 * @brief Update the crypto price data shown in the bottom bubbles.
 *
 * @param tickers  Array of CryptoTicker (from CryptoClient::getTickers())
 * @param count    Number of valid entries in the array
 */
void DisplayManager::setCryptoPrices(const CryptoTicker* tickers, uint8_t count) {
    if (tickers == nullptr) return;
    uint8_t n = count < CRYPTO_MAX_COINS ? count : CRYPTO_MAX_COINS;
    for (uint8_t i = 0; i < n; i++) {
        s_cryptoTickers[i] = tickers[i];
    }
    s_cryptoCount = n;
    s_cryptoSerial++;
}
