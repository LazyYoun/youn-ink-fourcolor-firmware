/**
 * @file weather_detail_renderer.cc
 * @brief Weather detail page renderer with hourly timeline
 */

#include "weather_detail_renderer.h"

#include "i18n.h"

#include "rawdraw/components/footer_bar.h"
#include "rawdraw/components/modal.h"
#include "rawdraw/layout_utils.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/style.h"
#include "rawdraw/theme.h"

#include <algorithm>
#include <cstdio>

extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;
extern const lv_font_t weather_icons_16;

namespace rawdraw {
namespace {

const char* IconGlyphForCode(const std::string& icon_code, const std::string& weather_text) {
    auto code = icon_code.empty() ? -1 : atoi(icon_code.c_str());
    if (code == 100 || weather_text.find("晴") != std::string::npos) return "\xef\x83\x9e";
    if ((code >= 101 && code <= 104) || weather_text.find("云") != std::string::npos) return "\xef\x83\x82";
    if ((code >= 300 && code <= 399) || weather_text.find("雨") != std::string::npos) return "\xef\x83\xa9";
    if ((code >= 400 && code <= 499) || weather_text.find("雪") != std::string::npos) return "\xef\x8b\x9c";
    return "\xef\x83\x9e";
}

}  // namespace

WeatherDetailRenderer::WeatherDetailRenderer()
    : font_(&SourceHanSansSC_Regular_slim),
      title_font_(&SourceHanSansSC_Medium_slim),
      icon_font_(&weather_icons_16) {}

WeatherDetailRenderer::~WeatherDetailRenderer() {}

void WeatherDetailRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    selected_hour_ = 0;
    detail_open_ = false;
    needs_full_refresh_ = true;
}

void WeatherDetailRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;
    const auto& theme = ThemeManager::Get();
    const PaintStyle bg_style = theme.Style(ThemeToken::BackgroundPrimary);
    const PaintStyle card_style = theme.Component(ComponentRole::CardDefault);
    const PaintStyle selected_style = theme.Component(ComponentRole::SettingsSelected);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    const Color accent = theme.ColorFor(ThemeToken::Accent);

    const int content_top = Style::kStatusBarHeight + 8;
    DrawStyledRect(fb, width, {0, Style::kStatusBarHeight, width, height - Style::kStatusBarHeight}, bg_style);

    if (!has_data_) {
        Modal modal;
        modal.SetTitle(i18n::Tr(i18n::StringId::kNoWeatherDetails));
        modal.SetBodyFooter(i18n::Tr(i18n::StringId::kWaitingForWeatherData));
        modal.CenterInScreen(width, height, 52);
        modal.Draw(fb, width, height);
    } else {
        const char* glyph = IconGlyphForCode(data_.weather_icon, data_.weather_text);
        DrawIcon(fb, width, 86, content_top + 38, glyph, icon_font_, accent);
        char temp_buf[20];
        snprintf(temp_buf, sizeof(temp_buf), "%s°C", data_.temp.empty() ? "--" : data_.temp.c_str());
        DrawText(fb, width, 170,
                 InkCenteredTextTopY(title_font_, temp_buf, content_top + 68, 0),
                 temp_buf, title_font_, accent);
        const char* weather_text = data_.weather_text.empty() ? "--" : data_.weather_text.c_str();
        DrawText(fb, width, 178,
                 InkCenteredTextTopY(font_, weather_text, content_top + 94, 0),
                 weather_text, font_, text);

        Rect metrics{238, content_top + 16, 130, 108};
        DrawStyledRoundRect(fb, width, height, metrics, Style::kBorderRadiusMD, card_style);
        const char* labels[] = {i18n::Tr(i18n::StringId::kFeelsLike), i18n::Tr(i18n::StringId::kHumidity),
                                 i18n::Tr(i18n::StringId::kVisibility), i18n::Tr(i18n::StringId::kPressure)};
        std::string values[] = {
            (data_.feels_like.empty() ? (data_.temp.empty() ? "--" : data_.temp) : data_.feels_like) + "°C",
            (data_.humidity.empty() ? "--" : data_.humidity) + "%",
            "20km",
            "1012hPa",
        };
        for (int i = 0; i < 4; ++i) {
            const int center_y = metrics.y + 20 + i * 24;
            DrawText(fb, width, metrics.x + 16,
                     InkCenteredTextTopY(font_, labels[i], center_y, 0),
                     labels[i], font_, secondary);
            const int val_w = MeasureTextWidth(values[i].c_str(), font_);
            DrawText(fb, width, metrics.x + metrics.w - val_w - 16,
                     InkCenteredTextTopY(font_, values[i].c_str(), center_y, 0),
                     values[i].c_str(), font_, text);
        }

        Rect timeline{50, 186, width - 100, 88};
        DrawStyledRoundRect(fb, width, height, timeline, Style::kBorderRadiusMD, card_style);

        const int count = static_cast<int>(hourly_.size());
        if (count > 0) {
            const int visible = std::min(5, count);
            const int start = std::max(0, std::min(selected_hour_ - 2, count - visible));
            const int usable_x = timeline.x + 10;
            const int usable_y = timeline.y + 12;
            const int usable_w = timeline.w - 20;
            const int col_w = usable_w / std::max(1, visible);

            for (int col = 0; col < visible; ++col) {
                const int i = start + col;
                const auto& point = hourly_[i];
                const int cx = usable_x + col * col_w + col_w / 2;
                const int label_w = MeasureTextWidth(point.label.c_str(), font_);
                DrawText(fb, width, cx - label_w / 2,
                         InkCenteredTextTopY(font_, point.label.c_str(), usable_y + 8, 0),
                         point.label.c_str(), font_, i == selected_hour_ ? accent : secondary);

                const char* hour_glyph = IconGlyphForCode(point.icon_code, point.weather_text);
                const int glyph_w = MeasureTextWidth(hour_glyph, icon_font_);
                DrawIcon(fb, width, cx - glyph_w / 2, usable_y + 28, hour_glyph, icon_font_,
                         i == selected_hour_ ? accent : text);

                char hour_temp[12];
                snprintf(hour_temp, sizeof(hour_temp), "%d°C", static_cast<int>(point.temp));
                int temp_w = MeasureTextWidth(hour_temp, font_);
                DrawText(fb, width, cx - temp_w / 2,
                         InkCenteredTextTopY(font_, hour_temp, usable_y + 66, 0),
                         hour_temp, font_, text);
                if (i == selected_hour_) {
                    DrawHLine(fb, width, usable_y + 76, cx - 14, cx + 14, selected_style.bg);
                }
            }
        }
    }

    if (detail_open_ && !hourly_.empty()) {
        DrawHourDetailModal(fb, width, height);
    }

    needs_full_refresh_ = false;
}

bool WeatherDetailRenderer::HandleInput(const ButtonEvent& event) {
    if (detail_open_) {
        if (event.type == ButtonEvent::kBootClick || event.type == ButtonEvent::kBootLongPress) {
            detail_open_ = false;
            needs_full_refresh_ = true;
            return true;
        }
        return true;
    }

    switch (event.type) {
        case ButtonEvent::kUpClick:
            if (selected_hour_ > 0) {
                selected_hour_--;
                needs_full_refresh_ = true;
                return true;
            }
            break;
        case ButtonEvent::kDownClick:
            if (selected_hour_ < static_cast<int>(hourly_.size()) - 1) {
                selected_hour_++;
                needs_full_refresh_ = true;
                return true;
            }
            break;
#if 0
        case ButtonEvent::kBootClick:
            if (!hourly_.empty()) {
                detail_open_ = true;
                needs_full_refresh_ = true;
                return true;
            }
            break;
#endif
        default:
            break;
    }
    return false;
}

void WeatherDetailRenderer::Update(const WeatherData& data) {
    data_ = data;
    has_data_ = true;
    if (hourly_.empty()) {
        BuildFallbackTimeline();
    }
    selected_hour_ = std::max(0, std::min(selected_hour_, static_cast<int>(hourly_.size()) - 1));
    needs_full_refresh_ = true;
}

void WeatherDetailRenderer::SetHourlyForecast(const std::vector<WeatherHourPoint>& points) {
    hourly_ = points;
    selected_hour_ = std::max(0, std::min(selected_hour_, static_cast<int>(hourly_.size()) - 1));
    needs_full_refresh_ = true;
}

void WeatherDetailRenderer::DrawHourDetailModal(uint8_t* fb, int width, int height) {
    const auto& point = hourly_[selected_hour_];
    const auto& theme = ThemeManager::Get();
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    const Color accent = theme.ColorFor(ThemeToken::Accent);
    Modal modal;
    modal.SetTitle(i18n::Tr(i18n::StringId::kHourlyDetail));
    modal.SetBodyFooter(i18n::Tr(i18n::StringId::kBootToClose));
    modal.CenterInScreen(width, height, 42);
    modal.Draw(fb, width, height);

    const Rect body = modal.GetContentBounds();
    char temp_buf[16];
    snprintf(temp_buf, sizeof(temp_buf), "%d°C", static_cast<int>(point.temp));
    DrawText(fb, width, body.x, body.y, point.label.c_str(), title_font_, accent);
    DrawText(fb, width, body.x, body.y + 24, temp_buf, title_font_, text);
    DrawText(fb, width, body.x, body.y + 48, point.weather_text.c_str(), font_, secondary);
}

void WeatherDetailRenderer::BuildFallbackTimeline() {
    hourly_.clear();
    const int now_temp = data_.temp.empty() ? data_.temp_int : atoi(data_.temp.c_str());
    const char* labels[] = {i18n::Tr(i18n::StringId::kNow),   i18n::Tr(i18n::StringId::k3h),  i18n::Tr(i18n::StringId::k6h),
                             i18n::Tr(i18n::StringId::k9h),    i18n::Tr(i18n::StringId::k12h), i18n::Tr(i18n::StringId::k15h)};
    static const int offsets[] = {0, -1, -2, 0, 2, 1};
    for (int i = 0; i < 6; ++i) {
        hourly_.push_back({labels[i], data_.weather_icon, data_.weather_text, now_temp + offsets[i]});
    }
}

}  // namespace rawdraw
