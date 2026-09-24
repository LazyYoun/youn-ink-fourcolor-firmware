/**
 * @file clock.cc
 * @brief Persistent clock component implementation
 *
 * Reads system time (RTC/SNTP) and renders "HH:MM" at top-right corner.
 * Only redraws when minute value changes to minimize EPD refresh.
 */

#include "clock.h"
#include "i18n.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace rawdraw {

Clock::Clock(int x, int y, const lv_font_t* font)
    : x_(x)
    , y_(y)
    , font_(font)
    , color_(RED)
    , last_minute_(-1) {
    time_buf_[0] = '\0';
}

const char* Clock::GetTimeString() {
    static char buf[6];
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    // If year < 2020, SNTP hasn't synced — show dashes instead of invalid time
    if (tm.tm_year + 1900 < 2020) {
        snprintf(buf, sizeof(buf), "--:--");
    } else {
        snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    }
    return buf;
}

std::string Clock::GetDateString(const char* date_format) {
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year + 1900 < 2020) return "--";
    if (date_format && strcmp(date_format, "iso") == 0) {
        unsigned int y = static_cast<unsigned int>(tm.tm_year + 1900);
        unsigned int m = static_cast<unsigned int>(tm.tm_mon + 1);
        unsigned int d = static_cast<unsigned int>(tm.tm_mday);
        char buf[24];
        snprintf(buf, sizeof(buf), "%04u-%02u-%02u", y, m, d);
        return buf;
    }
    // Default: Chinese format "M月D日" (English: "M/D"), no leading zeros
    // for single-digit month/day. Look up the separators fresh each call so
    // they always match the currently active runtime language.
    std::string s;
    s += std::to_string(tm.tm_mon + 1) + i18n::Tr(i18n::StringId::kDateShortMonthSep) + std::to_string(tm.tm_mday) + i18n::Tr(i18n::StringId::kDateShortDaySep);
    return s;
}

Rect Clock::ReservedZone() {
    return { kClockX, kClockY, kClockW, kClockH };
}

void Clock::SetPosition(int x, int y) {
    x_ = x;
    y_ = y;
}

void Clock::SetFont(const lv_font_t* font) {
    font_ = font;
}

void Clock::SetColor(Color color) {
    color_ = color;
}

Rect Clock::GetBounds() const {
    if (!font_) return { x_, y_, kClockW, kClockH };
    int text_w = MeasureTextWidth(time_buf_[0] ? time_buf_ : "00:00", font_);
    return { x_, y_, text_w, font_->line_height };
}

bool Clock::Draw(uint8_t* fb, int width, int height) {
    if (!fb || !font_) return false;

    // Get current time
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);

    // Only redraw when minute changes
    if (tm.tm_min == last_minute_ && time_buf_[0] != '\0') {
        return false;
    }

    // Format time string
    snprintf(time_buf_, sizeof(time_buf_), "%02d:%02d", tm.tm_hour, tm.tm_min);
    last_minute_ = tm.tm_min;

    // Draw time text
    DrawText(fb, width, x_, y_, time_buf_, font_, color_, height);

    return true;
}

bool Clock::Draw(Framebuffer* fb, int screen_width, int screen_height) {
    if (!fb) return false;
    bool changed = false;
    fb->SafeDraw([this, &changed, screen_width, screen_height](uint8_t* buffer) {
        changed = this->Draw(buffer, screen_width, screen_height);
    });
    return changed;
}

bool Clock::DrawWithClear(uint8_t* fb, int width, int height, Color bg_color) {
    if (!fb || !font_) return false;

    // Clear the clock region first
    Rect r = { x_, y_, kClockW, kClockH };
    DrawRect(fb, width, r, bg_color);

    return Draw(fb, width, height);
}

}  // namespace rawdraw
