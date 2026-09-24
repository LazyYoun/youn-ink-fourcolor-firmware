/**
 * @file news_renderer.cc
 * @brief Daily news feed page renderer for rawdraw mode
 */

#include "news_renderer.h"

#include "i18n.h"
#include "rawdraw/components/footer_bar.h"
#include "rawdraw/components/modal.h"
#include "rawdraw/layout_utils.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/style.h"
#include "rawdraw/theme.h"

#include <algorithm>
#include <string>
#include <vector>

extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;

namespace rawdraw {
namespace {

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

std::vector<std::string> WrapText(const std::string& text, const lv_font_t* font, int max_width, size_t max_lines) {
    std::vector<std::string> lines;
    if (!font || max_width <= 0 || text.empty()) return lines;

    std::string current;
    const char* p = text.c_str();
    while (*p) {
        const char* start = p;
        uint32_t ch = utf8_next(&p);
        if (ch == 0) break;
        if (ch == '\n') {
            if (!current.empty()) lines.push_back(current);
            current.clear();
            if (lines.size() >= max_lines) break;
            continue;
        }
        std::string next = current;
        next.append(start, p - start);
        if (!current.empty() && MeasureTextWidth(next.c_str(), font) > max_width) {
            lines.push_back(current);
            current.assign(start, p - start);
            if (lines.size() >= max_lines) break;
        } else {
            current = std::move(next);
        }
    }
    if (lines.size() < max_lines && !current.empty()) lines.push_back(current);
    if (lines.size() == max_lines && p && *p) lines.back() = FitTextToWidth(lines.back(), font, max_width);
    return lines;
}

constexpr int kNewsPanelX = 6;
constexpr int kNewsPanelY = Style::kStatusBarHeight + 4;
constexpr int kNewsPanelW = Style::kScreenWidth - 12;
constexpr int kNewsPanelH = 256;
constexpr int kNewsVisibleRows = 7;
constexpr int kNewsFooterY = 264;
constexpr int kNewsFooterH = 26;
constexpr int kItemH = kNewsPanelH / kNewsVisibleRows;
constexpr int kItemGap = 0;
constexpr int kHeaderH = 48;

bool SameNewsItems(const std::vector<NewsItem>& lhs, const std::vector<NewsItem>& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].title != rhs[i].title ||
            lhs[i].summary != rhs[i].summary ||
            lhs[i].source != rhs[i].source ||
            lhs[i].time_label != rhs[i].time_label) {
            return false;
        }
    }
    return true;
}

}  // namespace

NewsRenderer::NewsRenderer()
    : font_(&SourceHanSansSC_Regular_slim),
      title_font_(&SourceHanSansSC_Medium_slim) {}

NewsRenderer::~NewsRenderer() {}

void NewsRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    preview_open_ = false;
    footer_focus_ = 0;
    preview_scroll_ = 0;
    ClampSelection();
    ClampScrollOffset();
    needs_full_refresh_ = true;
}

void NewsRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;
    const auto& theme = ThemeManager::Get();
    const PaintStyle bg_style = theme.Style(ThemeToken::BackgroundPrimary);
    const PaintStyle panel_style = theme.Component(ComponentRole::Panel);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);

    DrawStyledRect(fb, width, {0, Style::kStatusBarHeight, width, height - Style::kStatusBarHeight}, bg_style);

    if (items_.empty()) {
        Modal modal;
        modal.SetTitle(i18n::Tr(i18n::StringId::kNoNews));
        modal.SetBodyFooter(i18n::Tr(i18n::StringId::kWaitingForData));
        modal.CenterInScreen(width, height, 52);
        modal.Draw(fb, width, height);
    } else {
        DrawStyledRoundRect(fb, width, height, {kNewsPanelX, kNewsPanelY, kNewsPanelW, kNewsPanelH},
                            Style::kBorderRadiusMD, panel_style);
        int window_start = std::max(0, selected_index_ - kNewsVisibleRows / 2);
        if (window_start + kNewsVisibleRows > static_cast<int>(items_.size())) {
            window_start = std::max(0, static_cast<int>(items_.size()) - kNewsVisibleRows);
        }
        for (int row = 0; row < kNewsVisibleRows; ++row) {
            const int item_index = window_start + row;
            if (item_index >= static_cast<int>(items_.size())) break;
            RenderItem(fb, width, kNewsPanelY + row * kItemH + 1, item_index, item_index == selected_index_);
        }
    }

    if (preview_open_ && !items_.empty()) {
        DrawPreviewModal(fb, width, height);
    }

    DrawStyledRoundRect(fb, width, height, {kNewsPanelX, kNewsFooterY, kNewsPanelW, kNewsFooterH},
                        Style::kBorderRadiusSM, panel_style);
    if (preview_open_) {
        const char* boot_hint = footer_focus_ == 1 ? i18n::Tr(i18n::StringId::kRead) : i18n::Tr(i18n::StringId::kClose);
        const char* select_hint = i18n::Tr(i18n::StringId::kUpDnSelect);
        DrawText(fb, width, 54,
                 InkCenteredTextTopY(font_, select_hint, kNewsFooterY + kNewsFooterH / 2, 0),
                 select_hint, font_, secondary);
        DrawText(fb, width, 262,
                 InkCenteredTextTopY(font_, boot_hint, kNewsFooterY + kNewsFooterH / 2, 0),
                 boot_hint, font_, text);
    } else {
        const char* page_hint = i18n::Tr(i18n::StringId::kUpDnPage);
        const char* open_hint = i18n::Tr(i18n::StringId::kBootOpen);
        DrawText(fb, width, 54,
                 InkCenteredTextTopY(font_, page_hint, kNewsFooterY + kNewsFooterH / 2, 0),
                 page_hint, font_, secondary);
        DrawText(fb, width, 262,
                 InkCenteredTextTopY(font_, open_hint, kNewsFooterY + kNewsFooterH / 2, 0),
                 open_hint, font_, text);
    }

    needs_full_refresh_ = false;
}

void NewsRenderer::RenderItem(uint8_t* fb, int width, int y, int index, bool selected) {
    const NewsItem& item = items_[index];
    const auto& theme = ThemeManager::Get();
    const PaintStyle selected_style = theme.Component(ComponentRole::SettingsSelected);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    const Color border = theme.ColorFor(ThemeToken::Border);
    Rect row{kNewsPanelX + 1, y, kNewsPanelW - 2, kItemH};

    const int center_y = row.y + row.h / 2;
    const int text_y = InkCenteredTextTopY(font_, "字", center_y, 0);
    char index_buf[16];
    snprintf(index_buf, sizeof(index_buf), "%d", index + 1);
    DrawText(fb, width, row.x + 8, text_y, index_buf, font_, selected ? selected_style.border : secondary);
    if (selected) {
        DrawRect(fb, width, {row.x + 4, center_y - 7, 3, 14}, selected_style.border);
    }
    const int index_w = MeasureTextWidth(index_buf, font_);
    const int title_x = row.x + index_w + 12;
    const std::string title = FitTextToWidth(item.title, title_font_, row.w - index_w - 12 - 60);
    DrawText(fb, width, title_x,
             InkCenteredTextTopY(title_font_, title.c_str(), center_y, 0),
             title.c_str(), title_font_, text);
    const std::string time = item.time_label.empty() ? item.source : item.time_label;
    const std::string fit_time = FitTextToWidth(time, font_, 54);
    const int time_w = MeasureTextWidth(fit_time.c_str(), font_);
    DrawText(fb, width, row.x + row.w - time_w - 6, text_y,
             fit_time.c_str(), font_, secondary);
    if (row.y + row.h < kNewsPanelY + kNewsPanelH - 1) {
        for (int x = kNewsPanelX + 8; x < kNewsPanelX + kNewsPanelW - 8; x += 4) {
            set_pixel(fb, width, x, row.y + row.h - 1, border);
        }
    }
}

void NewsRenderer::DrawPreviewModal(uint8_t* fb, int width, int height) {
    const NewsItem& item = items_[selected_index_];
    const auto& theme = ThemeManager::Get();
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    const Color accent = theme.ColorFor(ThemeToken::Accent);
    Modal modal;
    modal.SetTitle(i18n::Tr(i18n::StringId::kNewsPreview));
    modal.SetBodyFooter(footer_focus_ == 1 ? i18n::Tr(i18n::StringId::kRead2) : i18n::Tr(i18n::StringId::kClose2));
    modal.CenterInScreen(width, height, 36);
    modal.Draw(fb, width, height);

    const Rect body = modal.GetContentBounds();
    constexpr int kRowH = 20;  // Fixed row height, same concept as kAboutRowHeight
    const int visible_rows = body.h / kRowH;

    // Build all content lines
    std::vector<std::string> content_lines;

    // Title (may wrap to 2 lines)
    auto title_lines = WrapText(item.title, title_font_, body.w, 2);
    for (auto& l : title_lines) content_lines.push_back(l);

    // Meta (source · time)
    std::string meta = item.source;
    if (!item.time_label.empty()) {
        if (!meta.empty()) meta += " · ";
        meta += item.time_label;
    }
    if (!meta.empty()) {
        content_lines.push_back(FitTextToWidth(meta, font_, body.w));
    }

    // Summary lines
    auto summary_lines = WrapText(item.summary, font_, body.w, 20);
    for (auto& l : summary_lines) content_lines.push_back(l);

    const int total_lines = static_cast<int>(content_lines.size());
    const int max_scroll = std::max(0, total_lines - visible_rows);
    if (preview_scroll_ > max_scroll) preview_scroll_ = max_scroll;

    int y = body.y;
    for (int i = 0; i < visible_rows && i + preview_scroll_ < total_lines; ++i) {
        const int line_idx = i + preview_scroll_;
        const auto& line = content_lines[line_idx];
        const lv_font_t* f = (line_idx < static_cast<int>(title_lines.size())) ? title_font_ : font_;
        const int center_y = y + kRowH / 2;
        DrawText(fb, width, body.x, InkCenteredTextTopY(f, line.c_str(), center_y, 0),
                 line.c_str(), f, f == title_font_ ? text : secondary, height);
        y += kRowH;
    }

    // Scroll indicator: small triangle at bottom if more content below
    if (preview_scroll_ < max_scroll) {
        DrawText(fb, width, body.x + body.w - 14,
                 body.y + body.h - 10, "▼", font_, accent, height);
    }
    if (preview_scroll_ > 0) {
        DrawText(fb, width, body.x + body.w - 14,
                 body.y, "▲", font_, accent, height);
    }
}

bool NewsRenderer::HandleInput(const ButtonEvent& event) {
    if (preview_open_) {
        switch (event.type) {
            case ButtonEvent::kUpClick:
                if (preview_scroll_ > 0) {
                    preview_scroll_--;
                    needs_full_refresh_ = true;
                    return true;
                }
                footer_focus_ = (footer_focus_ == 0) ? 1 : 0;
                needs_full_refresh_ = true;
                return true;
            case ButtonEvent::kDownClick:
                footer_focus_ = (footer_focus_ == 0) ? 1 : 0;
                needs_full_refresh_ = true;
                return true;
            case ButtonEvent::kBootClick:
                if (footer_focus_ == 1 && tts_request_cb_) {
                    const NewsItem& item = items_[selected_index_];
                    std::string tts_text = item.title;
                    if (!item.summary.empty()) tts_text += " " + item.summary;
                    tts_request_cb_(tts_text);
                    // Keep the modal open after requesting speech. Closing here
                    // made BOOT feel like "close" even when DN had selected read.
                    footer_focus_ = 1;
                    needs_full_refresh_ = true;
                    return true;
                }
                // footer_focus_ == 0 → close
                preview_open_ = false;
                footer_focus_ = 0;
                preview_scroll_ = 0;
                needs_full_refresh_ = true;
                return true;
            default:
                return true;
        }
    }

    switch (event.type) {
        case ButtonEvent::kUpClick:
            if (selected_index_ > 0) {
                selected_index_--;
                if (selected_index_ * (kItemH + kItemGap) < scroll_offset_) {
                    scroll_offset_ = selected_index_ * (kItemH + kItemGap);
                }
                needs_full_refresh_ = true;
                return true;
            }
            break;
        case ButtonEvent::kDownClick:
            if (selected_index_ < static_cast<int>(items_.size()) - 1) {
                selected_index_++;
                const int content_h = kNewsPanelH;
                const int item_bottom = selected_index_ * (kItemH + kItemGap) + kItemH;
                if (item_bottom > scroll_offset_ + content_h) {
                    scroll_offset_ = item_bottom - content_h;
                }
                needs_full_refresh_ = true;
                return true;
            }
            break;
        case ButtonEvent::kBootClick:
            if (!items_.empty()) {
                preview_open_ = true;
                preview_scroll_ = 0;
                needs_full_refresh_ = true;
                return true;
            }
            break;
        default:
            break;
    }
    return false;
}

void NewsRenderer::Clear() {
    items_.clear();
    selected_index_ = 0;
    scroll_offset_ = 0;
    preview_open_ = false;
    footer_focus_ = 0;
    preview_scroll_ = 0;
    needs_full_refresh_ = true;
}

void NewsRenderer::SetItems(const std::vector<NewsItem>& items) {
    const bool same_items = SameNewsItems(items_, items);
    const int old_selected = selected_index_;
    const int old_scroll = scroll_offset_;
    const bool old_preview_open = preview_open_;
    const int old_footer_focus = footer_focus_;
    const int old_preview_scroll = preview_scroll_;

    items_ = items;
    if (same_items) {
        selected_index_ = old_selected;
        scroll_offset_ = old_scroll;
        preview_open_ = old_preview_open;
        footer_focus_ = old_footer_focus;
        preview_scroll_ = old_preview_scroll;
    } else {
        selected_index_ = 0;
        scroll_offset_ = 0;
        preview_open_ = false;
        footer_focus_ = 0;
        preview_scroll_ = 0;
    }
    ClampSelection();
    ClampScrollOffset();
    needs_full_refresh_ = true;
}

void NewsRenderer::AddItem(const NewsItem& item) {
    items_.push_back(item);
    ClampSelection();
    ClampScrollOffset();
    needs_full_refresh_ = true;
}

void NewsRenderer::ClampSelection() {
    if (items_.empty()) {
        selected_index_ = 0;
    } else {
        selected_index_ = std::max(0, std::min(selected_index_, static_cast<int>(items_.size()) - 1));
    }
}

void NewsRenderer::ClampScrollOffset() {
    const int content_h = kNewsPanelH;
    int max_offset = static_cast<int>(items_.size()) * (kItemH + kItemGap) - kItemGap - content_h;
    if (max_offset < 0) max_offset = 0;
    scroll_offset_ = std::max(0, std::min(scroll_offset_, max_offset));
}

}  // namespace rawdraw
