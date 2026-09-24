/**
 * @file voice_wakeup.cc
 * @brief Voice wakeup overlay implementation
 *
 * Renders a floating overlay showing voice recording status.
 * Auto-transitions from temporary states (OFFLINE_MSG, DONE) back to IDLE.
 */

#include "voice_wakeup.h"
#include <cstring>
#include <cstdio>
#include "esp_timer.h"
#include "i18n.h"
#include "style.h"

namespace rawdraw {

void VoiceWakeupInit(VoiceWakeupState* state, const lv_font_t* font) {
    if (!state) return;
    state->state = VoiceState::IDLE;
    state->visible = false;
    state->overlay_text[0] = '\0';
    state->state_start_us = 0;
    state->font = font;
    refresh_tracker_init(&state->refresh);
}

void VoiceWakeupStartRecording(VoiceWakeupState* state) {
    if (!state) return;
    state->state = VoiceState::RECORDING;
    snprintf(state->overlay_text, sizeof(state->overlay_text), "%s",
             i18n::Tr(i18n::StringId::kRecording));  // "录音中..."
    state->state_start_us = esp_timer_get_time();
    state->visible = true;
    refresh_mark_dirty(&state->refresh);
}

void VoiceWakeupWaiting(VoiceWakeupState* state) {
    if (!state) return;
    state->state = VoiceState::WAITING_RESPONSE;
    snprintf(state->overlay_text, sizeof(state->overlay_text), "%s",
             i18n::Tr(i18n::StringId::kProcessing));  // "处理中..."
    state->state_start_us = esp_timer_get_time();
    refresh_mark_dirty(&state->refresh);
}

void VoiceWakeupShowOffline(VoiceWakeupState* state) {
    if (!state) return;
    state->state = VoiceState::OFFLINE_MSG;
    snprintf(state->overlay_text, sizeof(state->overlay_text), "%s",
             i18n::Tr(i18n::StringId::kVoiceUnavailableWhileOffline));  // "离线状态下无法使用语音"
    state->state_start_us = esp_timer_get_time();
    state->visible = true;
    refresh_mark_dirty(&state->refresh);
}

void VoiceWakeupDone(VoiceWakeupState* state) {
    if (!state) return;
    state->state = VoiceState::DONE;
    snprintf(state->overlay_text, sizeof(state->overlay_text), "%s",
             i18n::Tr(i18n::StringId::kDone));  // "完成"
    state->state_start_us = esp_timer_get_time();
    refresh_mark_dirty(&state->refresh);
}

void VoiceWakeupReset(VoiceWakeupState* state) {
    if (!state) return;
    state->state = VoiceState::IDLE;
    state->visible = false;
    state->overlay_text[0] = '\0';
    state->state_start_us = 0;
    refresh_mark_clean(&state->refresh);
}

bool VoiceWakeupIsVisible(const VoiceWakeupState* state) {
    return state && state->visible;
}

void VoiceWakeupTick(VoiceWakeupState* state, int64_t now_us) {
    if (!state || !state->visible) return;

    int64_t elapsed_ms = (now_us - state->state_start_us) / 1000;

    switch (state->state) {
        case VoiceState::OFFLINE_MSG:
            if (elapsed_ms >= kVoiceOfflineMsgMs) {
                VoiceWakeupReset(state);
            }
            break;

        case VoiceState::DONE:
            if (elapsed_ms >= kVoiceDoneFadeMs) {
                VoiceWakeupReset(state);
            }
            break;

        case VoiceState::RECORDING:
        case VoiceState::WAITING_RESPONSE:
        case VoiceState::IDLE:
        case VoiceState::CHECKING_NETWORK:
            // These states are externally controlled, no auto-transition
            break;
    }
}

Rect VoiceWakeupGetBounds() {
    return { kVoiceOverlayX, kVoiceOverlayY, kVoiceOverlayW, kVoiceOverlayH };
}

const char* VoiceStateToString(VoiceState state) {
    switch (state) {
        case VoiceState::IDLE:              return "IDLE";
        case VoiceState::CHECKING_NETWORK:  return "CHECKING_NETWORK";
        case VoiceState::RECORDING:         return "RECORDING";
        case VoiceState::WAITING_RESPONSE:  return "WAITING_RESPONSE";
        case VoiceState::OFFLINE_MSG:       return "OFFLINE_MSG";
        case VoiceState::DONE:              return "DONE";
        default:                            return "UNKNOWN";
    }
}

bool VoiceWakeupDraw(uint8_t* fb, int width, int height, VoiceWakeupState* state) {
    if (!fb || !state || !state->visible) return false;

    Rect bounds = VoiceWakeupGetBounds();
    bounds = clamp_rect(bounds, width, height);
    if (rect_area(bounds) <= 0) return false;

    // Draw rounded rectangle background (white)
    DrawRoundRect(fb, width, height, bounds, Style::kBorderRadiusMD, WHITE, BLACK, Style::kBorderThin);

    // Draw state-specific icon + text
    if (state->font && state->overlay_text[0] != '\0') {
        // Center text in overlay
        int text_w = MeasureTextWidth(state->overlay_text, state->font);
        int text_h = state->font->line_height;
        int text_x = bounds.x + (bounds.w - text_w) / 2;
        int text_y = bounds.y + (bounds.h - text_h) / 2;

        DrawText(fb, width, text_x, text_y, state->overlay_text, state->font, BLACK, height);
    }

    // Update refresh counter
    refresh_update_counter(&state->refresh, esp_timer_get_time());

    return true;
}

bool VoiceWakeupDrawFb(Framebuffer* fb, int screen_width, int screen_height, VoiceWakeupState* state) {
    if (!fb) return false;
    bool drawn = false;
    fb->SafeDraw([&drawn, screen_width, screen_height, state](uint8_t* buffer) {
        drawn = VoiceWakeupDraw(buffer, screen_width, screen_height, state);
    });
    return drawn;
}

}  // namespace rawdraw
