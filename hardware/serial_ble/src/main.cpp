#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <atomic>
#include "protocol.h"

using namespace serial_ble;

static std::atomic<bool> connected{false}, encrypted{false}, mouseSubscribed{false}, keySubscribed{false};
static std::atomic<uint32_t> epoch{1};
static bool initialized = false;
static NimBLECharacteristic* mouseReport = nullptr;
static NimBLECharacteristic* keyReport = nullptr;

static uint8_t reportMap[] = {
    // Mouse: buttons, relative X/Y, vertical wheel, horizontal AC Pan.
    0x05,0x01,0x09,0x02,0xA1,0x01,0x85,0x01,0x09,0x01,0xA1,0x00,
    0x05,0x09,0x19,0x01,0x29,0x03,0x15,0x00,0x25,0x01,0x95,0x03,0x75,0x01,0x81,0x02,
    0x95,0x01,0x75,0x05,0x81,0x03,0x05,0x01,0x09,0x30,0x09,0x31,0x09,0x38,
    0x15,0x81,0x25,0x7F,0x75,0x08,0x95,0x03,0x81,0x06,
    0x05,0x0C,0x0A,0x38,0x02,0x95,0x01,0x81,0x06,0xC0,0xC0,
    // Keyboard: modifier byte, reserved byte, six key usages.
    0x05,0x01,0x09,0x06,0xA1,0x01,0x85,0x02,0x05,0x07,0x19,0xE0,0x29,0xE7,
    0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,0x81,0x02,0x95,0x01,0x75,0x08,0x81,0x03,
    0x95,0x06,0x75,0x08,0x15,0x00,0x25,0x65,0x05,0x07,0x19,0x00,0x29,0x65,0x81,0x00,0xC0
};

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
        encrypted = false; mouseSubscribed = false; keySubscribed = false;
        ++epoch;
        connected = true;
        server->updateConnParams(info.getConnHandle(), 12, 24, 0, 200);
    }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
        connected = false; encrypted = false; mouseSubscribed = false; keySubscribed = false;
        ++epoch;
        NimBLEDevice::startAdvertising();
    }
    void onAuthenticationComplete(NimBLEConnInfo& info) override {
        encrypted = info.isEncrypted();
        if (!info.isEncrypted()) {
            ++epoch;
            NimBLEDevice::getServer()->disconnect(info.getConnHandle());
        }
    }
} serverCallbacks;

class ReportCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic* report, NimBLEConnInfo&, uint16_t value) override {
        bool enabled = (value & 1) != 0;
        if (report == mouseReport) mouseSubscribed = enabled;
        else keySubscribed = enabled;
        if (!enabled) ++epoch;
    }
} reportCallbacks;

class BleIO : public IO {
public:
    uint32_t generation() override { return epoch; }
    bool ready() override {
        return initialized && connected && encrypted && mouseSubscribed && keySubscribed;
    }
    bool host() override { return static_cast<bool>(Serial); }
    uint32_t now() override { return millis(); }
    void pause(unsigned ms) override { delay(ms); }
    bool mouse(uint8_t buttons, int x, int y, int v, int h) override {
        if (!ready()) return false;
        const uint8_t data[] = {buttons, static_cast<uint8_t>(x), static_cast<uint8_t>(y),
                               static_cast<uint8_t>(v), static_cast<uint8_t>(h)};
        mouseReport->setValue(data, sizeof(data));
        return mouseReport->notify();
    }
    bool keyboard(uint8_t modifiers, const uint8_t* keys) override {
        if (!ready()) return false;
        uint8_t data[8] = {modifiers, 0};
        memcpy(data + 2, keys, 6);
        keyReport->setValue(data, sizeof(data));
        return keyReport->notify();
    }
} io;

static Engine engine(io);
static LineReader input;

static void reply(const Command& command, const char* error) {
    if (error) {
        Serial.printf("{\"id\":\"%s\",\"ok\":false,\"error\":\"%s\",\"generation\":%lu}\n",
                      command.id, error, static_cast<unsigned long>(epoch.load()));
    } else {
        Serial.printf("{\"id\":\"%s\",\"ok\":true,\"firmware\":\"%s\",\"generation\":%lu,"
                      "\"initialized\":%s,\"connected\":%s,\"encrypted\":%s,\"subscribed\":%s,\"ready\":%s}\n",
                      command.id, VERSION, static_cast<unsigned long>(epoch.load()),
                      initialized ? "true" : "false", connected ? "true" : "false",
                      encrypted ? "true" : "false", mouseSubscribed && keySubscribed ? "true" : "false",
                      engine.ready() ? "true" : "false");
    }
}

void setup() {
    Serial.begin(115200);
    if (!NimBLEDevice::init("iPhone-Agent-Serial")) return;
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    auto* server = NimBLEDevice::createServer();
    server->setCallbacks(&serverCallbacks);
    auto* hid = new NimBLEHIDDevice(server);
    mouseReport = hid->getInputReport(1);
    keyReport = hid->getInputReport(2);
    mouseReport->setCallbacks(&reportCallbacks);
    keyReport->setCallbacks(&reportCallbacks);
    hid->setManufacturer("ESP32 iPhone Agent");
    hid->setPnp(0x01, 0xFFFF, 0x0002, 0x0100);
    hid->setHidInfo(0, 0x02);
    hid->setReportMap(reportMap, sizeof(reportMap));
    hid->setBatteryLevel(100);
    hid->startServices();
    auto* advertising = NimBLEDevice::getAdvertising();
    advertising->setAppearance(HID_KEYBOARD);
    advertising->addServiceUUID(hid->getHidService()->getUUID());
    advertising->setName("iPhone-Agent-Serial");
    advertising->enableScanResponse(true);
    initialized = advertising->start();
}

void loop() {
    engine.tick();
    // Bound work per iteration so a noisy host cannot starve the drag watchdog.
    for (unsigned i = 0; i < 128 && Serial.available(); ++i) {
        if (!input.feed(static_cast<uint8_t>(Serial.read()))) continue;
        Command command;
        const char* error = input.invalid ? "invalid_line" : parse(input.data, command);
        if (error) engine.abort();
        else error = engine.run(command);
        reply(command, error);
        input.reset();
    }
    delay(2);
}
