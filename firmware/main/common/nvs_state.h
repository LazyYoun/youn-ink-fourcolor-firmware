/**
 * @file nvs_state.h
 * @brief Unified NVS persistent state for app-level preferences
 *
 * Handles persistence for:
 * - Weather city preference
 * - Calendar display preferences (show lunar, selected date)
 * - EPD refresh counters (trigger full refresh every 100 partials)
 * - Last active page / scroll positions
 */

#ifndef NVS_STATE_H
#define NVS_STATE_H

#include <cstdint>
#include <string>

/**
 * @brief Persistent weather preferences
 */
struct WeatherPrefs {
    std::string city;     // User's preferred city (e.g. "杭州")
    bool auto_update;     // Auto-refresh weather data (default true)
    int update_interval;  // Hours between auto-updates (default 1)
};

/**
 * @brief Persistent calendar preferences
 */
struct CalendarPrefs {
    bool show_lunar;           // Show lunar dates (default true)
    int selected_year;         // Last viewed year
    int selected_month;        // Last viewed month
    int selected_day;          // Last selected day (0 = none)
};

/**
 * @brief Persistent EPD refresh counters
 */
struct EpdRefreshState {
    int partial_count;         // Partial refreshes since last full refresh
    int lifetime_refreshes;    // Total refreshes (for panel wear tracking)
    int last_refresh_timestamp; // Unix timestamp of last refresh
};

/**
 * @brief Persistent UI navigation state
 */
struct UiNavState {
    int last_page;             // Last active page index
    int summary_scroll;        // Summary page scroll offset
    int lifebar_visible;       // Life bar visibility toggle (0/1)
};

/**
 * @brief Persistent BLE state
 */
struct BleState {
    bool enabled;              // BLE enabled (0=off, 1=on)
};

namespace nvs_state {

// ============================================================
// Initialization / cleanup
// ============================================================

/**
 * @brief Initialize NVS state namespace (called once at boot)
 * @return true if NVS partition is accessible
 */
bool Init();

/**
 * @brief Commit pending writes (flush to flash)
 */
void Commit();

// ============================================================
// Weather preferences
// ============================================================

/**
 * @brief Load weather city preference
 */
WeatherPrefs LoadWeatherPrefs();

/**
 * @brief Save weather city preference
 */
void SaveWeatherPrefs(const WeatherPrefs& prefs);

// ============================================================
// Calendar preferences
// ============================================================

/**
 * @brief Load calendar display preferences
 */
CalendarPrefs LoadCalendarPrefs();

/**
 * @brief Save calendar display preferences
 */
void SaveCalendarPrefs(const CalendarPrefs& prefs);

// ============================================================
// EPD refresh counters
// ============================================================

/**
 * @brief Load EPD refresh state
 */
EpdRefreshState LoadEpdRefreshState();

/**
 * @brief Save EPD refresh state (called on each refresh)
 */
void SaveEpdRefreshState(const EpdRefreshState& state);

/**
 * @brief Increment partial refresh counter, return true if full refresh needed
 * @param threshold Number of partial refreshes before forcing a full refresh (default 100)
 */
bool ShouldForceFullRefresh(int threshold = 100);

/**
 * @brief Reset partial refresh counter (called after a full refresh)
 */
void ResetPartialCounter();

// ============================================================
// UI navigation state
// ============================================================

/**
 * @brief Load UI navigation state
 */
UiNavState LoadUiNavState();

/**
 * @brief Save UI navigation state
 */
void SaveUiNavState(const UiNavState& state);

// ============================================================
// BLE state
// ============================================================

BleState LoadBleState();
void SaveBleState(const BleState& state);

// ============================================================
// UI language
// ============================================================

/**
 * @brief Load persisted on-device UI language (0 = zh-CN, 1 = en-US)
 */
int LoadUiLanguage();

/**
 * @brief Save on-device UI language (0 = zh-CN, 1 = en-US)
 */
void SaveUiLanguage(int language);

// ============================================================
// AP transfer boot mode
// ============================================================

bool LoadApTransferBootMode();
void SaveApTransferBootMode(bool enabled);

}  // namespace nvs_state

#endif  // NVS_STATE_H
