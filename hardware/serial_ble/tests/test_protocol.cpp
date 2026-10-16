#include "../src/protocol.h"
#include <assert.h>
#include <stdio.h>
#include <functional>
#include <string>
#include <vector>

using namespace serial_ble;

struct Report {
    bool keyboard;
    uint8_t buttons, mods, key;
    int x, y, v, h;
};

struct Fake : IO {
    uint32_t epoch = 1, clock = 0;
    bool available = true, hostPresent = true;
    int failAt = -1;
    std::function<void()> onPause;
    std::vector<Report> reports;
    uint32_t generation() override { return epoch; }
    bool ready() override { return available; }
    bool host() override { return hostPresent; }
    uint32_t now() override { return clock; }
    void pause(unsigned ms) override { clock += ms; if (onPause) onPause(); }
    bool mouse(uint8_t buttons, int x, int y, int v, int h) override {
        reports.push_back({false, buttons, 0, 0, x, y, v, h});
        return available && static_cast<int>(reports.size()) != failAt;
    }
    bool keyboard(uint8_t mods, const uint8_t* keys) override {
        reports.push_back({true, 0, mods, keys[0], 0, 0, 0, 0});
        return available && static_cast<int>(reports.size()) != failAt;
    }
};

static const char* ID = "0123456789abcdef0123456789abcdef";

struct Fixture {
    Fake io;
    Engine engine{io};
    Fixture() { engine.tick(); io.reports.clear(); }
    const char* run(const std::string& text, uint32_t epoch = 1) {
        std::string line = std::string(ID) + " " + std::to_string(epoch) + " " + text;
        Command cmd;
        const char* error = parse(&line[0], cmd);
        if (error) { engine.abort(); return error; }
        return engine.run(cmd);
    }
};

static void parsing() {
    for (const char* text : {"status", "ping", "home", "press", "release", "click",
                             "move -10000 10000", "scroll -127 127", "type   two  spaces ",
                             "key cmd+shift+h", "key enter", "key ctrl+a+b+c+d+e+f",
                             "key A+shift", "key shift+A"}) {
        std::string line = std::string(ID) + " 1 " + text;
        Command command;
        assert(!parse(&line[0], command));
        assert(!strcmp(command.id, ID));
    }
    for (const char* text : {"", "click left", "click ", "move 1", "move 1 2 3", "move 1  2",
                             "move +1 0", "move 10001 0", "scroll 0 -128", "move 000000 0",
                             "type ", "type \t", "type \r", "type \x7f", "type \xc3\xa9",
                             "key cmd+", "key +a", "key a++b", "key unknown", "key a+A",
                             "key a+b+c+d+e+f+g", "key ctrl+ctrl+a", "key space+ ",
                             "key 1+!", "key -+_", "key /+?"}) {
        std::string line = std::string(ID) + " 1 " + text;
        Command command;
        assert(parse(&line[0], command));
    }
    for (const char* generation : {"-1", "4294967296", "1x", ""}) {
        std::string line = std::string(ID) + " " + generation + " status";
        Command command;
        assert(parse(&line[0], command));
    }
    std::string text = std::string(ID) + " 1 type " + std::string(MAX_TEXT, 'x');
    Command cmd;
    assert(!parse(&text[0], cmd));
    text += 'x';
    Command tooLong;
    assert(parse(&text[0], tooLong));
    std::string spaces = std::string(ID) + " 1 type   hello  ";
    Command preserved;
    assert(!parse(&spaces[0], preserved));
    assert(!strcmp(preserved.text, "  hello  "));
    std::string chordLine = std::string(ID) + " 1 key ctrl+a+b+c+d+e+f";
    Command six;
    assert(!parse(&chordLine[0], six) && six.modifiers == 1);
    for (size_t i = 0; i < 6; ++i) assert(six.keys[i] == i + 4);
    LineReader reader;
    for (size_t i = 0; i < MAX_LINE + 40; ++i) assert(!reader.feed('a'));
    assert(reader.feed('\n') && reader.invalid && reader.used == MAX_LINE - 1);
    reader.reset();
    assert(!reader.feed(0));
    assert(reader.feed('\n') && reader.invalid);
    reader.reset();
    assert(!reader.feed('\r') && reader.invalid);
    assert(reader.feed('\n'));
    reader.reset();
    for (char ch : std::string("ok")) assert(!reader.feed(ch));
    assert(reader.feed('\n') && !reader.invalid && !strcmp(reader.data, "ok"));
    reader.reset();
    for (size_t i = 0; i < MAX_LINE - 1; ++i) assert(!reader.feed('a'));
    assert(reader.feed('\n') && !reader.invalid && reader.used == MAX_LINE - 1);
}

static void ascii() {
    for (int ch = 32; ch <= 126; ++ch) {
        uint8_t key = 0, mods = 0;
        assert(asciiKey(ch, key, mods));
        assert(key >= 4 && key <= 56 && (mods == 0 || mods == 2));
    }
    uint8_t key = 0, mods = 0;
    assert(asciiKey('!', key, mods) && key == 30 && mods == 2);
    mods = 0;
    assert(asciiKey('?', key, mods) && key == 56 && mods == 2);
    mods = 0;
    assert(asciiKey('A', key, mods) && key == 4 && mods == 2);
    const char* punctuation = " -=[]\\;'`,./";
    const uint8_t usages[] = {44, 45, 46, 47, 48, 49, 51, 52, 53, 54, 55, 56};
    for (size_t i = 0; punctuation[i]; ++i) {
        mods = 0;
        assert(asciiKey(punctuation[i], key, mods) && key == usages[i] && mods == 0);
    }
    const char* shifted = "_+{}|:\"~<>?";
    for (size_t i = 0; shifted[i]; ++i) {
        mods = 0;
        assert(asciiKey(shifted[i], key, mods) && key == usages[i + 1] && mods == 2);
    }
}

static void movement() {
    Fixture f;
    assert(!f.run("move 25 -12"));
    assert(f.io.reports.size() == 3);
    assert(f.io.reports[0].x == 10 && f.io.reports[0].y == -10);
    assert(f.io.reports[2].x == 5 && f.io.reports[2].y == 0);
    f.io.reports.clear();
    assert(!f.run("scroll -127 42"));
    assert(f.io.reports.back().v == -127 && f.io.reports.back().h == 42);
    assert(!f.run("press"));
    assert(!f.run("move 10 0") && f.io.reports.back().buttons == 1);
    assert(!f.run("release"));
    assert(f.io.reports[f.io.reports.size()-2].buttons == 0 && f.io.reports.back().key == 0);
    f.io.reports.clear();
    assert(!f.run("home"));
    assert(f.io.reports.size() == 52);
    for (size_t i = 2; i < f.io.reports.size(); ++i)
        assert(f.io.reports[i].x == -127 && f.io.reports[i].y == -127 && !f.io.reports[i].buttons);
    f.io.reports.clear();
    assert(!f.run("click"));
    assert(f.io.reports.size() == 2 && f.io.reports[0].buttons == 1 && !f.io.reports[1].buttons);

    Fixture maximum;
    assert(!maximum.run("move -10000 10000"));
    assert(maximum.io.reports.size() == 1000 && maximum.io.clock == 8000);
    int x = 0, y = 0;
    for (const auto& report : maximum.io.reports) {
        assert(report.x >= -10 && report.x <= 10 && report.y >= -10 && report.y <= 10);
        x += report.x; y += report.y;
    }
    assert(x == -10000 && y == 10000);
}

static void keyboard() {
    Fixture f;
    assert(!f.run("type  A! "));
    assert(f.io.reports.size() == 8);
    assert(f.io.reports[0].key == 44 && f.io.reports[2].key == 4 && f.io.reports[2].mods == 2);
    assert(f.io.reports[4].key == 30 && f.io.reports[6].key == 44);
    for (size_t i = 1; i < f.io.reports.size(); i += 2)
        assert(!f.io.reports[i].key && !f.io.reports[i].mods);
    f.io.reports.clear();
    assert(!f.run("key cmd+shift+h"));
    assert(f.io.reports.size() == 2 && f.io.reports[0].mods == 10 && f.io.reports[0].key == 11);
    assert(!f.io.reports[1].mods && !f.io.reports[1].key);
    f.io.reports.clear();
    assert(f.run("key cmd+unknown"));
    for (const auto& report : f.io.reports) assert(!report.key && !report.mods && !report.buttons);

    Fixture maximum;
    assert(!maximum.run("type " + std::string(MAX_TEXT, 'x')));
    assert(maximum.io.reports.size() == MAX_TEXT * 2 && maximum.io.clock == MAX_TEXT * 24);
}

static void safety() {
    Fixture f;
    f.io.available = false;
    assert(!f.run("status"));
    assert(f.run("click") && f.io.reports.empty());
    f.io.available = true;
    f.engine.tick();
    f.io.reports.clear();
    assert(f.run("click", 9));
    for (const auto& r : f.io.reports) assert(!r.buttons && !r.key);

    Fixture failed;
    failed.io.failAt = 1;
    assert(failed.run("click"));
    failed.engine.tick();
    int presses = 0;
    for (const auto& r : failed.io.reports) presses += r.buttons != 0;
    assert(presses == 1 && failed.engine.ready());

    Fixture releaseFailed;
    releaseFailed.io.failAt = 2;
    assert(releaseFailed.run("click"));
    releaseFailed.engine.tick();
    presses = 0;
    for (const auto& r : releaseFailed.io.reports) presses += r.buttons != 0;
    assert(presses == 1);

    for (const char* action : {"type abc", "key cmd+h"}) {
        Fixture keyFailure;
        keyFailure.io.failAt = 2;
        assert(keyFailure.run(action));
        keyFailure.engine.tick();
        int keyDowns = 0;
        for (const auto& r : keyFailure.io.reports) keyDowns += r.key != 0;
        assert(keyDowns == 1 && !keyFailure.io.reports.back().key);
    }

    for (const char* action : {"move 100 0", "home", "type abc", "key cmd+h", "click"}) {
        Fixture reconnect;
        bool changed = false;
        reconnect.io.onPause = [&]() { if (!changed) { ++reconnect.io.epoch; changed = true; } };
        assert(reconnect.run(action));
        size_t count = reconnect.io.reports.size();
        reconnect.engine.tick();
        assert(reconnect.io.reports.size() == count + 2);
        assert(reconnect.io.reports.back().keyboard && !reconnect.io.reports.back().key);
        assert(reconnect.run("click"));
        assert(!reconnect.run("click", reconnect.io.epoch));
    }

    Fixture lease;
    assert(!lease.run("press"));
    lease.io.clock = HOLD_MS;
    lease.engine.tick();
    assert(!lease.io.reports[lease.io.reports.size()-2].buttons);
    assert(lease.run("move 1 1"));
    assert(!lease.run("release") && !lease.run("move 1 1"));
    assert(!lease.run("press"));
    assert(lease.run("move 10000 0"));
    assert(lease.io.clock < HOLD_MS * 3);

    Fixture renewed;
    assert(!renewed.run("press"));
    renewed.io.clock = HOLD_MS - 100;
    assert(!renewed.run("move 1 0"));
    renewed.io.clock += HOLD_MS - 1;
    assert(!renewed.run("ping"));
    assert(renewed.io.reports.back().buttons == 1);
    ++renewed.io.clock;
    renewed.engine.tick();
    assert(!renewed.io.reports[renewed.io.reports.size()-2].buttons);
    assert(renewed.run("click"));

    Fixture absent;
    assert(!absent.run("press"));
    absent.io.hostPresent = false;
    absent.engine.tick();
    assert(!absent.io.reports[absent.io.reports.size()-2].buttons);
    assert(absent.run("click"));

    Fixture typingDrag;
    assert(!typingDrag.run("press"));
    assert(typingDrag.run("type abc"));
    for (const auto& r : typingDrag.io.reports) assert(!r.key);
}

int main() {
    parsing(); ascii(); movement(); keyboard(); safety();
    puts("serial BLE parser/state tests passed");
}
