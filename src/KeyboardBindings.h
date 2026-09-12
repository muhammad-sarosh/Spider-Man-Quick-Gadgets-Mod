#pragma once
#include <array>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>

namespace quickgadgets {
// Windows virtual-key values; independent of Windows.h for unit tests.
inline std::optional<unsigned short> ParseKeyboardKey(std::wstring_view input) {
    std::wstring key;
    for (wchar_t ch : input) {
        if (!std::iswspace(ch)) key += static_cast<wchar_t>(std::towupper(ch));
    }
    if (key == L"NONE" || key == L"DISABLED") return static_cast<unsigned short>(0);
    if (key.size() == 1 && ((key[0] >= L'A' && key[0] <= L'Z') ||
                            (key[0] >= L'0' && key[0] <= L'9')))
        return static_cast<unsigned short>(key[0]);
    if (key.size() >= 2 && key[0] == L'F') {
        unsigned number = 0;
        for (std::size_t i = 1; i < key.size(); ++i) {
            if (key[i] < L'0' || key[i] > L'9') return std::nullopt;
            number = number * 10 + static_cast<unsigned>(key[i] - L'0');
            if (number > 24) return std::nullopt;
        }
        if (number >= 1) return static_cast<unsigned short>(0x70 + number - 1);
        return std::nullopt;
    }
    if (key.size() >= 3 && key[0] == L'V' && key[1] == L'K') {
        unsigned number = 0;
        for (std::size_t i = 2; i < key.size(); ++i) {
            if (key[i] < L'0' || key[i] > L'9') return std::nullopt;
            number = number * 10 + static_cast<unsigned>(key[i] - L'0');
            if (number > 255) return std::nullopt;
        }
        if (number >= 1) return static_cast<unsigned short>(number);
        return std::nullopt;
    }
    constexpr struct { std::wstring_view name; unsigned short code; } named[] = {
        {L"MOUSE1", 0x01}, {L"MOUSE2", 0x02}, {L"MOUSE3", 0x04},
        {L"MOUSE4", 0x05}, {L"MOUSE5", 0x06},
        {L"SPACE", 0x20}, {L"TAB", 0x09}, {L"ENTER", 0x0D},
        {L"BACKSPACE", 0x08}, {L"INSERT", 0x2D}, {L"DELETE", 0x2E},
        {L"HOME", 0x24}, {L"END", 0x23}, {L"PAGEUP", 0x21},
        {L"PAGEDOWN", 0x22}, {L"LEFT", 0x25}, {L"UP", 0x26},
        {L"RIGHT", 0x27}, {L"DOWN", 0x28},
        {L"LSHIFT", 0xA0}, {L"RSHIFT", 0xA1},
        {L"LCTRL", 0xA2}, {L"RCTRL", 0xA3},
        {L"LALT", 0xA4}, {L"RALT", 0xA5},
        {L"SEMICOLON", 0xBA}, {L"EQUALS", 0xBB}, {L"COMMA", 0xBC},
        {L"MINUS", 0xBD}, {L"PERIOD", 0xBE}, {L"SLASH", 0xBF},
        {L"BACKTICK", 0xC0}, {L"LBRACKET", 0xDB}, {L"BACKSLASH", 0xDC},
        {L"RBRACKET", 0xDD}, {L"APOSTROPHE", 0xDE},
        {L"NUMPAD0", 0x60}, {L"NUMPAD1", 0x61}, {L"NUMPAD2", 0x62},
        {L"NUMPAD3", 0x63}, {L"NUMPAD4", 0x64}, {L"NUMPAD5", 0x65},
        {L"NUMPAD6", 0x66}, {L"NUMPAD7", 0x67}, {L"NUMPAD8", 0x68},
        {L"NUMPAD9", 0x69},
    };
    for (const auto& entry : named) if (key == entry.name) return entry.code;
    return std::nullopt;
}

class KeyboardEdges {
public:
    // Tracking inactive polls prevents a held key firing on refocus/enable.
    int Poll(const std::array<bool, 8>& down, bool active) {
        int result = -1;
        if (active && wasActive_) {
            for (int slot = 0; slot < 8; ++slot) {
                if (down[slot] && !previous_[slot]) { result = slot; break; }
            }
        }
        previous_ = down;
        wasActive_ = active;
        return result;
    }
private:
    std::array<bool, 8> previous_{};
    bool wasActive_ = false;
};

class KeyboardTapMode {
public:
    // In select/double-fire mode the first tap selects immediately. A second
    // tap on the same gadget inside the window fires it.
    bool ShouldFire(int slot, unsigned long long now, bool enabled,
                    unsigned long long windowMs) const {
        if (!enabled) return true;
        return slot == lastSlot_ && now >= lastTap_ &&
            now - lastTap_ <= windowMs;
    }
    void Commit(int slot, unsigned long long now, bool enabled, bool fired) {
        if (!enabled || fired) {
            lastSlot_ = -1;
            lastTap_ = 0;
        } else {
            lastSlot_ = slot;
            lastTap_ = now;
        }
    }
    void Reset() { lastSlot_ = -1; lastTap_ = 0; }
private:
    int lastSlot_ = -1;
    unsigned long long lastTap_ = 0;
};
}
