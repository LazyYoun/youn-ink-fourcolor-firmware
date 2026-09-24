# Wi-Fi provisioning regression checks

From `firmware/`, run the host tests without ESP-IDF:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I main/components/78__esp-wifi-connect/include \
  tests/wifi_provisioning_test.cc -o /tmp/wifi_provisioning_test
/tmp/wifi_provisioning_test
```

To include JSON round-trip tests after dependencies have been downloaded:

```sh
cc -fsanitize=address,undefined \
  -c managed_components/espressif__cjson/cJSON/cJSON.c -o /tmp/wifi_cjson.o
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -DWIFI_TEST_JSON -I managed_components/espressif__cjson/cJSON \
  -I main/components/78__esp-wifi-connect/include \
  tests/wifi_provisioning_test.cc /tmp/wifi_cjson.o -o /tmp/wifi_provisioning_test
/tmp/wifi_provisioning_test
```

Coverage: full-width 32-byte SSIDs and 64-digit hexadecimal PSKs, UTF-8 names,
open networks, invalid credential lengths, fragmented HTTP bodies, receive
errors, and JSON escaping for quotes, backslashes and long names.

Hardware validation remains required: provision on a 2.4 GHz network, reboot
and reconnect, retry after a wrong password, test a slow DHCP server, and
verify that credentials survive reboot. The device obtains the household
address and gateway via DHCP; the router need not use `192.168.0.1`.
The configuration hotspot uses `192.168.4.1`; same-subnet AP/STA conflicts
and WPA3/router-specific compatibility are not addressed by these tests.
