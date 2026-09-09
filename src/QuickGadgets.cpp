#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <new>
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
    bool keyboardWheelFallback = false;
    bool enabled = true;
};

Config g_config;
std::mutex g_configMutex;
std::atomic_bool g_running = true;
std::atomic_bool g_enabled = true;
volatile LONG g_workerStarted = 0;

// Keep this marker independent of the C++ stream/CRT logging path. If the
// loader calls either DLL_PROCESS_ATTACH or script_enable(), a marker should
// appear beside the DLL even when Script Hook's console/stdout capture is
// unavailable.
void WriteModuleMarker(HMODULE module, const char* message) {
    if (!module) return;
    std::array<wchar_t, MAX_PATH> path{};
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return;

    std::wstring markerPath(path.data(), length);
    const auto slash = markerPath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    markerPath.resize(slash + 1);
    markerPath += L"QuickGadgets.entry.log";

    const HANDLE file = CreateFileW(markerPath.c_str(), FILE_APPEND_DATA,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    DWORD written = 0;
    WriteFile(file, message, static_cast<DWORD>(std::strlen(message)), &written, nullptr);
    CloseHandle(file);
}

void WriteEntryMarker(const char* message) {
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&WriteEntryMarker), &module)) {
        WriteModuleMarker(module, message);
    }
}

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

    // Script Hook's console is the most useful diagnostic when a script is
    // being developed. Keep the file log as well, but mirror messages to
    // stdout so they appear beside the loader's "Successfully Loaded Script"
    // line when USE_CONSOLE_OUT=TRUE.
    std::fputs("[QuickGadgets] ", stdout);
    std::fputs(message.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// The community Script Hook exports a small set of engine helpers. These
// declarations intentionally stay local to this script so the released DLL
// remains compatible with the loader's existing ABI. GetComponentByName uses
// the MSVC std::string layout (small-string optimization), represented here by
// the fields the hook reads.
struct EngineString {
    union {
        char local[16];
        char* heap;
    } storage{};
    std::uint64_t size = 0;
    std::uint64_t capacity = 15;

    explicit EngineString(const char* text) {
        const auto length = std::strlen(text);
        if (length < 16) {
            std::memcpy(storage.local, text, length);
            storage.local[length] = '\0';
        } else {
            storage.heap = new char[length + 1];
            std::memcpy(storage.heap, text, length + 1);
            capacity = static_cast<std::uint64_t>(length);
        }
        size = static_cast<std::uint64_t>(length);
    }

    ~EngineString() {
        if (capacity >= 16) delete[] storage.heap;
    }

    EngineString(const EngineString&) = delete;
    EngineString& operator=(const EngineString&) = delete;
};

using GetPlayerHeroFn = void* (*)();
struct PointerVector {
    void** begin = nullptr;
    void** end = nullptr;
    void** capacity = nullptr;
};
using GetComponentsFn = void (*)(PointerVector*, void*);
using GetComponentByNameFn = void* (*)(void*, EngineString*);
using GetComponentNameFn = const char* (*)(void*);
using GameMainThreadCallFn = void (*)(void (*)());

struct NativeApi {
    GetPlayerHeroFn getPlayerHero = nullptr;
    GetComponentsFn getComponents = nullptr;
    GetComponentByNameFn getComponentByName = nullptr;
    GetComponentNameFn getComponentName = nullptr;
    GameMainThreadCallFn gameMainThreadCall = nullptr;

    bool Resolve() {
        const auto hook = GetModuleHandleA("ScriptHookSMPC.dll");
        if (!hook) return false;
        getPlayerHero = reinterpret_cast<GetPlayerHeroFn>(GetProcAddress(hook, "GetPlayerHero"));
        getComponents = reinterpret_cast<GetComponentsFn>(GetProcAddress(hook, "GetComponents"));
        getComponentByName = reinterpret_cast<GetComponentByNameFn>(GetProcAddress(hook, "GetComponentByName"));
        getComponentName = reinterpret_cast<GetComponentNameFn>(GetProcAddress(hook, "GetComponentName"));
        gameMainThreadCall = reinterpret_cast<GameMainThreadCallFn>(GetProcAddress(hook, "GameMainThreadCallFunc"));
        return getPlayerHero && getComponentByName && gameMainThreadCall;
    }
};

NativeApi g_native;
std::atomic_bool g_probeFinished = false;
std::atomic_bool g_probeLoggedHero = false;

// The equip manager's public component name is known, but its native method
// names are not exported by Script Hook. For one diagnostic run we clone the
// live instance vtable and wrap only the methods that are plausible loadout
// operations. The wrappers forward the original call unchanged and log the
// first few register arguments. This lets us observe the game's own wheel
// path without synthesising keyboard/controller input.
using TraceMethodFn = std::uintptr_t (*)(void*, void*, void*, void*);

struct EquipTraceState {
    void*** objectVtable = nullptr;
    void** originalVtable = nullptr;
    void** clonedVtable = nullptr;
    TraceMethodFn originals[32]{};
    std::atomic_uint32_t calls[32]{};
    bool installed = false;
};

EquipTraceState g_equipTrace;

std::uintptr_t TraceEquipCall(int slot, void* self, void* rdx, void* r8, void* r9) {
    if (slot >= 0 && slot < 32) {
        const auto count = g_equipTrace.calls[slot].fetch_add(1);
        if (count < 12) {
            char line[256]{};
            std::snprintf(line, sizeof(line),
                          "SunsetEquipManager native call slot %d: this=%p rdx=%p r8=%p r9=%p",
                          slot, self, rdx, r8, r9);
            Log(line);
        }
        if (g_equipTrace.originals[slot]) {
            return g_equipTrace.originals[slot](self, rdx, r8, r9);
        }
    }
    return 0;
}

#define QUICK_GADGETS_TRACE_WRAPPER(n) \
    std::uintptr_t TraceEquip##n(void* self, void* rdx, void* r8, void* r9) { \
        return TraceEquipCall(n, self, rdx, r8, r9); \
    }

QUICK_GADGETS_TRACE_WRAPPER(13)
QUICK_GADGETS_TRACE_WRAPPER(15)
QUICK_GADGETS_TRACE_WRAPPER(19)
QUICK_GADGETS_TRACE_WRAPPER(25)
QUICK_GADGETS_TRACE_WRAPPER(26)
QUICK_GADGETS_TRACE_WRAPPER(27)

#undef QUICK_GADGETS_TRACE_WRAPPER

void InstallEquipTrace(void* component) {
    if (g_equipTrace.installed || !component) return;

    auto*** objectVtable = reinterpret_cast<void***>(component);
    if (!objectVtable || !*objectVtable) return;

    auto** cloned = new (std::nothrow) void*[32];
    if (!cloned) return;
    std::memcpy(cloned, *objectVtable, sizeof(void*) * 32);

    g_equipTrace.objectVtable = objectVtable;
    g_equipTrace.originalVtable = *objectVtable;
    g_equipTrace.clonedVtable = cloned;
    constexpr int kSlots[] = {13, 15, 19, 25, 26, 27};
    void* kWrappers[] = {
        reinterpret_cast<void*>(&TraceEquip13),
        reinterpret_cast<void*>(&TraceEquip15),
        reinterpret_cast<void*>(&TraceEquip19),
        reinterpret_cast<void*>(&TraceEquip25),
        reinterpret_cast<void*>(&TraceEquip26),
        reinterpret_cast<void*>(&TraceEquip27),
    };
    for (int i = 0; i < static_cast<int>(std::size(kSlots)); ++i) {
        const int slot = kSlots[i];
        g_equipTrace.originals[slot] =
            reinterpret_cast<TraceMethodFn>(cloned[slot]);
        cloned[slot] = kWrappers[i];
    }

    *objectVtable = cloned;
    g_equipTrace.installed = true;
    Log("Installed temporary SunsetEquipManager native call trace (slots 13,15,19,25,26,27)");
}

void ProbeComponentsOnGameThread() {
    if (!g_running || !g_native.getPlayerHero || !g_native.getComponentByName) return;

    // This is a read-only probe and never writes to the game. The hook helper
    // validates the entity/component lookup before returning its pointer.
    void* hero = g_native.getPlayerHero();
    if (!hero) return; // The player entity is not created on every menu.

    if (!g_probeLoggedHero.exchange(true)) {
        char line[128]{};
        std::snprintf(line, sizeof(line), "Hero entity found at %p; probing gadget components", hero);
        Log(line);
    }

    if (g_native.getComponents) {
        PointerVector components{};
        g_native.getComponents(&components, hero);
        if (components.begin && components.end && components.end >= components.begin) {
            const auto count = static_cast<std::size_t>(components.end - components.begin);
            char line[128]{};
            std::snprintf(line, sizeof(line), "Hero component enumeration returned %zu entries", count);
            Log(line);
            for (std::size_t i = 0; i < count && i < 512; ++i) {
                void* component = components.begin[i];
                if (!component) continue;
                const char* name = g_native.getComponentName
                    ? g_native.getComponentName(component)
                    : nullptr;
                char componentLine[256]{};
                std::snprintf(componentLine, sizeof(componentLine),
                              "Hero component[%03zu] %-48s -> %p (vtable: %p)",
                              i,
                              name ? name : "<unnamed>",
                              component,
                              *reinterpret_cast<void**>(component));
                Log(componentLine);

                if (name && std::strcmp(name, "SunsetEquipManager") == 0) {
                    InstallEquipTrace(component);
                }

                const bool interesting = name &&
                    (std::strstr(name, "Gadget") ||
                     std::strstr(name, "Loadout") ||
                     std::strstr(name, "Equip") ||
                     std::strstr(name, "Inventory"));
                if (interesting) {
                    auto** vtable = reinterpret_cast<void***>(component);
                    if (!vtable || !*vtable) continue;
                    for (int slot = 0; slot < 32; ++slot) {
                        void* function = (*vtable)[slot];
                        if (!function) break;
                        char vtableLine[160]{};
                        std::snprintf(vtableLine, sizeof(vtableLine),
                                      "  vtable[%02d] = %p", slot, function);
                        Log(vtableLine);
                    }
                }
            }
        } else {
            Log("GetComponents returned an empty or invalid component vector");
        }
    }

    constexpr const char* kCandidates[] = {
        "GadgetControl",
        "GadgetActivationControl",
        "GadgetWheel",
        "GadgetHolster",
        "HeroGadgetConfig",
        "HeroQuickSelectData",
        "GadgetItemComponentAmmo",
        "GadgetItemAmmoManager",
        "GadgetAimLayer",
        "HeroGadget",
        "GadgetItem",
    };

    for (const char* candidate : kCandidates) {
        EngineString name(candidate);
        void* component = g_native.getComponentByName(hero, &name);
        if (!component) continue;

        const char* resolvedName = g_native.getComponentName
            ? g_native.getComponentName(component)
            : nullptr;
        char line[256]{};
        std::snprintf(line, sizeof(line),
                      "Component %-28s -> %p (engine name: %s, vtable: %p)",
                      candidate,
                      component,
                      resolvedName ? resolvedName : "<unknown>",
                      *reinterpret_cast<void**>(component));
        Log(line);
    }

    g_probeFinished = true;
}

void NativeProbeWorker() {
    if (!g_native.Resolve()) {
        Log("Script Hook component exports were not available; native probe disabled");
        return;
    }

    Log("Native probe armed; waiting for the player entity");
    for (int attempt = 0; g_running && !g_probeFinished && attempt < 120; ++attempt) {
        g_native.gameMainThreadCall(&ProbeComponentsOnGameThread);
        Sleep(1000);
    }
    if (g_running && !g_probeLoggedHero) Log("Native probe did not see a player entity during its 120-second window");
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
    config.keyboardWheelFallback = ReadBool(L"QuickGadgets", L"KeyboardWheelFallback", config.keyboardWheelFallback, path);
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
    bool keyboardWheelFallback = false;
    {
        std::lock_guard lock(g_configMutex);
        g_config = LoadConfig();
        g_enabled = g_config.enabled;
        keyboardWheelFallback = g_config.keyboardWheelFallback;
    }
    Log(keyboardWheelFallback
        ? "Quick Gadgets enabled. Keyboard wheel fallback is ON."
        : "Quick Gadgets enabled. Native route only; keyboard wheel fallback is OFF.");
    std::thread(NativeProbeWorker).detach();

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

        if (!config.keyboardWheelFallback) {
            modifierWasDown = false;
            modifierUsed = false;
            Sleep(2);
            continue;
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

DWORD WINAPI WorkerBootstrap(LPVOID) {
    // DllMain runs while the loader lock is held. Delay the actual C++ work
    // until after the callback returns so configuration/CRT calls are safe.
    Sleep(1500);
    WriteEntryMarker("fallback worker running\r\n");
    Worker();
    return 0;
}

void StartWorker() {
    if (InterlockedCompareExchange(&g_workerStarted, 1, 0) != 0) return;

    HANDLE thread = CreateThread(nullptr, 0, &WorkerBootstrap, nullptr, 0, nullptr);
    if (!thread) {
        InterlockedExchange(&g_workerStarted, 0);
        WriteEntryMarker("fallback CreateThread failed\r\n");
        return;
    }
    CloseHandle(thread);
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        WriteModuleMarker(module, "DllMain process attach\r\n");
        DisableThreadLibraryCalls(module);
        StartWorker();
    }
    if (reason == DLL_PROCESS_DETACH) g_running = false;
    return TRUE;
}

// The current MSMR community script loader invokes this export after loading a
// script package. Keep the worker outside DllMain so the loader lock is never held.
extern "C" __declspec(dllexport) void script_enable() {
    WriteEntryMarker("script_enable called\r\n");
    StartWorker();
}
