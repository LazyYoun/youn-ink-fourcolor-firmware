/**
 * @file font_debug_renderer.cc
 * @brief Large-row text alignment diagnostics for 400x300 rawdraw EPD.
 */

#include "font_debug_renderer.h"
#include "rawdraw/layout_utils.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/style.h"
#include "i18n.h"

#include <cstdio>

extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;

namespace rawdraw {

namespace {

void DashedHLine(uint8_t* fb, int w, int y, int x1, int x2, Color c) {
    for (int x = x1; x <= x2; x += 5) {
        set_pixel(fb, w, x, y, c);
        if (x + 1 <= x2) set_pixel(fb, w, x + 1, y, c);
    }
}

void DrawDiagnosticRow(uint8_t* fb,
                       int width,
                       int height,
                       int x,
                       int y,
                       int w,
                       int h,
                       const char* label,
                       const char* text,
                       const lv_font_t* font,
                       bool ink_centered) {
    DrawRectBorder(fb, width, {x, y, w, h}, 1, BLACK);
    const int center_y = y + h / 2;
    DashedHLine(fb, width, center_y, x + 1, x + w - 2, BLACK);

    const int text_x = x + 8;
    const int text_y = ink_centered
        ? InkCenteredTextTopY(font, text, center_y, 0)
        : CenterTextTopY(font, y, h, 0);
    DrawText(fb, width, text_x, text_y, text, font, BLACK, height);

    const TextInkBounds ink = MeasureTextInkBounds(font, text);
    if (ink.valid) {
        DrawHLine(fb, width, text_y + ink.top, text_x, text_x + 34, BLACK);
        DrawHLine(fb, width, text_y + ink.bottom, text_x, text_x + 34, BLACK);
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "%s y=%d", label, text_y);
    DrawText(fb, width, x + w - 122, y + 4, buf, &SourceHanSansSC_Regular_slim, BLACK, height);
}

}  // namespace

FontDebugRenderer::FontDebugRenderer()
    : font_(&::SourceHanSansSC_Regular_slim)
    , title_font_(&::SourceHanSansSC_Medium_slim) {}

FontDebugRenderer::~FontDebugRenderer() {}

void FontDebugRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    needs_full_refresh_ = true;
}

void FontDebugRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;
    DrawRect(fb, width, {0, Style::kStatusBarHeight, width, height - Style::kStatusBarHeight}, WHITE);

    const int x = 10;
    const int w = Style::kScreenWidth - 20;
    int y = Style::kStatusBarHeight + 10;

    const char* hint = i18n::Tr(i18n::StringId::kDashedBoxCenterShortBlackTrueGlyphInkBounds);
    DrawText(fb, width, x, y, hint, font_, BLACK, height);
    y += 22;

    DrawDiagnosticRow(fb, width, height, x, y, w, 42,
                      "line", i18n::Tr(i18n::StringId::kRecognizing), font_, false);
    y += 50;
    DrawDiagnosticRow(fb, width, height, x, y, w, 42,
                      "ink", i18n::Tr(i18n::StringId::kRecognizing), font_, true);
    y += 50;
    DrawDiagnosticRow(fb, width, height, x, y, w, 42,
                      "line", i18n::Tr(i18n::StringId::kSend), font_, false);
    y += 50;
    DrawDiagnosticRow(fb, width, height, x, y, w, 48,
                      "inkM", i18n::Tr(i18n::StringId::kMacintoshAbout), title_font_, true);

    const TextInkBounds regular = MeasureTextInkBounds(font_, i18n::Tr(i18n::StringId::kRecognizing));
    char footer[96];
    snprintf(footer, sizeof(footer), "Regular lh=%d bl=%d ink=%d..%d h=%d",
             static_cast<int>(font_->line_height),
             static_cast<int>(font_->base_line),
             regular.top,
             regular.bottom,
             regular.height);
    DrawText(fb, width, x, Style::kScreenHeight - 20, footer, font_, BLACK, height);

    needs_full_refresh_ = false;
}

bool FontDebugRenderer::HandleInput(const ButtonEvent& event) {
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
