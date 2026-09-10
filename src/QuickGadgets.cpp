#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <intrin.h>
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
    WORD controllerModifier = 0x0100; // XINPUT_GAMEPAD_LEFT_SHOULDER.
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
std::atomic_bool g_gadgetLayoutLogged = false;
std::atomic_int g_nativeProbeLevel = 1;
// Keep a queued request coherent across the worker and game threads. Separate
// atomics allowed the callback to observe a new slot before its fire/suppress
// flags had been published.
constexpr int kNoNativeRequest = -1;
constexpr int kNativeRequestSlotMask = 0xFF;
constexpr int kNativeRequestFire = 0x100;
constexpr int kNativeRequestSuppressFaces = 0x200;
std::atomic_int g_pendingNativeRequest = kNoNativeRequest;
std::atomic_bool g_controllerComboHeld = false;
std::atomic_bool g_controllerModifierHeld = false;
std::atomic_bool g_nativeFireIssuedForCombo = false;
std::atomic_bool g_suppressionCallbackQueued = false;
std::atomic_bool g_physicalUseSuppressedObserved = false;
std::atomic_bool g_forceUseGadgetPending = false;
std::atomic_bool g_forceUseGadgetObserved = false;
std::atomic_ullong g_forceUseGadgetDeadline = 0;
std::atomic<void*> g_forceUseGadgetContext = nullptr;
std::atomic_bool g_delayedNativeFirePending = false;
std::atomic_ullong g_delayedNativeFireAt = 0;
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
constexpr std::uintptr_t kSetActiveWeaponRva = 0x2161A10;
constexpr std::uintptr_t kResolveAssetHandleRva = 0x15A0560;
constexpr std::uintptr_t kResolveHandleRva = 0x16798F0;
constexpr std::uintptr_t kConsumeActionRva = 0x09097C0;
constexpr std::uintptr_t kTriggerActionRva = 0x09098C0;
constexpr std::uintptr_t kQueryActionRva = 0x0909320;
constexpr std::uintptr_t kDirectUseGadgetReturnRva = 0x08A2AED;
constexpr std::uintptr_t kQueryActionWindowRva = 0x09095E0;
// Verified from a genuine Impact Web shot on 4.0630.0.0. This is the gameplay
// action reader that writes the four UseGadget states consumed by the active
// weapon. Earlier callers at 0x8A2AE3 and 0x7943AC only drive controller/UI
// layers and can consume a synthetic edge without firing the selected gadget.
constexpr std::uintptr_t kDirectUseGadgetWindowReturnRva = 0x0E3D0CE;
constexpr std::uintptr_t kQueryActionFlagRva = 0x0909520;
constexpr std::uintptr_t kControllerUseGadgetFlagReturnRva = 0x07943AC;
constexpr std::size_t kInputContextHandleOffset = 0x78C;
constexpr std::size_t kWeaponInventoryBase = 0x1A8;
constexpr std::size_t kWeaponInventoryStride = 0x18;
constexpr std::size_t kWeaponInventoryCountOffset = 0x628;
constexpr std::size_t kWeaponInventoryHandleOffset = 0x0C;
constexpr std::size_t kWeaponInventoryIdOffset = 0x10;
constexpr std::size_t kWeaponAssetNameOffset = 0xB0;
constexpr std::size_t kActiveWeaponSlot0Offset = 0x6C;
constexpr std::size_t kGadgetOverrideOffset = 0x790;

constexpr std::uint32_t kActionAttack = 0x2B24146B;
constexpr std::uint32_t kActionDodge = 0x7CA907FC;
constexpr std::uint32_t kActionJump = 0xD69724B0;
constexpr std::uint32_t kActionWebStrike = 0x775E96E1;
constexpr std::uint32_t kActionUseGadget = 0x9D43C80F;
constexpr std::uint32_t kActionGadgetSelect = 0x519ACBBE;

using QueryActionFn = bool (*)(void*, std::uint32_t, float, bool, bool);
QueryActionFn g_originalQueryAction = nullptr;
void* g_queryActionTrampoline = nullptr;
std::array<std::uint8_t, 15> g_queryActionOriginalBytes{};
bool g_queryActionHookInstalled = false;
using QueryActionWindowFn = bool (*)(void*, std::uint32_t, float, float, bool);
QueryActionWindowFn g_originalQueryActionWindow = nullptr;
void* g_queryActionWindowTrampoline = nullptr;
std::array<std::uint8_t, 19> g_queryActionWindowOriginalBytes{};
bool g_queryActionWindowHookInstalled = false;
using QueryActionFlagFn = bool (*)(void*, std::uint32_t, bool);
QueryActionFlagFn g_originalQueryActionFlag = nullptr;
void* g_queryActionFlagTrampoline = nullptr;
std::array<std::uint8_t, 17> g_queryActionFlagOriginalBytes{};
bool g_queryActionFlagHookInstalled = false;

bool HookedQueryAction(void* inputContext, std::uint32_t action, float threshold,
                       bool allowHeld, bool useTimeWindow) {
    return g_originalQueryAction
        ? g_originalQueryAction(inputContext, action, threshold, allowHeld, useTimeWindow)
        : false;
}

bool HookedQueryActionWindow(void* inputContext, std::uint32_t action,
                             float minimum, float maximum, bool allowHeld) {
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto returnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (action == kActionUseGadget &&
        returnAddress == module + kDirectUseGadgetWindowReturnRva &&
        GetTickCount64() <= g_forceUseGadgetDeadline.load() &&
        g_forceUseGadgetPending.exchange(false)) {
        g_forceUseGadgetObserved = true;
        return true;
    }
    return g_originalQueryActionWindow
        ? g_originalQueryActionWindow(inputContext, action, minimum, maximum, allowHeld)
        : false;
}

bool HookedQueryActionFlag(void* inputContext, std::uint32_t action, bool released) {
    if (action == kActionUseGadget) {
        const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto returnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        if (returnAddress == module + kControllerUseGadgetFlagReturnRva &&
            g_controllerModifierHeld.load()) {
        // While the shortcut modifier is held, block an ordinary R1
        // Web-Shooter edge so it cannot mask the later gadget projectile.
            g_physicalUseSuppressedObserved = true;
            return false;
        }
    }
    return g_originalQueryActionFlag
        ? g_originalQueryActionFlag(inputContext, action, released)
        : false;
}

bool InstallQueryActionHook() {
    if (g_queryActionHookInstalled) return true;
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* target = reinterpret_cast<std::uint8_t*>(module + kQueryActionRva);
    constexpr std::uint8_t kExpectedPrologue[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
        0xEC, 0x30, 0x0F, 0x29, 0x74, 0x24, 0x20
    };
    if (std::memcmp(target, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        return false;
    }

    auto* trampoline = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;
    std::memcpy(g_queryActionOriginalBytes.data(), target,
                g_queryActionOriginalBytes.size());
    std::memcpy(trampoline, target, g_queryActionOriginalBytes.size());

    auto writeAbsoluteJump = [](std::uint8_t* address, const void* destination) {
        address[0] = 0xFF;
        address[1] = 0x25;
        *reinterpret_cast<std::uint32_t*>(address + 2) = 0;
        *reinterpret_cast<std::uintptr_t*>(address + 6) =
            reinterpret_cast<std::uintptr_t>(destination);
    };
    writeAbsoluteJump(trampoline + g_queryActionOriginalBytes.size(),
                      target + g_queryActionOriginalBytes.size());

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, g_queryActionOriginalBytes.size(),
                        PAGE_EXECUTE_READWRITE, &oldProtect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    g_queryActionTrampoline = trampoline;
    g_originalQueryAction = reinterpret_cast<QueryActionFn>(trampoline);
    writeAbsoluteJump(target, reinterpret_cast<const void*>(&HookedQueryAction));
    target[14] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), target,
                          g_queryActionOriginalBytes.size());
    DWORD ignored = 0;
    VirtualProtect(target, g_queryActionOriginalBytes.size(), oldProtect, &ignored);

    g_queryActionHookInstalled = true;
    return true;
}

bool InstallQueryActionWindowHook() {
    if (g_queryActionWindowHookInstalled) return true;
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* target = reinterpret_cast<std::uint8_t*>(module + kQueryActionWindowRva);
    constexpr std::uint8_t kExpectedPrologue[] = {
        0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x0F, 0x29, 0x74, 0x24,
        0x30, 0x0F, 0x28, 0xF3, 0x0F, 0x29, 0x7C, 0x24, 0x20
    };
    if (std::memcmp(target, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        return false;
    }

    auto* trampoline = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;
    std::memcpy(g_queryActionWindowOriginalBytes.data(), target,
                g_queryActionWindowOriginalBytes.size());
    std::memcpy(trampoline, target, g_queryActionWindowOriginalBytes.size());

    auto writeAbsoluteJump = [](std::uint8_t* address, const void* destination) {
        address[0] = 0xFF;
        address[1] = 0x25;
        *reinterpret_cast<std::uint32_t*>(address + 2) = 0;
        *reinterpret_cast<std::uintptr_t*>(address + 6) =
            reinterpret_cast<std::uintptr_t>(destination);
    };
    writeAbsoluteJump(trampoline + g_queryActionWindowOriginalBytes.size(),
                      target + g_queryActionWindowOriginalBytes.size());

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, g_queryActionWindowOriginalBytes.size(),
                        PAGE_EXECUTE_READWRITE, &oldProtect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    g_queryActionWindowTrampoline = trampoline;
    g_originalQueryActionWindow = reinterpret_cast<QueryActionWindowFn>(trampoline);
    writeAbsoluteJump(target, reinterpret_cast<const void*>(&HookedQueryActionWindow));
    for (std::size_t i = 14; i < g_queryActionWindowOriginalBytes.size(); ++i) {
        target[i] = 0x90;
    }
    FlushInstructionCache(GetCurrentProcess(), target,
                          g_queryActionWindowOriginalBytes.size());
    DWORD ignored = 0;
    VirtualProtect(target, g_queryActionWindowOriginalBytes.size(), oldProtect, &ignored);
    g_queryActionWindowHookInstalled = true;
    return true;
}

bool InstallQueryActionFlagHook() {
    if (g_queryActionFlagHookInstalled) return true;
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* target = reinterpret_cast<std::uint8_t*>(module + kQueryActionFlagRva);
    constexpr std::uint8_t kExpectedPrologue[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC,
        0x20, 0x41, 0x0F, 0xB6, 0xF8, 0x48, 0x8B, 0xD9
    };
    if (std::memcmp(target, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        return false;
    }

    auto* trampoline = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;
    std::memcpy(g_queryActionFlagOriginalBytes.data(), target,
                g_queryActionFlagOriginalBytes.size());
    std::memcpy(trampoline, target, g_queryActionFlagOriginalBytes.size());

    auto writeAbsoluteJump = [](std::uint8_t* address, const void* destination) {
        address[0] = 0xFF;
        address[1] = 0x25;
        *reinterpret_cast<std::uint32_t*>(address + 2) = 0;
        *reinterpret_cast<std::uintptr_t*>(address + 6) =
            reinterpret_cast<std::uintptr_t>(destination);
    };
    writeAbsoluteJump(trampoline + g_queryActionFlagOriginalBytes.size(),
                      target + g_queryActionFlagOriginalBytes.size());

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, g_queryActionFlagOriginalBytes.size(),
                        PAGE_EXECUTE_READWRITE, &oldProtect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    g_queryActionFlagTrampoline = trampoline;
    g_originalQueryActionFlag = reinterpret_cast<QueryActionFlagFn>(trampoline);
    writeAbsoluteJump(target, reinterpret_cast<const void*>(&HookedQueryActionFlag));
    for (std::size_t i = 14; i < g_queryActionFlagOriginalBytes.size(); ++i) {
        target[i] = 0x90;
    }
    FlushInstructionCache(GetCurrentProcess(), target,
                          g_queryActionFlagOriginalBytes.size());
    DWORD ignored = 0;
    VirtualProtect(target, g_queryActionFlagOriginalBytes.size(), oldProtect, &ignored);
    g_queryActionFlagHookInstalled = true;
    return true;
}

void RemoveQueryActionHook() {
    if (!g_queryActionHookInstalled) return;
    g_forceUseGadgetPending = false;
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* target = reinterpret_cast<std::uint8_t*>(module + kQueryActionRva);
    DWORD oldProtect = 0;
    if (VirtualProtect(target, g_queryActionOriginalBytes.size(),
                       PAGE_EXECUTE_READWRITE, &oldProtect)) {
        std::memcpy(target, g_queryActionOriginalBytes.data(),
                    g_queryActionOriginalBytes.size());
        FlushInstructionCache(GetCurrentProcess(), target,
                              g_queryActionOriginalBytes.size());
        DWORD ignored = 0;
        VirtualProtect(target, g_queryActionOriginalBytes.size(), oldProtect, &ignored);
    }
    // Do not free the trampoline here: an engine thread may have entered it
    // immediately before restoration. The allocation is reclaimed at process exit.
    g_queryActionHookInstalled = false;
}

void RemoveQueryActionWindowHook() {
    if (!g_queryActionWindowHookInstalled) return;
    g_forceUseGadgetPending = false;
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* target = reinterpret_cast<std::uint8_t*>(module + kQueryActionWindowRva);
    DWORD oldProtect = 0;
    if (VirtualProtect(target, g_queryActionWindowOriginalBytes.size(),
                       PAGE_EXECUTE_READWRITE, &oldProtect)) {
        std::memcpy(target, g_queryActionWindowOriginalBytes.data(),
                    g_queryActionWindowOriginalBytes.size());
        FlushInstructionCache(GetCurrentProcess(), target,
                              g_queryActionWindowOriginalBytes.size());
        DWORD ignored = 0;
        VirtualProtect(target, g_queryActionWindowOriginalBytes.size(), oldProtect, &ignored);
    }
    g_queryActionWindowHookInstalled = false;
}

void RemoveQueryActionFlagHook() {
    if (!g_queryActionFlagHookInstalled) return;
    g_forceUseGadgetPending = false;
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* target = reinterpret_cast<std::uint8_t*>(module + kQueryActionFlagRva);
    DWORD oldProtect = 0;
    if (VirtualProtect(target, g_queryActionFlagOriginalBytes.size(),
                       PAGE_EXECUTE_READWRITE, &oldProtect)) {
        std::memcpy(target, g_queryActionFlagOriginalBytes.data(),
                    g_queryActionFlagOriginalBytes.size());
        FlushInstructionCache(GetCurrentProcess(), target,
                              g_queryActionFlagOriginalBytes.size());
        DWORD ignored = 0;
        VirtualProtect(target, g_queryActionFlagOriginalBytes.size(), oldProtect, &ignored);
    }
    g_queryActionFlagHookInstalled = false;
}

bool ValidateNativeLayout() {
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!module) return false;
    constexpr std::uint8_t kExpectedSelectorBytes[] = {
        0x85, 0xD2, 0x0F, 0x84, 0x0C, 0x01, 0x00, 0x00
    };
    constexpr std::uint8_t kExpectedNotifyBytes[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x81
    };
    constexpr std::uint8_t kExpectedActiveWeaponSetterBytes[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
        0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30
    };
    constexpr std::uint8_t kExpectedResolverBytes[] = {
        0x8B, 0x11, 0x8B, 0xCA, 0xC1, 0xE9, 0x14, 0x85
    };
    constexpr std::uint8_t kExpectedAssetResolverBytes[] = {
        0x44, 0x8B, 0x01, 0x41, 0x8B, 0xD0, 0xC1, 0xEA
    };
    constexpr std::uint8_t kExpectedActionSetterBytes[] = {
        0x85, 0xD2, 0x0F, 0x84, 0xA0, 0x00, 0x00, 0x00
    };
    return std::memcmp(reinterpret_cast<const void*>(module + kSelectWeaponByIdRva),
                       kExpectedSelectorBytes, sizeof(kExpectedSelectorBytes)) == 0 &&
        std::memcmp(reinterpret_cast<const void*>(module + kSelectWeaponAndNotifyRva),
                    kExpectedNotifyBytes, sizeof(kExpectedNotifyBytes)) == 0 &&
        std::memcmp(reinterpret_cast<const void*>(module + kSetActiveWeaponRva),
                    kExpectedActiveWeaponSetterBytes,
                    sizeof(kExpectedActiveWeaponSetterBytes)) == 0 &&
        std::memcmp(reinterpret_cast<const void*>(module + kResolveHandleRva),
                    kExpectedResolverBytes, sizeof(kExpectedResolverBytes)) == 0 &&
        std::memcmp(reinterpret_cast<const void*>(module + kResolveAssetHandleRva),
                    kExpectedAssetResolverBytes, sizeof(kExpectedAssetResolverBytes)) == 0 &&
        std::memcmp(reinterpret_cast<const void*>(module + kTriggerActionRva),
                    kExpectedActionSetterBytes, sizeof(kExpectedActionSetterBytes)) == 0;
}

const char* ResolveWeaponAssetName(std::uintptr_t inventoryEntry) {
    using ResolveAssetHandleFn = void* (*)(void*);
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto resolveAssetHandle =
        reinterpret_cast<ResolveAssetHandleFn>(module + kResolveAssetHandleRva);
    void* asset = resolveAssetHandle(reinterpret_cast<void*>(
        inventoryEntry + kWeaponInventoryHandleOffset));
    if (!asset) return nullptr;
    return *reinterpret_cast<const char* const*>(
        reinterpret_cast<std::uintptr_t>(asset) + kWeaponAssetNameOffset);
}

std::uint32_t FindGadgetWeaponId(void* manager, int slot, const char** matchedName) {
    if (!manager || slot < 0 || slot >= kGadgetCount) return 0;
    constexpr std::array<std::array<const char*, 3>, kGadgetCount> kNames{{
        {{"WebShooter", nullptr, nullptr}},
        {{"ImpactWeb", nullptr, nullptr}},
        {{"SpiderDrone", nullptr, nullptr}},
        {{"ElectricWeb", nullptr, nullptr}},
        {{"WebBomb", nullptr, nullptr}},
        {{"TripMine", "WebTripMine", "GadgetTripmine"}},
        {{"ShockerBlast", "ConcussiveBlast", "GadgetBlast"}},
        {{"AirLauncher", "SuspensionMatrix", "GadgetMatrix"}},
    }};

    const auto base = reinterpret_cast<std::uintptr_t>(manager);
    const auto count = *reinterpret_cast<const std::uint32_t*>(
        base + kWeaponInventoryCountOffset);
    if (count > 256) return 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto entry = base + kWeaponInventoryBase +
            static_cast<std::size_t>(index) * kWeaponInventoryStride;
        const char* name = ResolveWeaponAssetName(entry);
        if (!name) continue;
        for (const char* candidate : kNames[slot]) {
            if (candidate && _stricmp(name, candidate) == 0) {
                if (matchedName) *matchedName = name;
                return *reinterpret_cast<const std::uint32_t*>(
                    entry + kWeaponInventoryIdOffset);
            }
        }
    }
    return 0;
}

void* ResolveInputContext(void* manager) {
    if (!manager) return nullptr;
    using ResolveHandleFn = void* (*)(void*);
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto resolveHandle = reinterpret_cast<ResolveHandleFn>(module + kResolveHandleRva);
    return resolveHandle(reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(manager) + kInputContextHandleOffset));
}

void ConsumeAction(void* inputContext, std::uint32_t action) {
    if (!inputContext) return;
    using ConsumeActionFn = void (*)(void*, std::uint32_t, float);
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto consumeAction =
        reinterpret_cast<ConsumeActionFn>(module + kConsumeActionRva);
    consumeAction(inputContext, action, 0.0f);
}

void TriggerAction(void* inputContext, std::uint32_t action) {
    if (!inputContext) return;
    using TriggerActionFn = void (*)(void*, std::uint32_t, float);
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto triggerAction =
        reinterpret_cast<TriggerActionFn>(module + kTriggerActionRva);
    // The third argument is a time offset, not an analog action value. A zero
    // offset stamps the action at the current input time. Passing 1.0 here
    // backdates the event by one second, outside UseGadget's press window.
    triggerAction(inputContext, action, 0.0f);
}

void SuppressFaceActions(void* inputContext) {
    ConsumeAction(inputContext, kActionAttack);
    ConsumeAction(inputContext, kActionDodge);
    ConsumeAction(inputContext, kActionJump);
    ConsumeAction(inputContext, kActionWebStrike);
}

void* FindHeroWeaponManager(void* hero);

void ArmNativeFireOnGameThread() {
    if (!g_running || !g_native.getPlayerHero) return;
    void* manager = FindHeroWeaponManager(g_native.getPlayerHero());
    void* inputContext = ResolveInputContext(manager);
    if (!inputContext) {
        Log("Delayed native fire failed: input action context was not available");
        return;
    }

    if (g_queryActionHookInstalled || g_queryActionWindowHookInstalled ||
        g_queryActionFlagHookInstalled) {
        g_forceUseGadgetContext = inputContext;
        g_forceUseGadgetDeadline = GetTickCount64() + 250;
        g_forceUseGadgetPending = true;
    } else {
        TriggerAction(inputContext, kActionUseGadget);
    }
    Log("Native gadget-use edge armed after equipment transition settled");
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

void LogGadgetLayout(void* hero, void* manager) {
    if (!hero || !manager || g_gadgetLayoutLogged.exchange(true)) return;

    const auto base = reinterpret_cast<std::uintptr_t>(manager);
    const auto inventoryCount = *reinterpret_cast<const std::uint32_t*>(
        base + kWeaponInventoryCountOffset);
    char line[256]{};
    std::snprintf(line, sizeof(line),
                  "HeroWeaponManager active equip ids: %08X %08X %08X; inventory count: %u",
                  *reinterpret_cast<const std::uint32_t*>(base + 0x6C),
                  *reinterpret_cast<const std::uint32_t*>(base + 0x94),
                  *reinterpret_cast<const std::uint32_t*>(base + 0xBC),
                  inventoryCount);
    Log(line);

    if (inventoryCount <= 256) {
        for (std::uint32_t index = 0; index < inventoryCount; ++index) {
            const auto entry = base + kWeaponInventoryBase +
                static_cast<std::size_t>(index) * kWeaponInventoryStride;
            const auto field0 = *reinterpret_cast<const std::uint64_t*>(entry);
            const auto field8 = *reinterpret_cast<const std::uint64_t*>(entry + 8);
            const auto weaponId = *reinterpret_cast<const std::uint32_t*>(
                entry + kWeaponInventoryIdOffset);
            const auto field14 = *reinterpret_cast<const std::uint32_t*>(entry + 0x14);
            std::snprintf(line, sizeof(line),
                          "Weapon inventory[%03u]: %016llX %016llX id=%08X tail=%08X",
                          index,
                          static_cast<unsigned long long>(field0),
                          static_cast<unsigned long long>(field8),
                          weaponId, field14);
            Log(line);
        }
    } else {
        Log("Weapon inventory count rejected as implausible");
    }

    constexpr const char* kGadgetComponents[] = {
        "GadgetWheel", "kGadgetWheel",
        "GadgetHolster", "kGadgetHolster",
        "GadgetItemAmmoManager", "AVGadgetItemAmmoManager",
        "HeroGadgetConfig", "AVHeroGadgetConfig",
        "GadgetAimLayer", "kGadgetAimLayer",
    };
    for (const char* candidate : kGadgetComponents) {
        EngineString name(candidate);
        void* component = g_native.getComponentByName(hero, &name);
        if (!component) continue;
        const char* resolvedName = g_native.getComponentName
            ? g_native.getComponentName(component)
            : nullptr;
        std::snprintf(line, sizeof(line),
                      "Gadget component %-24s -> %p (engine name: %s, vtable: %p)",
                      candidate, component,
                      resolvedName ? resolvedName : "<unknown>",
                      *reinterpret_cast<void**>(component));
        Log(line);
    }

    std::snprintf(line, sizeof(line),
                  "Manager gadget state: +790=%08X +798=%08X +79C=%08X +7B4=%08X +7B8=%08X +7BC=%08X",
                  *reinterpret_cast<const std::uint32_t*>(base + 0x790),
                  *reinterpret_cast<const std::uint32_t*>(base + 0x798),
                  *reinterpret_cast<const std::uint32_t*>(base + 0x79C),
                  *reinterpret_cast<const std::uint32_t*>(base + 0x7B4),
                  *reinterpret_cast<const std::uint32_t*>(base + 0x7B8),
                  *reinterpret_cast<const std::uint32_t*>(base + 0x7BC));
    Log(line);
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

    if (g_nativeProbeLevel.load() >= 2) {
        LogGadgetLayout(g_native.getPlayerHero(), manager);
    }

    const char* gadgetName = nullptr;
    const std::uint32_t weaponId = FindGadgetWeaponId(manager, slot, &gadgetName);
    if (!weaponId) {
        char line[160]{};
        std::snprintf(line, sizeof(line),
                      "Gadget slot %d is not present in the loaded weapon inventory",
                      slot + 1);
        Log(line);
        return;
    }

    using SetActiveWeaponFn = bool (*)(void*, std::uint32_t, std::uint32_t, bool);
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto setActiveWeapon =
        reinterpret_cast<SetActiveWeaponFn>(module + kSetActiveWeaponRva);

    // Use the generic equipment manager transition used by the wheel. It
    // deactivates the prior weapon object, updates slot 0, activates the new
    // object, and dispatches the HeroWeaponManager callbacks. Writing +6C and
    // calling the notification wrapper only changed bookkeeping/UI state and
    // left the gameplay fire handler attached to Web Shooter.
    const auto managerAddress = reinterpret_cast<std::uintptr_t>(manager);
    const std::uint32_t previousWeaponId = *reinterpret_cast<std::uint32_t*>(
        managerAddress + kActiveWeaponSlot0Offset);

    // Earlier diagnostic builds could leave +6C claiming Impact Web while the
    // live weapon object was still Web Shooter. Force a real transition away
    // and back when the requested ID already occupies the slot.
    bool repairedStaleSelection = false;
    if (previousWeaponId == weaponId && slot != WebShooter) {
        const std::uint32_t webShooterId = FindGadgetWeaponId(
            manager, WebShooter, nullptr);
        if (webShooterId && webShooterId != weaponId) {
            repairedStaleSelection = setActiveWeapon(
                manager, webShooterId, 0, true);
        }
    }
    const bool selected = setActiveWeapon(manager, weaponId, 0, true);

    if (!selected) {
        char line[192]{};
        std::snprintf(line, sizeof(line),
                      "Native equipment transition rejected slot %d (%s, weapon id 0x%08X)",
                      slot + 1, gadgetName ? gadgetName : "<unknown>", weaponId);
        Log(line);
        return;
    }

    if (fire || suppressFaces) {
        void* inputContext = ResolveInputContext(manager);
        if (!inputContext) {
            Log("Native fire failed: input action context was not available");
        } else {
            if (suppressFaces) SuppressFaceActions(inputContext);
            if (fire) {
                g_nativeFireIssuedForCombo = true;
                // The equipment setter dispatches activation events. Let the
                // game process several frames before exposing the synthetic
                // press, otherwise the fire handler still owns Web Shooter.
                g_delayedNativeFireAt = GetTickCount64() + 75;
                g_delayedNativeFirePending = true;
            }
        }
    }

    char line[192]{};
    std::snprintf(line, sizeof(line),
                  "Native transitioned slot %d (%s, weapon id 0x%08X, previous 0x%08X)%s%s",
                  slot + 1, gadgetName ? gadgetName : "<unknown>", weaponId,
                  previousWeaponId,
                  repairedStaleSelection ? ", repaired stale active id" : "",
                  fire ? " and queued delayed UseGadget" : "");
    Log(line);
}

void SuppressControllerComboOnGameThread() {
    if (g_controllerComboHeld && g_native.getPlayerHero) {
        void* manager = FindHeroWeaponManager(g_native.getPlayerHero());
        void* inputContext = ResolveInputContext(manager);
        if (inputContext) {
            SuppressFaceActions(inputContext);
            // Consume the physical RB action until the shortcut has selected
            // its gadget. Once TriggerAction stamps the synthetic press, do
            // not erase that press with a later suppression callback.
            if (!g_nativeFireIssuedForCombo) {
                ConsumeAction(inputContext, kActionUseGadget);
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
    config.controllerModifier = static_cast<WORD>(ReadInt(
        L"Controller", L"ModifierButton", config.controllerModifier, path));
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
            if (g_config.nativeDirectFire) {
                const bool pressHook = InstallQueryActionHook();
                const bool windowHook = InstallQueryActionWindowHook();
                const bool flagHook = InstallQueryActionFlagHook();
                if (pressHook && windowHook && flagHook) {
                    Log("Native UseGadget gameplay query hooks armed");
                } else {
                    Log("One or more native UseGadget query hooks are unavailable");
                }
            }
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
        if (g_delayedNativeFirePending &&
            GetTickCount64() >= g_delayedNativeFireAt.load() &&
            g_delayedNativeFirePending.exchange(false)) {
            g_native.gameMainThreadCall(&ArmNativeFireOnGameThread);
        }
        if (g_forceUseGadgetObserved.exchange(false)) {
            Log("Native gadget-use gameplay query intercepted");
        }
        if (g_physicalUseSuppressedObserved.exchange(false)) {
            Log("Physical R1 Web-Shooter action suppressed while shortcut modifier is held");
        }
        if (g_forceUseGadgetPending &&
            GetTickCount64() > g_forceUseGadgetDeadline.load() &&
            g_forceUseGadgetPending.exchange(false)) {
            Log("Native UseGadget query window expired before gameplay consumed it");
        }

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
                    constexpr WORD kFaces[] = { 0x1000, 0x2000, 0x4000, 0x8000 }; // A B X Y
                    constexpr WORD kFaceMask = 0xF000;
                    const WORD buttons = state.gamepad.buttons;
                    g_controllerModifierHeld =
                        (buttons & config.controllerModifier) != 0;
                    const bool comboHeld = (buttons & config.controllerModifier) &&
                        (buttons & kFaceMask);
                    g_controllerComboHeld = comboHeld;
                    if (!comboHeld) g_nativeFireIssuedForCombo = false;
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
                    g_controllerModifierHeld = false;
                    previousControllerButtons = 0;
                }
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
    if (reason == DLL_PROCESS_DETACH) {
        g_running = false;
        g_controllerModifierHeld = false;
        g_delayedNativeFirePending = false;
        g_forceUseGadgetPending = false;
        RemoveQueryActionFlagHook();
        RemoveQueryActionWindowHook();
        RemoveQueryActionHook();
    }
    return TRUE;
}

// The current MSMR community script loader invokes this export after loading a
// script package. Keep the worker outside DllMain so the loader lock is never held.
extern "C" __declspec(dllexport) void script_enable() {
    WriteEntryMarker("script_enable called\r\n");
    StartWorker();
}
