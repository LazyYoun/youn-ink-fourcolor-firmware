#pragma once

#include <cstring>
#include <string>

namespace wifi_credentials {
inline bool Valid(const std::string& ssid, const std::string& password) {
    if (ssid.empty() || ssid.size() > 32 || ssid.find('\0') != std::string::npos ||
        password.find('\0') != std::string::npos) return false;
    if (password.empty()) return true;  // Open network.
    if (password.size() >= 8 && password.size() <= 63) return true;
    if (password.size() != 64) return false;
    return password.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
}

// ESP-IDF accepts full-width SSIDs and hexadecimal PSKs without a terminator.
template <typename StationConfig>
bool Assign(StationConfig& config, const std::string& ssid, const std::string& password) {
    if (!Valid(ssid, password)) return false;
    static_assert(sizeof(config.ssid) == 32 && sizeof(config.password) == 64,
                  "Unexpected ESP-IDF credential field sizes");
    memset(config.ssid, 0, sizeof(config.ssid));
    memset(config.password, 0, sizeof(config.password));
    memcpy(config.ssid, ssid.data(), ssid.size());
    memcpy(config.password, password.data(), password.size());
    return true;
}
}  // namespace wifi_credentials
