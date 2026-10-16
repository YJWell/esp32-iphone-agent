#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace serial_ble {

constexpr size_t MAX_LINE = 600;
constexpr size_t MAX_TEXT = 256;
constexpr uint32_t HOLD_MS = 3000;
constexpr const char* VERSION = "esp32-iphone-serial-ble/1";

enum class Op { Status, Ping, Home, Move, Press, Release, Click, Scroll, Type, Key };

struct Command {
    char id[33] = {};
    uint32_t generation = 0;
    Op op = Op::Status;
    int a = 0, b = 0;
    const char* text = "";
    uint8_t modifiers = 0, keys[6] = {};
};

inline bool asciiKey(char ch, uint8_t& key, uint8_t& modifiers) {
    if (ch >= 'a' && ch <= 'z') { key = ch - 'a' + 4; return true; }
    if (ch >= 'A' && ch <= 'Z') { key = ch - 'A' + 4; modifiers |= 2; return true; }
    if (ch >= '1' && ch <= '9') { key = ch - '1' + 30; return true; }
    if (ch == '0') { key = 39; return true; }
    const char* plain = " -=[]\\;'`,./";
    const char* shifted = "_+{}|:\"~<>?";
    const uint8_t codes[] = {44, 45, 46, 47, 48, 49, 51, 52, 53, 54, 55, 56};
    const char* p = strchr(plain, ch);
    if (p && ch) { key = codes[p - plain]; return true; }
    p = strchr(shifted, ch);
    if (p && ch) { key = codes[p - shifted + 1]; modifiers |= 2; return true; }
    const char* digits = "!@#$%^&*()";
    p = strchr(digits, ch);
    if (p && ch) { key = 30 + (p - digits); modifiers |= 2; return true; }
    return false;
}

inline bool chord(char* text, Command& out) {
    size_t count = 0;
    uint8_t explicitModifiers = 0;
    while (*text) {
        char* end = strchr(text, '+');
        if (end) *end = 0;
        uint8_t key = 0, modifier = 0, implicit = 0;
        if (!strcmp(text, "ctrl")) modifier = 1;
        else if (!strcmp(text, "shift")) modifier = 2;
        else if (!strcmp(text, "alt")) modifier = 4;
        else if (!strcmp(text, "cmd")) modifier = 8;
        else {
            const char* names[] = {"enter", "esc", "backspace", "tab", "space", "right", "left", "down", "up"};
            const uint8_t values[] = {40, 41, 42, 43, 44, 79, 80, 81, 82};
            for (size_t i = 0; i < 9; ++i) if (!strcmp(text, names[i])) key = values[i];
            if (!key && (strlen(text) != 1 || !asciiKey(*text, key, implicit))) return false;
            if (count == 6) return false;
            for (size_t i = 0; i < count; ++i) if (out.keys[i] == key) return false;
            out.keys[count++] = key;
        }
        if (explicitModifiers & modifier) return false;
        explicitModifiers |= modifier;
        out.modifiers |= modifier | implicit;
        if (!end) return true;
        text = end + 1;
        if (!*text) return false;
    }
    return false;
}

inline bool integer(const char*& p, int& out, int bound) {
    const char* start = p;
    bool negative = *p == '-';
    if (negative) ++p;
    if (*p < '0' || *p > '9') return false;
    int value = 0;
    do {
        value = value * 10 + (*p++ - '0');
        if (value > bound) return false;
    } while (*p >= '0' && *p <= '9');
    if (p - start > 5 + static_cast<int>(negative)) return false;
    out = negative ? -value : value;
    return true;
}

// Mutates separators only after validating the entire printable, bounded frame.
inline const char* parse(char* line, Command& out) {
    size_t length = strlen(line);
    if (length >= MAX_LINE) return "line_too_long";
    for (size_t i = 0; i < length; ++i)
        if (line[i] < 32 || line[i] > 126) return "non_printable_ascii";
    if (length < 36 || line[32] != ' ') return "invalid_id";
    for (size_t i = 0; i < 32; ++i)
        if (!((line[i] >= '0' && line[i] <= '9') || (line[i] >= 'a' && line[i] <= 'f')))
            return "invalid_id";
    memcpy(out.id, line, 32);
    const char* p = line + 33;
    if (*p < '0' || *p > '9') return "invalid_generation";
    uint64_t generation = 0;
    do {
        generation = generation * 10 + (*p++ - '0');
        if (generation > UINT32_MAX) return "invalid_generation";
    } while (*p >= '0' && *p <= '9');
    if (*p++ != ' ') return "invalid_generation";
    out.generation = static_cast<uint32_t>(generation);
    char* name = line + (p - line);
    char* arg = strchr(name, ' ');
    if (arg) *arg++ = 0;
    if (!strcmp(name, "type")) {
        if (!arg || !*arg || strlen(arg) > MAX_TEXT) return "invalid_text";
        out.op = Op::Type; out.text = arg;
        return nullptr;
    }
    if (!strcmp(name, "key")) {
        out.op = Op::Key;
        return arg && chord(arg, out) ? nullptr : "invalid_chord";
    }
    if (!strcmp(name, "move") || !strcmp(name, "scroll")) {
        out.op = !strcmp(name, "move") ? Op::Move : Op::Scroll;
        p = arg;
        int bound = out.op == Op::Move ? 10000 : 127;
        if (!p || !integer(p, out.a, bound) || *p++ != ' ' ||
            !integer(p, out.b, bound) || *p) return "invalid_deltas";
        return nullptr;
    }
    if (arg) return "unexpected_arguments";
    if (!strcmp(name, "status")) out.op = Op::Status;
    else if (!strcmp(name, "ping")) out.op = Op::Ping;
    else if (!strcmp(name, "home")) out.op = Op::Home;
    else if (!strcmp(name, "press")) out.op = Op::Press;
    else if (!strcmp(name, "release")) out.op = Op::Release;
    else if (!strcmp(name, "click")) out.op = Op::Click;
    else return "unknown_command";
    return nullptr;
}

struct LineReader {
    char data[MAX_LINE] = {};
    size_t used = 0;
    bool invalid = false;
    bool feed(uint8_t ch) {
        if (ch == '\n') { data[used] = 0; return true; }
        if (ch < 32 || ch > 126 || used == MAX_LINE - 1) invalid = true;
        else if (!invalid) data[used++] = static_cast<char>(ch);
        return false;
    }
    void reset() { used = 0; invalid = false; }
};

struct IO {
    virtual ~IO() = default;
    virtual uint32_t generation() = 0;
    virtual bool ready() = 0;
    virtual bool host() = 0;
    virtual uint32_t now() = 0;
    virtual void pause(unsigned ms) = 0;
    virtual bool mouse(uint8_t buttons, int x, int y, int v, int h) = 0;
    virtual bool keyboard(uint8_t modifiers, const uint8_t* keys) = 0;
};

class Engine {
    IO& io;
    uint32_t epoch = 0, heldAt = 0;
    bool held = false, pending = true, expired = false;

    bool same(uint32_t expected) { return io.generation() == expected && io.ready() && io.host(); }
    bool clear() {
        const uint8_t keys[6] = {};
        const uint32_t expected = epoch;
        bool mouse = io.generation() == expected && io.ready() && io.mouse(0, 0, 0, 0, 0);
        bool keyboard = io.generation() == expected && io.ready() && io.keyboard(0, keys);
        held = false;
        pending = !(mouse && keyboard && io.generation() == expected && io.ready());
        return !pending;
    }
    bool mouse(uint32_t expected, int x = 0, int y = 0, int v = 0, int h = 0) {
        return same(expected) && io.mouse(held ? 1 : 0, x, y, v, h) && same(expected);
    }
    bool keyboard(uint32_t expected, uint8_t mods, const uint8_t* keys) {
        return same(expected) && io.keyboard(mods, keys) && same(expected);
    }
    const char* fail() { abort(); return "action_uncertain_not_retried"; }

public:
    explicit Engine(IO& output) : io(output) {}
    bool ready() { return same(epoch) && !pending; }
    void abort() {
        pending = true;
        held = false;
        if (io.generation() == epoch && io.ready()) clear();
    }
    void tick() {
        if (io.generation() != epoch) {
            epoch = io.generation();
            pending = true; held = false; expired = false;
        }
        if (!io.ready()) { pending = true; held = false; }
        if (!io.host() && held) abort();
        if (held && static_cast<uint32_t>(io.now() - heldAt) >= HOLD_MS) {
            expired = true;
            abort();
        }
        // Only neutral reports may be retried.
        if (pending && io.ready()) clear();
    }
    const char* run(const Command& command) {
        tick();
        if (command.op == Op::Status || command.op == Op::Ping) return nullptr;
        if (!ready() || command.generation != epoch) {
            abort();
            return "not_ready_or_generation_changed";
        }
        if (expired && command.op != Op::Release && command.op != Op::Home) {
            abort();
            return "drag_lease_expired";
        }
        const uint32_t expected = epoch;
        if (command.op == Op::Release || command.op == Op::Home) {
            expired = false;
            if (!clear()) return fail();
        }
        switch (command.op) {
            case Op::Press:
            case Op::Click:
                if (held) { abort(); return "already_pressed"; }
                held = true; heldAt = io.now();
                if (!mouse(expected)) return fail();
                if (command.op == Op::Press) break;
                io.pause(60);
                held = false;
                if (!mouse(expected)) return fail();
                break;
            case Op::Move: {
                int x = command.a, y = command.b;
                while (x || y) {
                    if (held && static_cast<uint32_t>(io.now() - heldAt) >= HOLD_MS) {
                        expired = true;
                        return fail();
                    }
                    int sx = x > 10 ? 10 : x < -10 ? -10 : x;
                    int sy = y > 10 ? 10 : y < -10 ? -10 : y;
                    if (!mouse(expected, sx, sy)) return fail();
                    x -= sx; y -= sy; io.pause(8);
                }
                if (held) heldAt = io.now();
                break;
            }
            case Op::Home:
                for (int i = 0; i < 50; ++i) {
                    if (!mouse(expected, -127, -127)) return fail();
                    io.pause(8);
                }
                break;
            case Op::Scroll:
                if (!mouse(expected, 0, 0, command.a, command.b)) return fail();
                break;
            case Op::Type:
                if (held) { abort(); return "release_before_typing"; }
                for (const char* p = command.text; *p; ++p) {
                    uint8_t keys[6] = {}, mods = 0;
                    asciiKey(*p, keys[0], mods);
                    if (!keyboard(expected, mods, keys)) return fail();
                    io.pause(12);
                    keys[0] = 0;
                    if (!keyboard(expected, 0, keys)) return fail();
                    io.pause(12);
                }
                break;
            case Op::Key: {
                if (held) { abort(); return "release_before_typing"; }
                if (!keyboard(expected, command.modifiers, command.keys)) return fail();
                io.pause(30);
                const uint8_t keys[6] = {};
                if (!keyboard(expected, 0, keys)) return fail();
                break;
            }
            default: break;
        }
        return same(expected) ? nullptr : fail();
    }
};

}  // namespace serial_ble
