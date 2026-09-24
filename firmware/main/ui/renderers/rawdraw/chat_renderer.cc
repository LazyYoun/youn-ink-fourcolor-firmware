/**
 * @file chat_renderer.cc
 * @brief Modernized chat page renderer implementation - flat text (no bubbles)
 *
 * Design for 400x300 1bpp ePaper:
 * - Flat text messages with ">" (user) / "[AI]" (AI) prefix markers
 * - System messages centered
 * - Horizontal dividers between messages
 * - Scroll indicator on right edge
 * - Streaming status: animated dots at bottom
 */

#include "chat_renderer.h"
#include "rawdraw/components/footer_bar.h"
#include "rawdraw/layout_utils.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/style.h"
#include "rawdraw/theme.h"
#include "i18n.h"
#include <algorithm>
#include <cstdio>
#include <vector>

// External font references
extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;

namespace rawdraw {

namespace {

struct BubbleMetrics {
    std::vector<std::string> lines;
    int width = 0;
    int height = 0;
    int line_box_h = 0;
    int line_gap = 0;
};

std::vector<std::string> WrapTextLines(const std::string& text,
                                       const lv_font_t* font,
                                       int max_width) {
    std::vector<std::string> lines;
    if (text.empty()) {
        return lines;
    }

    std::string current;
    const char* p = text.c_str();
    while (*p) {
        if (*p == '\n') {
            lines.push_back(current.empty() ? std::string(" ") : current);
            current.clear();
            ++p;
            continue;
        }

        const char* start = p;
        uint32_t cp = utf8_next(&p);
        (void)cp;
        std::string glyph(start, p - start);
        std::string next = current + glyph;
        if (!current.empty() && MeasureTextWidth(next.c_str(), font) > max_width) {
            lines.push_back(current);
            current = glyph;
        } else {
            current = next;
        }
    }

    if (!current.empty()) {
        lines.push_back(current);
    }
    if (lines.empty()) {
        lines.push_back(" ");
    }
    return lines;
}

std::string FitTextToWidth(const std::string& text, const lv_font_t* font, int max_width) {
    if (!font || max_width <= 0 || text.empty()) return "";
    if (MeasureTextWidth(text.c_str(), font) <= max_width) return text;
    std::string out;
    const char* p = text.c_str();
    while (*p) {
        const char* start = p;
        utf8_next(&p);
        std::string next = out;
        next.append(start, p - start);
        if (MeasureTextWidth((next + "...").c_str(), font) > max_width) break;
        out = std::move(next);
    }
    return out.empty() ? text : out + "...";
}

BubbleMetrics BuildBubbleMetrics(const ChatMessage& entry, const lv_font_t* font, int width) {
    const int bubble_max_w = (width * Style::kBubbleMaxWidthPct) / 100;
    const int text_max_w = bubble_max_w - Style::kBubblePadding * 2;
    BubbleMetrics metrics;
    metrics.lines = WrapTextLines(entry.text, font, text_max_w);
    int longest = 0;
    for (const auto& line : metrics.lines) {
        longest = std::max(longest, MeasureTextWidth(line.c_str(), font));
    }
    int max_ink_h = 0;
    for (const auto& line : metrics.lines) {
        const TextInkBounds ink = MeasureTextInkBounds(font, line.c_str());
        max_ink_h = std::max(max_ink_h, ink.valid ? ink.height : static_cast<int>(font->line_height));
    }

    // Chat bubbles use real glyph ink boxes instead of font line_height as
    // DrawText top-y. The old `bubble.y + padding + i * line_step` path made
    // Chinese glyphs look glued to the upper edge because the font line box is
    // taller than the visible pixels. Each line now owns a small box; text is
    // optically centered inside that box via InkCenteredTextTopYInBox().
    metrics.line_box_h = std::max(max_ink_h + 4, 20);
    metrics.line_gap = std::max(2, Style::kBubbleLineSpacing);
    const int line_count = static_cast<int>(metrics.lines.size());
    metrics.height = Style::kBubblePadding * 2 +
                     line_count * metrics.line_box_h +
                     std::max(0, line_count - 1) * metrics.line_gap;
    metrics.width = std::min(bubble_max_w, std::max(longest + Style::kBubblePadding * 2, 48));
    return metrics;
}

Rect GetBubbleRect(const ChatMessage& entry, const BubbleMetrics& metrics, int width) {
    const int bubble_w = metrics.width;
    const int bubble_h = metrics.height;

    if (entry.role == ChatRole::User) {
        return {width - Style::kBubbleMargin - bubble_w, entry.y_pos, bubble_w, bubble_h};
    }
    if (entry.role == ChatRole::System) {
        return {(width - bubble_w) / 2, entry.y_pos, bubble_w, bubble_h};
    }
    return {Style::kBubbleMargin, entry.y_pos, bubble_w, bubble_h};
}

constexpr int kChatContentY = Style::kStatusBarHeight + 10;

// Input bar disabled (voice-only mode). Bottom reserve = frame border only.
constexpr int kChatBottomReserve = 2;

}  // namespace

ChatRenderer::ChatRenderer()
    : is_streaming_(false)
    , is_listening_(false)
    , follow_latest_(true)
    , scroll_offset_(0)
    , max_scroll_offset_(0)
    , font_(&SourceHanSansSC_Regular_slim)
    , title_font_(&SourceHanSansSC_Medium_slim)
    , stream_frame_(0)
    , showing_volume_dialog_(false)
    , volume_dialog_value_(70) {
}

ChatRenderer::~ChatRenderer() {}

void ChatRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    needs_full_refresh_ = true;
    scroll_offset_ = 0;
    max_scroll_offset_ = 0;
    follow_latest_ = true;
}

void ChatRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;
    const auto& theme = ThemeManager::Get();
    const PaintStyle bg_style = theme.Style(ThemeToken::BackgroundPrimary);
    const PaintStyle card_style = theme.Component(ComponentRole::CardDefault);
    const PaintStyle user_style = theme.Component(ComponentRole::TodoSelected);
    const PaintStyle system_style = theme.Style(ThemeToken::TextSecondary);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);

    const int content_y = kChatContentY;
    const int content_bottom = height - kChatBottomReserve;
    const int content_height = std::max(40, content_bottom - content_y);

    DrawStyledRect(fb, width, {0, Style::kStatusBarHeight, width, height - Style::kStatusBarHeight}, bg_style);

    // Layout messages and compute positions
    LayoutMessages();

    // Render visible messages with scroll offset
    for (const auto& entry : messages_) {
        if (entry.text.empty()) {
            continue;
        }
        const BubbleMetrics metrics = BuildBubbleMetrics(entry, font_, width);
        Rect bubble = GetBubbleRect(entry, metrics, width);
        bubble.y = entry.y_pos - scroll_offset_ + content_y;

        if (bubble.y + bubble.h < content_y) continue;
        if (bubble.y > content_bottom) continue;

        const int visible_y = std::max(bubble.y, content_y);
        const int visible_bottom = std::min(bubble.y + bubble.h, content_bottom);
        const int visible_h = visible_bottom - visible_y;
        if (visible_h <= 0) continue;

        if (entry.role == ChatRole::System) {
            // System hints are plain centered text
            for (size_t line_index = 0; line_index < metrics.lines.size(); ++line_index) {
                const auto& line = metrics.lines[line_index];
                const int line_box_y = bubble.y + Style::kBubblePadding +
                                       static_cast<int>(line_index) *
                                           (metrics.line_box_h + metrics.line_gap);
                if (line_box_y + metrics.line_box_h >= content_y && line_box_y <= content_bottom) {
                    int text_w = MeasureTextWidth(line.c_str(), font_);
                    int text_x = (width - text_w) / 2;
                    DrawText(fb, width, text_x,
                             InkCenteredTextTopYInBox(font_, line.c_str(), line_box_y, metrics.line_box_h, 0),
                             line.c_str(), font_, system_style.fg);
                }
            }
        } else if (entry.role == ChatRole::User) {
            DrawStyledRoundRect(fb, width, height, {bubble.x, visible_y, bubble.w, visible_h},
                                Style::kBubbleRadius, user_style);
            for (size_t line_index = 0; line_index < metrics.lines.size(); ++line_index) {
                const auto& line = metrics.lines[line_index];
                const int line_box_y = bubble.y + Style::kBubblePadding +
                                       static_cast<int>(line_index) *
                                           (metrics.line_box_h + metrics.line_gap);
                if (line_box_y + metrics.line_box_h >= content_y && line_box_y <= content_bottom) {
                    DrawText(fb, width, bubble.x + Style::kBubblePadding,
                             InkCenteredTextTopYInBox(font_, line.c_str(), line_box_y, metrics.line_box_h, 0),
                             line.c_str(), font_, user_style.fg);
                }
            }
        } else {
            DrawStyledRoundRect(fb, width, height, {bubble.x, visible_y, bubble.w, visible_h},
                                Style::kBubbleRadius, card_style);
            for (size_t line_index = 0; line_index < metrics.lines.size(); ++line_index) {
                const auto& line = metrics.lines[line_index];
                const int line_box_y = bubble.y + Style::kBubblePadding +
                                       static_cast<int>(line_index) *
                                           (metrics.line_box_h + metrics.line_gap);
                if (line_box_y + metrics.line_box_h >= content_y && line_box_y <= content_bottom) {
                    DrawText(fb, width, bubble.x + Style::kBubblePadding,
                             InkCenteredTextTopYInBox(font_, line.c_str(), line_box_y, metrics.line_box_h, 0),
                             line.c_str(), font_, text);
                }
            }
        }
    }

    DrawScrollIndicator(fb, width, content_y, content_height);

#if 0
    // Bottom input bar disabled — voice input only
    Rect input_box{42, input_y, width - 42 - kChatSendW - kChatInputGap - 16, input_h};
    DrawRoundRect(fb, width, input_box, Style::kBorderRadiusMD, WHITE, BLACK, 1);
    const char* input_hint = nullptr;
    if (is_listening_) {
        input_hint = i18n::Tr(i18n::StringId::kRecordingAndRecognizing);
    } else if (is_streaming_) {
        input_hint = i18n::Tr(i18n::StringId::kAiIsReplying);
    } else if (!bottom_status_text_.empty()) {
        input_hint = bottom_status_text_.c_str();
    } else {
        input_hint = i18n::Tr(i18n::StringId::kHoldBootToSpeak);
    }
    const std::string hint = FitTextToWidth(input_hint, font_, input_box.w - 24);
    DrawText(fb, width, input_box.x + 12,
             InkCenteredTextTopY(font_, hint.c_str(), input_box.y + input_box.h / 2, 0),
             hint.c_str(), font_, BLACK);

    Rect send_box{input_box.x + input_box.w + kChatInputGap, input_y, kChatSendW, input_h};
    DrawRoundRect(fb, width, send_box, Style::kBorderRadiusMD, WHITE, BLACK, 1);
    const char* action = is_streaming_ ? i18n::Tr(i18n::StringId::kReplying) : i18n::Tr(i18n::StringId::kSend);
    const int action_w = MeasureTextWidth(action, font_);
    DrawText(fb, width, send_box.x + (send_box.w - action_w) / 2,
             InkCenteredTextTopY(font_, action, send_box.y + send_box.h / 2, 0),
             action, font_, BLACK);
#endif

    // Render volume dialog overlay if showing
    if (showing_volume_dialog_) {
        RenderVolumeDialog(fb, width, height);
    }

    needs_full_refresh_ = false;
}

void ChatRenderer::DrawScrollIndicator(uint8_t* fb, int width,
                                        int content_y, int content_height) {
    if (max_scroll_offset_ <= 0) return;
    const auto& theme = ThemeManager::Get();

    const int bar_width = Style::kScrollbarWidth;
    const int bar_x = width - bar_width - Style::kScrollMargin;
    DrawStyledRect(fb, width, {bar_x, content_y, bar_width, content_height},
                   theme.Style(ThemeToken::BackgroundSecondary));

    // Thumb
    const int total_height = content_height + max_scroll_offset_;
    int thumb_height = (content_height * content_height) / total_height;
    if (thumb_height < Style::kScrollbarMinH) thumb_height = Style::kScrollbarMinH;

    int thumb_offset = (scroll_offset_ * content_height) / total_height;
    int thumb_y = content_y + thumb_offset;

    // Clamp thumb to track
    if (thumb_y + thumb_height > content_y + content_height) {
        thumb_height = content_y + content_height - thumb_y;
    }

    DrawRoundRect(fb, width, {bar_x, thumb_y, bar_width, thumb_height},
                  Style::kBorderRadiusSM, theme.ColorFor(ThemeToken::Selected),
                  theme.ColorFor(ThemeToken::Selected), 0);
}

void ChatRenderer::DrawStreamingIndicator(uint8_t* fb, int width,
                                           int content_bottom) {
    // Animated dots: "思考中" + pulsing dots
    const int indicator_y = content_bottom - Style::kSpacingXL;
    const int padding = Style::kSpacingSM;

    // Dots animation (cycle through 1-3 dots)
    stream_frame_++;
    const int dot_count = (stream_frame_ / 10) % 3 + 1;
    char dots[8];
    dots[0] = '.';
    dots[1] = dot_count > 1 ? '.' : ' ';
    dots[2] = dot_count > 2 ? '.' : ' ';
    dots[3] = '\0';

    char buf[64];
    snprintf(buf, sizeof(buf), "%s%s", i18n::Tr(i18n::StringId::kThinking), dots);

    // Background pill
    int text_w = MeasureTextWidth(buf, font_);
    int pill_w = text_w + padding * 2;
    int pill_h = font_->line_height + padding;
    int pill_x = (width - pill_w) / 2;
    int pill_y = indicator_y;

    const PaintStyle pill_style = ThemeManager::Get().Style(ThemeToken::Badge);
    DrawStyledRoundRect(fb, width, 300, {pill_x, pill_y, pill_w, pill_h},
                        Style::kBorderRadiusPill, pill_style);

    // Text centered in pill
    int text_x = pill_x + padding;
    DrawText(fb, width, text_x,
             InkCenteredTextTopYInBox(font_, buf, pill_y, pill_h, 0),
             buf, font_, pill_style.fg);
}

void ChatRenderer::LayoutMessages() {
    if (messages_.empty()) {
        max_scroll_offset_ = 0;
        return;
    }

    int y = 0;

    for (auto& entry : messages_) {
        if (entry.text.empty()) {
            entry.y_pos = y;
            entry.block_h = 0;
            continue;
        }
        const BubbleMetrics metrics = BuildBubbleMetrics(entry, font_, width_);
        const Rect bubble = GetBubbleRect(entry, metrics, width_);
        entry.y_pos = y;
        entry.block_h = bubble.h + Style::kBubbleGap;
        y += entry.block_h;
    }

    const int total_height = y;
    const int visible_height = std::max(40, height_ - kChatBottomReserve - kChatContentY);
    max_scroll_offset_ = total_height - visible_height;
    if (max_scroll_offset_ < 0) max_scroll_offset_ = 0;

    // When content is shorter than visible area and following latest,
    // bottom-align messages so the newest message sits near the bottom
    // (like a real chat app). When not following latest, start from top.
    if (total_height <= visible_height && (follow_latest_ || is_streaming_)) {
        const int offset_y = visible_height - total_height;
        for (auto& entry : messages_) {
            entry.y_pos += offset_y;
        }
        max_scroll_offset_ = 0;
    }

    if (follow_latest_ || is_streaming_) {
        scroll_offset_ = max_scroll_offset_;
    } else if (scroll_offset_ > max_scroll_offset_) {
        scroll_offset_ = max_scroll_offset_;
    }
}

bool ChatRenderer::HandleInput(const ButtonEvent& event) {
    // Volume dialog: intercept all input when dialog is showing
    if (showing_volume_dialog_) {
        switch (event.type) {
            case ButtonEvent::kUpClick:
                UpdateVolumeValue(10, false);
                return true;
            case ButtonEvent::kDownClick:
                UpdateVolumeValue(-10, false);
                return true;
            case ButtonEvent::kUpLongPress:
                volume_dialog_value_ = 100;
                UpdateVolumeValue(0, false);
                return true;
            case ButtonEvent::kDownLongPress:
                volume_dialog_value_ = 0;
                UpdateVolumeValue(0, false);
                return true;
            case ButtonEvent::kBootClick:
                UpdateVolumeValue(0, true);
                return true;
            default:
                return true;  // Consume all input while volume dialog is open
        }
    }

    switch (event.type) {
        case ButtonEvent::kUpClick:
            if (scroll_offset_ > 0) {
                scroll_offset_ -= Style::kSpacingXL * 2;
                if (scroll_offset_ < 0) scroll_offset_ = 0;
                follow_latest_ = (scroll_offset_ >= max_scroll_offset_);
                needs_full_refresh_ = true;
                return true;
            }
            break;

        case ButtonEvent::kDownClick:
            if (scroll_offset_ < max_scroll_offset_) {
                scroll_offset_ += Style::kSpacingXL * 2;
                if (scroll_offset_ > max_scroll_offset_) {
                    scroll_offset_ = max_scroll_offset_;
                }
                follow_latest_ = (scroll_offset_ >= max_scroll_offset_);
                needs_full_refresh_ = true;
                return true;
            }
            break;

        case ButtonEvent::kUpLongPress:
            // Scroll to top
            scroll_offset_ = 0;
            follow_latest_ = false;
            needs_full_refresh_ = true;
            return true;

        case ButtonEvent::kDownLongPress:
            // Scroll to bottom
            scroll_offset_ = max_scroll_offset_;
            follow_latest_ = true;
            needs_full_refresh_ = true;
            return true;

        case ButtonEvent::kBootClick:
            // Show volume dialog when BOOT is clicked during chat
            // The handler will be set by lan_ui_handler to update actual volume
            showing_volume_dialog_ = true;
            needs_full_refresh_ = true;
            return true;

        default:
            break;
    }
    return false;
}

void ChatRenderer::Clear() {
    messages_.clear();
    scroll_offset_ = 0;
    max_scroll_offset_ = 0;
    is_streaming_ = false;
    follow_latest_ = true;
    needs_full_refresh_ = true;
}

void ChatRenderer::AddMessage(const std::string& text, ChatRole role) {
    messages_.push_back({text, role, 0, 0});
    if (messages_.size() <= 1) {
        follow_latest_ = true;
    }
    needs_full_refresh_ = true;
}

void ChatRenderer::ShowStatus(const std::string& status, ChatRole role) {
    if (!messages_.empty() && messages_.back().role == role && messages_.back().text == status) {
        bottom_status_text_ = status;
        needs_full_refresh_ = true;
        return;
    }
    AddMessage(status, role);
    bottom_status_text_ = status;
    needs_full_refresh_ = true;
}

void ChatRenderer::HideStatus() {
    // No-op in flat text mode; last message is hidden by not adding it
    needs_full_refresh_ = true;
}

void ChatRenderer::SetListening(bool listening) {
    is_listening_ = listening;
    if (listening) {
        bottom_status_text_ = i18n::Tr(i18n::StringId::kListening);
    } else {
        bottom_status_text_.clear();
    }
    needs_full_refresh_ = true;
}

void ChatRenderer::SetBottomStatus(const std::string& status) {
    bottom_status_text_ = status;
    needs_full_refresh_ = true;
}

void ChatRenderer::DrawBottomBar(uint8_t* fb, int width, int height) {
    (void)fb;
    (void)width;
    (void)height;
}

bool ChatRenderer::AppendText(const char* chunk) {
    if (!chunk || !is_streaming_) return false;
    if (messages_.empty()) return false;

    auto& last = messages_.back();
    if (last.role != ChatRole::AI) return false;

    last.text += chunk;
    needs_full_refresh_ = true;

    // Keep scrolled to bottom during streaming
    follow_latest_ = true;
    LayoutMessages();
    scroll_offset_ = max_scroll_offset_;

    return true;
}

void ChatRenderer::BeginStream() {
    is_streaming_ = true;
    stream_frame_ = 0;
    follow_latest_ = true;

    // Create new AI message for streaming
    messages_.push_back({"", ChatRole::AI, 0, 0});
    needs_full_refresh_ = true;
}

void ChatRenderer::EndStream() {
    is_streaming_ = false;
    if (!messages_.empty() && messages_.back().role == ChatRole::AI && messages_.back().text.empty()) {
        messages_.pop_back();
    }
    follow_latest_ = true;
    needs_full_refresh_ = true;
}

void ChatRenderer::ShowVolumeDialog(int volume) {
    volume_dialog_value_ = std::clamp(volume, 0, 100);
    showing_volume_dialog_ = true;
    needs_full_refresh_ = true;
}

void ChatRenderer::UpdateVolumeValue(int delta, bool commit) {
    if (commit) {
        showing_volume_dialog_ = false;
    } else {
        volume_dialog_value_ = std::clamp(volume_dialog_value_ + delta, 0, 100);
    }
    if (volume_dialog_handler_) {
        volume_dialog_handler_(volume_dialog_value_, commit);
    }
    needs_full_refresh_ = true;
}

void ChatRenderer::RenderVolumeDialog(uint8_t* fb, int width, int height) {
    if (!showing_volume_dialog_) return;
    const auto& theme = ThemeManager::Get();
    const PaintStyle bg_style = theme.Style(ThemeToken::BackgroundPrimary);
    const PaintStyle modal_style = theme.Component(ComponentRole::Modal);
    const PaintStyle shadow_style = theme.Style(ThemeToken::Shadow);
    const PaintStyle progress_style = theme.Component(ComponentRole::Progress);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color border = theme.ColorFor(ThemeToken::Border);

    const int dialog_w = 292;
    const int dialog_h = 166;
    const int dialog_x = (width - dialog_w) / 2;
    const int dialog_y = (height - dialog_h) / 2;
    const int inner_x = dialog_x + 18;
    const int inner_w = dialog_w - 36;

    // Clear dialog area
    DrawStyledRoundRect(fb, width, height, {dialog_x - 4, dialog_y - 4, dialog_w + 8, dialog_h + 8},
                        Style::kBorderRadiusLG, bg_style);

    // Shadow
    DrawStyledRoundRect(fb, width, height, {dialog_x + 2, dialog_y + 2, dialog_w, dialog_h},
                        Style::kBorderRadiusLG, shadow_style);
    // Dialog background
    DrawStyledRoundRect(fb, width, height, {dialog_x, dialog_y, dialog_w, dialog_h},
                        Style::kBorderRadiusLG, modal_style);

    // Title
    const char* title = i18n::Tr(i18n::StringId::kVolume);
    const int title_w = MeasureTextWidth(title, font_);
    DrawText(fb, width, dialog_x + (dialog_w - title_w) / 2,
             InkCenteredTextTopY(font_, title, dialog_y + 24, 0),
             title, font_, text, height);
    DrawHLine(fb, width, dialog_y + 42, dialog_x + 14, dialog_x + dialog_w - 14, border);

    // Volume percentage display
    char volume_buf[16];
    snprintf(volume_buf, sizeof(volume_buf), "%d%%", volume_dialog_value_);
    const int value_w = MeasureTextWidth(volume_buf, title_font_);
    DrawText(fb, width, dialog_x + (dialog_w - value_w) / 2,
             InkCenteredTextTopY(title_font_, volume_buf, dialog_y + 70, 0),
             volume_buf, title_font_, text, height);

    // Volume slider track
    const int track_x = inner_x;
    const int track_y = dialog_y + 102;
    const int track_w = inner_w;
    const int track_h = 16;
    DrawStyledRoundRect(fb, width, height, {track_x, track_y, track_w, track_h},
                        Style::kBorderRadiusPill, progress_style);

    // Fill bar
    int fill_w = (track_w - 4) * volume_dialog_value_ / 100;
    if (fill_w > 0) {
        DrawRect(fb, width, {track_x + 2, track_y + 2, fill_w, track_h - 4}, progress_style.fg);
    }

    // Tick marks
    for (int i = 0; i <= 4; ++i) {
        const int tick_x = track_x + 2 + (track_w - 4) * i / 4;
        DrawVLine(fb, width, tick_x, track_y + track_h + 4, track_y + track_h + 8, border);
    }

    // Hint text
    const int hint_center_y = dialog_y + dialog_h - 20;
    const char* volume_hint = i18n::Tr(i18n::StringId::kUpDnAdjustBootSave);
    DrawText(fb, width, inner_x + 6, InkCenteredTextTopY(font_, volume_hint, hint_center_y, 0),
             volume_hint, font_, theme.ColorFor(ThemeToken::TextSecondary), height);
}

}  // namespace rawdraw
