/**
 * @file ebook_renderer.cc
 * @brief Ebook list page and TXT reader renderer for rawdraw mode
 *
 * Two modes:
 * 1. File list: shows TXT files from SPIFFS, BOOT click selects
 * 2. Reader: paginated TXT display, BOOT click returns to file list
 */

#include "ebook_renderer.h"
#include "rawdraw/layout_utils.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/style.h"
#include "rawdraw/theme.h"
#include "i18n.h"
#include <algorithm>
#include <cstdio>
#include <esp_log.h>
#include <vector>

extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;

namespace rawdraw {

namespace {
constexpr char kTag[] = "EbookRenderer";
constexpr int kContentTopGap = 8;
constexpr int kListY = Style::kStatusBarHeight + kContentTopGap;
constexpr int kListH = 220;
constexpr int kItemH = 32;
constexpr int kFooterY = 272;
constexpr int kFooterH = 24;
}  // namespace

EbookRenderer::EbookRenderer()
    : font_(&SourceHanSansSC_Regular_slim)
    , title_font_(&SourceHanSansSC_Medium_slim) {
}

EbookRenderer::~EbookRenderer() {}

void EbookRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    needs_full_refresh_ = true;
}

void EbookRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;
    if (reader_mode_) {
        RenderReader(fb, width, height);
    } else {
        RenderFileList(fb, width, height);
    }
    needs_full_refresh_ = false;
}

void EbookRenderer::RenderFileList(uint8_t* fb, int width, int height) {
    const auto& theme = ThemeManager::Get();
    const PaintStyle bg_style = theme.Style(ThemeToken::BackgroundPrimary);
    const PaintStyle selected_style = theme.Component(ComponentRole::SettingsSelected);
    const PaintStyle footer_style = theme.Component(ComponentRole::Panel);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    // Clear content area
    DrawStyledRect(fb, width, {0, Style::kStatusBarHeight + 1, width, height_ - Style::kStatusBarHeight - 1}, bg_style);

    if (files_.empty()) {
        const char* hint = i18n::Tr(i18n::StringId::kNoTxtFiles);
        int hint_w = MeasureTextWidth(hint, font_);
        DrawText(fb, width, (width - hint_w) / 2, kListY + 80, hint, font_, text);
        const char* push_hint = i18n::Tr(i18n::StringId::kPushATxtFileToDevice);
        DrawText(fb, width, (width - MeasureTextWidth(push_hint, font_)) / 2,
                 kListY + 110, push_hint, font_, secondary);
    } else {
        // Draw file list
        int visible_start = std::max(0, selected_index_ - 5);
        for (int i = 0; i < 7; ++i) {
            int idx = visible_start + i;
            if (idx >= static_cast<int>(files_.size())) break;
            int y = kListY + i * kItemH;
            bool sel = idx == selected_index_;

            if (sel) {
                DrawStyledRoundRect(fb, width, height, {14, y + 2, width - 28, kItemH - 4},
                                    Style::kBorderRadiusSM, selected_style);
                DrawText(fb, width, 24,
                         InkCenteredTextTopY(font_, files_[idx].c_str(), y + kItemH / 2, 0),
                         files_[idx].c_str(), font_, selected_style.fg);
            } else {
                DrawText(fb, width, 24,
                         InkCenteredTextTopY(font_, files_[idx].c_str(), y + kItemH / 2, 0),
                         files_[idx].c_str(), font_, text);
            }
        }
    }

    // Footer hints
    const char* select_hint = i18n::Tr(i18n::StringId::kBootSelect);
    DrawStyledRoundRect(fb, width, height, {14, kFooterY, 110, kFooterH}, Style::kBorderRadiusSM, footer_style);
    DrawText(fb, width, 34,
             InkCenteredTextTopY(font_, select_hint, kFooterY + kFooterH / 2, 0),
             select_hint, font_, footer_style.fg);

    const char* back_hint = i18n::Tr(i18n::StringId::kDoubleClickBack);
    DrawStyledRoundRect(fb, width, height, {142, kFooterY, 130, kFooterH}, Style::kBorderRadiusSM, footer_style);
    DrawText(fb, width, 160,
             InkCenteredTextTopY(font_, back_hint, kFooterY + kFooterH / 2, 0),
             back_hint, font_, footer_style.fg);
}

void EbookRenderer::RenderReader(uint8_t* fb, int width, int height) {
    if (portrait_reader_) {
        RenderReaderPortrait(fb, width, height);
        return;
    }

    const auto& theme = ThemeManager::Get();
    DrawStyledRect(fb, width, {0, Style::kStatusBarHeight + 1, width, height_ - Style::kStatusBarHeight - 1},
                   theme.Style(ThemeToken::BackgroundPrimary));

    // Content area (no title bar — filename+page shown in status bar)
    // Start below the 28px status/menu bar with the same top gap as the list.
    // This avoids the first line visually touching the menu divider.
    int content_y = Style::kStatusBarHeight + kContentTopGap;
    int content_h = height_ - content_y - 4;

    RenderReaderPage(fb, width, height, content_y, content_h);
}

void EbookRenderer::RenderReaderPage(uint8_t* fb, int width, int height, int content_y, int content_h) {
    const auto& theme = ThemeManager::Get();
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);

    if (reader_content_.empty()) {
        const char* empty_hint = i18n::Tr(i18n::StringId::kFileIsEmptyOrFailedToRead);
        const int hint_w = MeasureTextWidth(empty_hint, font_);
        DrawText(fb, width, (width - hint_w) / 2,
                 InkCenteredTextTopY(font_, empty_hint, content_y + 42, 0),
                 empty_hint, font_, text, height);
        const char* retry_hint = i18n::Tr(i18n::StringId::kPushTheTxtFileAgainAndReopen);
        DrawText(fb, width, 24,
                 InkCenteredTextTopY(font_, retry_hint, content_y + 74, 0),
                 retry_hint, font_, secondary, height);
    } else {
        const int chars_per_page = CharsPerPage();
        int start_char = current_page_ * chars_per_page;
        int end_char = std::min(start_char + chars_per_page, static_cast<int>(reader_content_.size()));

        std::string page_text = reader_content_.substr(start_char, end_char - start_char);
        ESP_LOGI(kTag, "RenderReader file=%s portrait=%d bytes=%u page=%d/%d page_bytes=%u",
                 reader_filename_.c_str(),
                 portrait_reader_ ? 1 : 0,
                 static_cast<unsigned>(reader_content_.size()),
                 current_page_ + 1,
                 total_pages_,
                 static_cast<unsigned>(page_text.size()));

        // Wrap text into display lines first (handles \n and word-wrap)
        const int margin_x = 14;
        const int max_line_width = width - margin_x * 2;
        std::vector<std::string> display_lines;

        const char* p = page_text.c_str();
        std::string current_line;

        while (*p) {
            if (*p == '\n') {
                display_lines.push_back(current_line.empty() ? std::string(" ") : current_line);
                current_line.clear();
                ++p;
                continue;
            }

            const char* char_start = p;
            if ((*p & 0x80) == 0) { p++; }
            else if ((*p & 0xE0) == 0xC0) { p += 2; }
            else if ((*p & 0xF0) == 0xE0) { p += 3; }
            else if ((*p & 0xF8) == 0xF0) { p += 4; }
            else { p++; }

            std::string next_line = current_line;
            next_line.append(char_start, p - char_start);

            if (MeasureTextWidth(next_line.c_str(), font_) > max_line_width) {
                display_lines.push_back(current_line);
                current_line = std::string(char_start, p - char_start);
            } else {
                current_line = next_line;
            }
        }
        if (!current_line.empty()) {
            display_lines.push_back(current_line);
        }
        if (display_lines.empty()) {
            display_lines.push_back(" ");
        }

        int max_ink_h = 0;
        for (const auto& line : display_lines) {
            const TextInkBounds ink = MeasureTextInkBounds(font_, line.c_str());
            max_ink_h = std::max(max_ink_h, ink.valid ? ink.height : static_cast<int>(font_->line_height));
        }

        // Same lesson as Chat/Settings: do not use font->line_height as the
        // visible text height. The SourceHan line box is taller than the ink,
        // and treating its top as DrawText y makes multi-line TXT pages look
        // overlapped or glued to the previous row on the 1bpp panel.
        const int line_box_h = std::max(max_ink_h + 6, 22);
        const int line_gap = 3;
        const int line_step = line_box_h + line_gap;
        const int max_lines = content_h / line_step;
        int visible = std::min(static_cast<int>(display_lines.size()), max_lines);

        for (int i = 0; i < visible; ++i) {
            const int line_box_y = content_y + i * line_step;
            DrawText(fb, width, margin_x,
                     InkCenteredTextTopYInBox(font_, display_lines[i].c_str(),
                                             line_box_y, line_box_h, 0),
                     display_lines[i].c_str(), font_, text, height);
        }
    }
}

void EbookRenderer::RenderReaderPortrait(uint8_t* fb, int width, int height) {
    constexpr int kPortraitW = 300;
    constexpr int kPortraitH = 400;
    std::vector<uint8_t> portrait((kPortraitW * 2 + 7) / 8 * kPortraitH, 0x55);

    const int old_height_hint = height;
    SetFramebufferHeightHint(kPortraitH);
    DrawStyledRect(portrait.data(), kPortraitW, {0, 0, kPortraitW, kPortraitH},
                   ThemeManager::Get().Style(ThemeToken::BackgroundPrimary));
    RenderReaderPage(portrait.data(), kPortraitW, kPortraitH, 12, kPortraitH - 24);

    const auto& theme = ThemeManager::Get();
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    char page_buf[24];
    snprintf(page_buf, sizeof(page_buf), "%d/%d", current_page_ + 1, total_pages_);
    const int page_w = MeasureTextWidth(page_buf, font_);
    DrawText(portrait.data(), kPortraitW, kPortraitW - page_w - 10,
             kPortraitH - 18, page_buf, font_, secondary, kPortraitH);

    SetFramebufferHeightHint(height);
    DrawStyledRect(fb, width, {0, 0, width, height},
                   ThemeManager::Get().Style(ThemeToken::BackgroundPrimary));
    for (int y = 0; y < kPortraitH; ++y) {
        for (int x = 0; x < kPortraitW; ++x) {
            const Color c = get_pixel(portrait.data(), kPortraitW, x, y);
            const int phys_x = kPortraitH - 1 - y;
            const int phys_y = x;
            set_pixel(fb, width, phys_x, phys_y, c);
        }
    }
    SetFramebufferHeightHint(old_height_hint);

}

bool EbookRenderer::HandleInput(const ButtonEvent& event) {
    if (reader_mode_) {
        switch (event.type) {
            case ButtonEvent::kBootClick:
                // Exit reader, return to file list
                CloseReader();
                needs_full_refresh_ = true;
                return true;
            case ButtonEvent::kBootDoubleClick:
                // Signal to app to exit ebook page entirely.
                return false;
            case ButtonEvent::kUpDoubleClick:
                SetPortraitReader(false);
                return true;
            case ButtonEvent::kDownDoubleClick:
                SetPortraitReader(true);
                return true;
            case ButtonEvent::kUpClick:
                if (current_page_ > 0) {
                    current_page_--;
                    needs_full_refresh_ = true;
                    return true;
                }
                return false;
            case ButtonEvent::kDownClick:
                if (current_page_ < total_pages_ - 1) {
                    current_page_++;
                    needs_full_refresh_ = true;
                    return true;
                }
                return false;
            default:
                break;
        }
        return false;
    }

    // File list mode
    if (files_.empty()) return false;

    switch (event.type) {
        case ButtonEvent::kUpClick:
            if (selected_index_ > 0) {
                selected_index_--;
                needs_full_refresh_ = true;
                return true;
            }
            break;
        case ButtonEvent::kDownClick:
            if (selected_index_ < static_cast<int>(files_.size()) - 1) {
                selected_index_++;
                needs_full_refresh_ = true;
                return true;
            }
            break;
        case ButtonEvent::kBootClick:
            // Caller should handle file opening via GetSelectedFile()
            return false;  // Let app handle this
        default:
            break;
    }
    return false;
}

void EbookRenderer::SetFileList(const std::vector<std::string>& files) {
    files_ = files;
    selected_index_ = 0;
    needs_full_refresh_ = true;
}

std::string EbookRenderer::GetSelectedFile() const {
    if (selected_index_ >= 0 && selected_index_ < static_cast<int>(files_.size())) {
        return files_[selected_index_];
    }
    return "";
}

void EbookRenderer::OpenFile(const std::string& filename, const std::string& content) {
    reader_filename_ = filename;
    reader_content_ = content;
    reader_mode_ = true;
    portrait_reader_ = false;
    current_page_ = 0;
    CalcPages();
    needs_full_refresh_ = true;
}

void EbookRenderer::CloseReader() {
    reader_mode_ = false;
    portrait_reader_ = false;
    reader_content_.clear();
    reader_filename_.clear();
    current_page_ = 0;
    total_pages_ = 0;
    needs_full_refresh_ = true;
}

void EbookRenderer::CalcPages() {
    if (reader_content_.empty()) {
        total_pages_ = 1;
    } else {
        const int chars_per_page = CharsPerPage();
        total_pages_ = (static_cast<int>(reader_content_.size()) + chars_per_page - 1) / chars_per_page;
    }
}

int EbookRenderer::CharsPerPage() const {
    return portrait_reader_ ? kPortraitCharsPerPage : kLandscapeCharsPerPage;
}

void EbookRenderer::SetPortraitReader(bool portrait) {
    if (portrait_reader_ == portrait) {
        needs_full_refresh_ = true;
        return;
    }
    const int old_chars_per_page = CharsPerPage();
    const int current_offset = std::max(0, current_page_) * old_chars_per_page;
    portrait_reader_ = portrait;
    CalcPages();
    current_page_ = total_pages_ > 0
        ? std::min(total_pages_ - 1, current_offset / CharsPerPage())
        : 0;
    needs_full_refresh_ = true;
}

}  // namespace rawdraw
