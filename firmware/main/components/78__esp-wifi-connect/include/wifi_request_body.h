#pragma once
#include <cstddef>

// Return zero only after reading the entire body; propagate receive errors.
template <typename Receive>
int ReadWifiRequestBody(char* buffer, size_t length, Receive receive) {
    size_t received = 0;
    while (received < length) {
        int count = receive(buffer + received, length - received);
        if (count <= 0) return count == 0 ? -1 : count;
        if (static_cast<size_t>(count) > length - received) return -1;
        received += count;
    }
    buffer[length] = '\0';
    return 0;
}
