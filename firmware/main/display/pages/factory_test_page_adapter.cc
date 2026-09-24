#include "pages/factory_test_page_adapter.h"

#include "i18n.h"
#include "lcd_display.h"
#include "lvgl_theme.h"

#include <esp_log.h>

#include <cstdio>

namespace {

constexpr lv_coord_t kPageWidth = 400;
constexpr lv_coord_t kPageHeight = 300;
constexpr lv_coord_t kHeaderHeight = 48;
constexpr lv_coord_t kStepColumnWidth = 104;
constexpr lv_coord_t kFooterHeight = 24;
constexpr lv_coord_t kStepRowHeight = 24;
constexpr int kStepCount = 7;

// Looked up fresh on every call (not a static/const-init array) so the
// label reflects the language active when the page is built rather than
// freezing at first use.
const char* StepName(int index) {
    switch (index) {
        case 0: return "RF";
        case 1: return i18n::Tr(i18n::StringId::kAudio);
        case 2: return "RTC";
        case 3: return i18n::Tr(i18n::StringId::kCharging);
        case 4: return "LED";
        case 5: return i18n::Tr(i18n::StringId::kButton);
        case 6: return "NFC";
        default: return "";
    }
}

const char* StateText(FactoryTestStepState state) {
    switch (state) {
        case FactoryTestStepState::kRunning:
            return "RUN";
        case FactoryTestStepState::kPass:
            return "PASS";
        case FactoryTestStepState::kFail:
            return "FAIL";
        case FactoryTestStepState::kWait:
        default:
            return "WAIT";
    }
}

const char* KeyItemText(const char* raw, FactoryTestStepState* out_state) {
    if (out_state != nullptr) {
        *out_state = FactoryTestStepState::kWait;
    }
    if (raw == nullptr) {
        return "";
    }
    if (raw[0] == '[' && raw[2] == ']' && raw[3] == ' ') {
        if (out_state != nullptr) {
            switch (raw[1]) {
                case 'x':
                    *out_state = FactoryTestStepState::kPass;
                    break;
                case '>':
                    *out_state = FactoryTestStepState::kRunning;
                    break;
                case 'X':
                    *out_state = FactoryTestStepState::kFail;
                    break;
                default:
                    *out_state = FactoryTestStepState::kWait;
                    break;
            }
        }
        return raw + 4;
    }
    return raw;
}

void SetKeyItemVisual(lv_obj_t* label, const char* raw) {
    if (label == nullptr) {
        return;
    }

    FactoryTestStepState state = FactoryTestStepState::kWait;
    const char* text = KeyItemText(raw, &state);
    char buffer[96];
    if (state == FactoryTestStepState::kFail) {
        snprintf(buffer, sizeof(buffer), "X %s", text);
    } else {
        snprintf(buffer, sizeof(buffer), "%s", text);
    }
    lv_label_set_text(label, buffer);
    lv_obj_set_style_text_decor(label,
                                state == FactoryTestStepState::kPass
                                    ? LV_TEXT_DECOR_STRIKETHROUGH
                                    : LV_TEXT_DECOR_NONE,
                                0);
    lv_obj_set_style_bg_opa(label,
                            state == FactoryTestStepState::kRunning ? LV_OPA_COVER : LV_OPA_TRANSP,
                            0);
    lv_obj_set_style_bg_color(label,
                              state == FactoryTestStepState::kRunning ? lv_color_black() : lv_color_white(),
                              0);
    lv_obj_set_style_text_color(label,
                                state == FactoryTestStepState::kRunning ? lv_color_white() : lv_color_black(),
                                0);
}

void StyleScreen(lv_obj_t* obj) {
    lv_obj_set_size(obj, kPageWidth, kPageHeight);
    lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

void StyleCard(lv_obj_t* obj) {
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_black(), 0);
    lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(obj, 8, 0);
}

}  // namespace

FactoryTestPageAdapter::FactoryTestPageAdapter(LcdDisplay* host)
    : host_(host) {}

UiPageId FactoryTestPageAdapter::Id() const {
    return UiPageId::FactoryTest;
}

const char* FactoryTestPageAdapter::Name() const {
    return "FactoryTest";
}

void FactoryTestPageAdapter::Build() {
    if (built_ || host_ == nullptr) {
        built_ = true;
        return;
    }
    if (host_->factory_test_screen_ != nullptr) {
        screen_ = host_->factory_test_screen_;
        built_ = true;
        return;
    }

    auto* lvgl_theme = static_cast<LvglTheme*>(host_->current_theme_);
    const lv_font_t* text_font = lvgl_theme->text_font()->font();
    const lv_font_t* body_font = lvgl_theme->reminder_text_font()
        ? lvgl_theme->reminder_text_font()->font()
        : text_font;
    const lv_font_t* icon_font = lvgl_theme->icon_font()->font();

    screen_ = lv_obj_create(nullptr);
    host_->factory_test_screen_ = screen_;
    StyleScreen(screen_);

    lv_obj_t* header = lv_obj_create(screen_);
    StyleCard(header);
    lv_obj_set_size(header, kPageWidth, kHeaderHeight);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_left(header, 12, 0);
    lv_obj_set_style_pad_right(header, 12, 0);

    lv_obj_t* header_title = lv_label_create(header);
    if (body_font) {
        lv_obj_set_style_text_font(header_title, body_font, 0);
    }
    lv_label_set_text(header_title, i18n::Tr(i18n::StringId::kFtTest));
    lv_obj_align(header_title, LV_ALIGN_LEFT_MID, 0, 0);

    header_step_label_ = lv_label_create(header);
    if (text_font) {
        lv_obj_set_style_text_font(header_step_label_, text_font, 0);
    }
    lv_label_set_text(header_step_label_, "1/7");
    lv_obj_align(header_step_label_, LV_ALIGN_RIGHT_MID, -70, 0);

    header_state_label_ = lv_label_create(header);
    if (text_font) {
        lv_obj_set_style_text_font(header_state_label_, text_font, 0);
    }
    lv_label_set_text(header_state_label_, "WAIT");
    lv_obj_align(header_state_label_, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t* step_panel = lv_obj_create(screen_);
    StyleCard(step_panel);
    lv_obj_set_size(step_panel, kStepColumnWidth, kPageHeight - kHeaderHeight - kFooterHeight);
    lv_obj_align(step_panel, LV_ALIGN_TOP_LEFT, 0, kHeaderHeight);
    lv_obj_set_flex_flow(step_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(step_panel, 2, 0);

    for (int i = 0; i < kStepCount; ++i) {
        step_rows_[i] = lv_obj_create(step_panel);
        lv_obj_set_size(step_rows_[i], LV_PCT(100), kStepRowHeight);
        lv_obj_set_style_border_width(step_rows_[i], 1, 0);
        lv_obj_set_style_border_color(step_rows_[i], lv_color_black(), 0);
        lv_obj_set_style_radius(step_rows_[i], 0, 0);
        lv_obj_set_style_bg_color(step_rows_[i], lv_color_white(), 0);
        lv_obj_set_style_bg_opa(step_rows_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_pad_left(step_rows_[i], 6, 0);
        lv_obj_set_style_pad_right(step_rows_[i], 6, 0);

        step_name_labels_[i] = lv_label_create(step_rows_[i]);
        if (body_font) {
            lv_obj_set_style_text_font(step_name_labels_[i], body_font, 0);
        }
        lv_label_set_text(step_name_labels_[i], StepName(i));
        lv_obj_align(step_name_labels_[i], LV_ALIGN_LEFT_MID, 0, 0);

        step_state_labels_[i] = lv_label_create(step_rows_[i]);
        if (icon_font) {
            lv_obj_set_style_text_font(step_state_labels_[i], icon_font, 0);
        }
        lv_label_set_text(step_state_labels_[i], " ");
        lv_obj_align(step_state_labels_[i], LV_ALIGN_RIGHT_MID, 0, 0);
    }

    lv_obj_t* main_panel = lv_obj_create(screen_);
    StyleCard(main_panel);
    lv_obj_set_size(main_panel, kPageWidth - kStepColumnWidth, kPageHeight - kHeaderHeight - kFooterHeight);
    lv_obj_align(main_panel, LV_ALIGN_TOP_RIGHT, 0, kHeaderHeight);
    lv_obj_set_style_pad_left(main_panel, 12, 0);
    lv_obj_set_style_pad_right(main_panel, 12, 0);
    lv_obj_set_style_pad_top(main_panel, 10, 0);
    lv_obj_set_style_pad_row(main_panel, 6, 0);
    lv_obj_set_flex_flow(main_panel, LV_FLEX_FLOW_COLUMN);

    title_label_ = lv_label_create(main_panel);
    if (body_font) {
        lv_obj_set_style_text_font(title_label_, body_font, 0);
    }
    lv_obj_set_width(title_label_, LV_PCT(100));
    lv_label_set_long_mode(title_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(title_label_, "");

    hint_label_ = lv_label_create(main_panel);
    if (text_font) {
        lv_obj_set_style_text_font(hint_label_, text_font, 0);
    }
    lv_obj_set_width(hint_label_, LV_PCT(100));
    lv_label_set_long_mode(hint_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(hint_label_, "");

    detail1_label_ = lv_label_create(main_panel);
    detail2_label_ = lv_label_create(main_panel);
    detail3_label_ = lv_label_create(main_panel);
    detail4_label_ = lv_label_create(main_panel);
    if (text_font) {
        lv_obj_set_style_text_font(detail1_label_, text_font, 0);
        lv_obj_set_style_text_font(detail2_label_, text_font, 0);
        lv_obj_set_style_text_font(detail3_label_, text_font, 0);
        lv_obj_set_style_text_font(detail4_label_, text_font, 0);
    }
    lv_obj_set_width(detail1_label_, LV_PCT(100));
    lv_obj_set_width(detail2_label_, LV_PCT(100));
    lv_obj_set_width(detail3_label_, LV_PCT(100));
    lv_obj_set_width(detail4_label_, LV_PCT(100));
    lv_label_set_long_mode(detail1_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_long_mode(detail2_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_long_mode(detail3_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_long_mode(detail4_label_, LV_LABEL_LONG_WRAP);

    footer_label_ = lv_label_create(screen_);
    if (text_font) {
        lv_obj_set_style_text_font(footer_label_, text_font, 0);
    }
    lv_obj_set_width(footer_label_, kPageWidth - 12);
    lv_label_set_long_mode(footer_label_, LV_LABEL_LONG_WRAP);
    lv_obj_align(footer_label_, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_label_set_text(footer_label_, "");

    built_ = true;
    UpdateSnapshot(snapshot_);
}

lv_obj_t* FactoryTestPageAdapter::Screen() const {
    return screen_;
}

void FactoryTestPageAdapter::OnShow() {
    UpdateSnapshot(snapshot_);
}

bool FactoryTestPageAdapter::HandleEvent(const UiPageEvent& event) {
    (void)event;
    return false;
}

void FactoryTestPageAdapter::RefreshStepStateLocked(int index, FactoryTestStepState state) {
    if (index < 0 || index >= kStepCount || step_rows_[index] == nullptr) {
        return;
    }

    const bool selected = (index == static_cast<int>(snapshot_.current_step));
    const bool inverted = selected && snapshot_.current_step != FactoryTestStep::kComplete;
    lv_obj_set_style_bg_color(step_rows_[index], inverted ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_text_color(step_rows_[index], inverted ? lv_color_white() : lv_color_black(), 0);
    lv_obj_set_style_text_decor(step_name_labels_[index],
                                state == FactoryTestStepState::kPass
                                    ? LV_TEXT_DECOR_STRIKETHROUGH
                                    : LV_TEXT_DECOR_NONE,
                                0);

    const char* state_text = " ";
    if (state == FactoryTestStepState::kFail) {
        state_text = "X";
    } else if (state == FactoryTestStepState::kRunning) {
        state_text = ">";
    }
    lv_label_set_text(step_state_labels_[index], state_text);
}

void FactoryTestPageAdapter::UpdateSnapshot(const FactoryTestSnapshot& snapshot) {
    snapshot_ = snapshot;
    if (!built_ || screen_ == nullptr) {
        return;
    }

    int step_no = 0;
    switch (snapshot.current_step) {
        case FactoryTestStep::kRf:
            step_no = 1;
            break;
        case FactoryTestStep::kAudio:
            step_no = 2;
            break;
        case FactoryTestStep::kRtc:
            step_no = 3;
            break;
        case FactoryTestStep::kCharge:
            step_no = 4;
            break;
        case FactoryTestStep::kLed:
            step_no = 5;
            break;
        case FactoryTestStep::kKeys:
            step_no = 6;
            break;
        case FactoryTestStep::kNfc:
            step_no = 7;
            break;
        default:
            step_no = kStepCount;
            break;
    }

    char step_buf[16];
    snprintf(step_buf, sizeof(step_buf), "%d/%d", step_no, kStepCount);
    lv_label_set_text(header_step_label_, step_buf);
    lv_label_set_text(header_state_label_, StateText(snapshot.current_state));
    lv_label_set_text(title_label_, snapshot.title);
    lv_label_set_text(hint_label_, snapshot.hint);
    if (snapshot.current_step == FactoryTestStep::kKeys) {
        SetKeyItemVisual(detail1_label_, snapshot.detail1);
        SetKeyItemVisual(detail2_label_, snapshot.detail2);
        SetKeyItemVisual(detail3_label_, snapshot.detail3);
    } else {
        lv_label_set_text(detail1_label_, snapshot.detail1);
        lv_label_set_text(detail2_label_, snapshot.detail2);
        lv_label_set_text(detail3_label_, snapshot.detail3);
        lv_obj_set_style_text_decor(detail1_label_, LV_TEXT_DECOR_NONE, 0);
        lv_obj_set_style_text_decor(detail2_label_, LV_TEXT_DECOR_NONE, 0);
        lv_obj_set_style_text_decor(detail3_label_, LV_TEXT_DECOR_NONE, 0);
        lv_obj_set_style_bg_opa(detail1_label_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(detail2_label_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(detail3_label_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(detail1_label_, lv_color_white(), 0);
        lv_obj_set_style_bg_color(detail2_label_, lv_color_white(), 0);
        lv_obj_set_style_bg_color(detail3_label_, lv_color_white(), 0);
        lv_obj_set_style_text_color(detail1_label_, lv_color_black(), 0);
        lv_obj_set_style_text_color(detail2_label_, lv_color_black(), 0);
        lv_obj_set_style_text_color(detail3_label_, lv_color_black(), 0);
    }
    lv_label_set_text(detail4_label_, snapshot.detail4);
    lv_label_set_text(footer_label_, snapshot.footer);

    for (int i = 0; i < kStepCount; ++i) {
        RefreshStepStateLocked(i, snapshot.step_states[static_cast<size_t>(i)]);
    }
}
