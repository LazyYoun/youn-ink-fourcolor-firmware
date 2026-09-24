#include "application.h"

#include "boards/zectrix-s3-epaper-4.2/custom_lcd_display.h"
#include "boards/zectrix-s3-epaper-4.2/config.h"
#include "board.h"
#include "common/photo_storage.h"
#include "display.h"
#include "settings.h"
#include "ui/rawdraw_ui_manager.h"
#include "wifi_manager.h"

#include <esp_mac.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include <esp_wifi.h>

#include <ctime>

namespace {

constexpr char kTag[] = "Application";
constexpr char kSyncNamespace[] = "sync";
constexpr char kSyncIntervalKey[] = "sync_interval";
constexpr char kGalleryNamespace[] = "gallery";
constexpr char kSlideshowIntervalKey[] = "slide_min";
constexpr int kSettingsSlideshowIndex = 3;
constexpr int kSettingsWifiIndex = 5;
constexpr int kSettingsHttpServerIndex = 6;
constexpr int kSettingsLanIpIndex = 7;

std::string FormatMinutesLabel(int minutes) {
    if (minutes <= 0) return "关闭";
    char buf[16];
    snprintf(buf, sizeof(buf), "%dmin", minutes);
    return buf;
}

const char* FormatMinutesLogLabel(int minutes) {
    return minutes <= 0 ? "关闭" : "开启";
}

int NextSlideshowInterval(int current) {
    static constexpr int kOptions[] = {0, 5, 10, 30};
    for (size_t i = 0; i < sizeof(kOptions) / sizeof(kOptions[0]); ++i) {
        if (kOptions[i] == current) {
            return kOptions[(i + 1) % (sizeof(kOptions) / sizeof(kOptions[0]))];
        }
    }
    return 5;
}

void UpdateWifiSettingsItem(rawdraw::SettingsRenderer* renderer, bool connected,
                            const char* value = nullptr) {
    if (!renderer) return;
    renderer->UpdateChecked(kSettingsWifiIndex, connected);
    renderer->UpdateItem(kSettingsWifiIndex, value ? value : (connected ? "已连接" : "未连接"));
}

void UpdateHttpServerSettingsItem(rawdraw::SettingsRenderer* renderer, bool running,
                                  const std::string& ip_address = "") {
    if (!renderer) return;
    std::string value;
    if (running && !ip_address.empty()) {
        value = "http://" + ip_address;
    } else if (!ip_address.empty()) {
        value = ip_address;
    } else {
        value = running ? "已开启" : "已关闭";
    }
    renderer->UpdateChecked(kSettingsHttpServerIndex, running);
    renderer->UpdateItem(kSettingsHttpServerIndex, value);
}

void UpdateLanIpSettingsItem(rawdraw::SettingsRenderer* renderer, const std::string& ip_address) {
    if (!renderer) return;
    renderer->UpdateItem(kSettingsLanIpIndex, ip_address.empty() ? "未获取" : ip_address);
}

void StartSntpClockSyncOnce() {
    static bool s_started = false;
    if (s_started) return;

    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "cn.pool.ntp.org");
    esp_sntp_setservername(2, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb([](struct timeval*) {
        time_t now = 0;
        time(&now);
        struct tm local_tm = {};
        localtime_r(&now, &local_tm);
        char time_buf[32] = {};
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &local_tm);
        ESP_LOGI(kTag, "SNTP time synchronized: %s", time_buf);
        Application::GetInstance().UpdateStatusBarForUi();
    });
    esp_sntp_init();
    s_started = true;
    ESP_LOGI(kTag, "SNTP started: tz=Asia/Shanghai servers=ntp.aliyun.com,cn.pool.ntp.org,pool.ntp.org");
}

bool IsLocalHttpServiceRunning(const ui::RawDrawUiManager* manager) {
    return manager != nullptr && manager->IsHttpServerRunning();
}

}  // namespace

Application::Application() = default;

Application::~Application() {
    if (sleep_timer_ != nullptr) {
        esp_timer_stop(sleep_timer_);
        esp_timer_delete(sleep_timer_);
        sleep_timer_ = nullptr;
    }
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    AudioCodec* codec = board.GetAudioCodec();
    if (codec == nullptr) {
        ESP_LOGE(kTag, "Audio codec is null");
        SetDeviceState(kDeviceStateFatalError);
        return;
    }

    audio_service_.Initialize(codec);
    audio_service_.Start();

    Display* display = board.GetDisplay();
    if (display == nullptr) {
        ESP_LOGW(kTag, "No display available, skipping init");
        SetDeviceState(kDeviceStateFatalError);
        return;
    }
    if (photo_storage_init() == 0) {
        ESP_LOGI(kTag, "Photo storage ready (%d photos)", photo_get_count());
    } else {
        ESP_LOGW(kTag, "Photo storage init failed");
    }

    auto* lcd = static_cast<CustomLcdDisplay*>(display);
    rawdraw_ui_manager_ = std::make_unique<ui::RawDrawUiManager>();
    rawdraw_ui_manager_->Init(lcd, [lcd](const rawdraw::Rect&, bool urgent) {
        if (urgent) {
            lcd->RequestUrgentFullRefresh();
        } else {
            lcd->RequestUrgentRefresh();
        }
    });

    if (auto* sr = rawdraw_ui_manager_->GetSettingsRenderer()) {
        Settings gallery_nvs(kGalleryNamespace, false);
        int slideshow_interval = gallery_nvs.GetInt(kSlideshowIntervalKey, 5);
        if (slideshow_interval != 0 && slideshow_interval != 5 &&
            slideshow_interval != 10 && slideshow_interval != 30) {
            slideshow_interval = 5;
        }
        ESP_LOGI(kTag, "Startup gallery fullscreen slideshow: %s, interval=%s",
                 FormatMinutesLogLabel(slideshow_interval),
                 FormatMinutesLabel(slideshow_interval).c_str());
        rawdraw_ui_manager_->SetGallerySlideshowIntervalMinutes(slideshow_interval);

        std::vector<rawdraw::SettingsItemDef> items;
        items.push_back({"系统", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"重启", "执行", nullptr, rawdraw::SettingsItemType::Action, false,
                         []() { esp_restart(); }});
        items.push_back({"相册", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"轮播间隔", FormatMinutesLabel(slideshow_interval), nullptr,
                         rawdraw::SettingsItemType::Action, false,
                         [this, sr]() {
                             Settings nvs(kGalleryNamespace, true);
                             const int current = nvs.GetInt(kSlideshowIntervalKey, 5);
                             const int next = NextSlideshowInterval(current);
                             nvs.SetInt(kSlideshowIntervalKey, next);
                             if (rawdraw_ui_manager_) {
                                 rawdraw_ui_manager_->SetGallerySlideshowIntervalMinutes(next);
                             }
                             if (next > 0 && sleep_timer_ != nullptr) {
                                 esp_timer_stop(sleep_timer_);
                                 ESP_LOGI(kTag, "Sync sleep timer paused while gallery slideshow is enabled");
                             } else if (next <= 0 &&
                                        (wifi_connected_.load(std::memory_order_acquire) ||
                                         WifiManager::GetInstance().IsConnected())) {
                                 ArmSyncSleepTimer();
                             }
                             sr->UpdateItem(kSettingsSlideshowIndex, FormatMinutesLabel(next));
                         }});
        items.push_back({"网络", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"Wi-Fi", "未连接", nullptr, rawdraw::SettingsItemType::Checkbox, false,
                         [this, sr]() {
                             auto& wifi = WifiManager::GetInstance();
                             if (wifi_connected_.load(std::memory_order_acquire) || wifi.IsConnected()) {
                                 ESP_LOGI(kTag, "Wi-Fi setting toggled OFF");
                                 if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsLanHttpServerRunning()) {
                                     rawdraw_ui_manager_->StopLanHttpServer();
                                     UpdateHttpServerSettingsItem(sr, false);
                                 }
                                 wifi.StopStation();
                                 wifi_connected_.store(false, std::memory_order_release);
                                 UpdateWifiSettingsItem(sr, false);
                                 UpdateLanIpSettingsItem(sr, "");
                             } else {
                                 ESP_LOGI(kTag, "Wi-Fi setting toggled ON");
                                 UpdateWifiSettingsItem(sr, false, "连接中");
                                 wifi.StartStation();
                             }
                             UpdateStatusBarForUi();
                         }});
        items.push_back({"局域网服务", "已关闭", nullptr, rawdraw::SettingsItemType::Checkbox, false,
                         [this, sr]() {
                             if (!rawdraw_ui_manager_) return;
                             if (rawdraw_ui_manager_->IsLanHttpServerRunning()) {
                                 ESP_LOGI(kTag, "LAN HTTP server toggled OFF");
                                 rawdraw_ui_manager_->StopLanHttpServer();
                                 UpdateHttpServerSettingsItem(sr, false);
                                 UpdateStatusBarForUi();
                                 if (wifi_connected_.load(std::memory_order_acquire) ||
                                     WifiManager::GetInstance().IsConnected()) {
                                     ArmSyncSleepTimer();
                                 }
                                 return;
                             }

                             auto& wifi = WifiManager::GetInstance();
                             if (!wifi_connected_.load(std::memory_order_acquire) && !wifi.IsConnected()) {
                                 ESP_LOGW(kTag, "LAN HTTP server requires WiFi connection");
                                 UpdateHttpServerSettingsItem(sr, false, "需先连接WiFi");
                                 UpdateStatusBarForUi();
                                 return;
                             }
                             const std::string ip = wifi.GetIpAddress();
                             if (ip.empty()) {
                                 ESP_LOGW(kTag, "LAN HTTP server requires station IP");
                                 UpdateHttpServerSettingsItem(sr, false, "等待IP");
                                 UpdateStatusBarForUi();
                                 return;
                             }
                             const bool started = rawdraw_ui_manager_->StartLanHttpServer(ip);
                             ESP_LOGI(kTag, "LAN HTTP server toggled ON: started=%d url=http://%s/",
                                      started ? 1 : 0, ip.c_str());
                             if (started && sleep_timer_ != nullptr) {
                                 esp_timer_stop(sleep_timer_);
                                 ESP_LOGI(kTag, "Sync sleep timer paused while LAN HTTP server is running");
                             }
                             UpdateHttpServerSettingsItem(sr, started, started ? ip : "");
                             UpdateLanIpSettingsItem(sr, started ? ip : WifiManager::GetInstance().GetIpAddress());
                             UpdateStatusBarForUi();
                         }});
        items.push_back({"局域网IP", "未获取", nullptr, rawdraw::SettingsItemType::Normal, false});
        items.push_back({"省电模式", "手动进入", nullptr,
                         rawdraw::SettingsItemType::Action, false,
                         [this]() {
                             ESP_LOGI(kTag, "Manual sleep requested from settings");
                             EnterManualSleep();
                         }});
        items.push_back({"关于", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"固件", PROJECT_VER, nullptr, rawdraw::SettingsItemType::Normal, false});
        sr->SetItems(items);
        sr->SetFirmwareVersion("v" PROJECT_VER);

        uint8_t mac_bytes[6] = {};
        esp_read_mac(mac_bytes, ESP_MAC_WIFI_STA);
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac_bytes[0], mac_bytes[1], mac_bytes[2],
                 mac_bytes[3], mac_bytes[4], mac_bytes[5]);
        sr->SetDeviceInfo(mac_str, "ESP32-S3");
    }

    ESP_LOGI(kTag, "Rawdraw gallery UI initialized");
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
        ESP_LOGI(kTag, "Wake from deep sleep: flash activity LED and refresh UI");
        board.FlashActivityLed();
        if (rawdraw_ui_manager_) {
            rawdraw_ui_manager_->RequestActivePageRefresh();
        }
    }

    // Set up WiFi status callback to update StatusBar
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        switch (event) {
            case NetworkEvent::Connected:
                ESP_LOGI(kTag, "WiFi connected: %s", data.c_str());
                wifi_connected_.store(true, std::memory_order_release);
                StartSntpClockSyncOnce();
                if (rawdraw_ui_manager_ && !rawdraw_ui_manager_->IsLanHttpServerRunning()) {
                    const std::string ip = data.empty() ? WifiManager::GetInstance().GetIpAddress() : data;
                    if (!ip.empty()) {
                        const bool started = rawdraw_ui_manager_->StartLanHttpServer(ip);
                        ESP_LOGI(kTag, "LAN HTTP server auto-start after WiFi: started=%d url=http://%s/",
                                 started ? 1 : 0, ip.c_str());
                        if (auto* sr = rawdraw_ui_manager_->GetSettingsRenderer()) {
                            UpdateHttpServerSettingsItem(sr, started, started ? ip : "");
                            UpdateLanIpSettingsItem(sr, ip);
                        }
                    }
                }
                // Update the status model before switching away from the
                // provisioning page. SwitchPage() renders immediately, so
                // switching first would draw the gallery with the stale
                // disconnected icon and rely on a second, slow EPD refresh
                // to correct it.
                UpdateStatusBarForUi();
                if (rawdraw_ui_manager_ &&
                    rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::APTransfer &&
                    !rawdraw_ui_manager_->IsApTransferModeRunning()) {
                    ESP_LOGI(kTag, "WiFi connected while config page is visible, returning to gallery");
                    rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Gallery);
                }
                ArmSyncSleepTimer();
                break;
            case NetworkEvent::Disconnected:
                ESP_LOGI(kTag, "WiFi disconnected");
                wifi_connected_.store(false, std::memory_order_release);
                if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsLanHttpServerRunning()) {
                    rawdraw_ui_manager_->StopLanHttpServer();
                }
                UpdateStatusBarForUi();
                break;
            case NetworkEvent::Connecting:
            case NetworkEvent::Scanning:
                wifi_connected_.store(false, std::memory_order_release);
                UpdateStatusBarForUi();
                break;
            case NetworkEvent::WifiConfigModeEnter:
                ESP_LOGI(kTag, "WiFi config mode entered: %s", data.c_str());
                wifi_connected_.store(false, std::memory_order_release);
                if (rawdraw_ui_manager_) {
                    auto& wifi = WifiManager::GetInstance();
                    rawdraw_ui_manager_->ShowWifiConfigPage(wifi.GetApSsid(),
                                                            wifi.GetApPassword(),
                                                            wifi.GetApWebUrl());
                }
                UpdateStatusBarForUi();
                break;
            case NetworkEvent::WifiConfigModeExit:
                {
                    const bool connected = WifiManager::GetInstance().IsConnected();
                    wifi_connected_.store(connected, std::memory_order_release);
                    UpdateStatusBarForUi();

                    // Leaving AP mode only means provisioning has finished;
                    // the station may still be connecting. Keep the config
                    // page visible until NetworkEvent::Connected so the first
                    // gallery frame always has the correct Wi-Fi status.
                    if (connected && rawdraw_ui_manager_ &&
                        rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::APTransfer &&
                        !rawdraw_ui_manager_->IsApTransferModeRunning()) {
                        ESP_LOGI(kTag, "WiFi config AP exited while connected, returning to gallery");
                        rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Gallery);
                    }
                }
                break;
            case NetworkEvent::ModemDetecting:
            case NetworkEvent::ModemErrorNoSim:
            case NetworkEvent::ModemErrorRegDenied:
            case NetworkEvent::ModemErrorInitFailed:
            case NetworkEvent::ModemErrorTimeout:
                wifi_connected_.store(false, std::memory_order_release);
                UpdateStatusBarForUi();
                break;
        }
    });

    // Start network (non-blocking, WiFi connects asynchronously)
    board.RequestNetwork();

    SetDeviceState(kDeviceStateIdle);
}

void Application::OnUpClick() {
    ESP_LOGI(kTag, "UP click");
    Board::GetInstance().FlashActivityLed();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kUpClick});
    }
}

void Application::OnDownClick() {
    ESP_LOGI(kTag, "DOWN click");
    Board::GetInstance().FlashActivityLed();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kDownClick});
    }
}

void Application::OnUpLongPress() {
    ESP_LOGI(kTag, "UP long press");
    NoteButtonActivity();
    if (rawdraw_ui_manager_ &&
        rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::Settings) {
        ESP_LOGI(kTag, "UP long press - leaving settings");
        rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Gallery);
    }
}

void Application::OnDownLongPress() {
    ESP_LOGI(kTag, "DOWN long press");
    NoteButtonActivity();
    if (rawdraw_ui_manager_) {
        ESP_LOGI(kTag, "DOWN long press - entering settings");
        rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Settings);
    }
}

void Application::OnWifiConfigComboLongPress() {
    ESP_LOGI(kTag, "UP+DOWN long press");
    NoteButtonActivity();
    EnterWifiConfigMode();
}

void Application::OnBootClick() {
    ESP_LOGI(kTag, "BOOT click");
    Board::GetInstance().FlashActivityLed();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kBootClick});
    }
}

void Application::OnBootLongPress() {
    ESP_LOGI(kTag, "BOOT long press");
    NoteButtonActivity();
    if (WifiManager::GetInstance().IsConfigMode()) {
        ESP_LOGI(kTag, "BOOT long press - exiting WiFi config AP");
        // Do not render the gallery with the old disconnected state. The
        // Connected event updates the status model and performs the page
        // switch once the station has actually obtained a connection.
        WifiManager::GetInstance().StartStation();
        return;
    }
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kBootLongPress});
    }
}

void Application::NoteButtonActivity() {
    Board::GetInstance().FlashActivityLed();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->RequestActivePageRefresh();
    }
}

void Application::EnterWifiConfigMode() {
    if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsLanHttpServerRunning()) {
        rawdraw_ui_manager_->StopLanHttpServer();
    }
    wifi_connected_.store(false, std::memory_order_release);
    ESP_LOGI(kTag, "Entering WiFi config mode by long press");
    WifiManager::GetInstance().StartConfigAp();
    if (rawdraw_ui_manager_ && WifiManager::GetInstance().IsConfigMode()) {
        auto& wifi = WifiManager::GetInstance();
        rawdraw_ui_manager_->ShowWifiConfigPage(wifi.GetApSsid(),
                                                wifi.GetApPassword(),
                                                wifi.GetApWebUrl());
    }
    UpdateStatusBarForUi();
}

void Application::ArmSyncSleepTimer() {
    if (IsLocalHttpServiceRunning(rawdraw_ui_manager_.get())) {
        if (sleep_timer_ != nullptr) {
            esp_timer_stop(sleep_timer_);
        }
        ESP_LOGI(kTag, "Sync sleep timer skipped while local HTTP transfer service is running");
        return;
    }
    if (rawdraw_ui_manager_ &&
        rawdraw_ui_manager_->GetGallerySlideshowIntervalMinutes() > 0) {
        if (sleep_timer_ != nullptr) {
            esp_timer_stop(sleep_timer_);
        }
        ESP_LOGI(kTag, "Sync sleep timer skipped while gallery slideshow is enabled");
        return;
    }

    Settings nvs(kSyncNamespace, false);
    const int interval_minutes = nvs.GetInt(kSyncIntervalKey, 30);
    if (interval_minutes <= 0) {
        ESP_LOGI(kTag, "Sync sleep interval: 关闭");
        return;
    }
    if (sleep_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            static_cast<Application*>(arg)->EnterScheduledSleep();
        };
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "app_sync_sleep";
        ESP_ERROR_CHECK(esp_timer_create(&args, &sleep_timer_));
    }
    esp_timer_stop(sleep_timer_);
    const int64_t delay_us = static_cast<int64_t>(interval_minutes) * 60 * 1000 * 1000;
    ESP_LOGI(kTag, "Sync sleep interval: %d minutes", interval_minutes);
    ESP_LOGI(kTag, "Scheduling sleep after sync interval: %d minutes", interval_minutes);
    ESP_ERROR_CHECK(esp_timer_start_once(sleep_timer_, delay_us));
}

void Application::EnterScheduledSleep() {
    if (IsLocalHttpServiceRunning(rawdraw_ui_manager_.get())) {
        ESP_LOGI(kTag, "Scheduled sleep skipped: local HTTP transfer service is running");
        ArmSyncSleepTimer();
        return;
    }
    if (rawdraw_ui_manager_ &&
        rawdraw_ui_manager_->GetGallerySlideshowIntervalMinutes() > 0) {
        ESP_LOGI(kTag, "Scheduled sleep skipped: gallery slideshow is enabled");
        ArmSyncSleepTimer();
        return;
    }

    ESP_LOGI(kTag, "Entering deep sleep after sync interval; BOOT wakes device");
    wifi_connected_.store(false, std::memory_order_release);
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(BOOT_BUTTON_GPIO), 0);
    esp_deep_sleep_start();
}

void Application::EnterManualSleep() {
    ESP_LOGI(kTag, "Entering manual deep sleep; stopping local services and WiFi");
    if (sleep_timer_ != nullptr) {
        esp_timer_stop(sleep_timer_);
    }
    if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsHttpServerRunning()) {
        rawdraw_ui_manager_->StopApTransferMode();
    }
    wifi_connected_.store(false, std::memory_order_release);
    esp_wifi_disconnect();
    esp_wifi_stop();
    UpdateStatusBarForUi();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(BOOT_BUTTON_GPIO), 0);
    esp_deep_sleep_start();
}

void Application::Run() {
    while (true) {
        if (rawdraw_ui_manager_) {
            rawdraw_ui_manager_->PumpClockRefresh();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

bool Application::SetDeviceState(DeviceState state) {
    const DeviceState old_state = state_.exchange(state, std::memory_order_acq_rel);
    ESP_LOGI(kTag, "State %d -> %d", old_state, state);
    return true;
}

void Application::Schedule(std::function<void()>&& callback) {
    if (callback) {
        callback();
    }
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::PlaySound(const std::string_view& sound, int duration_ms) {
    audio_service_.PlaySound(sound, duration_ms);
}

void Application::MuteSound() {
    audio_service_.MuteOutput();
}

void Application::StopSound() {
    audio_service_.ResetDecoder();
}

bool Application::CanEnterSleepMode() const {
    return false;
}

void Application::UpdateStatusBarForUi() {
    auto& board = Board::GetInstance();
    int battery_level = -1;
    bool charging = false;
    bool discharging = false;
    board.GetBatteryLevel(battery_level, charging, discharging);

    if (rawdraw_ui_manager_) {
        const bool wifi_connected = wifi_connected_.load(std::memory_order_acquire);
        const bool http_server_running = rawdraw_ui_manager_->IsHttpServerRunning();
        ui::RawDrawStatusBarData data = rawdraw_ui_manager_->GetStatusBarData();
        data.page_title = ui::RawDrawUiManager::GetPageTitle(rawdraw_ui_manager_->GetCurrentPage());
        data.wifi_connected = wifi_connected;
        data.server_connected = http_server_running;
        data.battery_level = battery_level;
        data.battery_charging = charging;
        rawdraw_ui_manager_->UpdateStatusBar(data);
        UpdateWifiSettingsItem(rawdraw_ui_manager_->GetSettingsRenderer(), wifi_connected);
        const std::string lan_ip = wifi_connected ? WifiManager::GetInstance().GetIpAddress() : "";
        UpdateLanIpSettingsItem(rawdraw_ui_manager_->GetSettingsRenderer(), lan_ip);
        UpdateHttpServerSettingsItem(rawdraw_ui_manager_->GetSettingsRenderer(),
                                     rawdraw_ui_manager_->IsLanHttpServerRunning(),
                                     rawdraw_ui_manager_->IsLanHttpServerRunning()
                                         ? lan_ip
                                         : "");
        rawdraw_ui_manager_->RequestActivePageRefresh();
    }
    return;
}
