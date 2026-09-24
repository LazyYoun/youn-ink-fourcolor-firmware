/**
 * @file ap_transfer_renderer.cc
 * @brief AP Transfer mode renderer implementation
 */

#include "ap_transfer_renderer.h"
#include "i18n.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/layout_utils.h"
#include "rawdraw/style.h"
#include "rawdraw/theme.h"

#include <esp_log.h>
#include <cstdio>
#include <cstring>

extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;

namespace rawdraw {

namespace {
static const char* kTag = "ApTransferRenderer";
constexpr const char* kDefaultApIp = "192.168.4.1";

bool LooksLikeIpv4(const std::string& value) {
    int dots = 0;
    int digits = 0;
    for (char ch : value) {
        if (ch >= '0' && ch <= '9') {
            digits++;
        } else if (ch == '.') {
            dots++;
        } else {
            return false;
        }
    }
    return dots == 3 && digits >= 4;
}
}  // namespace

ApTransferRenderer::ApTransferRenderer()
    : font_(&SourceHanSansSC_Regular_slim)
    , title_font_(&SourceHanSansSC_Medium_slim) {
    ESP_LOGI(kTag, "ApTransferRenderer created");
}

ApTransferRenderer::~ApTransferRenderer() {
    ESP_LOGI(kTag, "ApTransferRenderer destroyed");
}

void ApTransferRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    state_ = kWaitingForConnection;
    status_message_.clear();
    ESP_LOGI(kTag, "ApTransferRenderer initialized: %dx%d", width, height);
}

void ApTransferRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;
    const auto& theme = ThemeManager::Get();
    const PaintStyle bg_style = theme.Style(ThemeToken::BackgroundPrimary);
    const PaintStyle title_style = theme.Style(ThemeToken::BackgroundSecondary);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color border = theme.ColorFor(ThemeToken::Border);

    // Clear to white
    DrawStyledRect(fb, width, {0, 0, width, height}, bg_style);

    // Draw outer frame (Macintosh style)
    DrawRoundRectBorder(fb, width, height, {1, 1, width - 2, height - 2},
                         Style::kBorderRadiusMD, 1, border);

    // Title bar area
    const int titlebar_h = 28;
    DrawStyledRect(fb, width, {1, 1, width - 2, titlebar_h}, title_style);
    DrawHLine(fb, width, titlebar_h, 1, width - 2, border);

    // Title
    const char* title = title_text_.empty() ? i18n::Tr(i18n::StringId::kWifiTransfer) : title_text_.c_str();
    const int title_w = MeasureTextWidth(title, title_font_);
    DrawText(fb, width, (width - title_w) / 2,
             InkCenteredTextTopYInBox(title_font_, title, 1, titlebar_h, 0),
             title, title_font_, text);

    // Content based on state
    switch (state_) {
        case kWaitingForConnection:
            RenderInstructions(fb, width, height);
            break;
        default:
            RenderStatus(fb, width, height);
            break;
    }
}

void ApTransferRenderer::RenderInstructions(uint8_t* fb, int width, int height) {
    const auto& theme = ThemeManager::Get();
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    const Color accent = theme.ColorFor(ThemeToken::Accent);
    const Color border = theme.ColorFor(ThemeToken::Border);
    const int content_top = 35;
    const int line_spacing = 28;
    const int left_margin = 20;

    int y = content_top;

    // WiFi icon area
    DrawRectBorder(fb, width, {left_margin, y, 60, 60}, 2, accent);
    
    // WiFi signal bars inside
    const int bar_x = left_margin + 10;
    const int bar_y = y + 30;
    const int bar_w = 8;
    const int bar_gap = 4;
    const int heights[] = {8, 16, 24, 32};
    for (int i = 0; i < 4; i++) {
        DrawRect(fb, width, {bar_x + i * (bar_w + bar_gap), bar_y - heights[i], bar_w, heights[i]}, accent);
    }

    // Instructions text
    y += 70;

    // Always show the browser URL. The AP/HTTP startup callback can arrive
    // while the e-paper is busy, so relying on status_message_ made the
    // address occasionally disappear and only show "启动中...".
    const std::string ip = LooksLikeIpv4(status_message_) ? status_message_ : kDefaultApIp;
    const std::string url = url_text_.empty() ? ("http://" + ip) : url_text_;
    ESP_LOGI(kTag, "RenderInstructions ip=%s state=%d message='%s'",
             ip.c_str(), static_cast<int>(state_), status_message_.c_str());
    const std::string state_hint = !hint_text_.empty()
        ? hint_text_
        : (status_message_.empty() || LooksLikeIpv4(status_message_)
        ? i18n::Tr(i18n::StringId::kStartingYouCanConnectToTheHotspotFirst)
        : status_message_);
    const std::string ssid_line = std::string(i18n::Tr(i18n::StringId::kConnect)) + (ssid_text_.empty() ? std::string("InkScreen-AP") : ssid_text_);
    const std::string pwd_line = password_text_.empty()
        ? std::string(i18n::Tr(i18n::StringId::kPasswordNone))
        : (std::string(i18n::Tr(i18n::StringId::kPassword)) + password_text_);
    const char* lines[] = {
        ssid_line.c_str(),
        pwd_line.c_str(),
        "",
        i18n::Tr(i18n::StringId::kOpenInBrowser),
        url.c_str(),
        state_hint.c_str(),
    };

    for (const auto* line : lines) {
        if (line[0] != '\0') {
            // Highlight URL
            bool is_url = (strncmp(line, "http://", 7) == 0);
            const lv_font_t* use_font = is_url ? title_font_ : font_;
            
            DrawText(fb, width, left_margin + 75,
                     InkCenteredTextTopY(use_font, line, y, 0),
                     line, use_font, is_url ? accent : text);
        }
        y += line_spacing;
    }

    // Bottom hint
    const int bottom_y = height - 30;
    DrawHLine(fb, width, bottom_y - 10, 20, width - 20, border);
    const char* exit_hint = exit_hint_text_.empty() ? i18n::Tr(i18n::StringId::kHoldBootToExit) : exit_hint_text_.c_str();
    DrawText(fb, width, 20,
             InkCenteredTextTopYInBox(font_, exit_hint, bottom_y, 24, 0),
             exit_hint, font_, secondary);
}

void ApTransferRenderer::RenderStatus(uint8_t* fb, int width, int height) {
    const auto& theme = ThemeManager::Get();
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    const Color danger = theme.ColorFor(ThemeToken::Danger);
    const Color accent = theme.ColorFor(ThemeToken::Accent);
    const Color border = theme.ColorFor(ThemeToken::Border);
    const int center_y = height / 2;

    // Status based on state
    const char* status_text = "";
    const char* detail_text = status_message_.empty() ? "" : status_message_.c_str();

    switch (state_) {
        case kClientConnected:
            status_text = i18n::Tr(i18n::StringId::kDeviceConnected);
            break;
        case kUploading:
            status_text = i18n::Tr(i18n::StringId::kUploading);
            break;
        case kProcessing:
            status_text = i18n::Tr(i18n::StringId::kProcessingImage);
            break;
        case kComplete:
            status_text = i18n::Tr(i18n::StringId::kTransferComplete);
            break;
        case kError:
            status_text = i18n::Tr(i18n::StringId::kTransferFailed);
            break;
        default:
            break;
    }

    // Draw status
    if (status_text[0] != '\0') {
        const int status_w = MeasureTextWidth(status_text, title_font_);
        DrawText(fb, width, (width - status_w) / 2,
                 InkCenteredTextTopY(title_font_, status_text, center_y - 20, 0),
                 status_text, title_font_, state_ == kError ? danger : accent);
    }

    // Draw detail
    if (detail_text[0] != '\0') {
        const int detail_w = MeasureTextWidth(detail_text, font_);
        DrawText(fb, width, (width - detail_w) / 2,
                 InkCenteredTextTopY(font_, detail_text, center_y + 20, 0),
                 detail_text, font_, secondary);
    }

    // Progress bar for uploading/processing
    if (state_ == kUploading || state_ == kProcessing) {
        const int bar_w = width - 60;
        const int bar_h = 8;
        const int bar_x = 30;
        const int bar_y = center_y + 50;
        
        DrawRectBorder(fb, width, {bar_x, bar_y, bar_w, bar_h}, 1, border);
        // Animated portion would be added later
        DrawRect(fb, width, {bar_x + 2, bar_y + 2, bar_w / 4, bar_h - 4}, accent);
    }

    const std::string ip = LooksLikeIpv4(status_message_) ? status_message_ : kDefaultApIp;
    const std::string url = url_text_.empty() ? ("http://" + ip) : url_text_;
    const int url_w = MeasureTextWidth(url.c_str(), font_);
    DrawText(fb, width, (width - url_w) / 2,
             InkCenteredTextTopY(font_, url.c_str(), height - 62, 0),
             url.c_str(), font_, accent);

    // Bottom hint
    const int bottom_y = height - 30;
    DrawHLine(fb, width, bottom_y - 10, 20, width - 20, border);
    const char* exit_hint = exit_hint_text_.empty() ? i18n::Tr(i18n::StringId::kHoldBootToExit) : exit_hint_text_.c_str();
    DrawText(fb, width, 20,
             InkCenteredTextTopYInBox(font_, exit_hint, bottom_y, 24, 0),
             exit_hint, font_, secondary);
}

bool ApTransferRenderer::HandleInput(const ButtonEvent& event) {
    ESP_LOGI(kTag, "HandleInput: type=%d", event.type);

    // AP mode is intentionally stable during slow e-paper refreshes. Only the
    // global BOOT long-press handler exits AP transfer; short clicks do nothing.
    if (event.type == ButtonEvent::kBootClick) {
        ESP_LOGI(kTag, "BOOT click ignored in AP transfer mode");
        return false;
    }

    return false;
}

void ApTransferRenderer::SetState(TransferState state, const std::string& message) {
    state_ = state;
    status_message_ = message;
    ESP_LOGI(kTag, "SetState: %d, message='%s'", state, message.c_str());
}

void ApTransferRenderer::UseDefaultTransferInstructions() {
    SetInstructionContent(i18n::Tr(i18n::StringId::kWifiTransfer),
                          "InkScreen-AP",
                          "12345678",
                          "http://192.168.4.1",
                          "",
                          i18n::Tr(i18n::StringId::kHoldBootToExit));
}

void ApTransferRenderer::SetInstructionContent(const std::string& title,
                                               const std::string& ssid,
                                               const std::string& password,
                                               const std::string& url,
                                               const std::string& hint,
                                               const std::string& exit_hint) {
    title_text_ = title;
    ssid_text_ = ssid;
    password_text_ = password;
    url_text_ = url;
    hint_text_ = hint;
    exit_hint_text_ = exit_hint;
    ESP_LOGI(kTag, "SetInstructionContent title='%s' ssid='%s' url='%s'",
             title_text_.c_str(), ssid_text_.c_str(), url_text_.c_str());
}

void ApTransferRenderer::SetExitCallback(std::function<void()> callback) {
    exit_callback_ = callback;
}

}  // namespace rawdraw
