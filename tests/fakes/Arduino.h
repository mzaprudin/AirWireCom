#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <set>
#include <vector>

#define IRAM_ATTR
#define INPUT 0
#define OUTPUT 1
#define CHANGE 2
#define WIFI_STA 1
#define ESP_OK 0
#define ESP_NOW_ROLE_COMBO 3
using std::min;

namespace fake {
inline uint32_t nowMs = 0;
inline uint32_t nowUs = 0;
inline bool espnowReady = false;
inline int peerAddsBeforeInit = 0;
inline int pwmRange = 255;
inline int pwmValue = 0;
inline std::vector<std::vector<uint8_t>> sent;
inline std::vector<std::array<uint8_t, 6>> destinations;
inline std::set<std::array<uint8_t, 6>> peers;
inline std::array<uint8_t, 16> eeprom{};
inline bool stored = false;
inline char role = 'S';
inline std::array<uint8_t, 6> peer{};
inline uint32_t baud = 115200;
}
inline uint32_t millis() { return fake::nowMs; }
inline uint32_t micros() { return fake::nowUs; }
inline void delay(uint32_t ms) { fake::nowMs += ms; }
inline void pinMode(uint8_t, int) {}
inline int digitalPinToInterrupt(uint8_t pin) { return pin; }
inline void attachInterrupt(int, void (*)(), int) {}
inline void analogWriteRange(int range) { fake::pwmRange = range; }
inline void analogWrite(uint8_t, int value) { fake::pwmValue = value; }
inline void ledcWrite(uint8_t, uint8_t) {}
inline void ledcSetup(uint8_t, uint32_t, uint8_t) {}
inline void ledcAttachPin(uint8_t, uint8_t) {}

struct FakeSerial {
    uint32_t baud = 0;
    std::vector<uint8_t> received;
    std::vector<uint8_t> written;
    void begin(uint32_t value) { baud = value; }
    void setTimeout(int) {}
    void flush() {}
    int available() { return static_cast<int>(received.size()); }
    int readBytes(uint8_t* out, int count) {
        int n = min(count, available());
        std::copy_n(received.begin(), n, out);
        received.erase(received.begin(), received.begin() + n);
        return n;
    }
    size_t write(const uint8_t* data, size_t len) {
        written.insert(written.end(), data, data + len);
        return len;
    }
};
inline FakeSerial Serial;

struct Button {
    explicit Button(uint8_t) {}
    bool clickEvent = false;
    bool holdEvent = false;
    bool releaseEvent = false;
    void setHoldTimeout(uint16_t) {}
    void tick() {}
    bool click() { bool value = clickEvent; clickEvent = false; return value; }
    bool hold() { bool value = holdEvent; holdEvent = false; return value; }
    bool release() { bool value = releaseEvent; releaseEvent = false; return value; }
};

struct FakeWiFi {
    void mode(int) {}
    void disconnect() {}
    void macAddress(uint8_t* out) { const uint8_t mac[] = {2, 0, 0, 0, 0, 1}; std::copy_n(mac, 6, out); }
};
inline FakeWiFi WiFi;

struct Preferences {
    void begin(const char*, bool) {}
    void end() {}
    void putChar(const char*, char value) { fake::role = value; }
    void putBytes(const char*, const uint8_t* data, size_t n) { std::copy_n(data, n, fake::peer.begin()); fake::stored = true; }
    void putUInt(const char*, uint32_t value) { fake::baud = value; }
    bool isKey(const char*) { return fake::stored; }
    char getChar(const char*, char fallback) { return fake::stored ? fake::role : fallback; }
    void getBytes(const char*, uint8_t* out, size_t n) { std::copy_n(fake::peer.begin(), n, out); }
    uint32_t getUInt(const char*, uint32_t fallback) { return fake::stored ? fake::baud : fallback; }
    void clear() { fake::stored = false; }
};

struct FakeEEPROM {
    void begin(int) {}
    void end() {}
    uint8_t read(size_t offset) { return fake::eeprom.at(offset); }
    void write(size_t offset, uint8_t value) { fake::eeprom.at(offset) = value; }
    void commit() {}
};
inline FakeEEPROM EEPROM;
