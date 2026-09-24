/**
 * @file wifi_renderer.cc
 * @brief Modernized WiFi status page renderer implementation
 *
 * Design for 400x300 1bpp ePaper:
 *
 * CONNECTING state:
 * - Large centered WiFi icon (blinking)
 * - "连接中..." text below
 * - Progress bar at bottom
 *
 * CONNECTED state:
 * - Card at top: WiFi icon + SSID + signal bars
 * - Server status card below: icon + status text + URI
 * - "按 BOOT 返回" hint at bottom
 *
 * DISCONNECTED state:
 * - Large X/disconnected icon centered
 * - "已断开" text
 * - Action hints below
 */

#include "wifi_renderer.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/style.h"
#include "rawdraw/layout_utils.h"  // FIX: 使用 InkCenteredTextTopYInBox 替代 line_height 居中
#include "rawdraw/theme.h"
#include "rawdraw/components/progress_bar.h"
#include "rawdraw/components/panel.h"
#include "i18n.h"
#include <algorithm>
#include <cstdio>
#include <string>

// External font references
extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;
extern const lv_font_t font_zectrix_16_1;
extern const lv_font_t weather_icons_48;
extern const lv_font_t weather_icons_16;

namespace rawdraw {

namespace {

std::string FitTextToWidth(const std::string& text, const lv_font_t* font, int max_width) {
    if (!font || max_width <= 0 || text.empty()) return "";
    if (MeasureTextWidth(text.c_str(), font) <= max_width) return text;

    static const std::string kEllipsis = "...";
    const int ellipsis_w = MeasureTextWidth(kEllipsis.c_str(), font);
    if (ellipsis_w >= max_width) return "";

    std::string fitted;
    const char* p = text.c_str();
    while (*p) {
        const char* start = p;
        uint32_t ch = utf8_next(&p);
        if (ch == 0) break;

        std::string next = fitted;
        next.append(start, p - start);
        next += kEllipsis;
        if (MeasureTextWidth(next.c_str(), font) > max_width) break;
        fitted.append(start, p - start);
    }

    if (fitted.empty()) return "";
    fitted += kEllipsis;
    return fitted;
}

}  // namespace

WifiRenderer::WifiRenderer()
    : is_blinking_(false)
    , blink_frame_(0)
    , font_(&SourceHanSansSC_Regular_slim)
    , title_font_(&SourceHanSansSC_Medium_slim)
    , icon_font_(&font_zectrix_16_1)
    , large_icon_font_(&weather_icons_48) {
}

WifiRenderer::~WifiRenderer() {}

void WifiRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    needs_full_refresh_ = true;
}

void WifiRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;

    // Note: Do NOT Clear framebuffer here - it's managed by RawDrawUiManager::RenderAll
    // which calls DrawStatusBar first, then page Render.
    // Clearing here would erase the status bar.

    // === Content based on state ===
    switch (status_.state) {
        case WifiState::Connecting:
            RenderConnecting(fb, width, height);
            break;
        case WifiState::Connected:
            RenderConnected(fb, width, height);
            break;
        case WifiState::Disconnected:
            RenderDisconnected(fb, width, height);
            break;
    }

    needs_full_refresh_ = false;
}

// ============================================================
// CONNECTING STATE
// ============================================================

void WifiRenderer::RenderConnecting(uint8_t* fb, int width, int height) {
    const auto& theme = ThemeManager::Get();
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary_text = theme.ColorFor(ThemeToken::TextSecondary);
    const Color accent = theme.ColorFor(ThemeToken::Accent);
    // Large WiFi icon (centered, blinking)
    const int icon_size = Style::kFontSizeXL;  // 48px
    blink_frame_++;
    const bool visible = (blink_frame_ % 20) < 14;  // Blink: on for 14 frames

    if (visible) {
        const int icon_x = (width - icon_size) / 2;
        const int icon_y = Style::kStatusBarHeight + Style::kSpacingXL;
        DrawWifiIcon(fb, width, icon_x, icon_y, icon_size, accent);
    }

    // Status text
    const char* status_text = i18n::Tr(i18n::StringId::kConnecting2);
    int text_w = MeasureTextWidth(status_text, font_);
    int text_x = (width - text_w) / 2;
    int text_y = Style::kStatusBarHeight + Style::kSpacingXL + icon_size + Style::kSpacingLG;
    DrawText(fb, width, text_x, text_y, status_text, font_, text);

    // SSID text if available
    if (!status_.ssid.empty()) {
        std::string ssid = FitTextToWidth(status_.ssid, font_, width - Style::kSpacingXL * 2);
        int ssid_w = MeasureTextWidth(ssid.c_str(), font_);
        int ssid_x = (width - ssid_w) / 2;
        int ssid_y = text_y + font_->line_height + Style::kSpacingSM;
        DrawText(fb, width, ssid_x, ssid_y, ssid.c_str(), font_, secondary_text);
    }

    // Progress bar
    if (status_.progress > 0) {
        const int bar_y = height - Style::kSpacingXXL - Style::kProgressHeight;
        const int bar_w = width - Style::kSpacingXL * 2;
        const int bar_x = Style::kSpacingXL;

        ProgressBar bar(bar_x, bar_y, bar_w, Style::kProgressHeight);
        bar.SetValue(status_.progress);
        bar.Draw(fb, width, height);

        // Progress percentage text
        char pct_buf[8];
        snprintf(pct_buf, sizeof(pct_buf), "%d%%", status_.progress);
        int pct_w = MeasureTextWidth(pct_buf, font_);
        DrawText(fb, width, (width - pct_w) / 2,
                 bar_y - font_->line_height - Style::kSpacingXS,
                 pct_buf, font_, text);
    }

    // Hint text
    const char* hint = i18n::Tr(i18n::StringId::kPleaseWait);
    int hint_w = MeasureTextWidth(hint, font_);
    DrawText(fb, width, (width - hint_w) / 2,
             height - font_->line_height - Style::kSpacingSM, hint, font_, secondary_text);
}

// ============================================================
// CONNECTED STATE
// ============================================================

void WifiRenderer::RenderConnected(uint8_t* fb, int width, int height) {
    const auto& theme = ThemeManager::Get();
    const PaintStyle card_style = theme.Component(ComponentRole::CardDefault);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary_text = theme.ColorFor(ThemeToken::TextSecondary);
    const Color border = theme.ColorFor(ThemeToken::Border);
    const Color accent = theme.ColorFor(ThemeToken::Accent);
    const int content_top = Style::kStatusBarHeight + Style::kSpacingSM;
    const int card_w = width - Style::kSpacingXL;
    const int card_x = Style::kSpacingXL / 2;

    // === WiFi Info Card ===
    int card_y = content_top;
    int card_h = 80;

    // Card background
    DrawStyledRoundRect(fb, width, height, {card_x, card_y, card_w, card_h},
                        Style::kBorderRadiusLG, card_style);

    // Separator line below title area
    DrawHLine(fb, width, card_y + Style::kPanelTitleHeight - 1,
              card_x + Style::kBorderRadiusLG, card_x + card_w - Style::kBorderRadiusLG, border);

    // Title text
    DrawText(fb, width, card_x + Style::kPanelPadding,
             card_y + Style::kSpacingXS, "WiFi", title_font_, text);

    // WiFi icon (left side of content)
    const int wifi_icon_size = 32;
    const int wifi_icon_x = card_x + Style::kPanelPadding;
    const int wifi_icon_y = card_y + Style::kPanelTitleHeight + Style::kSpacingSM;
    const int signal_pct = SignalToPercent(status_.signal_strength);
    const int bars_x = card_x + card_w - Style::kPanelPadding - 45;
    const int bars_y = wifi_icon_y + 4;
    DrawWifiIcon(fb, width, wifi_icon_x, wifi_icon_y, wifi_icon_size, accent);

    // SSID text (center-right)
    if (!status_.ssid.empty()) {
        const int ssid_x = wifi_icon_x + wifi_icon_size + Style::kSpacingSM;
        const int ssid_max_w = std::max(0, bars_x - Style::kSpacingSM - ssid_x);
        std::string ssid = FitTextToWidth(status_.ssid, title_font_, ssid_max_w);
        DrawText(fb, width, ssid_x, wifi_icon_y, ssid.c_str(), title_font_, text);
    }

    // Signal bars (right side)
    DrawSignalBars(fb, width, bars_x, bars_y, 5, signal_pct);

    // Signal text below bars
    char signal_buf[16];
    snprintf(signal_buf, sizeof(signal_buf), "%d%%", signal_pct);
    int sig_w = MeasureTextWidth(signal_buf, font_);
    DrawText(fb, width, bars_x + (45 - sig_w) / 2,
             bars_y + 28, signal_buf, font_, secondary_text);

    // === Server Status Card ===
    int server_y = card_y + card_h + Style::kSpacingSM;
    int server_h = 76;

    DrawServerCard(fb, width, card_x, server_y, card_w,
                   status_.server_connected, status_.server_uri);

    // === Bottom hint ===
    const char* hint = i18n::Tr(i18n::StringId::kPressBootToReturn);
    int hint_w = MeasureTextWidth(hint, font_);
    DrawText(fb, width, (width - hint_w) / 2,
             height - font_->line_height - Style::kSpacingSM, hint, font_, secondary_text);
}

void WifiRenderer::DrawServerCard(uint8_t* fb, int width, int x, int y,
                                   int w, bool connected,
                                   const std::string& uri) {
    const auto& theme = ThemeManager::Get();
    const PaintStyle card_style = theme.Component(ComponentRole::CardDefault);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary_text = theme.ColorFor(ThemeToken::TextSecondary);
    const Color status_color = theme.ColorFor(connected ? ThemeToken::SuccessLike : ThemeToken::Warning);
    const Color border = theme.ColorFor(ThemeToken::Border);
    const int card_h = 76;

    // Card background
    DrawStyledRoundRect(fb, width, 300, {x, y, w, card_h},
                        Style::kBorderRadiusLG, card_style);

    // Separator
    DrawHLine(fb, width, y + Style::kPanelTitleHeight - 1,
              x + Style::kBorderRadiusLG, x + w - Style::kBorderRadiusLG, border);

    // Title
    DrawText(fb, width, x + Style::kPanelPadding,
             y + Style::kSpacingXS, i18n::Tr(i18n::StringId::kServer), title_font_, text);

    // Status icon + text
    const int icon_y = y + Style::kPanelTitleHeight + Style::kSpacingSM;

    if (connected) {
        // F1 FIX: Draw checkmark as TEXT instead of broken icon font
        const char* check = "[v]";
        DrawText(fb, width, x + Style::kPanelPadding, icon_y,
                 check, font_, status_color);
        DrawText(fb, width, x + Style::kPanelPadding + Style::kFontSizeSM + Style::kSpacingSM,
                 icon_y, i18n::Tr(i18n::StringId::kConnected), font_, text);

        // Server URI
        if (!uri.empty()) {
            const int uri_x = x + Style::kPanelPadding + Style::kSpacingXS;
            const int uri_max_w = std::max(0, w - Style::kPanelPadding * 2 - Style::kSpacingXS * 2);
            std::string display_uri = FitTextToWidth(uri, font_, uri_max_w);
            DrawText(fb, width, uri_x,
                     icon_y + font_->line_height + Style::kSpacingXS,
                     display_uri.c_str(), font_, secondary_text);
        }
    } else {
        // F1 FIX: Draw X as TEXT instead of broken icon font
        const char* cross = "[X]";
        DrawText(fb, width, x + Style::kPanelPadding, icon_y,
                 cross, font_, status_color);
        DrawText(fb, width, x + Style::kPanelPadding + Style::kFontSizeSM + Style::kSpacingSM,
                 icon_y, i18n::Tr(i18n::StringId::kNotConnected), font_, text);
    }
}

// ============================================================
// DISCONNECTED STATE
// ============================================================

void WifiRenderer::RenderDisconnected(uint8_t* fb, int width, int height) {
    const auto& theme = ThemeManager::Get();
    const PaintStyle button_style = theme.Component(ComponentRole::ButtonSelected);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary_text = theme.ColorFor(ThemeToken::TextSecondary);
    const Color danger = theme.ColorFor(ThemeToken::Danger);
    const Color border = theme.ColorFor(ThemeToken::Border);
    const int center_y = height / 2 - Style::kSpacingXXL;

    // F1 FIX: Draw large X as TEXT instead of broken icon font
    const char* cross_text = "X";
    const int cross_icon_size = Style::kFontSizeXL;  // 48px
    int cross_w = MeasureTextWidth(cross_text, &weather_icons_48);
    int cross_center_x = (width - cross_w) / 2;
    DrawText(fb, width, cross_center_x, center_y, cross_text,
             large_icon_font_, danger);

    // Status text
    const char* status_text = i18n::Tr(i18n::StringId::kNetworkDisconnected);
    int text_w = MeasureTextWidth(status_text, title_font_);
    int text_x = (width - text_w) / 2;
    int text_y = center_y + cross_icon_size + Style::kSpacingLG;
    DrawText(fb, width, text_x, text_y, status_text, title_font_, text);

    // Divider
    const int divider_y = text_y + title_font_->line_height + Style::kSpacingSM;
    const int divider_w = 120;
    DrawHLine(fb, width, divider_y, (width - divider_w) / 2,
              (width + divider_w) / 2, border);

    // Action hints
    const int actions_y = divider_y + Style::kSpacingSM;

    // Primary action
    const char* primary = i18n::Tr(i18n::StringId::kPressBootToReconnect);
    int primary_w = MeasureTextWidth(primary, font_);
    int primary_x = (width - primary_w) / 2;

    // Draw as a button-like element
    const int btn_h = font_->line_height + Style::kSpacingSM * 2;
    const int btn_w = primary_w + Style::kSpacingXL;
    const int btn_x = (width - btn_w) / 2;
    const int btn_y = actions_y;

    DrawStyledRoundRect(fb, width, height, {btn_x, btn_y, btn_w, btn_h},
                        Style::kBorderRadiusPill, button_style);
    // FIX: 改用 InkCenteredTextTopYInBox，避免 line_height 居中导致中文偏上
    // 参见 wiki/projects/notellm-baseline-alignment.md
    DrawText(fb, width, btn_x + Style::kSpacingMD,
             InkCenteredTextTopYInBox(font_, primary, btn_y, btn_h, 0),
             primary, font_, button_style.fg);

    // Secondary hint
    const char* secondary_hint = i18n::Tr(i18n::StringId::kHoldBootToEnterWifiSetup);
    int sec_w = MeasureTextWidth(secondary_hint, font_);
    int sec_x = (width - sec_w) / 2;
    int sec_y = btn_y + btn_h + Style::kSpacingSM;
    if (sec_y + font_->line_height <= height - Style::kSpacingSM) {
        DrawText(fb, width, sec_x, sec_y, secondary_hint, font_, secondary_text);
    }
}

// ============================================================
// SIGNAL BARS
// ============================================================

void WifiRenderer::DrawSignalBars(uint8_t* fb, int width, int x, int y,
                                   int bar_count, int signal_pct) {
    const auto& theme = ThemeManager::Get();
    const Color active_color = theme.ColorFor(ThemeToken::SuccessLike);
    const Color inactive_bg = theme.ColorFor(ThemeToken::BackgroundSecondary);
    const Color border = theme.ColorFor(ThemeToken::Border);
    const int bar_w = 6;
    const int bar_gap = 3;
    const int max_bar_h = 24;
    const int total_w = bar_count * (bar_w + bar_gap) - bar_gap;

    for (int i = 0; i < bar_count; ++i) {
        const int threshold = (i + 1) * 100 / bar_count;
        const bool active = signal_pct >= threshold;

        // Bar height scales with position
        const int bar_h = max_bar_h * (i + 1) / bar_count;
        const int bar_x = x + i * (bar_w + bar_gap);
        const int bar_y = y + (max_bar_h - bar_h);

        if (active) {
            DrawRoundRect(fb, width, {bar_x, bar_y, bar_w, bar_h},
                          Style::kBorderRadiusSM, active_color, active_color, 0);
        } else {
            DrawRoundRect(fb, width, {bar_x, bar_y, bar_w, bar_h},
                          Style::kBorderRadiusSM, inactive_bg, border, Style::kBorderThin);
        }
    }
}

// ============================================================
// WIFI ICON
// ============================================================

void WifiRenderer::DrawWifiIcon(uint8_t* fb, int width, int x, int y,
                                 int size, Color color) {
    // Use the weather_icons_48 font for the WiFi icon
    const char* wifi = GetWifiIcon(status_.signal_strength);
    DrawIcon(fb, width, x, y, wifi, large_icon_font_, color);
}

const char* WifiRenderer::GetWifiIcon(int signal_dbm) const {
    // Map signal strength to WiFi icon levels (FontAwesome)
    int pct = SignalToPercent(signal_dbm);

    if (pct >= 75) {
        return "\xef\x8c\xab";  // Full strength
    } else if (pct >= 50) {
        return "\xef\x8c\xaa";  // Medium-high
    } else if (pct >= 25) {
        return "\xef\x8c\xa9";  // Medium-low
    } else {
        return "\xef\x8c\xa8";  // Low
    }
}

int WifiRenderer::SignalToPercent(int dbm) const {
    // Map dBm (-30 to -90) to percentage (100 to 0)
    int pct = (dbm + 90) * 100 / 60;
    return std::max(0, std::min(100, pct));
}

// ============================================================
// INPUT HANDLING
// ============================================================

bool WifiRenderer::HandleInput(const ButtonEvent& event) {
    switch (event.type) {
        case ButtonEvent::kBootClick:
            // Trigger reconnection if disconnected
            if (status_.state == WifiState::Disconnected) {
                return false;  // Let app handle
            }
            break;

        case ButtonEvent::kDownLongPress:
            // Enter WiFi config mode
            return false;  // Let app handle

        default:
            break;
    }

    return false;
}

// ============================================================
// DATA UPDATE
// ============================================================

void WifiRenderer::Update(const WifiStatus& status) {
    status_ = status;
    needs_full_refresh_ = true;
    is_blinking_ = (status.state == WifiState::Connecting);
}

void WifiRenderer::SetBlinking(bool blinking) {
    is_blinking_ = blinking;
    blink_frame_++;
    needs_full_refresh_ = true;
}

}  // namespace rawdraw
