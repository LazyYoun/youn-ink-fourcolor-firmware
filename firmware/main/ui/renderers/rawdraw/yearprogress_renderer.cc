/**
 * @file yearprogress_renderer.cc
 * @brief Year progress page renderer implementation
 *
 * Displays current date, year progress %, horizontal progress bar,
 * and 12-month grid with UP/DOWN navigation.
 */

#include "yearprogress_renderer.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/style.h"
#include "rawdraw/layout_utils.h"  // FIX: 使用 InkCenteredTextTopYInBox 替代 line_height 居中
#include "rawdraw/theme.h"
#include "i18n.h"
#include <algorithm>
#include <cstdio>
#include <ctime>

// External font references
extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;
extern const lv_font_t font_zectrix_16_1;

namespace rawdraw {

static const i18n::StringId kMonthNameIds[] = {
    i18n::StringId::kMonthShort1, i18n::StringId::kMonthShort2, i18n::StringId::kMonthShort3,
    i18n::StringId::kMonthShort4, i18n::StringId::kMonthShort5, i18n::StringId::kMonthShort6,
    i18n::StringId::kMonthShort7, i18n::StringId::kMonthShort8, i18n::StringId::kMonthShort9,
    i18n::StringId::kMonthShort10, i18n::StringId::kMonthShort11, i18n::StringId::kMonthShort12,
};

static const i18n::StringId kWeekdayNameIds[] = {
    i18n::StringId::kWeekdayFullSun, i18n::StringId::kWeekdayFullMon, i18n::StringId::kWeekdayFullTue,
    i18n::StringId::kWeekdayFullWed, i18n::StringId::kWeekdayFullThu, i18n::StringId::kWeekdayFullFri,
    i18n::StringId::kWeekdayFullSat,
};

// ============================================================
// Lifecycle
// ============================================================

YearProgressRenderer::YearProgressRenderer()
    : title_font_(&SourceHanSansSC_Medium_slim)
    , body_font_(&SourceHanSansSC_Regular_slim)
    , small_font_(&SourceHanSansSC_Regular_slim)
    , icon_font_(&font_zectrix_16_1)
    , year_(2026)
    , month_(0)
    , day_(1)
    , wday_(0)
    , day_of_year_(1)
    , total_days_(365)
    , progress_pct_(0)
    , selected_month_(-1) {
}

YearProgressRenderer::~YearProgressRenderer() {}

// ============================================================
// Date calculation helpers
// ============================================================

int YearProgressRenderer::GetDaysInYear(int year) const {
    return ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) ? 366 : 365;
}

int YearProgressRenderer::GetDaysInMonth(int year, int month) const {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int d = days[month];
    if (month == 1 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) {
        d = 29;
    }
    return d;
}

void YearProgressRenderer::FormatDate(char* buf, int len) const {
    snprintf(buf, len, i18n::Tr(i18n::StringId::kYearProgressDateLabel),
             year_, month_ + 1, day_, GetWeekdayName(wday_));
}

const char* YearProgressRenderer::GetMonthName(int month) const {
    return i18n::Tr(kMonthNameIds[month]);
}

const char* YearProgressRenderer::GetWeekdayName(int wday) const {
    return i18n::Tr(kWeekdayNameIds[wday]);
}

// ============================================================
// Update time from RTC
// ============================================================

void YearProgressRenderer::UpdateTime() {
    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);

    year_ = tm_buf.tm_year + 1900;
    month_ = tm_buf.tm_mon;
    day_ = tm_buf.tm_mday;
    wday_ = tm_buf.tm_wday;
    day_of_year_ = tm_buf.tm_yday + 1;  // 1-based

    total_days_ = GetDaysInYear(year_);
    progress_pct_ = (day_of_year_ * 100) / total_days_;
}

// ============================================================
// Init
// ============================================================

void YearProgressRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    selected_month_ = -1;
    needs_full_refresh_ = true;
    UpdateTime();
}

// ============================================================
// Render
// ============================================================

void YearProgressRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;
    const auto& theme = ThemeManager::Get();
    const PaintStyle progress_style = theme.Component(ComponentRole::Progress);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    const Color accent = theme.ColorFor(ThemeToken::Accent);

    // Refresh time periodically
    UpdateTime();

    const int content_top = Style::kStatusBarHeight + 8;  // 8px gap after status bar
    const int content_bottom = height - Style::kSpacingSM;
    int y = content_top;

    // === Section 1: Title "年度进度" — independent Y, >=20px gap below ===
    const char* title = i18n::Tr(i18n::StringId::kYearProgress);
    int title_w = MeasureTextWidth(title, title_font_);
    int title_x = (width - title_w) / 2;
    title_x = (title_x + 7) & ~7;
    DrawText(fb, width, title_x, y, title, title_font_, text);
    y += title_font_->line_height + 20;  // 20px gap after title

    // === Section 2: Date line — independent Y, >=20px gap below ===
    char date_buf[48];
    FormatDate(date_buf, sizeof(date_buf));
    int date_w = MeasureTextWidth(date_buf, body_font_);
    int date_x = (width - date_w) / 2;
    date_x = (date_x + 7) & ~7;
    DrawText(fb, width, date_x, y, date_buf, body_font_, secondary);
    y += body_font_->line_height + 20;  // 20px gap after date

    // === Section 3: Large progress percentage — independent Y, >=20px gap below ===
    char pct_str[16];
    snprintf(pct_str, sizeof(pct_str), "%d%%", progress_pct_);
    int pct_w = MeasureTextWidth(pct_str, &SourceHanSansSC_Medium_slim);
    int pct_x = (width - pct_w) / 2;
    pct_x = (pct_x + 7) & ~7;
    DrawText(fb, width, pct_x, y, pct_str, &SourceHanSansSC_Medium_slim, accent);
    y += title_font_->line_height + 20;  // 20px gap after percentage

    // === Section 4: Progress bar — independent Y, >=20px gap below ===
    int bar_w = (width * 80) / 100;
    bar_w = (bar_w + 7) & ~7;
    int bar_h = Style::kProgressHeight + 4;
    int bar_x = (width - bar_w) / 2;
    bar_x = (bar_x + 7) & ~7;

    DrawStyledProgress(fb, width, {bar_x, y, bar_w, bar_h}, progress_pct_,
                       progress_style, Style::kBorderRadiusPill);
    y += bar_h + 20;  // 20px gap after progress bar

    // === Section 5: "第X天/共Y天" — independent Y, >=20px gap below ===
    char day_str[64];
    snprintf(day_str, sizeof(day_str), i18n::Tr(i18n::StringId::kDayDD), day_of_year_, total_days_);
    int day_str_w = MeasureTextWidth(day_str, small_font_);
    int day_str_x = (width - day_str_w) / 2;
    day_str_x = (day_str_x + 7) & ~7;
    DrawText(fb, width, day_str_x, y, day_str, small_font_, secondary);
    y += small_font_->line_height + 20;  // 20px gap after day count

    // === Section 6: Month overview (only if enough space) ===
    if (y < content_bottom - small_font_->line_height * 2) {
        RenderMonthGrid(fb, width, height, y);
    }

    needs_full_refresh_ = false;
}

void YearProgressRenderer::RenderHeader(uint8_t* fb, int width, int y_start) const {
    const auto& theme = ThemeManager::Get();
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    // Title
    const char* title = i18n::Tr(i18n::StringId::kYearProgress);
    int title_w = MeasureTextWidth(title, title_font_);
    int title_x = (width - title_w) / 2;
    title_x = (title_x + 7) & ~7;  // 8-byte align
    DrawText(fb, width, title_x, y_start, title, title_font_, text);

    // Date line
    char date_buf[48];
    FormatDate(date_buf, sizeof(date_buf));
    int date_w = MeasureTextWidth(date_buf, body_font_);
    int date_x = (width - date_w) / 2;
    date_x = (date_x + 7) & ~7;  // 8-byte align
    int date_y = y_start + title_font_->line_height + Style::kSpacingSM;
    DrawText(fb, width, date_x, date_y, date_buf, body_font_, secondary);
}

void YearProgressRenderer::RenderMonthGrid(uint8_t* fb, int width, int height, int y_start) const {
    const auto& theme = ThemeManager::Get();
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color border = theme.ColorFor(ThemeToken::Border);
    const int row_h = 22;  // Slightly taller rows to prevent overlap
    const int content_bottom = height - Style::kSpacingSM;
    const int rows_visible = (content_bottom - y_start - small_font_->line_height - Style::kSpacingXS) / row_h;

    // Section title
    const char* section = i18n::Tr(i18n::StringId::kMonthOverview);
    int sec_x = Style::kSpacingMD;
    sec_x = (sec_x + 7) & ~7;
    DrawText(fb, width, sec_x, y_start, section, small_font_, text);

    // Divider line
    int line_y = y_start + small_font_->line_height + Style::kSpacingXS;
    DrawHLine(fb, width, line_y, Style::kSpacingMD,
              width - Style::kSpacingMD, border);

    int row_y = line_y + Style::kSpacingXS + 1;

    int scroll = 0;
    int max_scroll = 12 > rows_visible ? (12 - rows_visible) : 0;
    if (selected_month_ >= 0) {
        if (selected_month_ < scroll) scroll = selected_month_;
        else if (selected_month_ >= scroll + rows_visible) scroll = selected_month_ - rows_visible + 1;
    }
    if (scroll > max_scroll) scroll = max_scroll;

    for (int i = scroll; i < 12 && i < scroll + rows_visible; i++) {
        if (row_y + row_h > content_bottom) break;

        bool is_past = i < month_;
        bool is_current = (i == month_);
        bool is_selected = (i == selected_month_);

        RenderMonthRow(fb, width, row_y, i, is_past, is_current, is_selected);
        row_y += row_h;
    }
}

void YearProgressRenderer::RenderMonthRow(uint8_t* fb, int width, int y, int month,
                                           bool is_past, bool is_current,
                                           bool is_selected) const {
    const auto& theme = ThemeManager::Get();
    const PaintStyle selected_style = theme.Component(ComponentRole::SettingsSelected);
    PaintStyle progress_style = theme.Component(ComponentRole::Progress);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    const Color success = theme.ColorFor(ThemeToken::SuccessLike);
    const Color accent = theme.ColorFor(ThemeToken::Accent);
    const int row_h = 22;
    // FIX: 改用 InkCenteredTextTopYInBox，避免 line_height 居中导致中文偏上
    // 参见 wiki/projects/notellm-baseline-alignment.md
    const char* month_name = GetMonthName(month);
    const int text_y = InkCenteredTextTopYInBox(small_font_, month_name, y, row_h, 0);

    Color fg = is_past ? success : text;

    // Selected: inverted background
    if (is_selected) {
        int card_x = Style::kSpacingSM;
        int card_w = width - 2 * Style::kSpacingSM;
        DrawStyledRoundRect(fb, width, 300, {card_x, y, card_w, row_h},
                            Style::kBorderRadiusSM, selected_style);
        fg = selected_style.fg;
        progress_style.bg = selected_style.bg;
        progress_style.border = selected_style.border;
    }

    // Layout: Left side = month name + status icon, Right side = mini progress bar

    // Month name (left column)
    const char* name = GetMonthName(month);
    int name_x = Style::kSpacingMD;
    name_x = (name_x + 7) & ~7;
    DrawText(fb, width, name_x, text_y, name, small_font_, fg);

    // Status indicator after month name
    if (is_past) {
        // Simple checkmark text
        DrawText(fb, width, name_x + MeasureTextWidth(name, small_font_) + Style::kSpacingXS,
                 text_y, "OK", small_font_, fg);
    } else if (is_current && !is_selected) {
        // Current month indicator: filled dot
        int dot_x = name_x + MeasureTextWidth(name, small_font_) + Style::kSpacingXS + 4;
        int dot_y = y + row_h / 2;
        DrawCircle(fb, width, {dot_x, dot_y}, 3, accent);
    }

    // Mini progress bar (right side, takes ~40% width)
    int bar_w = 80;
    bar_w = (bar_w + 7) & ~7;
    int bar_h = 6;
    // Reserve space for percentage text: measure "100%" as worst case
    char worst_case_pct[8];
    snprintf(worst_case_pct, sizeof(worst_case_pct), "100%%");
    int pct_reserved_w = MeasureTextWidth(worst_case_pct, small_font_) + Style::kSpacingSM;
    int bar_x = width - Style::kSpacingMD - bar_w - pct_reserved_w;
    bar_x = (bar_x + 7) & ~7;
    int bar_y_offset = y + (row_h - bar_h) / 2;

    // Calculate month progress
    int month_pct = 0;
    if (is_past) {
        month_pct = 100;
    } else if (is_current) {
        month_pct = (day_ * 100) / GetDaysInMonth(year_, month);
    }

    progress_style.fg = is_past ? success : (is_current ? accent : progress_style.fg);
    if (!is_current && !is_past) {
        progress_style.fg = secondary;
    }
    DrawStyledProgress(fb, width, {bar_x, bar_y_offset, bar_w, bar_h},
                       month_pct, progress_style, Style::kBorderRadiusPill);

    // Percentage text (right of progress bar)
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", month_pct);
    int pct_w = MeasureTextWidth(buf, small_font_);
    int pct_x = width - pct_w - Style::kSpacingMD;
    pct_x = (pct_x + 7) & ~7;
    DrawText(fb, width, pct_x, text_y, buf, small_font_, fg);
}

// ============================================================
// Input handling
// ============================================================

bool YearProgressRenderer::HandleInput(const ButtonEvent& event) {
    switch (event.type) {
        case ButtonEvent::kUpClick:
            // Previous month (or select December from overview)
            if (selected_month_ < 0) {
                selected_month_ = 11;  // Start at December
            } else if (selected_month_ > 0) {
                selected_month_--;
            } else {
                selected_month_ = 11;  // Wrap to December
            }
            needs_full_refresh_ = true;
            return true;

        case ButtonEvent::kDownClick:
            // Next month (or select January from overview)
            if (selected_month_ < 0) {
                selected_month_ = 0;  // Start at January
            } else if (selected_month_ < 11) {
                selected_month_++;
            } else {
                selected_month_ = 0;  // Wrap to January
            }
            needs_full_refresh_ = true;
            return true;

        case ButtonEvent::kBootClick:
            // Return to overview
            if (selected_month_ >= 0) {
                selected_month_ = -1;
                needs_full_refresh_ = true;
                return true;
            }
            break;

        case ButtonEvent::kUpLongPress:
            // Jump to current month
            selected_month_ = month_;
            needs_full_refresh_ = true;
            return true;

        default:
            break;
    }

    return false;
}

}  // namespace rawdraw
