#pragma once
#include <stddef.h>
#include <stdint.h>

// Wire format and receive policy shared by the firmware and host checks.
namespace awc {
constexpr uint8_t PKT_DATA = 0x01;
constexpr uint8_t PKT_PAIR = 0x02;
constexpr uint8_t PKT_CLAIM = 0x03; // selected slave MAC follows the type
constexpr uint8_t PKT_ACK = 0x04;
constexpr uint8_t PKT_UNPAIR = 0x05;
constexpr uint8_t PKT_BAUD = 0x06;
constexpr uint8_t PKT_PING = 0x07;
constexpr uint8_t PKT_PONG = 0x08;
constexpr uint8_t PKT_CLAIM_LEN = 7;
constexpr uint8_t PKT_ACK_LEN = 5;
constexpr uint8_t PKT_BAUD_LEN = 5;
constexpr uint8_t MAC_LEN = 6;
constexpr uint8_t DATA_MAX = 249;
constexpr uint8_t DATA_BUF_SIZE = DATA_MAX + 1;

enum State {
    ST_SEEKING,
    ST_CONFIRM,
    ST_PAIRED_MASTER,
    ST_PAIRED_SLAVE
};

inline bool isPaired(State state) {
    return state == ST_PAIRED_MASTER || state == ST_PAIRED_SLAVE;
}

inline bool acceptsPacket(State state, bool fromPeer, uint8_t type, size_t len) {
    if (len == 0 || len > DATA_BUF_SIZE) return false;
    switch (type) {
    case PKT_PAIR:
        return state == ST_SEEKING;
    case PKT_CLAIM:
        return state == ST_CONFIRM && len >= PKT_CLAIM_LEN;
    case PKT_ACK:
        return (state == ST_SEEKING || state == ST_CONFIRM) && len >= PKT_ACK_LEN;
    case PKT_DATA:
        return isPaired(state) && fromPeer && len > 1;
    case PKT_BAUD:
        return state == ST_PAIRED_SLAVE && fromPeer && len >= PKT_BAUD_LEN;
    case PKT_UNPAIR:
    case PKT_PING:
    case PKT_PONG:
        return isPaired(state) && fromPeer;
    default:
        return false;
    }
}

inline void packU32(uint8_t* buf, uint32_t value) {
    buf[0] = value & 0xff;
    buf[1] = (value >> 8) & 0xff;
    buf[2] = (value >> 16) & 0xff;
    buf[3] = (value >> 24) & 0xff;
}

inline uint32_t unpackU32(const uint8_t* buf) {
    return static_cast<uint32_t>(buf[0]) |
           (static_cast<uint32_t>(buf[1]) << 8) |
           (static_cast<uint32_t>(buf[2]) << 16) |
           (static_cast<uint32_t>(buf[3]) << 24);
}
} // namespace awc
