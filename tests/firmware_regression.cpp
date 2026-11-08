// Compile the production firmware against deterministic platform adapters.
#include "Arduino.h"
// The ESP8266 SDK header is included inside extern "C" by the firmware.
#ifdef ESP8266
#include "espnow.h"
#endif
#include "../src/main.cpp"
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
const std::array<uint8_t, 6> partner = {2, 1, 2, 3, 4, 5};
const std::array<uint8_t, 6> stranger = {2, 6, 7, 8, 9, 10};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void reset() {
    state = ST_SEEKING;
    std::memset(peerMac, 0, MAC_LEN);
    std::memset(candidateMac, 0, MAC_LEN);
    currentBaud = BAUD_DEFAULT;
    lastContact = 0;
    hasCandidate = false;
    confirmStart = 0;
    lastSeek = 0;
    waitingForRelease = false;
    _minPulseUs = UINT32_MAX;
    _lastEdgeUs = 0;
    _baudDetected = false;
    fake::nowMs = 0;
    fake::nowUs = 0;
    fake::espnowReady = false;
    fake::peerAddsBeforeInit = 0;
    fake::pwmRange = 255;
    fake::sent.clear();
    fake::destinations.clear();
    fake::peers.clear();
    fake::eeprom.fill(0);
    fake::stored = false;
    fake::role = 'S';
    fake::peer.fill(0);
    fake::baud = BAUD_DEFAULT;
    Serial = FakeSerial{};
    btn = Button(PIN_BUTTON);
}

void paired(State role = ST_PAIRED_SLAVE) {
    state = role;
    std::copy(partner.begin(), partner.end(), peerMac);
    Serial.begin(BAUD_DEFAULT);
    fake::espnowReady = true;
}

void receive(std::array<uint8_t, 6> sender, std::vector<uint8_t> data) {
    onReceive(sender.data(), data.data(), static_cast<int>(data.size()));
}

std::vector<uint8_t> baudPacket(uint8_t type, uint32_t baud) {
    std::vector<uint8_t> data(5);
    data[0] = type;
    packU32(data.data() + 1, baud);
    return data;
}

void setHostBaud9600() {
    _minPulseUs = 104;
    _baudDetected = true;
}
}

int main() {
    int failures = 0;
    int count = 0;
    auto test = [&](const char* name, auto body) {
        ++count;
        reset();
        try { body(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) {
            ++failures;
            std::cout << "FAIL " << name << ": " << error.what() << '\n';
        }
    };

    test("baud codec matches the existing little-endian wire format", [] {
        const std::array<uint8_t, 4> wire = {0x00, 0xc2, 0x01, 0x00};
        std::array<uint8_t, 4> encoded{};
        packU32(encoded.data(), 115200);
        require(encoded == wire, "baud encoding changed on the wire");
        require(unpackU32(wire.data()) == 115200, "wire baud decoded incorrectly");
    });
    test("empty packet has no side effects", [] {
        paired(); fake::nowMs = 100;
        receive(partner, {});
        require(Serial.written.empty() && state == ST_PAIRED_SLAVE && lastContact == 0,
                "empty packet changed UART, pair or link status");
    });
    test("unknown packet does not refresh link status", [] {
        paired(); fake::nowMs = 100;
        receive(partner, {0xff, 0x12});
        require(Serial.written.empty() && state == ST_PAIRED_SLAVE && lastContact == 0,
                "unknown packet changed UART, pair or link status");
    });
    test("oversized DATA cannot write UART or refresh link status", [] {
        paired(); fake::nowMs = 100;
        std::vector<uint8_t> data(251, 0x99);
        data[0] = PKT_DATA;
        receive(partner, data);
        require(Serial.written.empty() && lastContact == 0, "oversized DATA accepted");
    });
    test("restore slave UART baud from flash", [] {
        paired(); currentBaud = 57600; saveState();
        state = ST_SEEKING; currentBaud = BAUD_DEFAULT; Serial = FakeSerial{};
        setup();
        require(state == ST_PAIRED_SLAVE, "saved slave role lost");
        require(currentBaud == 57600 && Serial.baud == 57600, "saved baud must configure the physical UART");
    });
    test("restore master UART baud from flash", [] {
        paired(ST_PAIRED_MASTER); currentBaud = 38400; saveState();
        state = ST_SEEKING; currentBaud = BAUD_DEFAULT; Serial = FakeSerial{};
        setup();
        require(state == ST_PAIRED_MASTER && Serial.baud == 38400, "saved master baud must configure UART");
    });
    test("master applies detected baud during pairing", [] {
        state = ST_CONFIRM; hasCandidate = true;
        std::copy(partner.begin(), partner.end(), candidateMac);
        Serial.begin(BAUD_DEFAULT); setHostBaud9600();
        becomeMaster();
        require(currentBaud == 9600 && Serial.baud == 9600, "pairing must configure the master UART too");
        require(fake::sent.back() == baudPacket(PKT_ACK, 9600), "ACK baud encoding changed");
        require(fake::destinations.front() == key(BROADCAST), "CLAIM must be broadcast");
        require(fake::destinations.back() == partner, "ACK must reach the selected slave");
    });
    test("master applies runtime baud change locally and remotely", [] {
        paired(ST_PAIRED_MASTER); setHostBaud9600();
        baudLoop();
        require(Serial.baud == 9600, "runtime baud must configure master UART");
        require(fake::sent.back() == baudPacket(PKT_BAUD, 9600), "runtime baud packet missing");
        require(fake::destinations.back() == partner, "BAUD must reach the paired slave");
    });
    test("stranger DATA cannot write UART", [] {
        paired(); receive(stranger, {PKT_DATA, 0x12, 0x34});
        require(Serial.written.empty(), "foreign data reached UART");
    });
    test("unpaired DATA cannot write UART", [] {
        receive(partner, {PKT_DATA, 0x12});
        require(Serial.written.empty(), "unpaired data reached UART");
    });
    test("stranger BAUD cannot change slave or flash", [] {
        paired(); saveState(); receive(stranger, baudPacket(PKT_BAUD, 19200));
        require(currentBaud == BAUD_DEFAULT && Serial.baud == BAUD_DEFAULT, "foreign baud was applied");
    });
    test("stranger UNPAIR preserves pair and flash", [] {
        paired(); saveState(); receive(stranger, {PKT_UNPAIR});
        require(state == ST_PAIRED_SLAVE, "foreign UNPAIR removed pair");
        require(loadState(), "foreign UNPAIR erased flash");
    });
    test("stranger PING cannot elicit a peer reply", [] {
        paired(); receive(stranger, {PKT_PING});
        require(fake::sent.empty(), "foreign PING elicited PONG");
    });
    test("paired DATA reaches UART and refreshes contact", [] {
        paired(); fake::nowMs = 100; receive(partner, {PKT_DATA, 0x00, 0xff, 0x7e});
        require(Serial.written == std::vector<uint8_t>({0x00, 0xff, 0x7e}), "binary data changed");
        require(lastContact == 100, "peer contact not refreshed");
        std::vector<uint8_t> maximumPacket(250, 0xa5);
        maximumPacket[0] = PKT_DATA;
        fake::nowMs = 200;
        receive(partner, maximumPacket);
        require(Serial.written.size() == 3 + 249 &&
                std::all_of(Serial.written.begin() + 3, Serial.written.end(),
                            [](uint8_t byte) { return byte == 0xa5; }),
                "maximum-sized DATA payload changed");
        require(lastContact == 200, "maximum-sized DATA did not refresh contact");
    });
    test("paired BAUD configures slave and persists", [] {
        paired(); receive(partner, baudPacket(PKT_BAUD, 57600));
        require(Serial.baud == 57600 && currentBaud == 57600, "peer baud not applied");
        currentBaud = BAUD_DEFAULT; require(loadState() && currentBaud == 57600, "peer baud not saved");
    });
    test("paired UNPAIR clears flash and seeks", [] {
        paired(); saveState(); receive(partner, {PKT_UNPAIR});
        require(state == ST_SEEKING && !loadState(), "peer UNPAIR not applied");
    });
    test("paired PING returns PONG", [] {
        paired(); receive(partner, {PKT_PING});
        require(fake::sent == std::vector<std::vector<uint8_t>>{{PKT_PONG}}, "peer heartbeat reply missing");
        require(fake::destinations == std::vector<std::array<uint8_t, 6>>{partner},
                "PONG must reach the paired peer");
    });
    test("master ignores BAUD from slave", [] {
        paired(ST_PAIRED_MASTER); receive(partner, baudPacket(PKT_BAUD, 19200));
        require(currentBaud == BAUD_DEFAULT, "slave changed master baud");
    });
    test("truncated BAUD, ACK and CLAIM frames have no side effects", [] {
        paired(); receive(partner, {PKT_BAUD, 0x80, 0x25});
        require(Serial.baud == BAUD_DEFAULT, "truncated baud frame accepted");
        state = ST_SEEKING;
        receive(partner, {PKT_ACK, 0x80, 0x25});
        require(state == ST_SEEKING, "truncated ACK created a pair");
        state = ST_CONFIRM;
        hasCandidate = true;
        std::copy(partner.begin(), partner.end(), candidateMac);
        receive(stranger, {PKT_CLAIM, 2, 1, 2});
        require(state == ST_CONFIRM && hasCandidate, "truncated CLAIM removed the candidate");
    });
    test("PAIR discovery and button select master", [] {
        receive(partner, {PKT_PAIR});
        require(state == ST_CONFIRM && hasCandidate, "PAIR did not create candidate");
        btn.clickEvent = true; handleButton();
        require(state == ST_PAIRED_MASTER, "click did not select master role");
        require(fake::sent.front() == std::vector<uint8_t>({PKT_CLAIM, 2, 1, 2, 3, 4, 5}), "CLAIM must carry selected slave MAC");
    });
    test("ACK assigns slave while still seeking", [] {
        receive(partner, baudPacket(PKT_ACK, 19200));
        require(state == ST_PAIRED_SLAVE && Serial.baud == 19200, "ACK did not pair seeker as slave");
    });
    test("confirmation timeout expires only a confirmation", [] {
        fake::nowMs = CONFIRM_TTL + 1;
        hasCandidate = true;
        confirmLoop();
        require(hasCandidate, "SEEKING must not run confirmation cleanup");
        state = ST_CONFIRM; confirmStart = 0; confirmLoop();
        require(state == ST_SEEKING && !hasCandidate, "confirmation did not expire");
    });
    test("offline timeout preserves saved pair", [] {
        paired(); saveState(); fake::nowMs = OFFLINE_AFTER + 1;
        loop();
        require(state == ST_PAIRED_SLAVE && loadState(), "offline link must keep pair for reconnect");
    });
    test("ESP-NOW initializes before registering broadcast", [] {
        setup();
        require(fake::peerAddsBeforeInit == 0, "broadcast peer added before ESP-NOW init");
        require(fake::peers.count(key(BROADCAST)) == 1, "broadcast peer missing");
    });
    test("serial output is packetized into 249-byte payloads", [] {
        paired(); Serial.received.resize(300);
        for (int i = 0; i < 300; ++i) Serial.received[i] = static_cast<uint8_t>(i);
        dataLoop(); dataLoop();
        require(fake::sent.size() == 2 && fake::sent[0].size() == 250 && fake::sent[1].size() == 52, "payload boundaries changed");
        require(fake::sent[0][1] == 0 && fake::sent[1][1] == 249, "packet data ordering changed");
    });
#ifdef ESP8266
    test("ESP8266 uses the configured 10-bit PWM range", [] {
        setup();
        require(fake::pwmRange == LED_PWM_MAX, "PWM range differs from LED calculations");
    });
#endif
    std::cout << count - failures << '/' << count << " passed\n";
    return failures ? 1 : 0;
}
