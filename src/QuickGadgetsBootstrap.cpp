#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace {

// Marvel's Spider-Man Remastered 4.0630.0.0. Overstrike loads this DLL while
// the game parses -scripts. The hero handle remains zero until a save has
// entered gameplay, which gives us a real readiness signal instead of a fixed
// startup delay.
constexpr std::uintptr_t kHeroSystemRva = 0x5D9DD00;
constexpr std::uintptr_t kHeroHandleOffset = 0x1C;

std::atomic_bool g_started = false;

std::wstring ModuleDirectory() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ModuleDirectory), &module)) {
        return L".";
    }

    std::array<wchar_t, MAX_PATH> path{};
    const DWORD length = GetModuleFileNameW(
        module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return L".";

    std::wstring directory(path.data(), length);
    const auto slash = directory.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : directory.substr(0, slash);
}

void Log(const wchar_t* message) {
    const std::wstring path = ModuleDirectory() + L"\\QuickGadgets.bootstrap.log";
    const HANDLE file = CreateFileW(
        path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t line[512]{};
    const int count = swprintf_s(
        line, L"[%02u:%02u:%02u] %ls\r\n",
        time.wHour, time.wMinute, time.wSecond, message);
    if (count > 0) {
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(count * sizeof(wchar_t)),
                  &written, nullptr);
    }
    CloseHandle(file);
}

bool HeroIsReady() {
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) return false;

    __try {
        const auto handle = *reinterpret_cast<const std::uint32_t*>(
            base + kHeroSystemRva + kHeroHandleOffset);
        return handle != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

DWORD WINAPI BootstrapWorker(void*) {
    Log(L"Steam bootstrap loaded; waiting for gameplay hero");

    while (!HeroIsReady()) Sleep(100);
    Log(L"Gameplay hero detected; loading ScriptHookSMPC.dll");

    const std::wstring hookPath =
        ModuleDirectory() + L"\\..\\ScriptHookSMPC.dll";
    if (LoadLibraryW(hookPath.c_str())) {
        Log(L"Script Hook loaded; QuickGadgets will be loaded by Script Hook");
    } else {
        wchar_t message[160]{};
        swprintf_s(message, L"Failed to load ScriptHookSMPC.dll (Win32 error %lu)",
                   GetLastError());
        Log(message);
    }
    return 0;
}

void Start() {
    if (g_started.exchange(true)) return;
    const HANDLE thread = CreateThread(nullptr, 0, &BootstrapWorker,
                                       nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        g_started = false;
        Log(L"Failed to create bootstrap worker");
    }
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}

extern "C" __declspec(dllexport) void script_enable() {
    Start();
}
