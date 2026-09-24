/**
 * @file holiday_fetcher.cc
 * @brief Chinese official holiday schedule fetcher
 *
 * Fetches State Council holiday data from timor.tech API.
 * Response format:
 *   {"code":0, "holiday":{"2026-01-01":{"name":"元旦","rest":1},...}}
 *
 * - rest=1: holiday/rest day (休)
 * - rest=0: compensatory workday (补班)
 *
 * Data is cached in NVS under key "holiday_YYYY" (blob, ~2KB per year).
 */

#include "holiday_fetcher.h"
#include "i18n.h"
#include <esp_log.h>
#include <esp_http_client.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <algorithm>

namespace holiday_fetcher {

static constexpr char kTag[] = "HolidayFetcher";
static constexpr char kNvsNamespace[] = "holiday_cache";
static constexpr int kMaxResponseSize = 8192;

// In-memory cache (populated from NVS at init)
static HolidayCache s_cache = {};
static bool s_loaded = false;

// HTTP response buffer
static char s_resp_buf[kMaxResponseSize + 1] = {0};
static int s_resp_len = 0;

static esp_err_t HttpEvent(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int n = std::min(evt->data_len, kMaxResponseSize - s_resp_len);
        if (n > 0) {
            memcpy(s_resp_buf + s_resp_len, evt->data, n);
            s_resp_len += n;
        }
    }
    return ESP_OK;
}

/**
 * @brief Parse JSON response and populate cache.
 * Expected: {"code":0, "holiday":{"2026-01-01":{"name":"X","rest":1},...}}
 */
static bool ParseResponse(int year, const char* json, int len) {
    // Find "holiday" object
    const char* holiday_start = strstr(json, "\"holiday\"");
    if (!holiday_start) {
        ESP_LOGE(kTag, "No 'holiday' key in response");
        return false;
    }

    // Find the opening brace of holiday object
    const char* obj_start = strchr(holiday_start, '{');
    if (!obj_start) return false;

    s_cache.year = year;
    s_cache.entry_count = 0;

    // Scan for date entries: "YYYY-MM-DD":{...}
    char date_pattern[16];
    const char* pos = obj_start + 1;

    while (s_cache.entry_count < kMaxHolidayEntries && pos < json + len) {
        // Find next date string "YYYY-MM-DD"
        snprintf(date_pattern, sizeof(date_pattern), "\"%04d-", year);
        const char* date_str = strstr(pos, date_pattern);
        if (!date_str || date_str >= json + len - 13) break;

        // Parse MM-DD
        int m = 0, d = 0;
        if (sscanf(date_str + 5, "%d-%d", &m, &d) != 2) {
            pos = date_str + 1;
            continue;
        }
        if (m < 1 || m > 12 || d < 1 || d > 31) {
            pos = date_str + 1;
            continue;
        }

        // Find the value object: {..."name":"...","rest":N...}
        const char* obj = strchr(date_str, '{');
        if (!obj || obj >= json + len) {
            pos = date_str + 1;
            continue;
        }

        // Find "rest" value
        const char* rest_key = strstr(obj, "\"rest\"");
        int is_rest = 1;  // default to rest
        if (rest_key && rest_key < json + len) {
            const char* colon = strchr(rest_key, ':');
            if (colon && colon < json + len) {
                is_rest = atoi(colon + 1);
            }
        }

        // Find "name" value
        const char* name_key = strstr(obj, "\"name\"");
        const char* name = "";
        if (name_key && name_key < json + len) {
            const char* q1 = strchr(name_key, ':');
            if (q1) {
                const char* q2 = strchr(q1, '"');
                if (q2 && q2 < json + len) {
                    q2++;  // skip opening quote
                    const char* q3 = strchr(q2, '"');
                    if (q3 && q3 < json + len && q3 > q2) {
                        static char name_buf[32];
                        int nl = std::min((int)(q3 - q2), 31);
                        memcpy(name_buf, q2, nl);
                        name_buf[nl] = '\0';
                        name = name_buf;
                    }
                }
            }
        }

        // Skip entries that have no name and are normal rest days
        if (name[0] == '\0' && is_rest) {
            pos = obj + 1;
            continue;
        }

        // Store entry
        HolidayEntry& e = s_cache.entries[s_cache.entry_count++];
        e.year = (int16_t)year;
        e.month = (int8_t)m;
        e.day = (int8_t)d;
        e.is_rest = (is_rest != 0);
        snprintf(e.name, sizeof(e.name), "%.15s", name);

        pos = obj + 1;
    }

    ESP_LOGI(kTag, "Parsed %d holiday entries for %d", s_cache.entry_count, year);
    return true;
}

/**
 * @brief Save cache to NVS using raw NVS blob API.
 */
static void SaveCache() {
    char key[32];
    snprintf(key, sizeof(key), "holiday_%d", s_cache.year);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "NVS open failed: %s", esp_err_to_name(err));
        return;
    }

    // Serialize: entry_count (int) + entries array
    int blob_size = sizeof(int) + s_cache.entry_count * sizeof(HolidayEntry);
    if (blob_size > 4000) {  // NVS blob limit
        nvs_close(handle);
        return;
    }

    uint8_t blob[4096];
    memcpy(blob, &s_cache.entry_count, sizeof(int));
    if (s_cache.entry_count > 0) {
        memcpy(blob + sizeof(int), s_cache.entries,
               s_cache.entry_count * sizeof(HolidayEntry));
    }

    err = nvs_set_blob(handle, key, blob, blob_size);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "NVS save failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(kTag, "Saved %d entries for %d to NVS", s_cache.entry_count, s_cache.year);
    }
    nvs_close(handle);
}

/**
 * @brief Load cache for a year from NVS.
 */
static bool LoadCache(int year) {
    char key[32];
    snprintf(key, sizeof(key), "holiday_%d", year);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    size_t blob_size = 0;
    err = nvs_get_blob(handle, key, nullptr, &blob_size);
    if (err != ESP_OK || blob_size < sizeof(int)) {
        nvs_close(handle);
        return false;
    }

    uint8_t blob[4096];
    if (blob_size > sizeof(blob)) {
        nvs_close(handle);
        return false;
    }

    err = nvs_get_blob(handle, key, blob, &blob_size);
    nvs_close(handle);
    if (err != ESP_OK) return false;

    int count = 0;
    memcpy(&count, blob, sizeof(int));
    if (count < 0 || count > kMaxHolidayEntries) return false;

    int expected = (int)sizeof(int) + count * (int)sizeof(HolidayEntry);
    if ((int)blob_size < expected) return false;

    s_cache.year = year;
    s_cache.entry_count = count;
    if (count > 0) {
        memcpy(s_cache.entries, blob + sizeof(int),
               count * sizeof(HolidayEntry));
    }

    ESP_LOGI(kTag, "Loaded %d cached entries for %d", count, year);
    s_loaded = true;
    return true;
}

// ============================================================
// Public API
// ============================================================

bool Init() {
    // Load current year's data from cache
    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    int year = tm_buf.tm_year + 1900;

    s_loaded = LoadCache(year);
    // Also try to load next year
    LoadCache(year + 1);

    return s_loaded;
}

bool Fetch(int year) {
    s_resp_len = 0;
    memset(s_resp_buf, 0, sizeof(s_resp_buf));

    char url[80];
    snprintf(url, sizeof(url), "https://timor.tech/api/holiday/year/%d", year);
    ESP_LOGI(kTag, "Fetching holiday data: %s", url);

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.event_handler = HttpEvent;
    config.timeout_ms = 10000;
    config.disable_auto_redirect = false;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(kTag, "Failed to init HTTP client");
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "HTTP request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    int status = esp_http_client_get_status_code(client);
    (void)esp_http_client_get_content_length(client);  // logged by ESP_LOGI below
    esp_http_client_cleanup(client);

    if (status != 200) {
        ESP_LOGE(kTag, "HTTP status: %d", status);
        return false;
    }

    s_resp_buf[s_resp_len] = '\0';
    ESP_LOGI(kTag, "Received %d bytes", s_resp_len);

    if (!ParseResponse(year, s_resp_buf, s_resp_len)) {
        ESP_LOGE(kTag, "Failed to parse response");
        return false;
    }

    s_loaded = true;
    SaveCache();
    return true;
}

static const HolidayEntry* FindEntry(int year, int month, int day) {
    if (!s_loaded) return nullptr;
    for (int i = 0; i < s_cache.entry_count; i++) {
        const HolidayEntry& e = s_cache.entries[i];
        if (e.year == year && e.month == month && e.day == day) {
            return &e;
        }
    }
    return nullptr;
}

bool IsHoliday(int year, int month, int day) {
    const HolidayEntry* e = FindEntry(year, month, day);
    return e != nullptr && e->is_rest;
}

bool IsMakeupWorkday(int year, int month, int day) {
    const HolidayEntry* e = FindEntry(year, month, day);
    return e != nullptr && !e->is_rest;
}

const char* GetHolidayName(int year, int month, int day) {
    const HolidayEntry* e = FindEntry(year, month, day);
    if (e && e->is_rest && e->name[0] != '\0') {
        return e->name;
    }
    return nullptr;
}

const char* GetMakeupLabel(int year, int month, int day) {
    const HolidayEntry* e = FindEntry(year, month, day);
    if (e && !e->is_rest) {
        // Looked up fresh on every call (not cached) so it reflects the
        // active language rather than freezing at first call.
        return i18n::Tr(i18n::StringId::kWork);
    }
    return nullptr;
}

}  // namespace holiday_fetcher
