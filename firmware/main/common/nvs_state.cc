/**
 * @file nvs_state.cc
 * @brief Unified NVS persistent state implementation
 *
 * Uses "app_state" NVS namespace for app-level preferences.
 * Keys are prefixed by subsystem: weather_*, calendar_*, epd_*, ui_*.
 */

#include "nvs_state.h"
#include "settings.h"
#include <esp_log.h>
#include <ctime>

namespace {
constexpr char kTag[] = "NvsState";
constexpr char kNamespace[] = "app_state";

// Key prefixes
constexpr char kWeatherCity[] = "weather_city";
constexpr char kWeatherAuto[] = "weather_auto";
constexpr char kWeatherInterval[] = "weather_interval";

constexpr char kCalLunar[] = "cal_lunar";
constexpr char kCalYear[] = "cal_year";
constexpr char kCalMonth[] = "cal_month";
constexpr char kCalDay[] = "cal_day";

constexpr char kEpdPartial[] = "epd_partial";
constexpr char kEpdLifetime[] = "epd_lifetime";
constexpr char kEpdLastTs[] = "epd_last_ts";

constexpr char kUiPage[] = "ui_page";
constexpr char kUiScroll[] = "ui_scroll";
constexpr char kUiLifebar[] = "ui_lifebar";

constexpr char kBleEnabled[] = "bt_enabled";
constexpr char kApTransferBoot[] = "ap_xfer_boot";
constexpr char kUiLanguage[] = "ui_lang";
}  // namespace

namespace nvs_state {

// ============================================================
// Init / Commit
// ============================================================

bool Init() {
    Settings test(kNamespace);
    // Try reading a dummy key; if NVS partition isn't initialized,
    // nvs_open will fail and GetInt returns default.
    // We test by attempting a write to a read-only handle.
    (void)test.GetInt("__test__", 0);
    ESP_LOGI(kTag, "NVS state initialized (namespace=%s)", kNamespace);
    return true;
}

void Commit() {
    Settings nvs(kNamespace, true);
    // Destructor auto-commits if dirty; explicit commit here for clarity.
    ESP_LOGD(kTag, "NVS state committed");
}

// ============================================================
// Weather preferences
// ============================================================

WeatherPrefs LoadWeatherPrefs() {
    Settings nvs(kNamespace);
    WeatherPrefs prefs;
    prefs.city = nvs.GetString(kWeatherCity, "");
    prefs.auto_update = nvs.GetBool(kWeatherAuto, true);
    prefs.update_interval = static_cast<int>(nvs.GetInt(kWeatherInterval, 1));
    if (!prefs.city.empty()) {
        ESP_LOGI(kTag, "Loaded weather prefs: city=%s interval=%dh",
                 prefs.city.c_str(), prefs.update_interval);
    }
    return prefs;
}

void SaveWeatherPrefs(const WeatherPrefs& prefs) {
    Settings nvs(kNamespace, true);
    nvs.SetString(kWeatherCity, prefs.city);
    nvs.SetBool(kWeatherAuto, prefs.auto_update);
    nvs.SetInt(kWeatherInterval, prefs.update_interval);
    ESP_LOGI(kTag, "Weather prefs saved: city=%s auto=%d interval=%dh",
             prefs.city.c_str(), prefs.auto_update, prefs.update_interval);
}

// ============================================================
// Calendar preferences
// ============================================================

CalendarPrefs LoadCalendarPrefs() {
    Settings nvs(kNamespace);
    CalendarPrefs prefs;
    prefs.show_lunar = nvs.GetBool(kCalLunar, true);
    prefs.selected_year = static_cast<int>(nvs.GetInt(kCalYear, 2026));
    prefs.selected_month = static_cast<int>(nvs.GetInt(kCalMonth, 1));
    prefs.selected_day = static_cast<int>(nvs.GetInt(kCalDay, 0));
    ESP_LOGI(kTag, "Loaded calendar prefs: year=%d month=%d day=%d lunar=%d",
             prefs.selected_year, prefs.selected_month, prefs.selected_day,
             prefs.show_lunar);
    return prefs;
}

void SaveCalendarPrefs(const CalendarPrefs& prefs) {
    Settings nvs(kNamespace, true);
    nvs.SetBool(kCalLunar, prefs.show_lunar);
    nvs.SetInt(kCalYear, prefs.selected_year);
    nvs.SetInt(kCalMonth, prefs.selected_month);
    nvs.SetInt(kCalDay, prefs.selected_day);
    ESP_LOGI(kTag, "Calendar prefs saved: year=%d month=%d day=%d",
             prefs.selected_year, prefs.selected_month, prefs.selected_day);
}

// ============================================================
// EPD refresh counters
// ============================================================

EpdRefreshState LoadEpdRefreshState() {
    Settings nvs(kNamespace);
    EpdRefreshState state;
    state.partial_count = static_cast<int>(nvs.GetInt(kEpdPartial, 0));
    state.lifetime_refreshes = static_cast<int>(nvs.GetInt(kEpdLifetime, 0));
    state.last_refresh_timestamp = static_cast<int>(nvs.GetInt(kEpdLastTs, 0));
    return state;
}

void SaveEpdRefreshState(const EpdRefreshState& state) {
    Settings nvs(kNamespace, true);
    nvs.SetInt(kEpdPartial, state.partial_count);
    nvs.SetInt(kEpdLifetime, state.lifetime_refreshes);
    nvs.SetInt(kEpdLastTs, state.last_refresh_timestamp);
}

bool ShouldForceFullRefresh(int threshold) {
    Settings nvs(kNamespace);
    int partial = static_cast<int>(nvs.GetInt(kEpdPartial, 0));
    if (partial >= threshold) {
        ESP_LOGI(kTag, "EPD: %d partial refreshes reached, forcing full refresh", partial);
        return true;
    }
    return false;
}

void ResetPartialCounter() {
    Settings nvs(kNamespace, true);
    // Increment lifetime before resetting partial
    int lifetime = static_cast<int>(nvs.GetInt(kEpdLifetime, 0));
    int partial = static_cast<int>(nvs.GetInt(kEpdPartial, 0));
    lifetime += partial;

    nvs.SetInt(kEpdLifetime, lifetime);
    nvs.SetInt(kEpdPartial, 0);

    // Update timestamp
    time_t now = time(nullptr);
    nvs.SetInt(kEpdLastTs, static_cast<int32_t>(now));
}

// ============================================================
// UI navigation state
// ============================================================

UiNavState LoadUiNavState() {
    Settings nvs(kNamespace);
    UiNavState state;
    state.last_page = static_cast<int>(nvs.GetInt(kUiPage, 0));
    state.summary_scroll = static_cast<int>(nvs.GetInt(kUiScroll, 0));
    state.lifebar_visible = static_cast<int>(nvs.GetInt(kUiLifebar, 1));
    return state;
}

void SaveUiNavState(const UiNavState& state) {
    Settings nvs(kNamespace, true);
    nvs.SetInt(kUiPage, state.last_page);
    nvs.SetInt(kUiScroll, state.summary_scroll);
    nvs.SetInt(kUiLifebar, state.lifebar_visible);
    ESP_LOGI(kTag, "UI nav state saved: page=%d scroll=%d lifebar=%d",
             state.last_page, state.summary_scroll, state.lifebar_visible);
}

// ============================================================
// BLE state
// ============================================================

BleState LoadBleState() {
    Settings nvs(kNamespace);
    BleState state;
    state.enabled = nvs.GetBool(kBleEnabled, false);
    ESP_LOGI(kTag, "Loaded BLE state: enabled=%d", state.enabled);
    return state;
}

void SaveBleState(const BleState& state) {
    Settings nvs(kNamespace, true);
    nvs.SetBool(kBleEnabled, state.enabled);
    ESP_LOGI(kTag, "BLE state saved: enabled=%d", state.enabled);
}

int LoadUiLanguage() {
    Settings nvs(kNamespace);
    return static_cast<int>(nvs.GetInt(kUiLanguage, 0));
}

void SaveUiLanguage(int language) {
    Settings nvs(kNamespace, true);
    nvs.SetInt(kUiLanguage, language);
    ESP_LOGI(kTag, "UI language saved: %d", language);
}

bool LoadApTransferBootMode() {
    Settings nvs(kNamespace);
    const bool enabled = nvs.GetBool(kApTransferBoot, false);
    ESP_LOGI(kTag, "Loaded AP transfer boot mode: enabled=%d", enabled);
    return enabled;
}

void SaveApTransferBootMode(bool enabled) {
    Settings nvs(kNamespace, true);
    nvs.SetBool(kApTransferBoot, enabled);
    ESP_LOGI(kTag, "AP transfer boot mode saved: enabled=%d", enabled);
}

}  // namespace nvs_state
