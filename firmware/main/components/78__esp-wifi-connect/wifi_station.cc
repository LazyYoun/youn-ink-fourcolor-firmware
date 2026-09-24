#include "wifi_station.h"
#include "wifi_credentials.h"
#include <cctype>
#include <cstring>
#include <algorithm>
#include <mutex>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <nvs.h>
#include "nvs_flash.h"
#include <esp_netif.h>
#include <esp_netif_net_stack.h>
#include <esp_system.h>
#include <lwip/dns.h>
#include <lwip/err.h>
#include <lwip/etharp.h>
#include <lwip/inet.h>
#include <lwip/netif.h>
#include <lwip/sockets.h>
#include <lwip/errno.h>
#include <sys/select.h>
#include <fcntl.h>
#include "ssid_manager.h"
#include "wifi_manager.h"

#define TAG "WifiStation"
#define FAST_RC_TAG "FAST_RC"
static bool kFastRcEnable = true;
#define WIFI_EVENT_CONNECTED BIT0
#define WIFI_EVENT_STOPPED BIT1
#define WIFI_EVENT_SCAN_DONE_BIT BIT2
#define MAX_RECONNECT_COUNT 5
static constexpr int kFastConnectTimeoutMs = 2000;
static constexpr int kFastFailThreshold = 3;
static constexpr int64_t kIpFastMaxAgeMs = 60LL * 60 * 1000;
static constexpr int kIpFastTotalBudgetMs = 900;
static constexpr int kIpFastArpBudgetMs = 120;
static constexpr int kIpFastGwBudgetMs = 200;
static constexpr int kIpFastDnsBudgetMs = 200;
static constexpr int kIpFastTcpBudgetMs = 380;
static constexpr int kIpFastTcpBudgetNoDnsMs = 580;
static const char kMqttNs[] = "mqtt";
static const char kMqttEndpointKey[] = "endpoint";
static const char kWebsocketNs[] = "websocket";
static const char kWebsocketUrlKey[] = "url";
static const char kWifiNs[] = "wifi";
static const char kWifiOtaUrlKey[] = "ota_url";

#ifndef CONFIG_OTA_URL
#define CONFIG_OTA_URL ""
#endif

namespace {
struct FastRcCache {
    bool have_wifi = false;
    std::string ssid;
    uint8_t bssid[6] = {0};
    uint8_t channel = 0;
    int64_t last_ms = 0;
    bool have_ip = false;
    esp_netif_ip_info_t ip_info{};
    esp_ip4_addr_t dns{};
    int64_t ip_ms = 0;
};

static FastRcCache g_fast_cache;
static std::mutex g_fast_cache_mutex;
}  // namespace

static const char* ProbeTargetToString(NetworkProbeTarget probe_target) {
    switch (probe_target) {
        case NetworkProbeTarget::HttpOta:
            return "http_ota";
        case NetworkProbeTarget::WebSocket:
            return "websocket";
        case NetworkProbeTarget::Mqtt:
            return "mqtt";
    }
    return "unknown";
}

static bool IsValidBssid(const uint8_t bssid[6]) {
    bool all_zero = true;
    bool all_ff = true;
    for (int i = 0; i < 6; ++i) {
        if (bssid[i] != 0) {
            all_zero = false;
        }
        if (bssid[i] != 0xFF) {
            all_ff = false;
        }
    }
    return !(all_zero || all_ff);
}

static bool IsValidIpInfo(const esp_netif_ip_info_t& info) {
    return info.ip.addr != 0 && info.gw.addr != 0 && info.netmask.addr != 0;
}

static bool IsValidDns(const esp_ip4_addr_t& dns) {
    return dns.addr != 0;
}

static const char* WifiReasonToString(int reason) {
    switch (reason) {
        case WIFI_REASON_UNSPECIFIED:
            return "UNSPECIFIED";
        case WIFI_REASON_AUTH_EXPIRE:
            return "AUTH_EXPIRE";
        case WIFI_REASON_AUTH_LEAVE:
            return "AUTH_LEAVE";
        case WIFI_REASON_ASSOC_LEAVE:
            return "ASSOC_LEAVE";
        case WIFI_REASON_ASSOC_TOOMANY:
            return "ASSOC_TOOMANY";
        case WIFI_REASON_ASSOC_NOT_AUTHED:
            return "ASSOC_NOT_AUTHED";
        case WIFI_REASON_DISASSOC_PWRCAP_BAD:
            return "DISASSOC_PWRCAP_BAD";
        case WIFI_REASON_DISASSOC_SUPCHAN_BAD:
            return "DISASSOC_SUPCHAN_BAD";
        case WIFI_REASON_IE_INVALID:
            return "IE_INVALID";
        case WIFI_REASON_MIC_FAILURE:
            return "MIC_FAILURE";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
            return "4WAY_HANDSHAKE_TIMEOUT";
        case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
            return "GROUP_KEY_UPDATE_TIMEOUT";
        case WIFI_REASON_IE_IN_4WAY_DIFFERS:
            return "IE_IN_4WAY_DIFFERS";
        case WIFI_REASON_GROUP_CIPHER_INVALID:
            return "GROUP_CIPHER_INVALID";
        case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
            return "PAIRWISE_CIPHER_INVALID";
        case WIFI_REASON_AKMP_INVALID:
            return "AKMP_INVALID";
        case WIFI_REASON_UNSUPP_RSN_IE_VERSION:
            return "UNSUPP_RSN_IE_VERSION";
        case WIFI_REASON_INVALID_RSN_IE_CAP:
            return "INVALID_RSN_IE_CAP";
        case WIFI_REASON_802_1X_AUTH_FAILED:
            return "802_1X_AUTH_FAILED";
        case WIFI_REASON_CIPHER_SUITE_REJECTED:
            return "CIPHER_SUITE_REJECTED";
        case WIFI_REASON_BEACON_TIMEOUT:
            return "BEACON_TIMEOUT";
        case WIFI_REASON_NO_AP_FOUND:
            return "NO_AP_FOUND";
        case WIFI_REASON_AUTH_FAIL:
            return "AUTH_FAIL";
        case WIFI_REASON_ASSOC_FAIL:
            return "ASSOC_FAIL";
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "HANDSHAKE_TIMEOUT";
        default:
            return "UNKNOWN";
    }
}

static bool LoadFastLastMs(int64_t* last_ms) {
    if (last_ms == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_fast_cache_mutex);
    if (!g_fast_cache.have_wifi) {
        return false;
    }
    *last_ms = g_fast_cache.last_ms;
    return true;
}

static bool ReadNvsString(nvs_handle_t nvs, const char* key, std::string* out) {
    if (out == nullptr) {
        return false;
    }
    size_t len = 0;
    if (nvs_get_str(nvs, key, nullptr, &len) != ESP_OK || len == 0) {
        return false;
    }
    std::string value;
    value.resize(len);
    if (nvs_get_str(nvs, key, value.data(), &len) != ESP_OK) {
        return false;
    }
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    if (value.empty()) {
        return false;
    }
    *out = value;
    return true;
}

static bool LoadFastConnectCache(std::string* ssid, uint8_t bssid[6], uint8_t* channel) {
    if (ssid == nullptr || bssid == nullptr || channel == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_fast_cache_mutex);
    if (!g_fast_cache.have_wifi || g_fast_cache.ssid.empty() || g_fast_cache.channel == 0) {
        return false;
    }
    *ssid = g_fast_cache.ssid;
    memcpy(bssid, g_fast_cache.bssid, 6);
    *channel = g_fast_cache.channel;
    return true;
}

static bool LoadIpFastCache(esp_netif_ip_info_t* ip_info, esp_ip4_addr_t* dns, int64_t* last_ms) {
    if (ip_info == nullptr || dns == nullptr || last_ms == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_fast_cache_mutex);
    if (!g_fast_cache.have_ip) {
        return false;
    }
    *ip_info = g_fast_cache.ip_info;
    *dns = g_fast_cache.dns;
    *last_ms = g_fast_cache.ip_ms;
    return true;
}

static void ClearIpFastCacheData() {
    std::lock_guard<std::mutex> lock(g_fast_cache_mutex);
    g_fast_cache.have_ip = false;
    g_fast_cache.ip_info = {};
    g_fast_cache.dns = {};
    g_fast_cache.ip_ms = 0;
}

static void SaveIpFastCache(esp_netif_t* netif, const esp_netif_ip_info_t& ip_info) {
    if (netif == nullptr || !IsValidIpInfo(ip_info)) {
        return;
    }
    esp_ip4_addr_t dns_info{};
    esp_netif_dns_info_t dns_cfg{};
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_cfg) == ESP_OK &&
        dns_cfg.ip.type == ESP_IPADDR_TYPE_V4) {
        dns_info.addr = dns_cfg.ip.u_addr.ip4.addr;
    }
    std::lock_guard<std::mutex> lock(g_fast_cache_mutex);
    g_fast_cache.have_ip = true;
    g_fast_cache.ip_info = ip_info;
    g_fast_cache.dns = dns_info;
    g_fast_cache.ip_ms = esp_timer_get_time() / 1000;
}

static bool LoadMqttEndpoint(std::string* endpoint) {
    if (endpoint == nullptr) {
        return false;
    }
    nvs_handle_t nvs;
    if (nvs_open(kMqttNs, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    std::string value;
    bool ok = ReadNvsString(nvs, kMqttEndpointKey, &value);
    nvs_close(nvs);
    if (!ok || value.empty()) {
        return false;
    }
    *endpoint = value;
    return true;
}

static bool LoadWebSocketUrl(std::string* url) {
    if (url == nullptr) {
        return false;
    }
    nvs_handle_t nvs;
    if (nvs_open(kWebsocketNs, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    std::string value;
    bool ok = ReadNvsString(nvs, kWebsocketUrlKey, &value);
    nvs_close(nvs);
    if (!ok || value.empty()) {
        return false;
    }
    *url = value;
    return true;
}

static bool LoadHttpOtaUrl(std::string* url, std::string* source) {
    if (url == nullptr || source == nullptr) {
        return false;
    }
    nvs_handle_t nvs;
    if (nvs_open(kWifiNs, NVS_READONLY, &nvs) == ESP_OK) {
        std::string value;
        bool ok = ReadNvsString(nvs, kWifiOtaUrlKey, &value);
        nvs_close(nvs);
        if (ok && !value.empty()) {
            *url = value;
            *source = "ota_url";
            return true;
        }
    }
    if (CONFIG_OTA_URL[0] == '\0') {
        return false;
    }
    *url = CONFIG_OTA_URL;
    *source = "config_ota_url";
    return true;
}

static bool ParseEndpoint(const std::string& endpoint, std::string* host, int* port) {
    if (host == nullptr || port == nullptr || endpoint.empty()) {
        return false;
    }
    std::string h = endpoint;
    int p = 8883;
    auto pos = endpoint.rfind(':');
    if (pos != std::string::npos && pos + 1 < endpoint.size()) {
        bool digits = true;
        for (size_t i = pos + 1; i < endpoint.size(); ++i) {
            if (endpoint[i] < '0' || endpoint[i] > '9') {
                digits = false;
                break;
            }
        }
        if (digits) {
            h = endpoint.substr(0, pos);
            p = std::stoi(endpoint.substr(pos + 1));
        }
    }
    if (h.empty()) {
        return false;
    }
    *host = h;
    *port = p;
    return true;
}

static bool ParseUrlAuthority(const std::string& url,
                              std::string* host,
                              int* port) {
    if (host == nullptr || port == nullptr || url.empty()) {
        return false;
    }

    size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return false;
    }
    std::string scheme = url.substr(0, scheme_end);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    int default_port = 0;
    if (scheme == "https" || scheme == "wss") {
        default_port = 443;
    } else if (scheme == "http" || scheme == "ws") {
        default_port = 80;
    } else {
        return false;
    }

    size_t authority_start = scheme_end + 3;
    size_t authority_end = url.find('/', authority_start);
    std::string authority = url.substr(authority_start, authority_end - authority_start);
    if (authority.empty()) {
        return false;
    }

    std::string resolved_host = authority;
    int resolved_port = default_port;
    size_t colon = authority.rfind(':');
    if (colon != std::string::npos && colon + 1 < authority.size()) {
        bool digits = true;
        for (size_t i = colon + 1; i < authority.size(); ++i) {
            if (authority[i] < '0' || authority[i] > '9') {
                digits = false;
                break;
            }
        }
        if (digits) {
            resolved_host = authority.substr(0, colon);
            resolved_port = std::stoi(authority.substr(colon + 1));
        }
    }
    if (resolved_host.empty()) {
        return false;
    }

    *host = resolved_host;
    *port = resolved_port;
    return true;
}

static bool ResolveProbeEndpoint(NetworkProbeTarget probe_target,
                                 std::string* host,
                                 int* port,
                                 std::string* source) {
    if (host == nullptr || port == nullptr || source == nullptr) {
        return false;
    }

    *host = "";
    *port = 0;
    *source = "missing";

    if (probe_target == NetworkProbeTarget::Mqtt) {
        std::string endpoint;
        if (!LoadMqttEndpoint(&endpoint)) {
            *source = "mqtt.endpoint";
            return false;
        }
        *source = "mqtt.endpoint";
        return ParseEndpoint(endpoint, host, port);
    }

    if (probe_target == NetworkProbeTarget::WebSocket) {
        std::string url;
        if (!LoadWebSocketUrl(&url)) {
            *source = "websocket.url";
            return false;
        }
        *source = "websocket.url";
        return ParseUrlAuthority(url, host, port);
    }

    std::string url;
    std::string http_source;
    if (!LoadHttpOtaUrl(&url, &http_source)) {
        *source = "ota_url";
        return false;
    }
    *source = http_source;
    return ParseUrlAuthority(url, host, port);
}

struct DnsQueryContext {
    EventGroupHandle_t done = nullptr;
    ip_addr_t addr;
    bool ok = false;
};

static constexpr EventBits_t kDnsDoneBit = 1;

static void DnsQueryCallback(const char* name, const ip_addr_t* ipaddr, void* arg) {
    auto* ctx = static_cast<DnsQueryContext*>(arg);
    if (ctx == nullptr || ctx->done == nullptr) {
        return;
    }
    if (ipaddr != nullptr) {
        ctx->addr = *ipaddr;
        ctx->ok = true;
    }
    xEventGroupSetBits(ctx->done, kDnsDoneBit);
}

static bool ResolveHostWithTimeout(const std::string& host, ip_addr_t* out_addr, int timeout_ms) {
    if (out_addr == nullptr || host.empty()) {
        return false;
    }
    ip_addr_t resolved{};
    err_t err = dns_gethostbyname(host.c_str(), &resolved, DnsQueryCallback, nullptr);
    if (err == ERR_OK) {
        *out_addr = resolved;
        return true;
    }
    if (err != ERR_INPROGRESS) {
        return false;
    }

    DnsQueryContext ctx;
    ctx.done = xEventGroupCreate();
    if (ctx.done == nullptr) {
        return false;
    }
    err = dns_gethostbyname(host.c_str(), &resolved, DnsQueryCallback, &ctx);
    if (err == ERR_OK) {
        vEventGroupDelete(ctx.done);
        *out_addr = resolved;
        return true;
    }
    if (err != ERR_INPROGRESS) {
        vEventGroupDelete(ctx.done);
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(ctx.done, kDnsDoneBit, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    vEventGroupDelete(ctx.done);
    if (bits & kDnsDoneBit && ctx.ok) {
        *out_addr = ctx.addr;
        return true;
    }
    return false;
}

static bool TcpConnectWithTimeout(const ip_addr_t& addr, int port, int timeout_ms) {
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        return false;
    }
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    dest.sin_addr.s_addr = ip_2_ip4(&addr)->addr;
    int res = connect(sock, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    if (res != 0 && errno != EINPROGRESS) {
        close(sock);
        return false;
    }
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    res = select(sock + 1, nullptr, &wfds, nullptr, &tv);
    if (res <= 0) {
        close(sock);
        return false;
    }
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0 || so_error != 0) {
        close(sock);
        return false;
    }
    close(sock);
    return true;
}

static bool ArpProbeConflict(struct netif* netif, const ip4_addr_t& target, int budget_ms, bool* conflict) {
    if (conflict != nullptr) {
        *conflict = false;
    }
    if (netif == nullptr) {
        return false;
    }
    int per_try_ms = budget_ms / 3;
    for (int i = 0; i < 3; ++i) {
        etharp_request(netif, &target);
        vTaskDelay(pdMS_TO_TICKS(per_try_ms));
        struct eth_addr* eth_ret = nullptr;
        const ip4_addr_t* ip_ret = nullptr;
        if (etharp_find_addr(netif, &target, &eth_ret, &ip_ret) >= 0 && eth_ret != nullptr) {
            if (memcmp(eth_ret->addr, netif->hwaddr, ETH_HWADDR_LEN) != 0) {
                if (conflict != nullptr) {
                    *conflict = true;
                }
                return true;
            }
        }
    }
    return true;
}

static bool ArpCheckReachable(struct netif* netif, const ip4_addr_t& target, int budget_ms) {
    if (netif == nullptr) {
        return false;
    }
    int per_try_ms = budget_ms / 3;
    for (int i = 0; i < 3; ++i) {
        etharp_request(netif, &target);
        vTaskDelay(pdMS_TO_TICKS(per_try_ms));
        struct eth_addr* eth_ret = nullptr;
        const ip4_addr_t* ip_ret = nullptr;
        if (etharp_find_addr(netif, &target, &eth_ret, &ip_ret) >= 0 && eth_ret != nullptr) {
            return true;
        }
    }
    return false;
}

static bool SaveFastConnectCache(const std::string& ssid, const uint8_t bssid[6], uint8_t channel) {
    std::lock_guard<std::mutex> lock(g_fast_cache_mutex);
    bool changed = false;
    if (!g_fast_cache.have_wifi || g_fast_cache.ssid != ssid) {
        changed = true;
    } else if (memcmp(g_fast_cache.bssid, bssid, 6) != 0) {
        changed = true;
    } else if (g_fast_cache.channel != channel) {
        changed = true;
    }

    g_fast_cache.have_wifi = true;
    g_fast_cache.ssid = ssid;
    memcpy(g_fast_cache.bssid, bssid, 6);
    g_fast_cache.channel = channel;
    g_fast_cache.last_ms = esp_timer_get_time() / 1000;
    return changed;
}

WifiStation::WifiStation() {
    // Create the event group
    event_group_ = xEventGroupCreate();

    // 读取配置
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %d", err);
        max_tx_power_ = 0;
        remember_bssid_ = 0;
    } else {
        err = nvs_get_i8(nvs, "max_tx_power", &max_tx_power_);
        if (err != ESP_OK) {
            max_tx_power_ = 0;
        }
        err = nvs_get_u8(nvs, "remember_bssid", &remember_bssid_);
        if (err != ESP_OK) {
            remember_bssid_ = 0;
        }
        nvs_close(nvs);
    }

    esp_timer_create_args_t fast_timer_args = {
        .callback = &WifiStation::FastConnectTimeout,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "fast_connect_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&fast_timer_args, &fast_timer_handle_);
}

WifiStation::~WifiStation() {
    Stop();
    if (fast_timer_handle_ != nullptr) {
        esp_timer_stop(fast_timer_handle_);
        esp_timer_delete(fast_timer_handle_);
        fast_timer_handle_ = nullptr;
    }
    if (event_group_) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

void WifiStation::AddAuth(const std::string &&ssid, const std::string &&password) {
    auto& ssid_manager = SsidManager::GetInstance();
    ssid_manager.AddSsid(ssid, password);
}

void WifiStation::Stop() {
    ESP_LOGI(TAG, "Stopping WiFi station");
    
    // Unregister event handlers FIRST to prevent scan done from triggering connect
    if (instance_any_id_ != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_);
        instance_any_id_ = nullptr;
    }
    if (instance_got_ip_ != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_);
        instance_got_ip_ = nullptr;
    }

    // Stop timer
    if (timer_handle_ != nullptr) {
        esp_timer_stop(timer_handle_);
        esp_timer_delete(timer_handle_);
        timer_handle_ = nullptr;
    }
    if (fast_timer_handle_ != nullptr) {
        esp_timer_stop(fast_timer_handle_);
    }

    // Now safe to stop scan, disconnect and stop WiFi (no event callbacks will fire)
    esp_wifi_scan_stop();
    esp_wifi_disconnect();
    esp_wifi_stop();

    if (station_netif_ != nullptr) {
        esp_netif_destroy_default_wifi(station_netif_);
        station_netif_ = nullptr;
    }
    
    // Reset was_connected_ flag to prevent stale state from affecting subsequent sessions
    was_connected_ = false;
    fast_attempt_ = false;
    force_scan_ = false;
    ip_fast_attempt_ = false;
    ip_fast_ready_ = false;

    // Clear connected bit
    xEventGroupClearBits(event_group_, WIFI_EVENT_CONNECTED);
    
    // Set stopped event AFTER cleanup is complete to unblock WaitForConnected
    // This ensures no race condition with subsequent WiFi operations
    xEventGroupSetBits(event_group_, WIFI_EVENT_STOPPED);
}

void WifiStation::SuspendForExternalAp() {
    ESP_LOGI(TAG, "Suspending WiFi station events for external AP");

    if (timer_handle_ != nullptr) {
        esp_timer_stop(timer_handle_);
    }
    if (fast_timer_handle_ != nullptr) {
        esp_timer_stop(fast_timer_handle_);
    }

    esp_wifi_scan_stop();

    if (instance_any_id_ != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_);
        instance_any_id_ = nullptr;
    }
    if (instance_got_ip_ != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_);
        instance_got_ip_ = nullptr;
    }
}

void WifiStation::ResumeAfterExternalAp() {
    ESP_LOGI(TAG, "Resuming WiFi station events after external AP");

    if (instance_any_id_ == nullptr) {
        esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            &WifiStation::WifiEventHandler,
                                                            this,
                                                            &instance_any_id_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register WiFi event handler: %s", esp_err_to_name(err));
        }
    }
    if (instance_got_ip_ == nullptr) {
        esp_err_t err = esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            &WifiStation::IpEventHandler,
                                                            this,
                                                            &instance_got_ip_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(err));
        }
    }

    reconnect_count_ = 0;
    force_scan_ = false;
    fast_attempt_ = false;
    ip_fast_attempt_ = false;
    ip_fast_ready_ = false;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "esp_wifi_connect after external AP failed: %s", esp_err_to_name(err));
    }
}

void WifiStation::OnScanBegin(std::function<void()> on_scan_begin) {
    on_scan_begin_ = on_scan_begin;
}

void WifiStation::OnConnect(std::function<void(const std::string& ssid)> on_connect) {
    on_connect_ = on_connect;
}

void WifiStation::OnConnected(std::function<void(const std::string& ssid)> on_connected) {
    on_connected_ = on_connected;
}

void WifiStation::OnDisconnected(std::function<void()> on_disconnected) {
    on_disconnected_ = on_disconnected;
}

void WifiStation::Start() {
    // Note: esp_netif_init() and esp_wifi_init() should be called once before calling this method
    // WiFi driver is initialized by WifiManager::Initialize() and kept alive
    
    // Clear stopped event bit so WaitForConnected works properly
    // Clear scan done bit so Stop() can wait for scan to complete
    xEventGroupClearBits(event_group_, WIFI_EVENT_STOPPED | WIFI_EVENT_SCAN_DONE_BIT);
    
    // Create the default WiFi station interface
    station_netif_ = esp_netif_create_default_wifi_sta();
    int64_t t_ms = esp_timer_get_time() / 1000;
    ESP_LOGI(FAST_RC_TAG, "stage=wifi event=station_start path=slow t_ms=%lld fast_enable=%d",
             static_cast<long long>(t_ms), kFastRcEnable ? 1 : 0);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &WifiStation::WifiEventHandler,
                                                        this,
                                                        &instance_any_id_));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &WifiStation::IpEventHandler,
                                                        this,
                                                        &instance_got_ip_));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (max_tx_power_ != 0) {
        ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(max_tx_power_));
    }

    // Setup the timer to scan WiFi
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            esp_wifi_scan_start(nullptr, false);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "WiFiScanTimer",
        .skip_unhandled_events = true
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
}

bool WifiStation::WaitForConnected(int timeout_ms) {
    // Wait for either connected or stopped event
    auto bits = xEventGroupWaitBits(event_group_, WIFI_EVENT_CONNECTED | WIFI_EVENT_STOPPED, 
                                    pdFALSE, pdFALSE, timeout_ms / portTICK_PERIOD_MS);
    // Return true only if connected (not if stopped)
    return (bits & WIFI_EVENT_CONNECTED) != 0;
}

void WifiStation::HandleScanResult() {
    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    wifi_ap_record_t *ap_records = (wifi_ap_record_t *)malloc(ap_num * sizeof(wifi_ap_record_t));
    esp_wifi_scan_get_ap_records(&ap_num, ap_records);
    // sort by rssi descending
    std::sort(ap_records, ap_records + ap_num, [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
        return a.rssi > b.rssi;
    });

    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    for (int i = 0; i < ap_num; i++) {
        auto ap_record = ap_records[i];
        auto it = std::find_if(ssid_list.begin(), ssid_list.end(), [ap_record](const SsidItem& item) {
            return strcmp((char *)ap_record.ssid, item.ssid.c_str()) == 0;
        });
        if (it != ssid_list.end() && wifi_credentials::Valid(it->ssid, it->password)) {
            ESP_LOGI(TAG, "Found AP: %s, BSSID: %02x:%02x:%02x:%02x:%02x:%02x, RSSI: %d, Channel: %d, Authmode: %d",
                (char *)ap_record.ssid, 
                ap_record.bssid[0], ap_record.bssid[1], ap_record.bssid[2],
                ap_record.bssid[3], ap_record.bssid[4], ap_record.bssid[5],
                ap_record.rssi, ap_record.primary, ap_record.authmode);
            WifiApRecord record = {
                .ssid = it->ssid,
                .password = it->password,
                .channel = ap_record.primary,
                .authmode = ap_record.authmode,
                .bssid = {0}
            };
            memcpy(record.bssid, ap_record.bssid, 6);
            connect_queue_.push_back(record);
        }
    }
    free(ap_records);

    if (connect_queue_.empty()) {
        ESP_LOGI(TAG, "No AP found, next scan in %d seconds", scan_current_interval_microseconds_ / 1000 / 1000);
        esp_timer_start_once(timer_handle_, scan_current_interval_microseconds_);
        UpdateScanInterval();
        return;
    }

    StartConnect();
}

void WifiStation::StartConnect() {
    auto ap_record = connect_queue_.front();
    connect_queue_.erase(connect_queue_.begin());
    ssid_ = ap_record.ssid;
    password_ = ap_record.password;

    if (on_connect_) {
        on_connect_(ssid_);
    }

    wifi_config_t wifi_config;
    bzero(&wifi_config, sizeof(wifi_config));
    if (!wifi_credentials::Assign(wifi_config.sta, ap_record.ssid, ap_record.password)) return;
    if (remember_bssid_) {
        wifi_config.sta.channel = ap_record.channel;
        memcpy(wifi_config.sta.bssid, ap_record.bssid, 6);
        wifi_config.sta.bssid_set = true;
    }
    wifi_config.sta.listen_interval = 10;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    reconnect_count_ = 0;
    ESP_ERROR_CHECK(esp_wifi_connect());
}

int8_t WifiStation::GetRssi() {
    // Check if connected first
    if (!IsConnected()) {
        return 0;  // Return 0 if not connected
    }
    
    // Get station info
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get AP info: %s", esp_err_to_name(err));
        return 0;
    }
    return ap_info.rssi;
}

uint8_t WifiStation::GetChannel() {
    // Check if connected first
    if (!IsConnected()) {
        return 0;  // Return 0 if not connected
    }
    
    // Get station info
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get AP info: %s", esp_err_to_name(err));
        return 0;
    }
    return ap_info.primary;
}

bool WifiStation::IsConnected() {
    return xEventGroupGetBits(event_group_) & WIFI_EVENT_CONNECTED;
}

void WifiStation::SetScanIntervalRange(int min_interval_seconds, int max_interval_seconds) {
    scan_min_interval_microseconds_ = min_interval_seconds * 1000 * 1000;
    scan_max_interval_microseconds_ = max_interval_seconds * 1000 * 1000;
    scan_current_interval_microseconds_ = scan_min_interval_microseconds_;
}

void WifiStation::SetPowerSaveLevel(WifiPowerSaveLevel level) {
    wifi_ps_type_t ps_type;
    switch (level) {
        case WifiPowerSaveLevel::LOW_POWER:
            ps_type = WIFI_PS_MAX_MODEM;  // Maximum power saving
            ESP_LOGI(TAG, "Setting WiFi power save level: LOW_POWER (MAX_MODEM)");
            break;
        case WifiPowerSaveLevel::BALANCED:
            ps_type = WIFI_PS_MIN_MODEM;  // Minimum power saving
            ESP_LOGI(TAG, "Setting WiFi power save level: BALANCED (MIN_MODEM)");
            break;
        case WifiPowerSaveLevel::PERFORMANCE:
        default:
            ps_type = WIFI_PS_NONE;       // No power saving
            ESP_LOGI(TAG, "Setting WiFi power save level: PERFORMANCE (NONE)");
            break;
    }
    ESP_ERROR_CHECK(esp_wifi_set_ps(ps_type));
}

void WifiStation::ClearFastReconnectCache(const char* reason) {
    {
        std::lock_guard<std::mutex> lock(g_fast_cache_mutex);
        g_fast_cache.have_wifi = false;
        g_fast_cache.ssid.clear();
        memset(g_fast_cache.bssid, 0, sizeof(g_fast_cache.bssid));
        g_fast_cache.channel = 0;
        g_fast_cache.last_ms = 0;
    }
    fast_fail_count_ = 0;
    ESP_LOGI(FAST_RC_TAG,
             "stage=wifi event=cache_clear scope=wifi path=ram t_ms=%lld reason=%s",
             static_cast<long long>(esp_timer_get_time() / 1000),
             reason ? reason : "unknown");
}

void WifiStation::ClearIpFastCache(const char* reason) {
    ClearIpFastCacheData();
    ip_fast_cached_ms_ = 0;
    ip_fast_attempt_ = false;
    ip_fast_ready_ = false;
    ESP_LOGI(FAST_RC_TAG,
             "stage=ip event=cache_clear scope=ip path=ram t_ms=%lld reason=%s",
             static_cast<long long>(esp_timer_get_time() / 1000),
             reason ? reason : "unknown");
}

void WifiStation::SetFastProbeTarget(NetworkProbeTarget probe_target) {
    probe_target_ = probe_target;
}

void WifiStation::UpdateScanInterval() {
    // Apply exponential backoff: double the interval, up to max
    if (scan_current_interval_microseconds_ < scan_max_interval_microseconds_) {
        scan_current_interval_microseconds_ *= 2;
        if (scan_current_interval_microseconds_ > scan_max_interval_microseconds_) {
            scan_current_interval_microseconds_ = scan_max_interval_microseconds_;
        }
    }
}

// Static event handler functions
void WifiStation::WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    if (event_id == WIFI_EVENT_STA_START) {
        int64_t t_ms = esp_timer_get_time() / 1000;
        ESP_LOGI(FAST_RC_TAG, "stage=wifi event=sta_start path=slow t_ms=%lld",
                 static_cast<long long>(t_ms));
        std::string fast_ssid;
        uint8_t fast_bssid[6] = {0};
        uint8_t fast_channel = 0;
        int64_t fast_last_ms = 0;
        bool have_fast_cache = LoadFastConnectCache(&fast_ssid, fast_bssid, &fast_channel);
        bool have_fast_last = LoadFastLastMs(&fast_last_ms);
        int64_t fast_age_ms = have_fast_last ? (t_ms - fast_last_ms) : -1;
        ESP_LOGI(FAST_RC_TAG,
                 "stage=wifi event=fast_cache path=fast t_ms=%lld have=%d ssid=%s channel=%u "
                 "bssid=%02x:%02x:%02x:%02x:%02x:%02x last_age_ms=%lld fail_count=%d",
                 static_cast<long long>(t_ms),
                 have_fast_cache ? 1 : 0,
                 have_fast_cache ? fast_ssid.c_str() : "-",
                 fast_channel,
                 fast_bssid[0], fast_bssid[1], fast_bssid[2],
                 fast_bssid[3], fast_bssid[4], fast_bssid[5],
                 static_cast<long long>(fast_age_ms),
                 this_->fast_fail_count_);
        if (kFastRcEnable && this_->fast_fail_count_ < kFastFailThreshold) {
            if (have_fast_cache && IsValidBssid(fast_bssid)) {
                const auto& ssid_list = SsidManager::GetInstance().GetSsidList();
                auto it = std::find_if(ssid_list.begin(), ssid_list.end(),
                                       [&fast_ssid](const SsidItem& item) {
                                           return item.ssid == fast_ssid;
                                       });
                if (it != ssid_list.end() && wifi_credentials::Valid(it->ssid, it->password)) {
                    this_->ssid_ = it->ssid;
                    this_->password_ = it->password;
                    if (this_->on_connect_) {
                        this_->on_connect_(this_->ssid_);
                    }

                    wifi_config_t wifi_config;
                    bzero(&wifi_config, sizeof(wifi_config));
                    wifi_credentials::Assign(wifi_config.sta, it->ssid, it->password);
                    wifi_config.sta.channel = fast_channel;
                    memcpy(wifi_config.sta.bssid, fast_bssid, 6);
                    wifi_config.sta.bssid_set = true;
                    wifi_config.sta.listen_interval = 10;
                    ESP_LOGI(FAST_RC_TAG,
                             "stage=wifi event=fast_direct_connect path=fast t_ms=%lld ssid=%s channel=%u",
                             static_cast<long long>(t_ms), it->ssid.c_str(), fast_channel);
                    this_->fast_attempt_ = true;
                    if (this_->fast_timer_handle_ != nullptr) {
                        esp_timer_stop(this_->fast_timer_handle_);
                        esp_timer_start_once(this_->fast_timer_handle_, kFastConnectTimeoutMs * 1000ULL);
                    }
                    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
                    this_->reconnect_count_ = 0;
                    ESP_ERROR_CHECK(esp_wifi_connect());
                    return;
                }
                ESP_LOGI(FAST_RC_TAG, "stage=wifi event=fast_direct_connect_skip path=fast t_ms=%lld reason=ssid_not_found",
                         static_cast<long long>(t_ms));
            } else {
                ESP_LOGI(FAST_RC_TAG, "stage=wifi event=fast_direct_connect_skip path=fast t_ms=%lld reason=cache_miss",
                         static_cast<long long>(t_ms));
            }
        } else if (kFastRcEnable) {
            ESP_LOGI(FAST_RC_TAG, "stage=wifi event=fast_direct_connect_skip path=fast t_ms=%lld reason=fail_threshold fail_count=%d",
                     static_cast<long long>(t_ms), this_->fast_fail_count_);
        }
        esp_wifi_scan_start(nullptr, false);
        if (this_->on_scan_begin_) {
            this_->on_scan_begin_();
        }
    } else if (event_id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(this_->event_group_, WIFI_EVENT_SCAN_DONE_BIT);
        this_->HandleScanResult();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        int64_t t_ms = esp_timer_get_time() / 1000;
        auto* disc = static_cast<wifi_event_sta_disconnected_t*>(event_data);
        int reason = disc ? static_cast<int>(disc->reason) : -1;
        ESP_LOGI(FAST_RC_TAG,
                 "stage=wifi event=sta_disconnected path=slow t_ms=%lld reason=%d(%s) "
                 "fast_attempt=%d fast_fail=%d was_connected=%d ip_fast_attempt=%d ip_fast_ready=%d ssid=%s",
                 static_cast<long long>(t_ms),
                 reason, WifiReasonToString(reason),
                 this_->fast_attempt_ ? 1 : 0,
                 this_->fast_fail_count_,
                 this_->was_connected_ ? 1 : 0,
                 this_->ip_fast_attempt_ ? 1 : 0,
                 this_->ip_fast_ready_ ? 1 : 0,
                 this_->ssid_.c_str());
        this_->ip_fast_ready_ = false;
        this_->ip_fast_attempt_ = false;
        xEventGroupClearBits(this_->event_group_, WIFI_EVENT_CONNECTED);
        if (this_->fast_attempt_) {
            this_->HandleFastFallback("sta_disconnected", reason);
            return;
        }
        if (this_->force_scan_) {
            this_->force_scan_ = false;
            esp_wifi_scan_start(nullptr, false);
            if (this_->on_scan_begin_) {
                this_->on_scan_begin_();
            }
            return;
        }
        
        // Notify disconnected callback only once when transitioning from connected to disconnected
        bool was_connected = this_->was_connected_;
        this_->was_connected_ = false;
        if (was_connected && this_->on_disconnected_) {
            ESP_LOGI(TAG, "WiFi disconnected, notifying callback");
            this_->on_disconnected_();
        }
        
        if (this_->reconnect_count_ < MAX_RECONNECT_COUNT) {
            esp_wifi_connect();
            this_->reconnect_count_++;
            ESP_LOGI(TAG, "Reconnecting %s (attempt %d / %d)", this_->ssid_.c_str(), this_->reconnect_count_, MAX_RECONNECT_COUNT);
            return;
        }

        if (!this_->connect_queue_.empty()) {
            this_->StartConnect();
            return;
        }
        
        ESP_LOGI(TAG, "No more AP to connect, next scan in %d seconds", 
                 this_->scan_current_interval_microseconds_ / 1000 / 1000);
        esp_timer_start_once(this_->timer_handle_, this_->scan_current_interval_microseconds_);
        this_->UpdateScanInterval();
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
        int64_t t_ms = esp_timer_get_time() / 1000;
        ESP_LOGI(FAST_RC_TAG, "stage=wifi event=sta_connected path=slow t_ms=%lld",
                 static_cast<long long>(t_ms));
        if (kFastRcEnable && !this_->ip_fast_attempt_ && this_->ip_fast_task_handle_ == nullptr) {
            esp_netif_ip_info_t ip_info{};
            esp_ip4_addr_t dns_info{};
            int64_t ip_ms = 0;
            if (LoadIpFastCache(&ip_info, &dns_info, &ip_ms)) {
                int64_t now_ms = esp_timer_get_time() / 1000;
                char ip_buf[16];
                char gw_buf[16];
                char mask_buf[16];
                char dns_buf[16];
                esp_ip4addr_ntoa(&ip_info.ip, ip_buf, sizeof(ip_buf));
                esp_ip4addr_ntoa(&ip_info.gw, gw_buf, sizeof(gw_buf));
                esp_ip4addr_ntoa(&ip_info.netmask, mask_buf, sizeof(mask_buf));
                esp_ip4addr_ntoa(&dns_info, dns_buf, sizeof(dns_buf));
                ESP_LOGI(FAST_RC_TAG,
                         "stage=ip event=fast_cache path=fast t_ms=%lld cached_age_ms=%lld ip=%s gw=%s mask=%s dns=%s",
                         static_cast<long long>(now_ms),
                         static_cast<long long>(now_ms - ip_ms),
                         ip_buf, gw_buf, mask_buf, dns_buf);
                if (IsValidIpInfo(ip_info) && (now_ms - ip_ms) <= kIpFastMaxAgeMs) {
                    this_->ip_fast_info_ = ip_info;
                    this_->ip_fast_dns_ = dns_info;
                    this_->ip_fast_cached_ms_ = ip_ms;
                    this_->ip_fast_attempt_ = true;
                    this_->ip_fast_ready_ = false;
                    if (this_->station_netif_ != nullptr) {
                        esp_netif_dhcpc_stop(this_->station_netif_);
                        esp_netif_set_ip_info(this_->station_netif_, &this_->ip_fast_info_);
                        if (IsValidDns(this_->ip_fast_dns_)) {
                            esp_netif_dns_info_t dns_cfg{};
                            dns_cfg.ip.type = ESP_IPADDR_TYPE_V4;
                            dns_cfg.ip.u_addr.ip4 = this_->ip_fast_dns_;
                            esp_netif_set_dns_info(this_->station_netif_, ESP_NETIF_DNS_MAIN, &dns_cfg);
                        }
                    }
                    this_->StartIpFastTask();
                } else {
                    ESP_LOGI(FAST_RC_TAG, "stage=ip event=fast_skip path=fast t_ms=%lld reason=stale_or_invalid",
                             static_cast<long long>(t_ms));
                }
            } else {
                ESP_LOGI(FAST_RC_TAG, "stage=ip event=fast_skip path=fast t_ms=%lld reason=cache_miss",
                         static_cast<long long>(t_ms));
            }
        }
    }
}

void WifiStation::IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    if (this_->ip_fast_ready_) {
        int64_t t_ms = esp_timer_get_time() / 1000;
        ESP_LOGI(FAST_RC_TAG, "stage=ip event=got_ip_ignore path=fast t_ms=%lld reason=ip_fast_ready",
                 static_cast<long long>(t_ms));
        return;
    }
    this_->ip_fast_attempt_ = false;
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);

    char ip_address[16];
    esp_ip4addr_ntoa(&event->ip_info.ip, ip_address, sizeof(ip_address));
    this_->ip_address_ = ip_address;
    ESP_LOGI(TAG, "Got IP: %s", this_->ip_address_.c_str());
    int64_t t_ms = esp_timer_get_time() / 1000;
    ESP_LOGI(FAST_RC_TAG, "stage=ip event=got_ip path=slow t_ms=%lld ip=%s",
             static_cast<long long>(t_ms), this_->ip_address_.c_str());
    if (this_->fast_attempt_) {
        this_->fast_attempt_ = false;
        if (this_->fast_timer_handle_ != nullptr) {
            esp_timer_stop(this_->fast_timer_handle_);
        }
        ESP_LOGI(FAST_RC_TAG, "stage=wifi event=fast_success path=fast t_ms=%lld",
                 static_cast<long long>(t_ms));
    }
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        uint8_t channel = ap_info.primary;
        bool wrote = SaveFastConnectCache(this_->ssid_, ap_info.bssid, channel);
        ESP_LOGI(FAST_RC_TAG,
                 "stage=wifi event=cache_writeback path=slow t_ms=%lld wrote=%d ssid=%s channel=%u bssid=%02x:%02x:%02x:%02x:%02x:%02x",
                 static_cast<long long>(t_ms), wrote ? 1 : 0, this_->ssid_.c_str(), channel,
                 ap_info.bssid[0], ap_info.bssid[1], ap_info.bssid[2],
                 ap_info.bssid[3], ap_info.bssid[4], ap_info.bssid[5]);
    } else {
        ESP_LOGW(FAST_RC_TAG, "stage=wifi event=cache_writeback path=slow t_ms=%lld wrote=0 reason=ap_info_fail",
                 static_cast<long long>(t_ms));
    }
    SaveIpFastCache(this_->station_netif_, event->ip_info);
    
    xEventGroupSetBits(this_->event_group_, WIFI_EVENT_CONNECTED);
    this_->was_connected_ = true;  // Mark as connected for disconnect notification
    if (this_->on_connected_) {
        this_->on_connected_(this_->ssid_);
    }
    this_->connect_queue_.clear();
    this_->reconnect_count_ = 0;
    
    // Reset scan interval to minimum for fast reconnect if disconnected later
    this_->scan_current_interval_microseconds_ = this_->scan_min_interval_microseconds_;
}

void WifiStation::HandleFastFallback(const char* reason, int reason_id) {
    if (!fast_attempt_) {
        return;
    }
    fast_attempt_ = false;
    if (fast_timer_handle_ != nullptr) {
        esp_timer_stop(fast_timer_handle_);
    }
    fast_fail_count_++;
    int64_t t_ms = esp_timer_get_time() / 1000;
    ESP_LOGI(FAST_RC_TAG,
             "stage=wifi event=fast_fallback path=fast t_ms=%lld reason=%s reason_id=%d fail_count=%d force_scan=%d",
             static_cast<long long>(t_ms), reason ? reason : "unknown", reason_id, fast_fail_count_,
             force_scan_ ? 1 : 0);
    if (reason_id >= 0) {
        ESP_LOGI(FAST_RC_TAG, "stage=wifi event=fast_fallback_action action=scan");
        esp_wifi_scan_start(nullptr, false);
        if (on_scan_begin_) {
            on_scan_begin_();
        }
    } else {
        WifiManager::GetInstance().ClearFastReconnectCache("fast_timeout");
        force_scan_ = true;
        esp_err_t disconnect_ret = esp_wifi_disconnect();
        esp_err_t stop_ret = esp_wifi_stop();
        esp_err_t start_ret = esp_wifi_start();
        ESP_LOGI(FAST_RC_TAG,
                 "stage=wifi event=fast_fallback_action action=restart_sta disconnect_ret=%s stop_ret=%s start_ret=%s",
                 esp_err_to_name(disconnect_ret), esp_err_to_name(stop_ret), esp_err_to_name(start_ret));
    }
}

void WifiStation::FastConnectTimeout(void* arg) {
    auto* this_ = static_cast<WifiStation*>(arg);
    if (this_->ip_fast_attempt_) {
        this_->IpFastFallback("timeout");
        return;
    }
    this_->HandleFastFallback("timeout", -1);
}

void WifiStation::StopFastConnectTimer() {
    if (fast_timer_handle_ != nullptr) {
        esp_timer_stop(fast_timer_handle_);
    }
}

void WifiStation::StartIpFastTask() {
    if (ip_fast_task_handle_ != nullptr) {
        return;
    }
    xTaskCreate(&WifiStation::IpFastTask, "ip_fast", 4096, this, 2, &ip_fast_task_handle_);
}

void WifiStation::IpFastTask(void* arg) {
    auto* this_ = static_cast<WifiStation*>(arg);
    this_->RunIpFast();
    this_->ip_fast_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

void WifiStation::IpFastFallback(const char* reason) {
    int64_t t_ms = esp_timer_get_time() / 1000;
    int64_t age_ms = (ip_fast_cached_ms_ > 0) ? (t_ms - ip_fast_cached_ms_) : -1;
    ESP_LOGW(FAST_RC_TAG,
             "stage=ip event=fast_fallback scope=ip path=fast t_ms=%lld reason=%s cached_age_ms=%lld ready=%d",
             static_cast<long long>(t_ms), reason ? reason : "unknown",
             static_cast<long long>(age_ms),
             ip_fast_ready_ ? 1 : 0);
    StopFastConnectTimer();
    fast_attempt_ = false;
    ClearIpFastCache(reason);
    if (station_netif_ != nullptr) {
        esp_netif_dhcpc_start(station_netif_);
    }
}

bool WifiStation::RunIpFast() {
    if (!ip_fast_attempt_ || station_netif_ == nullptr) {
        return false;
    }
    int64_t start_ms = esp_timer_get_time() / 1000;
    int64_t deadline_ms = start_ms + kIpFastTotalBudgetMs;
    char ip_buf[16];
    char gw_buf[16];
    char mask_buf[16];
    char dns_buf[16];
    esp_ip4addr_ntoa(&ip_fast_info_.ip, ip_buf, sizeof(ip_buf));
    esp_ip4addr_ntoa(&ip_fast_info_.gw, gw_buf, sizeof(gw_buf));
    esp_ip4addr_ntoa(&ip_fast_info_.netmask, mask_buf, sizeof(mask_buf));
    esp_ip4addr_ntoa(&ip_fast_dns_, dns_buf, sizeof(dns_buf));
    int64_t age_ms = (ip_fast_cached_ms_ > 0) ? (start_ms - ip_fast_cached_ms_) : -1;
    ESP_LOGI(FAST_RC_TAG, "stage=ip event=fast_start path=fast t_ms=%lld", static_cast<long long>(start_ms));
    ESP_LOGI(FAST_RC_TAG,
             "stage=ip event=fast_cache path=fast t_ms=%lld cached_age_ms=%lld ip=%s gw=%s mask=%s dns=%s",
             static_cast<long long>(start_ms),
             static_cast<long long>(age_ms),
             ip_buf, gw_buf, mask_buf, dns_buf);

    if (!IsValidIpInfo(ip_fast_info_)) {
        IpFastFallback("ip_invalid");
        return false;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - ip_fast_cached_ms_ > kIpFastMaxAgeMs) {
        IpFastFallback("ip_stale");
        return false;
    }

    struct netif* lwip_netif = reinterpret_cast<struct netif*>(
        esp_netif_get_netif_impl(station_netif_));
    if (lwip_netif == nullptr) {
        IpFastFallback("netif_null");
        return false;
    }

    ip4_addr_t probe_ip{};
    probe_ip.addr = ip_fast_info_.ip.addr;
    ip4_addr_t gw_ip{};
    gw_ip.addr = ip_fast_info_.gw.addr;
    bool conflict = false;
    if (now_ms + kIpFastArpBudgetMs <= deadline_ms) {
        bool ok = ArpProbeConflict(lwip_netif, probe_ip, kIpFastArpBudgetMs, &conflict);
        int64_t t_ms = esp_timer_get_time() / 1000;
        ESP_LOGI(FAST_RC_TAG, "stage=ip step=arp result=%s t_ms=%lld conflict=%d",
                 ok ? "ok" : "fail", static_cast<long long>(t_ms), conflict ? 1 : 0);
        if (!ok || conflict) {
            IpFastFallback("arp_conflict");
            return false;
        }
    } else {
        IpFastFallback("budget_exhausted_arp");
        return false;
    }

    now_ms = esp_timer_get_time() / 1000;
    if (now_ms + kIpFastGwBudgetMs <= deadline_ms) {
        bool ok = ArpCheckReachable(lwip_netif, gw_ip, kIpFastGwBudgetMs);
        int64_t t_ms = esp_timer_get_time() / 1000;
        ESP_LOGI(FAST_RC_TAG, "stage=ip step=gw result=%s t_ms=%lld",
                 ok ? "ok" : "fail", static_cast<long long>(t_ms));
        if (!ok) {
            IpFastFallback("gw_unreachable");
            return false;
        }
    } else {
        IpFastFallback("budget_exhausted_gw");
        return false;
    }

    std::string host;
    int port = 0;
    std::string probe_source;
    bool have_endpoint = ResolveProbeEndpoint(probe_target_, &host, &port, &probe_source);
    ESP_LOGI(FAST_RC_TAG,
             "stage=ip event=probe_target target=%s source=%s host=%s port=%d t_ms=%lld valid=%d",
             ProbeTargetToString(probe_target_), probe_source.c_str(),
             host.empty() ? "-" : host.c_str(), port,
             static_cast<long long>(esp_timer_get_time() / 1000),
             have_endpoint ? 1 : 0);
    ip_addr_t server_addr{};
    bool resolved = false;
    bool host_is_ip = false;
    if (have_endpoint) {
        ip4_addr_t ip4{};
        if (ip4addr_aton(host.c_str(), &ip4)) {
            server_addr.type = IPADDR_TYPE_V4;
            server_addr.u_addr.ip4 = ip4;
            host_is_ip = true;
            resolved = true;
        }
    }

    now_ms = esp_timer_get_time() / 1000;
    if (!host_is_ip) {
        int dns_budget = kIpFastDnsBudgetMs;
        if (now_ms + dns_budget > deadline_ms) {
            IpFastFallback("budget_exhausted_dns");
            return false;
        }
        if (!have_endpoint || host.empty()) {
            IpFastFallback("endpoint_missing");
            return false;
        }
        resolved = ResolveHostWithTimeout(host, &server_addr, dns_budget);
        int64_t t_ms = esp_timer_get_time() / 1000;
        ESP_LOGI(FAST_RC_TAG, "stage=ip step=dns result=%s t_ms=%lld",
                 resolved ? "ok" : "fail", static_cast<long long>(t_ms));
        if (!resolved) {
            IpFastFallback("dns_fail");
            return false;
        }
    }

    now_ms = esp_timer_get_time() / 1000;
    int tcp_budget = host_is_ip ? kIpFastTcpBudgetNoDnsMs : kIpFastTcpBudgetMs;
    if (now_ms + tcp_budget > deadline_ms) {
        IpFastFallback("budget_exhausted_tcp");
        return false;
    }
    bool tcp_ok = TcpConnectWithTimeout(server_addr, port, tcp_budget);
    int64_t t_ms = esp_timer_get_time() / 1000;
    ESP_LOGI(FAST_RC_TAG, "stage=ip step=tcp result=%s t_ms=%lld",
             tcp_ok ? "ok" : "fail", static_cast<long long>(t_ms));
    if (!tcp_ok) {
        IpFastFallback("tcp_fail");
        return false;
    }
    if (!ip_fast_attempt_) {
        return false;
    }

    if (!IsValidIpInfo(ip_fast_info_)) {
        IpFastFallback("ip_invalid_post");
        return false;
    }
    char ip_address[16];
    esp_ip4addr_ntoa(&ip_fast_info_.ip, ip_address, sizeof(ip_address));
    ip_address_ = ip_address;
    StopFastConnectTimer();
    fast_attempt_ = false;
    xEventGroupSetBits(event_group_, WIFI_EVENT_CONNECTED);
    was_connected_ = true;
    if (on_connected_) {
        on_connected_(ssid_);
    }
    connect_queue_.clear();
    reconnect_count_ = 0;
    scan_current_interval_microseconds_ = scan_min_interval_microseconds_;
    ip_fast_ready_ = true;
    ip_fast_attempt_ = false;
    ESP_LOGI(FAST_RC_TAG, "stage=ip event=fast_success path=fast t_ms=%lld ip=%s",
             static_cast<long long>(esp_timer_get_time() / 1000), ip_address_.c_str());
    return true;
}
