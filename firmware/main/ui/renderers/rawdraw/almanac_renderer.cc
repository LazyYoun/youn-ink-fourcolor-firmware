/**
 * @file almanac_renderer.cc
 * @brief Almanac page renderer - displays lunar date, solar terms, and almanac info
 *
 * Uses Calendar::ToLunarDate() for lunar calendar conversion (2000-2050).
 * Shows: today's Gregorian date, lunar date, solar term, weekday, and
 * traditional almanac info (yiji - auspicious/inauspicious activities).
 */

#include "almanac_renderer.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/style.h"
#include "rawdraw/layout_utils.h"  // FIX: 使用 InkCenteredTextTopYInBox 替代 line_height 居中
#include "rawdraw/components/calendar.h"
#include "rawdraw/theme.h"
#include "i18n.h"
#include <cstring>
#include <ctime>
#include <cstdio>

// External font references
extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;
extern const lv_font_t weather_icons_48;

// Weekday characters (matches calendar.cc)
static const i18n::StringId kWeekdayFullIds[] = {
    i18n::StringId::kWeekdayLongSun, i18n::StringId::kWeekdayLongMon, i18n::StringId::kWeekdayLongTue,
    i18n::StringId::kWeekdayLongWed, i18n::StringId::kWeekdayLongThu, i18n::StringId::kWeekdayLongFri,
    i18n::StringId::kWeekdayLongSat,
};
static const char* WeekdayFullName(int i) {
    return i18n::Tr(kWeekdayFullIds[i]);
}

// Lunar month/day names (same as calendar.cc)
static const i18n::StringId kLunarMonthIds[] = {
    i18n::StringId::kLunarMonthFull1, i18n::StringId::kLunarMonthFull2, i18n::StringId::kLunarMonthFull3,
    i18n::StringId::kLunarMonthFull4, i18n::StringId::kLunarMonthFull5, i18n::StringId::kLunarMonthFull6,
    i18n::StringId::kLunarMonthFull7, i18n::StringId::kLunarMonthFull8, i18n::StringId::kLunarMonthFull9,
    i18n::StringId::kLunarMonthFull10, i18n::StringId::kLunarMonthFull11, i18n::StringId::kLunarMonthFull12,
};
static const i18n::StringId kLunarDayIds[] = {
    i18n::StringId::kLunarDay1, i18n::StringId::kLunarDay2, i18n::StringId::kLunarDay3, i18n::StringId::kLunarDay4,
    i18n::StringId::kLunarDay5, i18n::StringId::kLunarDay6, i18n::StringId::kLunarDay7, i18n::StringId::kLunarDay8,
    i18n::StringId::kLunarDay9, i18n::StringId::kLunarDay10, i18n::StringId::kLunarDay11, i18n::StringId::kLunarDay12,
    i18n::StringId::kLunarDay13, i18n::StringId::kLunarDay14, i18n::StringId::kLunarDay15, i18n::StringId::kLunarDay16,
    i18n::StringId::kLunarDay17, i18n::StringId::kLunarDay18, i18n::StringId::kLunarDay19, i18n::StringId::kLunarDay20,
    i18n::StringId::kLunarDay21, i18n::StringId::kLunarDay22, i18n::StringId::kLunarDay23, i18n::StringId::kLunarDay24,
    i18n::StringId::kLunarDay25, i18n::StringId::kLunarDay26, i18n::StringId::kLunarDay27, i18n::StringId::kLunarDay28,
    i18n::StringId::kLunarDay29, i18n::StringId::kLunarDay30,
};

// Tian Gan / Di Zhi (used via Calendar::GetLunarYearName)
// static const char* kTianGan[] = ...;  // Not used directly, Calendar handles it
// static const char* kDiZhi[] = ...;

// Solar terms (same as calendar.cc)
struct SolarTermEntry {
    int month;
    int day;
    i18n::StringId name_id;
};
static const SolarTermEntry kSolarTerms[] = {
    { 1,  5, i18n::StringId::kSolarTermMinorCold }, { 1, 20, i18n::StringId::kSolarTermMajorCold },
    { 2,  4, i18n::StringId::kSolarTermStartOfSpring }, { 2, 19, i18n::StringId::kSolarTermRainWater },
    { 3,  5, i18n::StringId::kSolarTermAwakeningOfInsects }, { 3, 20, i18n::StringId::kSolarTermSpringEquinox },
    { 4,  4, i18n::StringId::kSolarTermClearAndBrightQingming }, { 4, 20, i18n::StringId::kSolarTermGrainRain },
    { 5,  5, i18n::StringId::kSolarTermStartOfSummer }, { 5, 21, i18n::StringId::kSolarTermGrainFull },
    { 6,  5, i18n::StringId::kSolarTermGrainInEar }, { 6, 21, i18n::StringId::kSolarTermSummerSolstice },
    { 7,  7, i18n::StringId::kSolarTermMinorHeat }, { 7, 23, i18n::StringId::kSolarTermMajorHeat },
    { 8,  7, i18n::StringId::kSolarTermStartOfAutumn }, { 8, 23, i18n::StringId::kSolarTermEndOfHeat },
    { 9,  7, i18n::StringId::kSolarTermWhiteDew }, { 9, 23, i18n::StringId::kSolarTermAutumnEquinox },
    {10,  8, i18n::StringId::kSolarTermColdDew }, {10, 23, i18n::StringId::kSolarTermFrostSDescent },
    {11,  7, i18n::StringId::kSolarTermStartOfWinter }, {11, 22, i18n::StringId::kSolarTermMinorSnow },
    {12,  7, i18n::StringId::kSolarTermMajorSnow }, {12, 22, i18n::StringId::kSolarTermWinterSolstice },
};

static const char* GetSolarTerm(int month, int day) {
    for (size_t i = 0; i < sizeof(kSolarTerms) / sizeof(kSolarTerms[0]); i++) {
        if (kSolarTerms[i].month == month && kSolarTerms[i].day == day) {
            return i18n::Tr(kSolarTerms[i].name_id);
        }
    }
    return nullptr;
}

// Simplified yiji (宜忌) based on lunar day patterns
// This is a traditional approximation, not a full almanac calculation
static const i18n::StringId kYiTableIds[][4] = {
    {i18n::StringId::kAlmanacYi11, i18n::StringId::kAlmanacYi12, i18n::StringId::kAlmanacYi13, i18n::StringId::kAlmanacYi14},
    {i18n::StringId::kAlmanacYi21, i18n::StringId::kAlmanacYi22, i18n::StringId::kAlmanacYi23, i18n::StringId::kAlmanacYi24},
    {i18n::StringId::kAlmanacYi31, i18n::StringId::kAlmanacYi32, i18n::StringId::kAlmanacYi33, i18n::StringId::kAlmanacYi34},
    {i18n::StringId::kAlmanacYi41, i18n::StringId::kAlmanacYi42, i18n::StringId::kAlmanacYi43, i18n::StringId::kAlmanacYi44},
    {i18n::StringId::kAlmanacYi51, i18n::StringId::kAlmanacYi52, i18n::StringId::kAlmanacYi53, i18n::StringId::kAlmanacYi54},
    {i18n::StringId::kAlmanacYi61, i18n::StringId::kAlmanacYi62, i18n::StringId::kAlmanacYi63, i18n::StringId::kAlmanacYi64},
    {i18n::StringId::kAlmanacYi71, i18n::StringId::kAlmanacYi72, i18n::StringId::kAlmanacYi73, i18n::StringId::kAlmanacYi74},
    {i18n::StringId::kAlmanacYi81, i18n::StringId::kAlmanacYi82, i18n::StringId::kAlmanacYi83, i18n::StringId::kAlmanacYi84},
    {i18n::StringId::kAlmanacYi91, i18n::StringId::kAlmanacYi92, i18n::StringId::kAlmanacYi93, i18n::StringId::kAlmanacYi94},
    {i18n::StringId::kAlmanacYi101, i18n::StringId::kAlmanacYi102, i18n::StringId::kAlmanacYi103, i18n::StringId::kAlmanacYi104},
};
static const i18n::StringId kJiTableIds[][3] = {
    {i18n::StringId::kAlmanacJi11, i18n::StringId::kAlmanacJi12, i18n::StringId::kAlmanacJi13},
    {i18n::StringId::kAlmanacJi21, i18n::StringId::kAlmanacJi22, i18n::StringId::kAlmanacJi23},
    {i18n::StringId::kAlmanacJi31, i18n::StringId::kAlmanacJi32, i18n::StringId::kAlmanacJi33},
    {i18n::StringId::kAlmanacJi41, i18n::StringId::kAlmanacJi42, i18n::StringId::kAlmanacJi43},
    {i18n::StringId::kAlmanacJi51, i18n::StringId::kAlmanacJi52, i18n::StringId::kAlmanacJi53},
    {i18n::StringId::kAlmanacJi61, i18n::StringId::kAlmanacJi62, i18n::StringId::kAlmanacJi63},
    {i18n::StringId::kAlmanacJi71, i18n::StringId::kAlmanacJi72, i18n::StringId::kAlmanacJi73},
    {i18n::StringId::kAlmanacJi81, i18n::StringId::kAlmanacJi82, i18n::StringId::kAlmanacJi83},
    {i18n::StringId::kAlmanacJi91, i18n::StringId::kAlmanacJi92, i18n::StringId::kAlmanacJi93},
    {i18n::StringId::kAlmanacJi101, i18n::StringId::kAlmanacJi102, i18n::StringId::kAlmanacJi103},
};

namespace rawdraw {

AlmanacRenderer::AlmanacRenderer()
    : font_(&SourceHanSansSC_Regular_slim)
    , title_font_(&SourceHanSansSC_Medium_slim)
    , icon_font_(&weather_icons_48) {
}

AlmanacRenderer::~AlmanacRenderer() = default;

void AlmanacRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    needs_full_refresh_ = true;
    RefreshData();
}

void AlmanacRenderer::RefreshData() {
    time_t now = time(nullptr);
    localtime_r(&now, &tm_);

    year_ = tm_.tm_year + 1900;
    month_ = tm_.tm_mon + 1;
    day_ = tm_.tm_mday;
    weekday_ = tm_.tm_wday;  // 0=Sun

    // Lunar date via Calendar algorithm
    lunar_ = Calendar::ToLunarDate(year_, month_, day_);
    lunar_year_name_ = Calendar::GetLunarYearName(year_);

    // Solar term
    solar_term_ = GetSolarTerm(month_, day_);

    // Yiji (宜忌) - simplified based on lunar day
    yi_idx_ = (lunar_.lunar_day - 1) % 10;
    ji_idx_ = (lunar_.lunar_day) % 10;
}

static const char* GetYiEntry(int row, int col) {
    return i18n::Tr(kYiTableIds[row][col]);
}

static const char* GetJiEntry(int row, int col) {
    return i18n::Tr(kJiTableIds[row][col]);
}

void AlmanacRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;

    const int content_top = Style::kStatusBarHeight + kTitleBarH + Style::kSpacingXS;
    int y = content_top + Style::kSpacingMD;
    const auto& theme = ThemeManager::Get();
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    const Color accent = theme.ColorFor(ThemeToken::Accent);
    const Color danger = theme.ColorFor(ThemeToken::Danger);
    const Color border = theme.ColorFor(ThemeToken::Border);

    // === Title bar ===
    DrawTitleBar(fb, width);

    // === Large lunar year name + date ===
    // e.g. "丙午年 三月初八"
    char lunar_full[64];
    if (lunar_.lunar_month > 0 && lunar_.lunar_day > 0) {
        snprintf(lunar_full, sizeof(lunar_full), i18n::Tr(i18n::StringId::kSYearSS2),
                 lunar_year_name_, GetLunarMonthName(lunar_.lunar_month),
                 GetLunarDayName(lunar_.lunar_day));
    } else {
        snprintf(lunar_full, sizeof(lunar_full), i18n::Tr(i18n::StringId::kSYear), lunar_year_name_);
    }

    // Draw centered
    int lunar_w = MeasureTextWidth(lunar_full, title_font_);
    int lunar_x = (width - lunar_w) / 2;
    DrawText(fb, width, lunar_x, y, lunar_full, title_font_, accent);
    y += title_font_->line_height + Style::kSpacingMD;

    // === Gregorian date ===
    char greg_buf[64];
    snprintf(greg_buf, sizeof(greg_buf), i18n::Tr(i18n::StringId::kAlmanacGregorianDate),
             year_, month_, day_, WeekdayFullName(weekday_));
    int greg_w = MeasureTextWidth(greg_buf, font_);
    int greg_x = (width - greg_w) / 2;
    DrawText(fb, width, greg_x, y, greg_buf, font_, secondary);
    y += font_->line_height + Style::kSpacingMD;

    // === Solar term (if today) ===
    if (solar_term_) {
        char st_buf[32];
        snprintf(st_buf, sizeof(st_buf), i18n::Tr(i18n::StringId::kBracketedValue), solar_term_);
        int st_w = MeasureTextWidth(st_buf, title_font_);
        int st_x = (width - st_w) / 2;
        DrawText(fb, width, st_x, y, st_buf, title_font_, accent);
        y += title_font_->line_height + Style::kSpacingMD;
    }

    // === Divider ===
    DrawHLine(fb, width, y, Style::kSpacingLG, width - Style::kSpacingLG, border);
    y += Style::kSpacingSM;

    // === 宜 (auspicious) section ===
    const char* yi_label = i18n::Tr(i18n::StringId::kDo);
    DrawText(fb, width, Style::kSpacingLG, y, yi_label, title_font_, accent);
    int yi_label_w = MeasureTextWidth(yi_label, title_font_);
    int yi_start = Style::kSpacingLG + yi_label_w + Style::kSpacingSM;
    int yi_y = y;
    for (int i = 0; i < 4; i++) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%s", GetYiEntry(yi_idx_, i));
        DrawText(fb, width, yi_start + i * 60, yi_y, buf, font_, text);
    }
    y += font_->line_height + Style::kSpacingMD;

    // === 忌 (inauspicious) section ===
    const char* ji_label = i18n::Tr(i18n::StringId::kAvoid);
    DrawText(fb, width, Style::kSpacingLG, y, ji_label, title_font_, danger);
    int ji_label_w = MeasureTextWidth(ji_label, title_font_);
    int ji_start = Style::kSpacingLG + ji_label_w + Style::kSpacingSM;
    int ji_y = y;
    for (int i = 0; i < 3; i++) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%s", GetJiEntry(ji_idx_, i));
        DrawText(fb, width, ji_start + i * 60, ji_y, buf, font_, text);
    }

    needs_full_refresh_ = false;
}

void AlmanacRenderer::DrawTitleBar(uint8_t* fb, int width) {
    const auto& theme = ThemeManager::Get();
    const PaintStyle bar_style = theme.Style(ThemeToken::BackgroundSecondary);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color border = theme.ColorFor(ThemeToken::Border);
    const int title_y_start = Style::kStatusBarHeight;
    const int title_bar_h = kTitleBarH;

    // Background
    DrawStyledRect(fb, width, {0, title_y_start, width, title_bar_h}, bar_style);

    // Top divider (2px)
    DrawHLine(fb, width, title_y_start, 0, width, border);
    DrawHLine(fb, width, title_y_start + 1, 0, width, border);

    // Bottom divider (2px)
    const int line_y = title_y_start + title_bar_h - 2;
    DrawHLine(fb, width, line_y, 0, width, border);
    DrawHLine(fb, width, line_y + 1, 0, width, border);

    // FIX: 改用 InkCenteredTextTopYInBox，避免 line_height 居中导致中文偏上
    // 参见 wiki/projects/notellm-baseline-alignment.md
    const char* title_str = i18n::Tr(i18n::StringId::kAlmanac);
    int title_text_y = InkCenteredTextTopYInBox(font_, title_str, title_y_start, title_bar_h, 1);
    DrawText(fb, width, Style::kSpacingLG, title_text_y, title_str, font_, text);
}

const char* AlmanacRenderer::GetLunarMonthName(int month) {
    if (month < 1 || month > 12) return "";
    return i18n::Tr(kLunarMonthIds[month - 1]);
}

const char* AlmanacRenderer::GetLunarDayName(int day) {
    if (day < 1 || day > 30) return "";
    return i18n::Tr(kLunarDayIds[day - 1]);
}

bool AlmanacRenderer::HandleInput(const ButtonEvent& event) {
    switch (event.type) {
        case ButtonEvent::kUpClick:
        case ButtonEvent::kDownClick:
            // Navigate months (UP=prev, DOWN=next)
            if (event.type == ButtonEvent::kUpClick) {
                month_--;
                if (month_ < 1) { month_ = 12; year_--; }
            } else {
                month_++;
                if (month_ > 12) { month_ = 1; year_++; }
            }
            lunar_ = Calendar::ToLunarDate(year_, month_, day_);
            solar_term_ = GetSolarTerm(month_, day_);
            needs_full_refresh_ = true;
            return true;

        case ButtonEvent::kBootLongPress:
            // Jump to today
            RefreshData();
            needs_full_refresh_ = true;
            return true;

        default:
            break;
    }
    return false;
}

}  // namespace rawdraw
