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
    return failures ? 1 : 0;
}
