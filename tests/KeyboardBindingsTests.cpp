#include "KeyboardBindings.h"
#include <cstdio>
int main() {
    int failures = 0;
    const auto check = [&failures](bool condition, const char* description) {
        if (!condition) { std::fprintf(stderr, "FAIL: %s\n", description); ++failures; }
    };
    using quickgadgets::ParseKeyboardKey;
    check(ParseKeyboardKey(L" f1 ") == 0x70, "normalization");
    check(ParseKeyboardKey(L"F24") == 0x87, "F24");
    check(ParseKeyboardKey(L"a") == 0x41, "letter");
    check(ParseKeyboardKey(L"2") == 0x32, "number row");
    check(ParseKeyboardKey(L"Numpad2") == 0x62, "numpad");
    check(ParseKeyboardKey(L"Mouse4") == 0x05, "mouse side button");
    check(ParseKeyboardKey(L"LShift") == 0xA0, "left shift");
    check(ParseKeyboardKey(L"Backslash") == 0xDC, "punctuation");
    check(ParseKeyboardKey(L"VK186") == 0xBA, "advanced numeric virtual key");
    check(!ParseKeyboardKey(L"VK256"), "out-of-range virtual key");
    check(ParseKeyboardKey(L"None") == 0, "disabled");
    check(!ParseKeyboardKey(L"F25"), "invalid F25");
    check(!ParseKeyboardKey(L"F0"), "invalid F0");
    check(!ParseKeyboardKey(L"Alt+1"), "unsupported chord");
    check(!ParseKeyboardKey(L""), "empty");
    quickgadgets::KeyboardEdges edges;
    std::array<bool, 8> down{};
    check(edges.Poll(down, true) == -1, "initial activation");
    down[1] = true;
    check(edges.Poll(down, true) == 1, "press");
    check(edges.Poll(down, true) == -1, "hold");
    down[1] = false;
    edges.Poll(down, true);
    down[1] = true;
    check(edges.Poll(down, true) == 1, "second press");
    check(edges.Poll(down, false) == -1, "inactive");
    check(edges.Poll(down, true) == -1, "held on refocus");
    down[1] = false;
    edges.Poll(down, true);
    down[1] = down[4] = true;
    check(edges.Poll(down, true) == 1, "simultaneous priority");
    check(edges.Poll(down, true) == -1, "no delayed simultaneous shot");
    quickgadgets::KeyboardTapMode taps;
    check(taps.ShouldFire(1, 1000, false, 350), "normal mode fires first tap");
    taps.Commit(1, 1000, false, true);
    check(!taps.ShouldFire(1, 2000, true, 350), "mode first tap selects");
    taps.Commit(1, 2000, true, false);
    check(taps.ShouldFire(1, 2250, true, 350), "mode second tap fires");
    taps.Commit(1, 2250, true, true);
    check(!taps.ShouldFire(1, 3000, true, 350), "new sequence selects");
    taps.Commit(1, 3000, true, false);
    check(!taps.ShouldFire(1, 3401, true, 350), "late second tap selects again");
    taps.Commit(1, 3401, true, false);
    check(!taps.ShouldFire(2, 3500, true, 350), "different gadget selects");
    taps.Commit(2, 3500, true, false);
    check(taps.ShouldFire(2, 3700, true, 350), "different gadget second tap fires");
    return failures ? 1 : 0;
}
