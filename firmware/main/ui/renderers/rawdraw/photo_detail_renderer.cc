/**
 * @file photo_detail_renderer.cc
 * @brief Photo detail page renderer with metadata modal
 */

#include "photo_detail_renderer.h"

#include "i18n.h"
#include "rawdraw/components/modal.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/style.h"
#include "rawdraw/theme.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

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

int BytesPerRow1bpp(int width) {
    return std::max(1, (width + 7) / 8);
}

int BytesPerRow2bpp(int width) {
    return std::max(1, (width + 3) / 4);
}

bool IsBwry2bppImage(int width, int height, uint32_t size) {
    return width > 0 && height > 0 && size >= static_cast<uint32_t>(BytesPerRow2bpp(width) * height);
}

bool IsMono1bppImage(int width, int height, uint32_t size) {
    return width > 0 && height > 0 && size >= static_cast<uint32_t>(BytesPerRow1bpp(width) * height);
}

Color ReadPhotoPixelColor(const uint8_t* data, uint32_t size, int photo_width, bool bwry2bpp,
                          int src_x, int src_y) {
    if (!data || photo_width <= 0 || src_x < 0 || src_y < 0) return BLACK;
    if (bwry2bpp) {
        const int bpr = BytesPerRow2bpp(photo_width);
        const int offset = src_y * bpr + (src_x >> 2);
        if (offset < 0 || offset >= static_cast<int>(size)) return BLACK;
        const int shift = 6 - ((src_x & 0x03) * 2);
        const uint8_t color = (data[offset] >> shift) & 0x03;
        return static_cast<Color>(color);
    }

    const int bpr = BytesPerRow1bpp(photo_width);
    const int offset = src_y * bpr + (src_x >> 3);
    if (offset < 0 || offset >= static_cast<int>(size)) return BLACK;
    const int bit = 7 - (src_x & 0x07);
    return ((data[offset] >> bit) & 0x01) != 0 ? WHITE : BLACK;
}

}  // namespace

PhotoDetailRenderer::PhotoDetailRenderer()
    : font_(&SourceHanSansSC_Regular_slim),
      title_font_(&SourceHanSansSC_Medium_slim) {}

PhotoDetailRenderer::~PhotoDetailRenderer() {
    if (current_photo_data_) {
        free(current_photo_data_);
        current_photo_data_ = nullptr;
    }
}

void PhotoDetailRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    metadata_open_ = false;
    RefreshPhotoList();
    LoadPhotoData(selected_index_);
    needs_full_refresh_ = true;
}

void PhotoDetailRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;
    const auto& theme = ThemeManager::Get();
    DrawStyledRect(fb, width, {0, Style::kStatusBarHeight, width, height - Style::kStatusBarHeight},
                   theme.Style(ThemeToken::BackgroundPrimary));

    if (photos_.empty() || !current_photo_data_ || current_photo_size_ == 0) {
        Modal modal;
        modal.SetTitle(i18n::Tr(i18n::StringId::kNoPhotos));
        modal.SetBodyFooter(i18n::Tr(i18n::StringId::kWaitingForUpload));
        modal.CenterInScreen(width, height, 52);
        modal.Draw(fb, width, height);
    } else {
        // 图片详情页去掉底栏，最大化展示区域
        const int frame_x = 8;
        const int frame_y = Style::kStatusBarHeight + 4;
        const int frame_w = width - 16;
        const int frame_h = height - frame_y - 4;
        DrawStyledRoundRect(fb, width, height, {frame_x, frame_y, frame_w, frame_h},
                            Style::kBorderRadiusMD, theme.Component(ComponentRole::CardDefault));

        const bool bwry2bpp = IsBwry2bppImage(current_photo_width_, current_photo_height_, current_photo_size_);
        const int photo_byte_width = bwry2bpp ? BytesPerRow2bpp(current_photo_width_)
                                              : BytesPerRow1bpp(current_photo_width_);
        const int expected_rows = (bwry2bpp || IsMono1bppImage(current_photo_width_, current_photo_height_, current_photo_size_))
                                      ? std::min<int>(current_photo_height_, current_photo_size_ / photo_byte_width)
                                      : 0;
        const int inner_x = frame_x + 6;
        const int inner_y = frame_y + 6;
        const int inner_w = frame_w - 12;
        const int inner_h = frame_h - 12;

        for (int ty = 0; ty < inner_h; ++ty) {
            const int src_y = (ty * current_photo_height_) / std::max(1, inner_h);
            if (src_y >= expected_rows) break;
            for (int tx = 0; tx < inner_w; ++tx) {
                const int src_x = (tx * current_photo_width_) / std::max(1, inner_w);
                const Color src_color = ReadPhotoPixelColor(current_photo_data_, current_photo_size_,
                                                            current_photo_width_, bwry2bpp, src_x, src_y);
                set_pixel(fb, width, inner_x + tx, inner_y + ty, src_color);
            }
        }
    }

    if (metadata_open_ && !photos_.empty()) {
        DrawMetadataModal(fb, width, height);
    }

    // 图片详情页无底栏，最大化展示
    // 操作提示：UP/DN翻页，BOOT查看信息（可通过 statusBar 或 modal 提示）

    needs_full_refresh_ = false;
}

bool PhotoDetailRenderer::HandleInput(const ButtonEvent& event) {
    if (metadata_open_) {
        if (event.type == ButtonEvent::kBootClick || event.type == ButtonEvent::kBootLongPress) {
            metadata_open_ = false;
            needs_full_refresh_ = true;
            return true;
        }
        return true;
    }

    switch (event.type) {
        case ButtonEvent::kUpClick:
            if (selected_index_ > 0) {
                selected_index_--;
                LoadPhotoData(selected_index_);
                needs_full_refresh_ = true;
                return true;
            }
            break;
        case ButtonEvent::kDownClick:
            if (selected_index_ < static_cast<int>(photos_.size()) - 1) {
                selected_index_++;
                LoadPhotoData(selected_index_);
                needs_full_refresh_ = true;
                return true;
            }
            break;
        case ButtonEvent::kBootClick:
            if (!photos_.empty()) {
                metadata_open_ = true;
                needs_full_refresh_ = true;
                return true;
            }
            break;
        default:
            break;
    }
    return false;
}

void PhotoDetailRenderer::RefreshPhotoList() {
    photos_.clear();
    PhotoInfo info;
    const int count = photo_get_count();
    for (int i = 0; i < count && i < PHOTO_MAX_PHOTOS; ++i) {
        if (photo_get_by_index(i, &info) == 0) {
            photos_.push_back(info);
        }
    }
    ClampSelection();
}

void PhotoDetailRenderer::SetSelection(int index) {
    selected_index_ = index;
    ClampSelection();
    LoadPhotoData(selected_index_);
    needs_full_refresh_ = true;
}

bool PhotoDetailRenderer::IsCurrentPhotoBwry2bpp() const {
    return IsBwry2bppImage(current_photo_width_, current_photo_height_, current_photo_size_);
}

void PhotoDetailRenderer::LoadPhotoData(int index) {
    if (current_photo_data_) {
        free(current_photo_data_);
        current_photo_data_ = nullptr;
    }
    current_photo_size_ = 0;

    if (index < 0 || index >= static_cast<int>(photos_.size())) return;
    const auto& info = photos_[index];
    current_photo_data_ = static_cast<uint8_t*>(malloc(info.file_size));
    if (!current_photo_data_) return;
    const int bytes_read = photo_load(info.id, current_photo_data_, info.file_size);
    if (bytes_read > 0) {
        current_photo_size_ = bytes_read;
        current_photo_width_ = info.width;
        current_photo_height_ = info.height;
    } else {
        free(current_photo_data_);
        current_photo_data_ = nullptr;
    }
}

void PhotoDetailRenderer::ClampSelection() {
    if (photos_.empty()) {
        selected_index_ = 0;
    } else {
        selected_index_ = std::max(0, std::min(selected_index_, static_cast<int>(photos_.size()) - 1));
    }
}

void PhotoDetailRenderer::DrawMetadataModal(uint8_t* fb, int width, int height) {
    const auto& info = photos_[selected_index_];
    const auto& theme = ThemeManager::Get();
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    Modal modal;
    modal.SetTitle(i18n::Tr(i18n::StringId::kPhotoInfo));
    modal.SetBodyFooter(i18n::Tr(i18n::StringId::kBootClose));
    modal.CenterInScreen(width, height, 40);
    modal.Draw(fb, width, height);

    const Rect body = modal.GetContentBounds();
    int y = body.y;
    DrawText(fb, width, body.x, y, FitTextToWidth(info.title, title_font_, body.w).c_str(), title_font_, text);
    y += title_font_->line_height + 8;
    DrawText(fb, width, body.x, y, FitTextToWidth(info.date, font_, body.w).c_str(), font_, secondary);
    y += font_->line_height + 4;
    DrawText(fb, width, body.x, y, FitTextToWidth(info.location, font_, body.w).c_str(), font_, secondary);
    y += font_->line_height + 8;
    DrawText(fb, width, body.x, y, FitTextToWidth(info.body, font_, body.w).c_str(), font_, text);
}

}  // namespace rawdraw
