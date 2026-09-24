/**
 * @file calendar_renderer.cc
 * @brief Monthly calendar grid renderer — delegates to rawdraw::Calendar component
 *
 * Navigation: UP/DOWN=翻月, BOOT=进入选择模式/确认日期
 * In selection mode: UP/DOWN=移动光标, BOOT=确认选择
 */

#include "calendar_renderer.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/style.h"
#include "i18n.h"
#include <algorithm>
#include <cstdio>
#include <ctime>

namespace rawdraw {

// ============================================================
// Lifecycle
// ============================================================

CalendarRenderer::CalendarRenderer()
    : cal_(0, 0, 400, 300)
    , title_font_(&SourceHanSansSC_Medium_slim)
    , body_font_(&SourceHanSansSC_Regular_slim)
    , small_font_(&SourceHanSansSC_Regular_slim)
    , year_(2026)
    , month_(1)
    , today_day_(1)
    , today_month_(1)
    , today_year_(2026)
    , selected_date_{0, 0, 0} {
}

CalendarRenderer::~CalendarRenderer() {}

// ============================================================
// Init
// ============================================================

void CalendarRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    needs_full_refresh_ = true;

    // Get today's date
    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    today_year_ = tm_buf.tm_year + 1900;
    today_month_ = tm_buf.tm_mon + 1;
    today_day_ = tm_buf.tm_mday;

    // Calendar has no bottom footer/menu bar. Start lower than generic pages
    // so the first week (1-4) is not visually glued to the status bar, while
    // still using the full lower panel for the 6-row month grid.
    const int content_top = Style::kStatusBarHeight + 22;
    cal_.SetBounds(0, content_top, width, height - content_top);
    cal_.SetTitleFont(title_font_);
    cal_.SetBodyFont(body_font_);
    cal_.SetSmallFont(small_font_);
    cal_.SetShowLunar(true);
    cal_.SetShowOverflowDays(false);
    cal_.SetShowHeader(false);  // Use global status bar for title instead

    // Sync state
    year_ = today_year_;
    month_ = today_month_;
    selected_date_ = {0, 0, 0};
}

// ============================================================
// Render
// ============================================================

void CalendarRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;

    // Sync year/month to component (component may have changed via navigation)
    cal_.SetDate(year_, month_);
    cal_.Draw(fb, width, height);

    needs_full_refresh_ = false;
}

// ============================================================
// Input handling
// ============================================================

bool CalendarRenderer::HandleInput(const ButtonEvent& event) {
    // If in selection mode, route to calendar cursor navigation
    if (cal_.InSelectionMode()) {
        switch (event.type) {
            case ButtonEvent::kUpClick:
                cal_.NavigateSelection(-1);  // Move cursor up
                needs_full_refresh_ = true;
                return true;

            case ButtonEvent::kDownClick:
                cal_.NavigateSelection(1);   // Move cursor down
                needs_full_refresh_ = true;
                return true;

            case ButtonEvent::kBootClick:
                if (cal_.ConfirmSelection()) {
                    // Capture the confirmed date
                    int sel_day = cal_.GetSelectedDay();
                    selected_date_ = {cal_.GetYear(), cal_.GetMonth(), sel_day};
                    year_ = cal_.GetYear();
                    month_ = cal_.GetMonth();
                    needs_full_refresh_ = true;
                }
                return true;

            case ButtonEvent::kUpLongPress:
                cal_.PrevMonth();
                needs_full_refresh_ = true;
                return true;

            case ButtonEvent::kDownLongPress:
                cal_.NextMonth();
                needs_full_refresh_ = true;
                return true;

            default:
                break;
        }
        return false;
    }

    // Normal mode (not in selection)
    switch (event.type) {
        case ButtonEvent::kUpClick:
            cal_.PrevMonth();
            year_ = cal_.GetYear();
            month_ = cal_.GetMonth();
            needs_full_refresh_ = true;
            return true;

        case ButtonEvent::kDownClick:
            cal_.NextMonth();
            year_ = cal_.GetYear();
            month_ = cal_.GetMonth();
            needs_full_refresh_ = true;
            return true;

        case ButtonEvent::kBootClick:
            // Enter selection mode with cursor on today
            cal_.EnterSelectionMode();
            year_ = cal_.GetYear();
            month_ = cal_.GetMonth();
            needs_full_refresh_ = true;
            return true;

        default:
            break;
    }

    return false;
}

CalendarRenderer::SelectedDate CalendarRenderer::GetSelectedDate() const {
    return selected_date_;
}

std::string CalendarRenderer::GetVoiceQueryContext() const {
    if (selected_date_.year == 0 || selected_date_.month == 0 || selected_date_.day == 0) {
        return "";
    }

    // Get lunar date
    rawdraw::Calendar::LunarDate ld = rawdraw::Calendar::ToLunarDate(
        selected_date_.year, selected_date_.month, selected_date_.day);

    char buf[128];
    if (ld.lunar_year > 0) {
        const char* year_name = rawdraw::Calendar::GetLunarYearName(selected_date_.year);
        const char* leap_prefix = ld.is_leap_month ? i18n::Tr(i18n::StringId::kLeap) : "";
        snprintf(buf, sizeof(buf), i18n::Tr(i18n::StringId::kDDDSYearSSS),
                 selected_date_.year, selected_date_.month, selected_date_.day,
                 year_name, leap_prefix,
                 rawdraw::Calendar::GetLunarMonthName(ld.lunar_month),
                 rawdraw::Calendar::GetLunarDayName(ld.lunar_day));
    } else {
        snprintf(buf, sizeof(buf), i18n::Tr(i18n::StringId::kCalendarFullDate),
                 selected_date_.year, selected_date_.month, selected_date_.day);
    }
    return std::string(buf);
}

}  // namespace rawdraw
