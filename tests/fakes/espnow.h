#pragma once
#include "esp_now.h"
inline int esp_now_add_peer(uint8_t* mac, int, uint8_t, uint8_t*, uint8_t) {
    esp_now_peer_info_t peer{};
    std::copy_n(mac, 6, peer.peer_addr);
    return esp_now_add_peer(&peer);
}
inline void esp_now_set_self_role(int) {}
inline void esp_now_register_recv_cb(void (*)(uint8_t*, uint8_t*, uint8_t)) {}
