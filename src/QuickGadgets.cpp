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
    bool nativeDirectSelect = true;
    bool nativeDirectFire = true;
    bool controllerEnabled = true;
    int controllerIndex = -1;
    std::array<int, 4> controllerSlots{ 4, 3, 1, 2 }; // A, B, X, Y; zero-based.
    bool nativeProbe = false;
    int nativeProbeLevel = 1;
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
std::atomic_int g_nativeProbeLevel = 1;
// Keep a queued request coherent across the worker and game threads. Separate
// atomics allowed the callback to observe a new slot before its fire/suppress
// flags had been published.
constexpr int kNoNativeRequest = -1;
constexpr int kNativeRequestSlotMask = 0xFF;
constexpr int kNativeRequestFire = 0x100;
constexpr int kNativeRequestSuppressFaces = 0x200;
std::atomic_int g_pendingNativeRequest = kNoNativeRequest;
std::atomic_bool g_nativeUseReleasePending = false;
std::atomic_ullong g_nativeUseReleaseAt = 0;
std::atomic_bool g_nativeFirePulseActive = false;
std::atomic_bool g_controllerComboHeld = false;
std::atomic_bool g_suppressionCallbackQueued = false;
std::atomic<void*> g_weaponManager = nullptr;
std::atomic<void*> g_cachedHero = nullptr;

// Verified against Spider-Man.exe 4.0630.0.0. The manager owns eight weapon
// slots (40 bytes each) and the game's own setter accepts the weapon id stored
// in one of those slots. This changes the active gadget without sending any
// keyboard input or opening/cycling the wheel.
constexpr std::uintptr_t kHeroWeaponManagerVtableRva = 0x38B55C8;
constexpr std::uintptr_t kHeroWeaponManagerLocalVtableRva = 0x38B59B8;
constexpr std::uintptr_t kHeroWeaponManagerRemoteVtableRva = 0x38B5B98;
constexpr std::uintptr_t kSelectWeaponByIdRva = 0x09A5FF0;
constexpr std::uintptr_t kSelectWeaponAndNotifyRva = 0x09A4110;
constexpr std::uintptr_t kResolveHandleRva = 0x16798F0;
constexpr std::uintptr_t kSetActionValueRva = 0x09098C0;
constexpr std::size_t kWeaponSlotBase = 0x6C;
constexpr std::size_t kWeaponSlotStride = 0x28;
constexpr std::size_t kInputContextHandleOffset = 0x78C;

constexpr std::uint32_t kActionAttack = 0x2B24146B;
constexpr std::uint32_t kActionDodge = 0x7CA907FC;
constexpr std::uint32_t kActionJump = 0xD69724B0;
constexpr std::uint32_t kActionWebStrike = 0x775E96E1;
constexpr std::uint32_t kActionUseGadget = 0x9D43C80F;

bool ValidateNativeLayout() {
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!module) return false;
    constexpr std::uint8_t kExpectedSelectorBytes[] = {
        0x85, 0xD2, 0x0F, 0x84, 0x0C, 0x01, 0x00, 0x00
    };
    constexpr std::uint8_t kExpectedNotifyBytes[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x81
    };
    constexpr std::uint8_t kExpectedResolverBytes[] = {
        0x8B, 0x11, 0x8B, 0xCA, 0xC1, 0xE9, 0x14, 0x85
    };
    constexpr std::uint8_t kExpectedActionSetterBytes[] = {
        0x85, 0xD2, 0x0F, 0x84, 0xA0, 0x00, 0x00, 0x00
    };
    return std::memcmp(reinterpret_cast<const void*>(module + kSelectWeaponByIdRva),
                       kExpectedSelectorBytes, sizeof(kExpectedSelectorBytes)) == 0 &&
        std::memcmp(reinterpret_cast<const void*>(module + kSelectWeaponAndNotifyRva),
                    kExpectedNotifyBytes, sizeof(kExpectedNotifyBytes)) == 0 &&
        std::memcmp(reinterpret_cast<const void*>(module + kResolveHandleRva),
                    kExpectedResolverBytes, sizeof(kExpectedResolverBytes)) == 0 &&
        std::memcmp(reinterpret_cast<const void*>(module + kSetActionValueRva),
                    kExpectedActionSetterBytes, sizeof(kExpectedActionSetterBytes)) == 0;
}

void* ResolveInputContext(void* manager) {
    if (!manager) return nullptr;
    using ResolveHandleFn = void* (*)(void*);
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto resolveHandle = reinterpret_cast<ResolveHandleFn>(module + kResolveHandleRva);
    return resolveHandle(reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(manager) + kInputContextHandleOffset));
}

void SetActionValue(void* inputContext, std::uint32_t action, float value) {
    if (!inputContext) return;
    using SetActionValueFn = void (*)(void*, std::uint32_t, float);
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto setActionValue =
        reinterpret_cast<SetActionValueFn>(module + kSetActionValueRva);
    setActionValue(inputContext, action, value);
}

void SuppressFaceActions(void* inputContext) {
    SetActionValue(inputContext, kActionAttack, 0.0f);
    SetActionValue(inputContext, kActionDodge, 0.0f);
    SetActionValue(inputContext, kActionJump, 0.0f);
    SetActionValue(inputContext, kActionWebStrike, 0.0f);
}

void* FindHeroWeaponManager(void* hero) {
    if (g_cachedHero.load() != hero) {
        g_weaponManager = nullptr;
        g_cachedHero = hero;
    }
    if (void* cached = g_weaponManager.load()) return cached;
    if (!hero || !g_native.getComponents) return nullptr;

    PointerVector components{};
    g_native.getComponents(&components, hero);
    if (!components.begin || !components.end || components.end < components.begin) return nullptr;

    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const std::uintptr_t expected[] = {
        module + kHeroWeaponManagerVtableRva,
        module + kHeroWeaponManagerLocalVtableRva,
        module + kHeroWeaponManagerRemoteVtableRva,
    };
    const auto count = static_cast<std::size_t>(components.end - components.begin);
    for (std::size_t i = 0; i < count && i < 512; ++i) {
        void* component = components.begin[i];
        if (!component) continue;
        auto vtable = *reinterpret_cast<std::uintptr_t*>(component);
        if (std::find(std::begin(expected), std::end(expected), vtable) == std::end(expected)) continue;
        g_weaponManager = component;
        char line[160]{};
        std::snprintf(line, sizeof(line), "Found HeroWeaponManager at %p (vtable %p)",
                      component, reinterpret_cast<void*>(vtable));
        Log(line);
        return component;
    }
    return nullptr;
}

void SelectNativeSlotOnGameThread() {
    const int request = g_pendingNativeRequest.exchange(kNoNativeRequest);
    if (request == kNoNativeRequest) return;

    const int slot = request & kNativeRequestSlotMask;
    const bool fire = (request & kNativeRequestFire) != 0;
    const bool suppressFaces = (request & kNativeRequestSuppressFaces) != 0;
    if (slot < 0 || slot >= kGadgetCount || !g_native.getPlayerHero) return;

    void* manager = FindHeroWeaponManager(g_native.getPlayerHero());
    if (!manager) {
        Log("Native select failed: HeroWeaponManager was not found");
        return;
    }

    const auto slotAddress = reinterpret_cast<std::uintptr_t>(manager) +
        kWeaponSlotBase + static_cast<std::size_t>(slot) * kWeaponSlotStride;
    const auto weaponId = *reinterpret_cast<const std::uint32_t*>(slotAddress);
    if (!weaponId) {
        char line[128]{};
        std::snprintf(line, sizeof(line), "Native slot %d is empty or locked", slot + 1);
        Log(line);
        return;
    }

    using SelectWeaponAndNotifyFn = void (*)(void*, std::uint32_t);
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto selectWeaponAndNotify =
        reinterpret_cast<SelectWeaponAndNotifyFn>(module + kSelectWeaponAndNotifyRva);
    selectWeaponAndNotify(manager, weaponId);

    if (fire || suppressFaces) {
        void* inputContext = ResolveInputContext(manager);
        if (!inputContext) {
            Log("Native fire failed: input action context was not available");
        } else {
            if (suppressFaces) SuppressFaceActions(inputContext);
            if (fire) {
                g_nativeFirePulseActive = true;
                SetActionValue(inputContext, kActionUseGadget, 1.0f);
                g_nativeUseReleaseAt = GetTickCount64() + 34;
                g_nativeUseReleasePending = true;
            }
        }
    }

    char line[160]{};
    std::snprintf(line, sizeof(line), "Native selected slot %d (weapon id 0x%08X)",
                  slot + 1, weaponId);
    Log(line);
}

void ReleaseNativeUseOnGameThread() {
    g_nativeFirePulseActive = false;
    if (!g_native.getPlayerHero) return;
    void* manager = FindHeroWeaponManager(g_native.getPlayerHero());
    void* inputContext = ResolveInputContext(manager);
    if (!inputContext) return;
    SetActionValue(inputContext, kActionUseGadget, 0.0f);
    SuppressFaceActions(inputContext);
}

void SuppressControllerComboOnGameThread() {
    if (g_controllerComboHeld && g_native.getPlayerHero) {
        void* manager = FindHeroWeaponManager(g_native.getPlayerHero());
        void* inputContext = ResolveInputContext(manager);
        if (inputContext) {
            SuppressFaceActions(inputContext);
            if (!g_nativeFirePulseActive) {
                SetActionValue(inputContext, kActionUseGadget, 0.0f);
            }
        }
    }
    g_suppressionCallbackQueued = false;
}

bool QueueNativeSlot(int slot, bool fire, bool suppressFaces) {
    if (!g_native.gameMainThreadCall || slot < 0 || slot >= kGadgetCount) return false;

    const int request = slot |
        (fire ? kNativeRequestFire : 0) |
        (suppressFaces ? kNativeRequestSuppressFaces : 0);
    int expected = kNoNativeRequest;
    if (!g_pendingNativeRequest.compare_exchange_strong(expected, request)) return false;

    g_native.gameMainThreadCall(&SelectNativeSlotOnGameThread);
    return true;
}

struct XInputGamepad {
    WORD buttons;
    BYTE leftTrigger;
    BYTE rightTrigger;
    SHORT thumbLX;
    SHORT thumbLY;
    SHORT thumbRX;
    SHORT thumbRY;
};

struct XInputState {
    DWORD packetNumber;
    XInputGamepad gamepad;
};

static_assert(sizeof(XInputGamepad) == 12);
static_assert(sizeof(XInputState) == 16);

using XInputGetStateFn = DWORD (WINAPI*)(DWORD, XInputState*);

XInputGetStateFn ResolveXInputGetState() {
    constexpr const char* kModules[] = {
        "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"
    };
    for (const char* name : kModules) {
        HMODULE module = GetModuleHandleA(name);
        if (!module) module = LoadLibraryA(name);
        if (module) {
            if (const auto proc = GetProcAddress(module, "XInputGetState")) {
                return reinterpret_cast<XInputGetStateFn>(proc);
            }
        }
    }
    return nullptr;
}

void ProbeComponentsOnGameThread() {
    if (!g_running || !g_native.getPlayerHero) return;

    // This is a read-only probe and never writes to the game. The hook helper
    // validates the entity/component lookup before returning its pointer.
    void* hero = g_native.getPlayerHero();
    if (!hero) return; // The player entity is not created on every menu.

    if (!g_probeLoggedHero.exchange(true)) {
        char line[128]{};
        std::snprintf(line, sizeof(line), "Hero entity found at %p; probing gadget components", hero);
        Log(line);
    }

    const int probeLevel = g_nativeProbeLevel.load();
    if (probeLevel <= 1) {
        Log("Native probe level 1 complete (GetPlayerHero only)");
        g_probeFinished = true;
        return;
    }

    if (g_native.getComponents) {
        PointerVector components{};
        g_native.getComponents(&components, hero);
        if (components.begin && components.end && components.end >= components.begin) {
            const auto count = static_cast<std::size_t>(components.end - components.begin);
            char line[128]{};
            std::snprintf(line, sizeof(line), "Hero component enumeration returned %zu entries", count);
            Log(line);
            if (probeLevel == 2) {
                Log("Native probe level 2 complete (GetComponents count only)");
                g_probeFinished = true;
                return;
            }
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

                if (probeLevel < 4) continue;
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

    if (probeLevel == 3) {
        Log("Native probe level 3 complete (component names only)");
        g_probeFinished = true;
        return;
    }

    if (probeLevel == 4) {
        Log("Native probe level 4 complete (component vtables)");
        g_probeFinished = true;
        return;
    }

    if (!g_native.getComponentByName) {
        Log("Native probe level 5 skipped: GetComponentByName export unavailable");
        g_probeFinished = true;
        return;
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

    char line[128]{};
    std::snprintf(line, sizeof(line), "Native probe armed at level %d; waiting for the player entity",
                  g_nativeProbeLevel.load());
    Log(line);
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
    config.nativeDirectSelect = ReadBool(L"QuickGadgets", L"NativeDirectSelect", config.nativeDirectSelect, path);
    config.nativeDirectFire = ReadBool(L"QuickGadgets", L"NativeDirectFire", config.nativeDirectFire, path);
    config.controllerEnabled = ReadBool(L"Controller", L"Enabled", config.controllerEnabled, path);
    config.controllerIndex = std::clamp(
        ReadInt(L"Controller", L"Index", config.controllerIndex, path), -1, 3);
    config.controllerSlots[0] = std::clamp(ReadInt(L"Controller", L"A", config.controllerSlots[0] + 1, path) - 1, 0, 7);
    config.controllerSlots[1] = std::clamp(ReadInt(L"Controller", L"B", config.controllerSlots[1] + 1, path) - 1, 0, 7);
    config.controllerSlots[2] = std::clamp(ReadInt(L"Controller", L"X", config.controllerSlots[2] + 1, path) - 1, 0, 7);
    config.controllerSlots[3] = std::clamp(ReadInt(L"Controller", L"Y", config.controllerSlots[3] + 1, path) - 1, 0, 7);
    config.nativeProbe = ReadBool(L"QuickGadgets", L"NativeProbe", config.nativeProbe, path);
    config.nativeProbeLevel = std::clamp(ReadInt(L"QuickGadgets", L"NativeProbeLevel",
                                                 config.nativeProbeLevel, path), 1, 5);
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
    bool nativeDirectSelect = false;
    bool nativeProbe = false;
    int nativeProbeLevel = 1;
    {
        std::lock_guard lock(g_configMutex);
        g_config = LoadConfig();
        g_enabled = g_config.enabled;
        keyboardWheelFallback = g_config.keyboardWheelFallback;
        nativeDirectSelect = g_config.nativeDirectSelect;
        nativeProbe = g_config.nativeProbe;
        nativeProbeLevel = g_config.nativeProbeLevel;
    }
    Log(keyboardWheelFallback
        ? "Quick Gadgets enabled. Keyboard wheel fallback is ON."
        : "Quick Gadgets enabled. Native route only; keyboard wheel fallback is OFF.");
    if (nativeDirectSelect) {
        if (!g_native.Resolve() || !g_native.getComponents) {
            nativeDirectSelect = false;
            Log("Native direct select unavailable: required Script Hook exports are missing");
        } else if (!ValidateNativeLayout()) {
            nativeDirectSelect = false;
            Log("Native direct select disabled: Spider-Man.exe layout does not match 4.0630.0.0");
        } else {
            Log("Native direct select armed for Spider-Man.exe 4.0630.0.0");
        }
    }
    if (nativeProbe) {
        g_nativeProbeLevel = nativeProbeLevel;
        std::thread(NativeProbeWorker).detach();
    } else {
        Log("Native probe is disabled; no game component calls will be made.");
    }

    bool modifierWasDown = false;
    bool modifierUsed = false;
    auto modifierDownAt = std::chrono::steady_clock::now();
    const auto xinputGetState = ResolveXInputGetState();
    if (xinputGetState) {
        Log("XInput controller polling is available");
    } else {
        Log("XInput controller polling is unavailable; keyboard triggers remain available");
    }
    WORD previousControllerButtons = 0;
    int activeControllerIndex = -1;

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

        if (nativeDirectSelect && g_enabled) {
            if (Down(config.modifier)) {
                for (int target = 0; target < kGadgetCount; ++target) {
                    if (Pressed(config.slotKeys[target])) {
                        QueueNativeSlot(target, config.nativeDirectFire, false);
                        break;
                    }
                }
            }

            if (config.controllerEnabled && xinputGetState) {
                XInputState state{};
                int candidateIndex = config.controllerIndex;
                DWORD controllerResult = ERROR_DEVICE_NOT_CONNECTED;
                if (candidateIndex < 0) {
                    if (activeControllerIndex >= 0) {
                        candidateIndex = activeControllerIndex;
                        controllerResult = xinputGetState(
                            static_cast<DWORD>(candidateIndex), &state);
                    }
                    if (controllerResult != ERROR_SUCCESS) {
                        activeControllerIndex = -1;
                        for (DWORD index = 0; index < 4; ++index) {
                            if (xinputGetState(index, &state) == ERROR_SUCCESS) {
                                candidateIndex = static_cast<int>(index);
                                controllerResult = ERROR_SUCCESS;
                                break;
                            }
                        }
                    }
                } else {
                    controllerResult = xinputGetState(
                        static_cast<DWORD>(candidateIndex), &state);
                }

                const bool connected = candidateIndex >= 0 &&
                    controllerResult == ERROR_SUCCESS;
                if (connected) {
                    if (activeControllerIndex != candidateIndex) {
                        activeControllerIndex = candidateIndex;
                        previousControllerButtons = 0;
                        char line[96]{};
                        std::snprintf(line, sizeof(line), "Using XInput controller index %d",
                                      activeControllerIndex);
                        Log(line);
                    }
                    constexpr WORD kRightShoulder = 0x0200;
                    constexpr WORD kFaces[] = { 0x1000, 0x2000, 0x4000, 0x8000 }; // A B X Y
                    constexpr WORD kFaceMask = 0xF000;
                    const WORD buttons = state.gamepad.buttons;
                    const bool comboHeld = (buttons & kRightShoulder) &&
                        (buttons & kFaceMask);
                    g_controllerComboHeld = comboHeld;
                    if (comboHeld) {
                        if (!g_suppressionCallbackQueued.exchange(true)) {
                            g_native.gameMainThreadCall(&SuppressControllerComboOnGameThread);
                        }
                        for (int face = 0; face < 4; ++face) {
                            if ((buttons & kFaces[face]) &&
                                !(previousControllerButtons & kFaces[face])) {
                                QueueNativeSlot(config.controllerSlots[face],
                                                config.nativeDirectFire, true);
                                break;
                            }
                        }
                    }
                    previousControllerButtons = buttons;
                } else {
                    activeControllerIndex = -1;
                    g_controllerComboHeld = false;
                    previousControllerButtons = 0;
                }
            }
        }

        if (g_nativeUseReleasePending &&
            GetTickCount64() >= g_nativeUseReleaseAt.load()) {
            if (g_nativeUseReleasePending.exchange(false) &&
                g_native.gameMainThreadCall) {
                g_native.gameMainThreadCall(&ReleaseNativeUseOnGameThread);
            }
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
