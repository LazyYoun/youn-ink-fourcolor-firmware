/**
 * @file font_metrics_renderer.cc
 * @brief Compact font metrics page for diagnosing rawdraw text placement.
 */

#include "font_metrics_renderer.h"
#include "rawdraw/layout_utils.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/style.h"
#include "i18n.h"

#include <cstdio>

extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;

namespace rawdraw {

namespace {

void DrawMetricLine(uint8_t* fb, int width, int height, int x, int& y, const char* text,
                    const lv_font_t* font) {
    DrawText(fb, width, x, y, text, font, BLACK, height);
    y += static_cast<int>(font->line_height) + 6;
}

}  // namespace

FontMetricsRenderer::FontMetricsRenderer()
    : font_(&::SourceHanSansSC_Regular_slim)
    , title_font_(&::SourceHanSansSC_Medium_slim) {}

FontMetricsRenderer::~FontMetricsRenderer() {}

void FontMetricsRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    needs_full_refresh_ = true;
}

void FontMetricsRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;
    DrawRect(fb, width, {0, Style::kStatusBarHeight, width, height - Style::kStatusBarHeight}, WHITE);

    const int x = 10;
    int y = Style::kStatusBarHeight + 10;
    char buf[96];

    DrawMetricLine(fb, width, height, x, y,
                   i18n::Tr(i18n::StringId::kFontMetricsPageFormulasNotAesthetics), font_);

    const TextInkBounds r_ink = MeasureTextInkBounds(font_, i18n::Tr(i18n::StringId::kRecognizing));
    snprintf(buf, sizeof(buf), "Regular: lh=%d bl=%d inkTop=%d inkBot=%d inkH=%d",
             static_cast<int>(font_->line_height),
             static_cast<int>(font_->base_line),
             r_ink.top,
             r_ink.bottom,
             r_ink.height);
    DrawMetricLine(fb, width, height, x, y, buf, font_);

    const TextInkBounds s_ink = MeasureTextInkBounds(font_, i18n::Tr(i18n::StringId::kSend));
    snprintf(buf, sizeof(buf), "%s: inkTop=%d inkBot=%d inkH=%d",
             i18n::Tr(i18n::StringId::kSend), s_ink.top, s_ink.bottom, s_ink.height);
    DrawMetricLine(fb, width, height, x, y, buf, font_);

    const TextInkBounds m_ink = MeasureTextInkBounds(title_font_, "Macintosh");
    snprintf(buf, sizeof(buf), "Medium: lh=%d bl=%d inkTop=%d inkBot=%d inkH=%d",
             static_cast<int>(title_font_->line_height),
             static_cast<int>(title_font_->base_line),
             m_ink.top,
             m_ink.bottom,
             m_ink.height);
    DrawMetricLine(fb, width, height, x, y, buf, font_);

    DrawHLine(fb, width, y, x, Style::kScreenWidth - x, BLACK);
    y += 10;

    const int box_y = y;
    const int box_h = 42;
    const int center_y = box_y + box_h / 2;
    const int line_y = CenterTextTopY(font_, box_y, box_h, 0);
    const int ink_y = InkCenteredTextTopYInBox(font_, i18n::Tr(i18n::StringId::kRecognizing), box_y, box_h, 0);
    snprintf(buf, sizeof(buf), "%s: lineTop=%d inkTop=%d delta=%d",
             i18n::Tr(i18n::StringId::k42pxBox), line_y, ink_y, ink_y - line_y);
    DrawMetricLine(fb, width, height, x, y, buf, font_);

    snprintf(buf, sizeof(buf), "%s: top + (h-lh)/2 = %d", i18n::Tr(i18n::StringId::kLineFormula), line_y);
    DrawMetricLine(fb, width, height, x, y, buf, font_);

    snprintf(buf, sizeof(buf), "%s: center(%d)-inkCenter = %d", i18n::Tr(i18n::StringId::kInkFormula), center_y, ink_y);
    DrawMetricLine(fb, width, height, x, y, buf, font_);

    DrawRectBorder(fb, width, {x, y, Style::kScreenWidth - x * 2, box_h}, 1, BLACK);
    DrawHLine(fb, width, y + box_h / 2, x, Style::kScreenWidth - x, BLACK);
    DrawText(fb, width, x + 10, InkCenteredTextTopYInBox(font_, i18n::Tr(i18n::StringId::kRecognizing), y, box_h, 0),
             i18n::Tr(i18n::StringId::kRecognizingInkCentered), font_, BLACK, height);

    needs_full_refresh_ = false;
}

bool FontMetricsRenderer::HandleInput(const ButtonEvent& event) {
    switch (event.type) {
        case ButtonEvent::kBootClick:
        case ButtonEvent::kUpClick:
        case ButtonEvent::kDownClick:
            needs_full_refresh_ = true;
            return true;
        default:
            return false;
    }
}

}  // namespace rawdraw
