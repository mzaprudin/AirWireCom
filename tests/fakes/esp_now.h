#pragma once
#include "Arduino.h"
struct esp_now_peer_info_t {
    uint8_t peer_addr[6]{};
    uint8_t channel = 0;
    bool encrypt = false;
};
inline std::array<uint8_t, 6> key(const uint8_t* mac) {
    std::array<uint8_t, 6> result{};
    std::copy_n(mac, 6, result.begin());
    return result;
}
inline int esp_now_init() { fake::espnowReady = true; return ESP_OK; }
inline bool esp_now_is_peer_exist(const uint8_t* mac) { return fake::peers.count(key(mac)); }
inline int esp_now_add_peer(const esp_now_peer_info_t* peer) {
    if (!fake::espnowReady) { ++fake::peerAddsBeforeInit; return -1; }
    fake::peers.insert(key(peer->peer_addr));
    return ESP_OK;
}
inline int esp_now_del_peer(const uint8_t* mac) { fake::peers.erase(key(mac)); return ESP_OK; }
inline int esp_now_send(const uint8_t* destination, const uint8_t* data, size_t len) {
    fake::destinations.push_back(key(destination));
    fake::sent.emplace_back(data, data + len);
    return ESP_OK;
}
inline void esp_now_register_recv_cb(void (*)(const uint8_t*, const uint8_t*, int)) {}
