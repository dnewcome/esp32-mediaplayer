#include "ui.h"
#include <U8g2lib.h>
#include <Wire.h>

namespace ui {

namespace {

// Shares the codec I2C bus (SDA=33, SCL=32). arduino-audiokit initializes
// Wire for the codec; U8g2 reuses it via HW_I2C + U8X8_PIN_NONE clocks.
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);

// Runtime OLED detect: probe 0x3C once at begin(). If the chip doesn't
// ACK (e.g. breadboard A1S with nothing wired to the J1/J2 I2C pins),
// every draw call would otherwise generate i2c_master_transmit errors
// at the log level set for CORE_DEBUG. We skip drawing entirely and
// fall back to the Serial UI.
constexpr uint8_t  OLED_ADDR = 0x3C;
bool               oledPresent = false;

bool probeI2C(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

constexpr int LINE_H  = 10;
constexpr int VISIBLE = 5;

void drawHeader(const char* label) {
    oled.setFont(u8g2_font_6x10_tf);
    oled.drawStr(0, 9, label);
    oled.drawHLine(0, 11, 128);
}

} // namespace

void begin() {
    // U8g2's hardware-I2C path expects Wire already set up with the
    // right pins. Bring it up on the codec bus ourselves, then probe
    // before touching U8g2 so we don't spam the log if the OLED is
    // absent.
    Wire.begin(/*SDA*/33, /*SCL*/32);
    oledPresent = probeI2C(OLED_ADDR);
    Serial.print(F("[ui] SSD1306 @ 0x3C: "));
    Serial.println(oledPresent ? F("present") : F("absent (serial UI only)"));
    if (!oledPresent) return;

    oled.begin();
    oled.setFont(u8g2_font_6x10_tf);
    oled.clearBuffer();
    oled.drawStr(0, 20, "esp32-mediaplayer");
    oled.drawStr(0, 40, "booting...");
    oled.sendBuffer();
}

void showBrowser(const char* const* filenames, int count, int selected) {
    if (!oledPresent) return;
    oled.clearBuffer();
    drawHeader("Files");

    int start = selected - VISIBLE / 2;
    if (start > count - VISIBLE) start = count - VISIBLE;
    if (start < 0) start = 0;

    for (int i = 0; i < VISIBLE && (start + i) < count; ++i) {
        int idx = start + i;
        int yTop = 13 + i * LINE_H;
        int yBase = yTop + 9;
        if (idx == selected) {
            oled.drawBox(0, yTop, 128, LINE_H);
            oled.setDrawColor(0);
            oled.drawStr(2, yBase, filenames[idx]);
            oled.setDrawColor(1);
        } else {
            oled.drawStr(2, yBase, filenames[idx]);
        }
    }
    oled.sendBuffer();
}

void showNowPlaying(const char* filename, float speed, bool paused,
                    bool keylock, bool cueSet) {
    if (!oledPresent) return;
    oled.clearBuffer();
    drawHeader(paused ? "Paused" : "Playing");

    // Mode + cue indicators in the top-right corner of the header row.
    char flags[12];
    snprintf(flags, sizeof(flags), "%s%s",
             keylock ? "KEY " : "    ",
             cueSet  ? "CUE"  : "   ");
    oled.drawStr(80, 9, flags);

    oled.drawStr(0, 26, filename);

    char buf[24];
    snprintf(buf, sizeof(buf), "Speed: %.2fx", speed);
    oled.drawStr(0, 44, buf);

    // Pitch bar: center mark + filled bar from center.
    constexpr int BAR_Y = 54;
    constexpr int BAR_W = 128;
    constexpr int MID   = BAR_W / 2;
    oled.drawFrame(0, BAR_Y, BAR_W, 6);
    int delta = (int)((speed - 1.0f) * 60.0f);
    if (delta > 60)  delta = 60;
    if (delta < -60) delta = -60;
    if (delta >= 0) oled.drawBox(MID, BAR_Y + 1, delta, 4);
    else            oled.drawBox(MID + delta, BAR_Y + 1, -delta, 4);
    oled.drawVLine(MID, BAR_Y - 1, 8);
    oled.sendBuffer();
}

void showMessage(const char* msg) {
    if (!oledPresent) return;
    oled.clearBuffer();
    oled.setFont(u8g2_font_6x10_tf);
    oled.drawStr(0, 32, msg);
    oled.sendBuffer();
}

} // namespace ui
