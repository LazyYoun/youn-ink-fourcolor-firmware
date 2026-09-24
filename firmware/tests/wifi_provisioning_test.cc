// Host regression test: c++ -std=c++17 -fsanitize=address,undefined \
//   -I main/components/78__esp-wifi-connect/include tests/wifi_provisioning_test.cc \
//   -o /tmp/wifi_provisioning_test && /tmp/wifi_provisioning_test
#include "wifi_credentials.h"
#include "wifi_request_body.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#ifdef WIFI_TEST_JSON
#include "cJSON.h"
#endif

struct Station {
    uint8_t ssid[32];
    uint8_t password[64];
    uint8_t guard = 0xA5;
};

int main() {
    Station station{};
    const std::string full_ssid(32, 'S'), psk(64, 'a');
    assert(wifi_credentials::Assign(station, full_ssid, psk));
    assert(memcmp(station.ssid, full_ssid.data(), 32) == 0);
    assert(memcmp(station.password, psk.data(), 64) == 0);
    assert(station.guard == 0xA5);
    assert(wifi_credentials::Assign(station, "家里的无线网络", "password"));
    assert(wifi_credentials::Assign(station, "Home\"WiFi\\", ""));
    assert(station.password[0] == 0 && station.password[63] == 0);
    assert(wifi_credentials::Valid("Home", std::string(63, 'x')));
    assert(!wifi_credentials::Valid("", "password"));
    assert(!wifi_credentials::Valid(std::string(33, 'x'), "password"));
    assert(!wifi_credentials::Valid("Home", std::string(64, 'z')));
    assert(!wifi_credentials::Valid("Home", std::string(65, 'a')));
    assert(!wifi_credentials::Valid("Home", "short"));
    assert(!wifi_credentials::Valid(std::string("a\0b", 3), "password"));

    const std::string body = R"({"ssid":"Home","password":"password"})";
    for (size_t chunk = 1; chunk <= body.size(); ++chunk) {
        std::string buffer(body.size() + 1, '?');
        size_t offset = 0;
        int result = ReadWifiRequestBody(buffer.data(), body.size(),
            [&](char* out, size_t remaining) {
                size_t count = std::min(chunk, remaining);
                memcpy(out, body.data() + offset, count);
                offset += count;
                return static_cast<int>(count);
            });
        assert(result == 0 && std::string(buffer.c_str()) == body);
    }
    char output[20]{};
    assert(ReadWifiRequestBody(output, 10, [](char*, size_t) { return 0; }) != 0);
    assert(ReadWifiRequestBody(output, 10, [](char*, size_t) { return -3; }) == -3);
#ifdef WIFI_TEST_JSON
    // Exercise the same cJSON serialization used by scan and saved-list replies.
    for (const std::string& name : {std::string("Home\"WiFi"), std::string("Home\\WiFi"),
                                   std::string("家里的无线网络"), std::string(32, '"')}) {
        cJSON* record = cJSON_CreateObject();
        cJSON_AddStringToObject(record, "ssid", name.c_str());
        cJSON_AddNumberToObject(record, "rssi", -40);
        cJSON_AddNumberToObject(record, "authmode", 3);
        char* text = cJSON_PrintUnformatted(record);
        cJSON* decoded = cJSON_Parse(text);
        assert(decoded && name == cJSON_GetObjectItemCaseSensitive(decoded, "ssid")->valuestring);
        cJSON_Delete(decoded);
        cJSON_free(text);
        cJSON_Delete(record);
        cJSON* list = cJSON_CreateArray();
        cJSON_AddItemToArray(list, cJSON_CreateString(name.c_str()));
        text = cJSON_PrintUnformatted(list);
        decoded = cJSON_Parse(text);
        assert(decoded && name == cJSON_GetArrayItem(decoded, 0)->valuestring);
        cJSON_Delete(decoded);
        cJSON_free(text);
        cJSON_Delete(list);
    }
    std::cout << "Special SSID JSON round trips passed\n";
#endif
    std::cout << "WiFi credential boundaries and fragmented requests passed\n";
}
