#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace {

constexpr int kGadgetCount = 8;

// This order is configurable in QuickGadgets.ini. It is the default wheel order
// used by Marvel's Spider-Man Remastered.
enum Gadget : int {
    WebShooter = 0,
    ImpactWeb,
    SpiderDrone,
    ElectricWeb,
    WebBomb,
    TripMine,
    ConcussiveBlast,
    SuspensionMatrix,
};

struct Config {
    // Physical hotkeys read by the mod (not the game's own controls).
    WORD modifier = VK_MENU; // Left Alt by default.
    std::array<WORD, kGadgetCount> slotKeys{ '1', '2', '3', '4', '5', '6', '7', '8' };
    WORD toggleKey = VK_F10;

    // Bind these three keys in the game's Keyboard & Mouse controls.
    WORD gamePrevious = VK_OEM_4; // [
    WORD gameNext = VK_OEM_6;     // ]
    WORD gameUse = 'E';

    // Each direct shortcut first walks backwards until the game reaches the
    // first wheel entry. This prevents desync after the normal wheel is used.
    int anchorPreviousPulses = 12;
    int inputDelayMs = 18;
    bool restoreWebShooter = false;
    bool enabled = true;
};

Config g_config;
std::mutex g_configMutex;
std::atomic_bool g_running = true;
std::atomic_bool g_enabled = true;

std::wstring GetModuleDirectory() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&GetModuleDirectory), &module)) {
        return L".";
    }
    std::array<wchar_t, MAX_PATH> path{};
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return L".";
    std::wstring directory(path.data(), length);
    const auto slash = directory.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : directory.substr(0, slash);
}

void Log(const std::string& message) {
    std::ofstream out(GetModuleDirectory() + L"\\QuickGadgets.log", std::ios::app);
    if (out) out << message << '\n';
}

int ReadInt(const wchar_t* section, const wchar_t* key, int fallback, const std::wstring& path) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, path.c_str()));
}

bool ReadBool(const wchar_t* section, const wchar_t* key, bool fallback, const std::wstring& path) {
    return ReadInt(section, key, fallback ? 1 : 0, path) != 0;
}

Config LoadConfig() {
    const std::wstring path = GetModuleDirectory() + L"\\QuickGadgets.ini";
    Config config;
    config.modifier = static_cast<WORD>(ReadInt(L"QuickGadgets", L"Modifier", config.modifier, path));
    config.toggleKey = static_cast<WORD>(ReadInt(L"QuickGadgets", L"ToggleKey", config.toggleKey, path));
    config.gamePrevious = static_cast<WORD>(ReadInt(L"GameBindings", L"Previous", config.gamePrevious, path));
    config.gameNext = static_cast<WORD>(ReadInt(L"GameBindings", L"Next", config.gameNext, path));
    config.gameUse = static_cast<WORD>(ReadInt(L"GameBindings", L"Use", config.gameUse, path));
    config.anchorPreviousPulses = std::max(1, ReadInt(L"QuickGadgets", L"AnchorPreviousPulses", config.anchorPreviousPulses, path));
    config.inputDelayMs = std::max(1, ReadInt(L"QuickGadgets", L"InputDelayMs", config.inputDelayMs, path));
    config.restoreWebShooter = ReadBool(L"QuickGadgets", L"RestoreWebShooter", config.restoreWebShooter, path);
    config.enabled = ReadBool(L"QuickGadgets", L"Enabled", config.enabled, path);
    for (int i = 0; i < kGadgetCount; ++i) {
        const std::wstring name = L"Slot" + std::to_wstring(i + 1);
        config.slotKeys[i] = static_cast<WORD>(ReadInt(L"QuickGadgets", name.c_str(), config.slotKeys[i], path));
    }
    return config;
}

void SendKey(WORD key, int delayMs) {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = key;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = key;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
    Sleep(static_cast<DWORD>(delayMs));
}

void SelectAndUse(const Config& config, int target) {
    // Anchoring to index 0 on every press means the mod does not maintain a
    // shadow selected-gadget state that can become stale after wheel use,
    // unlocks, mission restrictions, or cutscenes.
    for (int i = 0; i < config.anchorPreviousPulses; ++i) SendKey(config.gamePrevious, config.inputDelayMs);
    for (int i = 0; i < target; ++i) SendKey(config.gameNext, config.inputDelayMs);
    SendKey(config.gameUse, config.inputDelayMs);
    if (config.restoreWebShooter && target != WebShooter) {
        for (int i = 0; i < config.anchorPreviousPulses; ++i) SendKey(config.gamePrevious, config.inputDelayMs);
    }
}

bool Pressed(WORD key) {
    return (GetAsyncKeyState(key) & 1) != 0;
}

bool Down(WORD key) {
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

void Worker() {
    {
        std::lock_guard lock(g_configMutex);
        g_config = LoadConfig();
        g_enabled = g_config.enabled;
    }
    Log("Quick Gadgets enabled. F10 toggles it by default.");

    bool modifierWasDown = false;
    bool modifierUsed = false;
    auto modifierDownAt = std::chrono::steady_clock::now();

    while (g_running) {
        Config config;
        {
            std::lock_guard lock(g_configMutex);
            config = g_config;
        }

        if (Pressed(config.toggleKey)) {
            g_enabled = !g_enabled;
            Log(g_enabled ? "Quick Gadgets: enabled" : "Quick Gadgets: disabled");
        }

        const bool modifierDown = g_enabled && Down(config.modifier);
        if (modifierDown && !modifierWasDown) {
            modifierDownAt = std::chrono::steady_clock::now();
            modifierUsed = false;
        }

        if (modifierDown) {
            for (int target = 0; target < kGadgetCount; ++target) {
                if (Pressed(config.slotKeys[target])) {
                    SelectAndUse(config, target);
                    modifierUsed = true;
                    break;
                }
            }
        }

        // Spider-Man 2's bare R1 tap is the normal Web Shooter. A shortcut
        // marks the modifier as used, so it does not also fire this tap action.
        if (!modifierDown && modifierWasDown && !modifierUsed) {
            const auto held = std::chrono::steady_clock::now() - modifierDownAt;
            if (held <= std::chrono::milliseconds(400)) SelectAndUse(config, WebShooter);
        }
        modifierWasDown = modifierDown;
        Sleep(2);
    }
}

} // namespace

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) g_running = false;
    return TRUE;
}

// The current MSMR community script loader invokes this export after loading a
// script package. Keep the worker outside DllMain so the loader lock is never held.
extern "C" __declspec(dllexport) void script_enable() {
    static std::once_flag started;
    std::call_once(started, [] { std::thread(Worker).detach(); });
}
