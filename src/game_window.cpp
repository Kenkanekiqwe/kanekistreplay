#include "game_window.h"

#include "core.h"

#include <Dwmapi.h>
#include <Shellapi.h>
#include <TlHelp32.h>
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <string_view>

#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

namespace kanekist {
namespace {

std::wstring lowerAscii(std::wstring text) {
    std::ranges::transform(text, text.begin(), [](wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
    });
    return text;
}

bool isGameProcessName(std::wstring_view name) {
    const auto lower = lowerAscii(std::wstring(name));
    static constexpr std::wstring_view names[] = {
        L"gta5.exe", L"gta5_enhanced.exe", L"playgtav.exe",
        L"ragemp_v.exe", L"ragemp.exe", L"ragemp_game_ui.exe"
    };
    return std::ranges::any_of(names, [&](std::wstring_view candidate) {
        return lower == candidate;
    });
}

std::wstring processImageName(DWORD processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return {};
    wchar_t path[MAX_PATH]{};
    DWORD size = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        name = std::filesystem::path(path).filename().wstring();
    }
    CloseHandle(process);
    return name;
}

bool coversMonitor(HWND window, HMONITOR* monitorOut = nullptr) {
    if (!window || !IsWindow(window)) return false;
    RECT rect{};
    if (!GetWindowRect(window, &rect)) return false;
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
    if (!monitor) monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return false;
    if (monitorOut) *monitorOut = monitor;
    constexpr int slack = 4;
    return rect.left <= info.rcMonitor.left + slack &&
           rect.top <= info.rcMonitor.top + slack &&
           rect.right >= info.rcMonitor.right - slack &&
           rect.bottom >= info.rcMonitor.bottom - slack;
}

bool isCandidateWindow(HWND window) {
    if (!window || !IsWindow(window) || !IsWindowVisible(window)) return false;
    if (GetWindow(window, GW_OWNER)) return false;
    LONG ex = GetWindowLongW(window, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return false;
    RECT rect{};
    if (!GetWindowRect(window, &rect)) return false;
    return (rect.right - rect.left) >= 640 && (rect.bottom - rect.top) >= 360;
}

bool isCloaked(HWND window) {
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return cloaked != FALSE;
}

DWORD findGameProcessId() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{sizeof(entry)};
    DWORD found = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (isGameProcessName(entry.szExeFile)) {
                found = entry.th32ProcessID;
                if (lowerAscii(entry.szExeFile) == L"gta5.exe" ||
                    lowerAscii(entry.szExeFile) == L"gta5_enhanced.exe") {
                    break;
                }
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

void postAltEnter(HWND window) {
    if (!window || !IsWindow(window)) return;
    PostMessageW(window, WM_SYSKEYDOWN, VK_RETURN,
        static_cast<LPARAM>(0x20000001 | (0x1C << 16)));
    PostMessageW(window, WM_KEYDOWN, VK_RETURN,
        static_cast<LPARAM>(0x00000001 | (0x1C << 16)));
    PostMessageW(window, WM_SYSKEYUP, VK_RETURN,
        static_cast<LPARAM>(0xE0000001 | (0x1C << 16)));
    PostMessageW(window, WM_KEYUP, VK_RETURN,
        static_cast<LPARAM>(0xC0000001 | (0x1C << 16)));
}

void sendAltEnter() {
    INPUT inputs[4]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_MENU;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = VK_RETURN;
    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = VK_RETURN;
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_MENU;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, inputs, sizeof(INPUT));
}

} // namespace

bool isD3dExclusiveFullscreen() noexcept {
    QUERY_USER_NOTIFICATION_STATE state{};
    if (FAILED(SHQueryUserNotificationState(&state))) return false;
    return state == QUNS_RUNNING_D3D_FULL_SCREEN;
}

GameTarget resolveGameTarget() {
    GameTarget target;
    if (HWND gta = FindWindowW(L"grcWindow", nullptr); isCandidateWindow(gta)) {
        DWORD processId{};
        GetWindowThreadProcessId(gta, &processId);
        target.window = gta;
        target.processId = processId;
        target.processName = processImageName(processId);
        target.fullscreen = coversMonitor(gta, &target.monitor);
        if (!target.monitor) target.monitor = MonitorFromWindow(gta, MONITOR_DEFAULTTOPRIMARY);
        target.exclusiveD3d = isCloaked(gta) || (target.fullscreen && isD3dExclusiveFullscreen());
        return target;
    }

    const DWORD gamePid = findGameProcessId();
    struct Search {
        DWORD pid;
        HWND window{};
    } search{gamePid};
    EnumWindows([](HWND window, LPARAM param) -> BOOL {
        auto* state = reinterpret_cast<Search*>(param);
        DWORD processId{};
        GetWindowThreadProcessId(window, &processId);
        if (state->pid && processId != state->pid) return TRUE;
        if (!isCandidateWindow(window)) return TRUE;
        if (!state->pid) {
            const auto image = processImageName(processId);
            if (!isGameProcessName(image)) return TRUE;
        }
        if (coversMonitor(window) || !state->window) state->window = window;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));

    HWND window = search.window;
    if (!window) {
        window = GetForegroundWindow();
        DWORD processId{};
        if (window) GetWindowThreadProcessId(window, &processId);
        if (processId == GetCurrentProcessId() || !isCandidateWindow(window)) window = nullptr;
    }
    if (!window) return target;

    DWORD processId{};
    GetWindowThreadProcessId(window, &processId);
    target.window = window;
    target.processId = processId;
    target.processName = processImageName(processId);
    target.fullscreen = coversMonitor(window, &target.monitor);
    if (!target.monitor) target.monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
    target.exclusiveD3d = isCloaked(window) || (target.fullscreen && isD3dExclusiveFullscreen());
    return target;
}

bool tryMakeBorderless(HWND window) {
    if (!window || !IsWindow(window)) return false;
    DWORD processId{};
    GetWindowThreadProcessId(window, &processId);
    const auto name = processImageName(processId);
    wchar_t className[64]{};
    GetClassNameW(window, className, 64);
    const bool knownGame = isGameProcessName(name) ||
        lowerAscii(className) == L"grcwindow";
    if (!knownGame) return false;

    const bool exclusive = isCloaked(window) || isD3dExclusiveFullscreen();
    if (exclusive) {
        static HWND lastToggleWindow{};
        static auto lastToggle = std::chrono::steady_clock::now() - std::chrono::seconds(30);
        const auto now = std::chrono::steady_clock::now();
        if (lastToggleWindow != window || now - lastToggle > std::chrono::seconds(8)) {
            lastToggleWindow = window;
            lastToggle = now;
            const DWORD gameThread = GetWindowThreadProcessId(window, nullptr);
            const DWORD ourThread = GetCurrentThreadId();
            const bool attached = gameThread && gameThread != ourThread &&
                AttachThreadInput(ourThread, gameThread, TRUE);
            AllowSetForegroundWindow(processId);
            SetForegroundWindow(window);
            BringWindowToTop(window);
            postAltEnter(window);
            sendAltEnter();
            if (attached) AttachThreadInput(ourThread, gameThread, FALSE);
            Logger::instance().write(L"INFO", L"Requested exclusive-fullscreen toggle for " +
                (name.empty() ? std::wstring(className) : name));
        }
    }

    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return false;

    ShowWindow(window, SW_RESTORE);
    const LONG style = GetWindowLongW(window, GWL_STYLE);
    SetWindowLongW(window, GWL_STYLE,
        (style & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU)) |
        WS_POPUP | WS_VISIBLE);
    const LONG ex = GetWindowLongW(window, GWL_EXSTYLE);
    SetWindowLongW(window, GWL_EXSTYLE,
        (ex & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_WINDOWEDGE | WS_EX_TOPMOST)) | WS_EX_APPWINDOW);
    const RECT& area = info.rcMonitor;
    const BOOL placed = SetWindowPos(window, HWND_NOTOPMOST, area.left, area.top,
        area.right - area.left, area.bottom - area.top,
        SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOCOPYBITS);
    if (placed) {
        Logger::instance().write(L"INFO", L"Game window switched to borderless for overlay: " +
            (name.empty() ? std::wstring(className) : name));
    }
    return placed != FALSE;
}

void pinOverlayToGame(HWND overlay, HWND game) {
    if (!overlay || !IsWindow(overlay)) return;
    SetWindowLongPtrW(overlay, GWLP_HWNDPARENT, 0);
    SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    (void)game;
}

} // namespace kanekist
