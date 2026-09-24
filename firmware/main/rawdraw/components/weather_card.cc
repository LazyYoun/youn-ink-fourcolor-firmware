/**
 * @file weather_card.cc
 * @brief Weather card component implementation
 *
 * Layout for 400x300 1bpp ePaper:
 *
 * ┌────────────────────────────────────────┐
 * │  ☀️   25°C           杭州              │
 * │       体感 27°C      14:30 更新        │
 * │       东南风 3级     湿度 45%          │
 * └────────────────────────────────────────┘
 *
 * All x coordinates aligned to 8-byte boundary via align_x8().
 * Avoids clock zone (X=320, W=80, H=32 at top-right).
 */

#include "weather_card.h"
#include "common/weather_api.h"
#include "i18n.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/style.h"
#include "rawdraw/theme.h"
#include "rawdraw/clock.h"
#include "rawdraw/framebuffer.h"
#include <esp_timer.h>
#include <cstring>
#include <cstdio>

// External font references
extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;
extern const lv_font_t font_zectrix_16_1;
extern const lv_font_t weather_icons_48;
extern const lv_font_t weather_icons_16;

namespace rawdraw {

WeatherCard::WeatherCard(int x, int y, int w)
    : x_(x)
    , y_(y)
    , w_(w)
    , has_data_(false)
    , temp_font_(&weather_icons_48)       // Large font for temperature digits
    , info_font_(&SourceHanSansSC_Regular_slim)  // Normal text
    , icon_font_(&weather_icons_48) {           // Weather icons
    refresh_tracker_init(&refresh_);
}

WeatherCard::~WeatherCard() {}

void WeatherCard::SetPosition(int x, int y) {
    x_ = x;
    y_ = y;
}

void WeatherCard::SetWidth(int w) {
    w_ = w;
}

void WeatherCard::SetData(const WeatherData& data) {
    data_ = data;
    has_data_ = true;
    refresh_mark_dirty(&refresh_);
}

void WeatherCard::SetCityName(const char* name) {
    city_name_ = name;
}

Rect WeatherCard::GetBounds() const {
    return {x_, y_, w_, kWeatherCardMaxHeight};
}

// ============================================================
// Weather icon mapping to weather_icons_48 font
// ============================================================

void WeatherCard::DrawWeatherIcon(uint8_t* fb, int width, int x, int y, WeatherIcon icon) {
    // weather_icons_48 uses FontAwesome-compatible codes
    const char* icon_code;
    switch (icon) {
        case WeatherIcon::Sunny:
            icon_code = "\xef\x83\x9e";   // U+F0DE (sun)
            break;
        case WeatherIcon::Cloudy:
            icon_code = "\xef\x82\x82";   // U+F082 (cloud)
            break;
        case WeatherIcon::Overcast:
            icon_code = "\xef\x83\x82";   // U+F0C2 (cloud overcast)
            break;
        case WeatherIcon::Rain:
            icon_code = "\xef\x83\xa9";   // U+F0E9 (rain)
            break;
        case WeatherIcon::Snow:
            icon_code = "\xef\x8b\x9c";   // U+F2DC (snowflake)
            break;
        case WeatherIcon::Fog:
            icon_code = "\xef\x9a\x9f";   // U+F69F (smog/fog)
            break;
        default:
            icon_code = "\xef\x83\x9e";   // Default: sun
            break;
    }
    DrawIcon(fb, width, x, y, icon_code, icon_font_,
             ThemeManager::Get().ColorFor(ThemeToken::Accent));
}

// ============================================================
// Rendering
// ============================================================

bool WeatherCard::Draw(uint8_t* fb, int width, int height) {
    if (!fb || !has_data_) return false;

    // Align x to 8-byte boundary for EPD hardware
    Rect bounds = align_x8({x_, y_, w_, kWeatherCardMaxHeight});

    // Clamp to screen bounds
    bounds = clamp_rect(bounds, width, height);
    if (rect_area(bounds) <= 0) return false;

    const auto& theme = ThemeManager::Get();
    const PaintStyle card_style = theme.Component(ComponentRole::CardElevated);
    const PaintStyle badge_style = theme.Style(ThemeToken::Badge);
    const PaintStyle chip_style = theme.Component(ComponentRole::CardDefault);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    const Color border = theme.ColorFor(ThemeToken::Border);

    // Draw a layered card with theme-owned fill, subtle offset shadow and border.
    DrawStyledRect(fb, width, bounds, theme.Style(ThemeToken::BackgroundPrimary));
    DrawStyledRoundRect(fb, width, height, {bounds.x + 2, bounds.y + 2, bounds.w, bounds.h},
                        Style::kCardRadius, theme.Style(ThemeToken::Shadow));
    DrawStyledRoundRect(fb, width, height, bounds, Style::kBorderRadiusMD, card_style);

    const int card_left = bounds.x + 14;
    const int card_top = bounds.y + 12;
    const int card_right = bounds.x + bounds.w - 14;

    // Header tag
    const char* city = city_name_.empty() ? data_.city.c_str() : city_name_.c_str();
    const char* city_fallback = i18n::Tr(i18n::StringId::kWeather);
    int city_tag_w = MeasureTextWidth(city && city[0] ? city : city_fallback, info_font_) + 18;
    DrawStyledRoundRect(fb, width, height, {card_left, card_top, city_tag_w, 16},
                        Style::kBorderRadiusPill, badge_style);
    DrawStyledText(fb, width, card_left + 9, card_top + 1, city && city[0] ? city : city_fallback,
                   info_font_, badge_style, height);

    if (!data_.update_time.empty()) {
        char update_buf[32];
        snprintf(update_buf, sizeof(update_buf), i18n::Tr(i18n::StringId::kUpdatedS), data_.update_time.c_str());
        int update_w = MeasureTextWidth(update_buf, info_font_);
        DrawText(fb, width, card_right - update_w, card_top + 1, update_buf, info_font_, secondary, height);
    }

    const int body_y = card_top + 26;
    WeatherIcon icon = ParseWeatherIcon(data_.weather_text.c_str());
    const int icon_x = card_left;
    const int icon_y = body_y + 8;
    DrawWeatherIcon(fb, width, icon_x, icon_y, icon);

    const int col2_x = icon_x + 58;
    if (!data_.temp.empty()) {
        char temp_display[20];
        snprintf(temp_display, sizeof(temp_display), "%s°C", data_.temp.c_str());
        DrawText(fb, width, col2_x, body_y + 2, temp_display, info_font_, text, height);
    }

    const int desc_y = body_y + 22;
    if (!data_.weather_text.empty()) {
        DrawText(fb, width, col2_x, desc_y, data_.weather_text.c_str(), info_font_, text, height);
    }
    if (!data_.feels_like.empty()) {
        char feels_buf[32];
        snprintf(feels_buf, sizeof(feels_buf), i18n::Tr(i18n::StringId::kFeelsLikeSC), data_.feels_like.c_str());
        DrawText(fb, width, col2_x + 60, desc_y, feels_buf, info_font_, secondary, height);
    }

    const int stats_y = body_y + 46;
    auto draw_chip = [&](int x, const char* text) {
        int chip_w = MeasureTextWidth(text, info_font_) + 14;
        DrawStyledRoundRect(fb, width, height, {x, stats_y, chip_w, 18},
                            Style::kBorderRadiusPill, chip_style);
        DrawStyledText(fb, width, x + 7, stats_y + 2, text, info_font_, chip_style, height);
        return chip_w;
    };

    int chip_x = col2_x;
    if (!data_.wind_dir.empty()) {
        char wind_buf[64];
        if (!data_.wind_scale.empty()) {
            snprintf(wind_buf, sizeof(wind_buf), i18n::Tr(i18n::StringId::kSLevelS),
                     data_.wind_dir.c_str(), data_.wind_scale.c_str());
        } else {
            snprintf(wind_buf, sizeof(wind_buf), "%s", data_.wind_dir.c_str());
        }
        chip_x += draw_chip(chip_x, wind_buf) + 6;
    }
    if (!data_.humidity.empty()) {
        char hum_buf[32];
        snprintf(hum_buf, sizeof(hum_buf), i18n::Tr(i18n::StringId::kHumidityS), data_.humidity.c_str());
        draw_chip(chip_x, hum_buf);
    }

    // Bottom summary strip
    const int strip_y = bounds.y + bounds.h - 28;
    DrawHLine(fb, width, strip_y - 6, bounds.x + 12, bounds.x + bounds.w - 12, border);
    if (!data_.weather_text.empty()) {
        DrawText(fb, width, card_left, strip_y, data_.weather_text.c_str(), info_font_, text, height);
    }
    if (!data_.feels_like.empty()) {
        char footer_buf[32];
        snprintf(footer_buf, sizeof(footer_buf), i18n::Tr(i18n::StringId::kCurrentSC), data_.temp.c_str());
        int footer_w = MeasureTextWidth(footer_buf, info_font_);
        DrawText(fb, width, card_right - footer_w, strip_y, footer_buf, info_font_, secondary, height);
    }

    // Update refresh counter
    refresh_update_counter(&refresh_, esp_timer_get_time());

    return true;
}

bool WeatherCard::Draw(Framebuffer* fb, int screen_width, int screen_height) {
    if (!fb) return false;
    bool drawn = false;
    fb->SafeDraw([&drawn, screen_width, screen_height, this](uint8_t* buffer) {
        drawn = Draw(buffer, screen_width, screen_height);
    });
    return drawn;
}

}  // namespace rawdraw
