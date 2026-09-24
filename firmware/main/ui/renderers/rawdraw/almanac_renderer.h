/**
 * @file almanac_renderer.h
 * @brief Almanac page renderer for rawdraw mode
 *
 * Displays lunar date, solar terms, and traditional almanac info (宜忌).
 * Uses Calendar::ToLunarDate() for lunar calendar conversion.
 */

#ifndef RAWDRAW_ALMANAC_RENDERER_H
#define RAWDRAW_ALMANAC_RENDERER_H

#include "page_renderer.h"
#include <ctime>
#include "rawdraw/components/calendar.h"

namespace rawdraw {

class AlmanacRenderer : public PageRenderer {
public:
    static constexpr int kTitleBarH = 28;

    AlmanacRenderer();
    ~AlmanacRenderer() override;

    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

private:
    void DrawTitleBar(uint8_t* fb, int width);
    void RefreshData();
    static const char* GetLunarMonthName(int month);
    static const char* GetLunarDayName(int day);

    const lv_font_t* font_;
    const lv_font_t* title_font_;
    const lv_font_t* icon_font_;

    int year_ = 0;
    int month_ = 0;
    int day_ = 0;
    int weekday_ = 0;
    struct tm tm_{};
    Calendar::LunarDate lunar_{};
    const char* lunar_year_name_ = "";
    const char* solar_term_ = nullptr;
    int yi_idx_ = 0;
    int ji_idx_ = 0;
};

}  // namespace rawdraw

#endif  // RAWDRAW_ALMANAC_RENDERER_H
