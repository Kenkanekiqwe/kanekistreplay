#include "app.h"

#include "core.h"
#include "game_window.h"
#include "ui_embed.h"
#include <Dwmapi.h>
#include <Shellapi.h>
#include <WebView2.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>
#include <wrl/event.h>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace kanekist {
namespace {

constexpr COLORREF Background = RGB(7, 9, 13);
constexpr COLORREF Text = RGB(243, 246, 251);
constexpr UINT StatusTimer = 7;
constexpr UINT RecordingSavedMessage = WM_APP + 42;
constexpr UINT ReplayRotatedMessage = WM_APP + 43;
constexpr UINT TrayMessage = WM_APP + 44;
constexpr UINT RecordingStartedMessage = WM_APP + 45;
constexpr UINT LibraryPreviewMessage = WM_APP + 46;
constexpr auto ReplaySegmentLength = std::chrono::seconds(10);

HWND g_hotkeyHwnd = nullptr;
HHOOK g_keyboardHook = nullptr;
std::atomic<uint32_t> g_packedHotkeys[5]{};

bool dispatchPackedHotkey(UINT vk) {
    if (!g_hotkeyHwnd) return false;
    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
        vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
        vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
        vk == VK_LWIN || vk == VK_RWIN) {
        return false;
    }
    uint32_t mods = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) {
        mods |= MOD_WIN;
    }
    const uint32_t packed = packHotkey(mods, vk);
    for (int i = 0; i < 5; ++i) {
        if (g_packedHotkeys[i].load() == packed) {
            PostMessageW(g_hotkeyHwnd, WM_HOTKEY, i + 1, 0);
            return true;
        }
    }
    return false;
}

LRESULT CALLBACK overlayKeyboardHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        const auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        dispatchPackedHotkey(static_cast<UINT>(info->vkCode));
    }
    return CallNextHookEx(g_keyboardHook, code, wParam, lParam);
}

std::filesystem::path findFfmpegExecutable() {
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    auto ffmpeg = std::filesystem::path(modulePath).parent_path() / L"ffmpeg.exe";
    std::error_code error;
    if (std::filesystem::is_regular_file(ffmpeg, error)) return ffmpeg;
    wchar_t found[MAX_PATH]{};
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, found, nullptr)) return found;
    return {};
}
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_BORDER_COLOR 34
#define DWMWA_CAPTION_COLOR 35
#define DWMWA_TEXT_COLOR 36
#endif

std::wstring jsonEscape(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size() + 8);
    for (const auto ch : value) {
        if (ch == L'\\' || ch == L'"') result.push_back(L'\\');
        result.push_back(ch);
    }
    return result;
}

std::wstring jsonString(std::wstring_view text, std::wstring_view key) {
    const auto needle = L"\"" + std::wstring(key) + L"\"";
    const auto pos = text.find(needle);
    if (pos == std::wstring_view::npos) return {};
    const auto colon = text.find(L':', pos);
    const auto first = text.find(L'"', colon + 1);
    if (first == std::wstring_view::npos) return {};
    std::wstring result;
    for (size_t i = first + 1; i < text.size(); ++i) {
        if (text[i] == L'\\' && i + 1 < text.size()) result.push_back(text[++i]);
        else if (text[i] == L'"') return result;
        else result.push_back(text[i]);
    }
    return {};
}

int jsonInt(std::wstring_view text, std::wstring_view key, int fallback = 0) {
    const auto needle = L"\"" + std::wstring(key) + L"\"";
    const auto pos = text.find(needle);
    if (pos == std::wstring_view::npos) return fallback;
    const auto colon = text.find(L':', pos);
    if (colon == std::wstring_view::npos) return fallback;
    wchar_t* end{};
    const auto value = wcstol(text.data() + colon + 1, &end, 10);
    return end == text.data() + colon + 1 ? fallback : static_cast<int>(value);
}

bool jsonBool(std::wstring_view text, std::wstring_view key, bool fallback = false) {
    const auto needle = L"\"" + std::wstring(key) + L"\"";
    const auto pos = text.find(needle);
    if (pos == std::wstring_view::npos) return fallback;
    const auto colon = text.find(L':', pos);
    if (colon == std::wstring_view::npos) return fallback;
    const auto rest = text.substr(colon + 1);
    if (rest.find(L"true") < rest.find_first_of(L",}")) return true;
    if (rest.find(L"false") < rest.find_first_of(L",}")) return false;
    return fallback;
}

std::wstring formatFileSize(uintmax_t bytes) {
    wchar_t text[64]{};
    if (bytes >= 1024 * 1024) {
        swprintf_s(text, L"%.1f МБ", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        swprintf_s(text, L"%.0f КБ", static_cast<double>(bytes) / 1024.0);
    }
    return text;
}

std::wstring formatModifiedTime(const std::filesystem::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return {};
    FILETIME local{};
    SYSTEMTIME time{};
    FileTimeToLocalFileTime(&data.ftLastWriteTime, &local);
    FileTimeToSystemTime(&local, &time);
    wchar_t text[64]{};
    swprintf_s(text, L"%02u.%02u.%04u  %02u:%02u",
               time.wDay, time.wMonth, time.wYear, time.wHour, time.wMinute);
    return text;
}

    std::wstring formatDuration(double seconds) {
        if (seconds <= 0) return {};
        const int total = static_cast<int>(seconds + 0.5);
        const int hours = total / 3600;
        const int minutes = (total % 3600) / 60;
        const int secs = total % 60;
        wchar_t text[32]{};
        if (hours) swprintf_s(text, L"%d:%02d:%02d", hours, minutes, secs);
        else swprintf_s(text, L"%d:%02d", minutes, secs);
        return text;
    }

    bool isLibraryMedia(const std::filesystem::path& path) {
        const auto name = path.filename().wstring();
        if (name.find(L".partial") != std::wstring::npos) return false;
        if (name.ends_with(L".pcm") || name.ends_with(L".mux.mp4")) return false;
        const auto ext = path.extension().wstring();
        return _wcsicmp(ext.c_str(), L".mp4") == 0 || _wcsicmp(ext.c_str(), L".png") == 0;
    }

    std::wstring devicesJson(const std::vector<AudioDeviceInfo>& devices) {
        std::wstring json = L"[";
        bool first = true;
        for (const auto& device : devices) {
            if (!first) json += L",";
            first = false;
            json += L"{\"id\":\"" + jsonEscape(device.id) + L"\",\"name\":\"" +
                jsonEscape(device.name) + L"\"}";
        }
        json += L"]";
        return json;
    }

    std::wstring utf8ToWide(const char* text) {
    if (!text || !*text) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (count <= 1) return {};
    std::wstring result(static_cast<size_t>(count - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), count);
    return result;
}

} // namespace

App::App(HINSTANCE instance) : instance_(instance), settings_(Settings::load()) {
    settings_.normalizeEncodePreset();
    settings_.replaySeconds = normalizeReplaySeconds(settings_.replaySeconds);
    applyReplayLimits();
    (void)ReplayRecorder::cleanupStaleTempFiles(appDataDirectory() / L"ReplaySegments",
                                                std::chrono::hours(1));
}

int App::run(int showCommand) {
    ComApartment apartment(COINIT_APARTMENTTHREADED);
    Logger::instance().write(L"INFO", L"Application starting");
    if (FAILED(apartment.result()) || !createMainWindow(showCommand)) return 1;
    Logger::instance().write(L"INFO", L"Main window created");
    HRESULT hr = engine_.initialize(settings_);
    Logger::instance().write(L"INFO", SUCCEEDED(hr) ? L"Media engine initialized" : L"Media engine failed");
    if (FAILED(hr)) reportFailure(L"Инициализация захвата", hr);
    else {
        engine_.startPreview();
        fpsMonitor_.start([this] { return engine_.capturedFrames(); }, window_);
        overlay_.create(instance_, &fpsMonitor_, window_, &settings_);
        overlayMenu_.create(instance_, window_,
            [this](OverlayAction action) { handleOverlayAction(action); },
            [this] { return performanceSnapshot(); },
            POINT{
                settings_.overlayMenuX == UINT32_MAX ? -1 : static_cast<LONG>(settings_.overlayMenuX),
                settings_.overlayMenuY == UINT32_MAX ? -1 : static_cast<LONG>(settings_.overlayMenuY)
            },
            [this](int x, int y) {
                settings_.overlayMenuX = static_cast<uint32_t>(x);
                settings_.overlayMenuY = static_cast<uint32_t>(y);
                persistSettings();
            });
        Logger::instance().write(L"INFO", L"Overlay created");
        overlay_.show(settings_.showOverlay);
        status_ = L"Готово";
    }
    engine_.configureAudio(AudioMixer::outputFormat());
    applyAudioSettings();
    wireSystemAudio();
    wireMicrophone();
    if (settings_.instantReplayEnabled) setReplayEnabled(true);
    SetTimer(window_, StatusTimer, 1000, nullptr);
    g_hotkeyHwnd = window_;
    g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, overlayKeyboardHook, instance_, 0);
    if (!g_keyboardHook) {
        Logger::instance().write(L"WARN", L"Low-level keyboard hook failed");
    }
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x06;
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = window_;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        Logger::instance().write(L"WARN", L"Raw input hotkeys failed");
    }
    Logger::instance().write(L"INFO", L"Entering UI loop");

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    unregisterAppHotkeys();
    settings_.save();
    return static_cast<int>(message.wParam);
}

bool App::createMainWindow(int showCommand) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = instance_;
    wc.lpfnWndProc = windowProc;
    wc.lpszClassName = L"KanekistReplayWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = CreateSolidBrush(Background);
    if (!RegisterClassExW(&wc)) return false;
    window_ = CreateWindowExW(0, wc.lpszClassName, L"Kanekist Replay",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1240, 900,
        nullptr, nullptr, instance_, this);
    if (!window_) return false;
    excludeWindowFromCapture(window_);
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    COLORREF caption = Background;
    COLORREF border = RGB(118, 232, 151);
    COLORREF captionText = Text;
    DwmSetWindowAttribute(window_, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    DwmSetWindowAttribute(window_, DWMWA_BORDER_COLOR, &border, sizeof(border));
    DwmSetWindowAttribute(window_, DWMWA_TEXT_COLOR, &captionText, sizeof(captionText));
    registerAppHotkeys();
    addTrayIcon();
    hideAfterUiReady_ = settings_.startMinimized;
    ShowWindow(window_, hideAfterUiReady_ ? SW_SHOWNA : showCommand);
    UpdateWindow(window_);
    createWebView();
    return true;
}

void App::createWebView() {
    const auto userData = appDataDirectory() / L"WebView2";
    std::filesystem::create_directories(userData);
    const auto userDataText = userData.wstring();
    const HRESULT created = CreateCoreWebView2EnvironmentWithOptions(nullptr, userDataText.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                if (FAILED(result) || !environment || !window_) {
                    MessageBoxW(window_,
                        L"Не найден Microsoft Edge WebView2 Runtime.\n"
                        L"Установите его и перезапустите Kanekist.",
                        L"Kanekist Replay", MB_OK | MB_ICONERROR);
                    return result;
                }
                environment->CreateCoreWebView2Controller(window_,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT controllerResult, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(controllerResult) || !controller) return controllerResult;
                            webviewController_ = controller;
                            controller->get_CoreWebView2(&webview_);
                            if (!webview_) return E_FAIL;
                            Logger::instance().write(L"INFO", L"WebView2 controller created");
                            ComPtr<ICoreWebView2Controller2> controller2;
                            if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&controller2)))) {
                                COREWEBVIEW2_COLOR color{255, 11, 12, 14};
                                controller2->put_DefaultBackgroundColor(color);
                            }
                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(webview_->get_Settings(&settings)) && settings) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(FALSE);
                                settings->put_AreHostObjectsAllowed(FALSE);
                                settings->put_AreDefaultScriptDialogsEnabled(TRUE);
                            }
                            ComPtr<ICoreWebView2Settings3> settings3;
                            if (settings && SUCCEEDED(settings.As(&settings3))) {
                                settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
                            }
                            mapMediaHosts();
                            EventRegistrationToken token{};
                            webview_->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        wchar_t* json{};
                                        if (SUCCEEDED(args->get_WebMessageAsJson(&json)) && json) {
                                            handleUiMessage(json);
                                            CoTaskMemFree(json);
                                        }
                                        return S_OK;
                                    }).Get(), &token);
                            webview_->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                                        webviewReady_ = true;
                                        showWebView();
                                        refreshLibrary();
                                        postUiState();
                                        if (hideAfterUiReady_) {
                                            hideAfterUiReady_ = false;
                                            hideToTray();
                                        }
                                        return S_OK;
                                    }).Get(), &token);
                            showWebView();
                            const auto html = utf8ToWide(kEmbeddedUiHtml);
                            if (html.empty() || FAILED(webview_->NavigateToString(html.c_str()))) {
                                reportFailure(L"Загрузка интерфейса", E_FAIL);
                                return E_FAIL;
                            }
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
    if (FAILED(created)) {
        MessageBoxW(window_,
            L"Не найден Microsoft Edge WebView2 Runtime.\n"
            L"Установите его и перезапустите Kanekist.",
            L"Kanekist Replay", MB_OK | MB_ICONERROR);
    }
}

void App::showWebView() {
    if (!webviewController_ || !window_) return;
    resizeWebView();
    webviewController_->put_IsVisible(TRUE);
    webviewController_->NotifyParentWindowPositionChanged();
}

void App::resizeWebView() {
    if (!webviewController_ || !window_) return;
    RECT client{};
    GetClientRect(window_, &client);
    if (client.right <= 0 || client.bottom <= 0) return;
    webviewController_->put_Bounds(client);
}

void App::postUiState(bool includeLibrary) {
    if (!webview_ || !webviewReady_) return;
    const uint32_t packed[] = {
        settings_.hotkeyRecord, settings_.hotkeySaveReplay, settings_.hotkeyScreenshot,
        settings_.hotkeyFpsHud, settings_.hotkeyOverlay
    };
    std::wstring hotkeys = L"[";
    for (int i = 0; i < 5; ++i) {
        if (i) hotkeys += L",";
        hotkeys += L"\"" + jsonEscape(hotkeyLabel(Hotkey::fromPacked(packed[i]))) + L"\"";
    }
    hotkeys += L"]";
    std::wstring json = L"{";
    json += L"\"recording\":" + std::wstring(engine_.recording() ? L"true" : L"false");
    json += L",\"saving\":" + std::wstring(saving_ ? L"true" : L"false");
    json += L",\"starting\":" + std::wstring(starting_ ? L"true" : L"false");
    json += L",\"replay\":" + std::wstring(replayEnabled_ ? L"true" : L"false");
    json += L",\"mic\":" + std::wstring(settings_.captureMicrophone ? L"true" : L"false");
    json += L",\"sysAudio\":" + std::wstring(settings_.captureSystemAudio ? L"true" : L"false");
    json += L",\"codec\":" + std::to_wstring(settings_.videoCodec);
    json += L",\"micDevice\":\"" + jsonEscape(settings_.microphoneDeviceId) + L"\"";
    json += L",\"sysDevice\":\"" + jsonEscape(settings_.systemAudioDeviceId) + L"\"";
    json += L",\"micDevices\":" + devicesJson(listAudioDevices(true));
    json += L",\"sysDevices\":" + devicesJson(listAudioDevices(false));
    json += L",\"hud\":" + std::wstring(settings_.showOverlay ? L"true" : L"false");
    json += L",\"trayStart\":" + std::wstring(settings_.startMinimized ? L"true" : L"false");
    json += L",\"quality\":" + std::to_wstring(resolutionPresetIndex(settings_.width, settings_.height));
    json += L",\"res\":" + std::to_wstring(resolutionPresetIndex(settings_.width, settings_.height));
    json += L",\"encodeFps\":" + std::to_wstring(settings_.fps);
    json += L",\"bitrateMbps\":" + std::to_wstring(settings_.bitrateMbps);
    json += L",\"qualityLabel\":\"" + jsonEscape(encodePresetLabel(settings_.width, settings_.height,
        settings_.fps, settings_.videoCodec)) + L"\"";
    json += L",\"systemVolume\":" + std::to_wstring(settings_.systemVolume);
    json += L",\"micVolume\":" + std::to_wstring(settings_.microphoneVolume);
    json += L",\"noiseGate\":" + std::to_wstring(settings_.noiseGate);
    json += L",\"capturingHotkey\":" + std::to_wstring(capturingHotkey_);
    json += L",\"fps\":" + std::to_wstring(static_cast<int>(std::lround(fpsMonitor_.fps())));
    json += L",\"cpu\":" + std::to_wstring(static_cast<int>(std::lround(fpsMonitor_.cpuPercent())));
    json += L",\"gpu\":" + std::to_wstring(static_cast<int>(std::lround(fpsMonitor_.gpuBusyPercent())));
    json += L",\"dropped\":" + std::to_wstring(engine_.droppedFrames());
    json += L",\"captured\":" + std::to_wstring(engine_.capturedFrames());
    json += L",\"encoded\":" + std::to_wstring(engine_.encodedFrames());
    json += L",\"status\":\"" + jsonEscape(status_) + L"\"";
    json += L",\"overlayHotkey\":\"" + jsonEscape(hotkeyLabel(Hotkey::fromPacked(settings_.hotkeyOverlay))) + L"\"";
    json += L",\"hotkeys\":" + hotkeys;
    if (includeLibrary) json += L",\"library\":" + libraryJson_;
    json += L"}";
    webview_->PostWebMessageAsJson(json.c_str());
}

void App::handleUiMessage(const std::wstring& json) {
    const auto cmd = jsonString(json, L"cmd");
    if (cmd == L"ready") {
        postUiState();
    } else if (cmd == L"toggleRecord") {
        toggleRecording();
        postUiState();
    } else if (cmd == L"saveReplay") {
        saveReplay();
        postUiState();
    } else if (cmd == L"screenshot") {
        takeScreenshot();
    } else if (cmd == L"setReplay") {
        setReplayEnabled(jsonBool(json, L"on"));
        postUiState();
    } else if (cmd == L"setMic") {
        setMicrophoneEnabled(jsonBool(json, L"on"));
        postUiState();
    } else if (cmd == L"setSysAudio") {
        setSystemAudioEnabled(jsonBool(json, L"on"));
        postUiState();
    } else if (cmd == L"setMicDevice") {
        settings_.microphoneDeviceId = jsonString(json, L"id");
        if (settings_.captureMicrophone) wireMicrophone();
        persistSettings();
        postUiState();
    } else if (cmd == L"setSysDevice") {
        settings_.systemAudioDeviceId = jsonString(json, L"id");
        if (settings_.captureSystemAudio) wireSystemAudio();
        persistSettings();
        postUiState();
    } else if (cmd == L"setBitrate") {
        settings_.bitrateMbps = std::clamp(
            static_cast<uint32_t>(std::max(8, jsonInt(json, L"bitrate"))), 8u, 150u);
        engine_.updateEncodingSettings(settings_);
        persistSettings();
        postUiState();
    } else if (cmd == L"setCodec") {
        settings_.videoCodec = std::min(2u, static_cast<uint32_t>(std::max(0, jsonInt(json, L"index"))));
        settings_.bitrateMbps = suggestedBitrateMbps(settings_.width, settings_.height,
            settings_.fps, settings_.videoCodec);
        engine_.updateEncodingSettings(settings_);
        persistSettings();
        postUiState();
    } else if (cmd == L"setHud") {
        settings_.showOverlay = jsonBool(json, L"on");
        overlay_.show(settings_.showOverlay && !overlayMenu_.visible());
        persistSettings();
        postUiState();
    } else if (cmd == L"setTrayStart") {
        settings_.startMinimized = jsonBool(json, L"on");
        persistSettings();
    } else if (cmd == L"setQuality") {
        applyResolutionIndex(jsonInt(json, L"index"));
        postUiState();
    } else if (cmd == L"setResolution") {
        applyResolutionIndex(jsonInt(json, L"index"));
        postUiState();
    } else if (cmd == L"setEncodeFps") {
        applyEncodeFpsIndex(static_cast<uint32_t>(std::max(0, jsonInt(json, L"fps"))));
        postUiState();
    } else if (cmd == L"setSlider") {
        const auto id = jsonString(json, L"id");
        const auto value = static_cast<uint32_t>(std::max(0, jsonInt(json, L"value")));
        if (id == L"system") settings_.systemVolume = std::min(200u, value);
        else if (id == L"mic") settings_.microphoneVolume = std::min(200u, value);
        else if (id == L"gate") settings_.noiseGate = std::min(40u, value);
        applyAudioSettings();
        postUiState();
    } else if (cmd == L"persist") {
        persistSettings();
    } else if (cmd == L"captureHotkey") {
        beginHotkeyCapture(jsonInt(json, L"id"));
        postUiState();
    } else if (cmd == L"cancelHotkey") {
        capturingHotkey_ = 0;
        registerAppHotkeys();
        postUiState();
    } else if (cmd == L"hotkey") {
        finishHotkeyCapture(Hotkey{
            static_cast<uint32_t>(jsonInt(json, L"mods")),
            static_cast<uint32_t>(jsonInt(json, L"vk"))
        });
        postUiState();
    } else if (cmd == L"openMedia") {
        openMedia(jsonString(json, L"name"));
    } else if (cmd == L"openFolder") {
        openMedia({});
    } else if (cmd == L"deleteMedia") {
        deleteMedia(jsonString(json, L"name"));
    } else if (cmd == L"renameMedia") {
        renameMedia(jsonString(json, L"name"), jsonString(json, L"next"));
    } else if (cmd == L"copyMediaPath") {
        copyMediaPath(jsonString(json, L"name"));
    }
}

void App::updateStatus() {
    overlay_.show(settings_.showOverlay && !overlayMenu_.visible());
    if (replaySavePending_ && !replayRotating_ && !replayRecorder_.saving()) {
        replaySavePending_ = false;
        saving_ = false;
        if (replayRecorder_.lastError().empty()) {
            overlayMenu_.showToast(L"Instant Replay сохранён");
            refreshLibrary();
        } else {
            overlayMenu_.showToast(L"Ошибка сохранения Replay");
        }
    }
    if (replayEnabled_ && !saving_ && !replayRotating_ && !replaySavePending_ &&
        std::chrono::steady_clock::now() - replaySegmentStarted_ >= ReplaySegmentLength) {
        rotateReplaySegment(false);
    }
    if (saving_) return;
    wchar_t buffer[256]{};
    uint64_t freeBytes{};
    uint32_t diskMinutes = 0;
    if (!engine_.recording() && !replayEnabled_ &&
        queryFreeDiskBytes(settings_.outputDirectory, freeBytes)) {
        const auto perSecond = estimateBytesPerSecond(settings_.bitrateMbps);
        diskMinutes = perSecond ? static_cast<uint32_t>(freeBytes / perSecond / 60) : 0;
    }
    if (engine_.recording() || replayEnabled_) {
        swprintf_s(buffer, L"ЗАПИСЬ  ·  %.0f FPS %s",
            fpsMonitor_.fps(), fpsMonitor_.gameFpsActive() ? L"ИГРЫ" : L"ЗАХВАТА");
    } else {
        swprintf_s(buffer, L"ГОТОВО  ·  %.0f FPS %s  ·  диск: %.0f ГБ (~%u мин)",
            fpsMonitor_.fps(), fpsMonitor_.gameFpsActive() ? L"ИГРЫ" : L"ЗАХВАТА",
            freeBytes / (1024.0 * 1024.0 * 1024.0), diskMinutes);
    }
    status_ = buffer;
    const auto game = resolveGameTarget();
    overlay_.setGameWindow(game.window);
    if (game.window && game.exclusiveD3d) {
        static auto lastBorderlessAttempt = std::chrono::steady_clock::now() -
            std::chrono::seconds(5);
        const auto nowAttempt = std::chrono::steady_clock::now();
        if (nowAttempt - lastBorderlessAttempt > std::chrono::seconds(2)) {
            lastBorderlessAttempt = nowAttempt;
            tryMakeBorderless(game.window);
        }
    }
    if (settings_.showOverlay) overlay_.refresh();
    postUiState(false);
}

void App::toggleRecording() {
    if (saving_ || starting_) return;
    if (replayEnabled_) setReplayEnabled(false);
    if (engine_.recording()) {
        saving_ = true;
        postUiState();
        saveThread_ = std::jthread([this] {
            ComApartment apartment;
            engine_.stopRecording();
            PostMessageW(window_, RecordingSavedMessage, TRUE, FALSE);
        });
        return;
    }
    if (!confirmDiskSpace(false)) return;
    const auto path = uniqueMediaPath(settings_.outputDirectory, L"Recording", L".mp4");
    engine_.updateEncodingSettings(settings_);
    starting_ = true;
    postUiState();
    saveThread_ = std::jthread([this, path] {
        ComApartment apartment;
        const HRESULT hr = engine_.startRecording(path);
        PostMessageW(window_, RecordingStartedMessage, SUCCEEDED(hr) ? TRUE : FALSE, hr);
    });
}

void App::toggleReplay() {
    setReplayEnabled(!replayEnabled_);
}

void App::setReplayEnabled(bool enable) {
    if (enable && !replayEnabled_) {
        if (engine_.recording()) {
            MessageBoxW(window_, L"Остановите обычную запись перед включением Instant Replay.",
                        L"Kanekist Replay", MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (!confirmDiskSpace(true)) return;
        applyReplayLimits();
        replayRecorder_.clear();
        replaySegmentPath_ = nextReplaySegmentPath();
        engine_.updateEncodingSettings(settings_);
        engine_.setQuietIo(true);
        const auto hr = engine_.startRecording(replaySegmentPath_);
        if (FAILED(hr)) {
            engine_.setQuietIo(false);
            reportFailure(L"Instant Replay", hr);
            return;
        }
        replayEnabled_ = true;
        replaySegmentStarted_ = std::chrono::steady_clock::now();
    } else if (!enable && replayEnabled_) {
        engine_.stopRecording();
        engine_.setQuietIo(false);
        replayEnabled_ = false;
        replayRecorder_.clear();
    }
    settings_.instantReplayEnabled = replayEnabled_;
    persistSettings();
    postUiState();
}

void App::setMicrophoneEnabled(bool enable) {
    settings_.captureMicrophone = enable;
    wireMicrophone();
    persistSettings();
}

void App::setSystemAudioEnabled(bool enable) {
    settings_.captureSystemAudio = enable;
    wireSystemAudio();
    persistSettings();
}

void App::wireMicrophone() {
    microphone_.stop();
    audioMixer_.clear(AudioMixer::Source::microphone);
    if (!settings_.captureMicrophone) return;
    const auto micHr = microphone_.start([this](AudioPacket packet) {
        const auto submitted = audioMixer_.submit(AudioMixer::Source::microphone, packet.pcm,
            microphone_.format());
        if (submitted != AudioMixer::SubmitResult::ok) {
            static std::atomic_bool logged{false};
            if (!logged.exchange(true)) {
                Logger::instance().write(L"ERROR", L"Microphone format rejected");
            }
        }
        drainMixedAudio(packet.timestamp100ns);
    }, settings_.microphoneDeviceId);
    if (FAILED(micHr)) {
        Logger::instance().write(L"ERROR", L"Microphone capture failed: " + hresultMessage(micHr));
        overlayMenu_.showToast(L"Микрофон недоступен");
    }
}

void App::wireSystemAudio() {
    systemAudio_.stop();
    audioMixer_.clear(AudioMixer::Source::system);
    if (!settings_.captureSystemAudio) return;
    const auto audioHr = systemAudio_.start([this](AudioPacket packet) {
        const auto submitted = audioMixer_.submit(AudioMixer::Source::system, packet.pcm,
            systemAudio_.format());
        if (submitted != AudioMixer::SubmitResult::ok) {
            static std::atomic_bool logged{false};
            if (!logged.exchange(true)) {
                Logger::instance().write(L"ERROR", L"System audio format rejected");
            }
        }
        drainMixedAudio(packet.timestamp100ns);
    }, settings_.systemAudioDeviceId);
    if (FAILED(audioHr)) {
        Logger::instance().write(L"ERROR", L"System audio capture failed: " + hresultMessage(audioHr));
        overlayMenu_.showToast(L"Системный звук недоступен");
    }
}

int App::qualityIndex() const {
    return resolutionPresetIndex(settings_.width, settings_.height);
}

void App::applyQualityIndex(int selected) {
    applyResolutionIndex(selected);
}

void App::applyResolutionIndex(int selected) {
    uint32_t nativeW = engine_.captureWidth() ? engine_.captureWidth() : 1920;
    uint32_t nativeH = engine_.captureHeight() ? engine_.captureHeight() : 1080;
    applyResolutionPreset(settings_, selected, nativeW, nativeH);
    engine_.updateEncodingSettings(settings_);
    applyReplayLimits();
    persistSettings();
}

void App::applyEncodeFpsIndex(uint32_t fps) {
    applyEncodeFps(settings_, fps);
    engine_.updateEncodingSettings(settings_);
    applyReplayLimits();
    persistSettings();
}

void App::saveReplay() {
    if (saving_) return;
    if (!replayEnabled_) {
        MessageBoxW(window_, L"Сначала включите Instant Replay.", L"Kanekist Replay", MB_OK);
        return;
    }
    saving_ = true;
    replaySavePending_ = true;
    postUiState();
    rotateReplaySegment(true);
}

std::filesystem::path App::nextReplaySegmentPath() {
    const auto directory = appDataDirectory() / L"ReplaySegments";
    std::filesystem::create_directories(directory);
    return directory / (L"segment-" + std::to_wstring(++replaySegmentIndex_) + L".mp4");
}

void App::rotateReplaySegment(bool saveAfterRotation) {
    if (!replayEnabled_ || replayRotating_.exchange(true)) return;
    const auto completed = replaySegmentPath_;
    const auto next = nextReplaySegmentPath();
    replayRotateThread_ = std::jthread([this, completed, next, saveAfterRotation] {
        engine_.stopRecording();
        const auto duration = engine_.lastMediaDuration();
        std::error_code error;
        const auto bytes = std::filesystem::file_size(completed, error);
        bool added = !error && replayRecorder_.addCompletedSegment(completed, duration, bytes);
        replaySegmentPath_ = next;
        const bool restarted = SUCCEEDED(engine_.startRecording(next));
        bool saveStarted = true;
        if (saveAfterRotation && added) {
            saveStarted = replayRecorder_.saveAsync(findFfmpegExecutable(),
                uniqueMediaPath(settings_.outputDirectory, L"Replay", L".mp4"));
        }
        PostMessageW(window_, ReplayRotatedMessage,
                     saveAfterRotation ? (saveStarted ? TRUE : FALSE) : TRUE,
                     restarted ? TRUE : FALSE);
    });
}

void App::takeScreenshot() {
    const auto path = uniqueMediaPath(settings_.outputDirectory, L"Screenshot", L".png");
    const auto hr = engine_.takeScreenshot(path);
    if (FAILED(hr)) reportFailure(L"Скриншот", hr);
    else refreshLibrary();
}

void App::refreshLibrary(bool kickPreviews) {
    libraryJson_ = L"[";
    std::error_code error;
    if (std::filesystem::exists(settings_.outputDirectory, error)) {
        std::vector<std::filesystem::directory_entry> files;
        std::filesystem::directory_iterator it(settings_.outputDirectory, error);
        const std::filesystem::directory_iterator end;
        for (; !error && it != end; it.increment(error)) {
            if (error) break;
            std::error_code fileError;
            if (it->is_regular_file(fileError) && !fileError && isLibraryMedia(it->path())) {
                files.push_back(*it);
            }
        }
        std::ranges::sort(files, [](const auto& a, const auto& b) {
            std::error_code leftError, rightError;
            return a.last_write_time(leftError) > b.last_write_time(rightError);
        });
        const auto thumbs = appDataDirectory() / L"Thumbs";
        bool first = true;
        for (const auto& entry : files) {
            if (!first) libraryJson_ += L",";
            first = false;
            const auto preview = previewMediaFile(entry.path(), thumbs, false);
            const auto name = entry.path().filename().wstring();
            const auto ext = entry.path().extension().wstring();
            const bool png = _wcsicmp(ext.c_str(), L".png") == 0;
            std::wstring thumb;
            if (png) thumb = L"https://kanekist.media/" + name;
            else if (preview.hasThumbnail) thumb = L"https://kanekist.thumbs/" + name + L".jpg";
            std::error_code sizeError;
            const auto bytes = entry.file_size(sizeError);
            libraryJson_ += L"{\"name\":\"" + jsonEscape(name) +
                L"\",\"size\":\"" + jsonEscape(sizeError ? L"" : formatFileSize(bytes)) +
                L"\",\"modified\":\"" + jsonEscape(formatModifiedTime(entry.path())) +
                L"\",\"duration\":\"" + jsonEscape(formatDuration(preview.durationSeconds)) +
                L"\",\"kind\":\"" + jsonEscape(png ? L"PNG" : L"MP4") +
                L"\",\"thumb\":\"" + jsonEscape(thumb) + L"\"}";
        }
    }
    libraryJson_ += L"]";
    postUiState();
    if (kickPreviews) queueLibraryPreviews();
}

void App::queueLibraryPreviews() {
    if (libraryBusy_.exchange(true)) return;
    const auto directory = settings_.outputDirectory;
    const auto thumbs = appDataDirectory() / L"Thumbs";
    libraryThread_ = std::jthread([this, directory, thumbs] {
        ComApartment apartment;
        std::error_code error;
        std::filesystem::directory_iterator it(directory, error);
        const std::filesystem::directory_iterator end;
        for (; libraryBusy_ && !error && it != end; it.increment(error)) {
            std::error_code fileError;
            if (!it->is_regular_file(fileError) || fileError || !isLibraryMedia(it->path())) continue;
            previewMediaFile(it->path(), thumbs, true);
        }
        libraryBusy_ = false;
        if (window_) PostMessageW(window_, LibraryPreviewMessage, 0, 0);
    });
}

std::filesystem::path App::mediaFile(const std::wstring& name) const {
    if (name.empty() || name.find_first_of(L"/\\:") != std::wstring::npos) return {};
    if (name == L"." || name == L"..") return {};
    auto path = (settings_.outputDirectory / name).lexically_normal();
    std::error_code error;
    const auto root = settings_.outputDirectory.lexically_normal();
    const auto relative = std::filesystem::relative(path, root, error);
    if (error || relative.empty() || relative.native().starts_with(L"..")) return {};
    return path;
}

void App::openMedia(const std::wstring& name) {
    if (name.empty()) {
        ShellExecuteW(window_, L"open", settings_.outputDirectory.c_str(), nullptr, nullptr, SW_SHOW);
        return;
    }
    const auto path = mediaFile(name);
    if (path.empty() || !std::filesystem::exists(path)) return;
    ShellExecuteW(window_, L"open", path.c_str(), nullptr, nullptr, SW_SHOW);
}

void App::deleteMedia(const std::wstring& name) {
    const auto path = mediaFile(name);
    if (path.empty()) return;
    std::error_code error;
    std::filesystem::remove(path, error);
    const auto thumbs = appDataDirectory() / L"Thumbs";
    std::filesystem::remove(thumbs / (name + L".jpg"), error);
    std::filesystem::remove(thumbs / (name + L".dur"), error);
    refreshLibrary();
}

void App::renameMedia(const std::wstring& name, const std::wstring& nextName) {
    const auto path = mediaFile(name);
    if (path.empty() || nextName.empty()) return;
    std::wstring clean = nextName;
    clean.erase(std::remove_if(clean.begin(), clean.end(), [](wchar_t ch) {
        return ch == L'/' || ch == L'\\' || ch == L':' || ch == L'*' || ch == L'?' ||
               ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|';
    }), clean.end());
    while (!clean.empty() && (clean.back() == L' ' || clean.back() == L'.')) clean.pop_back();
    if (clean.empty()) return;
    if (clean.find(L'.') == std::wstring::npos) clean += path.extension().wstring();
    const auto destination = mediaFile(clean);
    if (destination.empty() || destination == path) return;
    std::error_code error;
    if (std::filesystem::exists(destination, error)) return;
    std::filesystem::rename(path, destination, error);
    if (error) return;
    const auto thumbs = appDataDirectory() / L"Thumbs";
    std::filesystem::rename(thumbs / (name + L".jpg"), thumbs / (clean + L".jpg"), error);
    std::filesystem::rename(thumbs / (name + L".dur"), thumbs / (clean + L".dur"), error);
    refreshLibrary();
}

void App::copyMediaPath(const std::wstring& name) {
    const auto path = name.empty() ? settings_.outputDirectory : mediaFile(name);
    if (path.empty()) return;
    const auto text = path.wstring();
    if (!OpenClipboard(window_)) return;
    EmptyClipboard();
    const auto bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* locked = GlobalLock(memory)) {
            std::memcpy(locked, text.c_str(), bytes);
            GlobalUnlock(memory);
            SetClipboardData(CF_UNICODETEXT, memory);
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

void App::mapMediaHosts() {
    if (!webview_) return;
    ComPtr<ICoreWebView2_3> web3;
    if (FAILED(webview_.As(&web3)) || !web3) return;
    const auto thumbs = appDataDirectory() / L"Thumbs";
    std::error_code error;
    std::filesystem::create_directories(thumbs, error);
    std::filesystem::create_directories(settings_.outputDirectory, error);
    web3->SetVirtualHostNameToFolderMapping(L"kanekist.media",
        settings_.outputDirectory.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
    web3->SetVirtualHostNameToFolderMapping(L"kanekist.thumbs",
        thumbs.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
}

void App::handleHotkey(WPARAM id) {
    static std::chrono::steady_clock::time_point last{};
    static WPARAM lastId{};
    const auto now = std::chrono::steady_clock::now();
    if (id == lastId && now - last < std::chrono::milliseconds(250)) return;
    last = now;
    lastId = id;
    if (id == 1) toggleRecording();
    else if (id == 2) saveReplay();
    else if (id == 3) takeScreenshot();
    else if (id == 4) {
        settings_.showOverlay = !settings_.showOverlay;
        overlay_.show(settings_.showOverlay);
        persistSettings();
        postUiState();
    } else if (id == 5) {
        overlayMenu_.toggle();
        overlay_.show(settings_.showOverlay && !overlayMenu_.visible());
    }
}

void App::handleOverlayAction(OverlayAction action) {
    switch (action) {
    case OverlayAction::toggleRecording: {
        const bool wasRecording = engine_.recording();
        toggleRecording();
        overlayMenu_.showToast(wasRecording ? L"Запись сохраняется" : L"Запись запущена");
        break;
    }
    case OverlayAction::toggleReplay:
        toggleReplay();
        overlayMenu_.showToast(replayEnabled_ ? L"Instant Replay включён" : L"Instant Replay выключен");
        break;
    case OverlayAction::saveReplay:
        saveReplay();
        overlayMenu_.showToast(L"Повтор сохраняется в фоне");
        break;
    case OverlayAction::screenshot:
        takeScreenshot();
        overlayMenu_.showToast(L"Скриншот сохранён");
        break;
    case OverlayAction::gallery:
        overlayMenu_.close();
        openMedia({});
        break;
    case OverlayAction::toggleMicrophone:
        setMicrophoneEnabled(!settings_.captureMicrophone);
        overlayMenu_.showToast(settings_.captureMicrophone ? L"Микрофон включён" : L"Микрофон выключен");
        break;
    case OverlayAction::toggleFpsHud:
        settings_.showOverlay = !settings_.showOverlay;
        overlayMenu_.showToast(settings_.showOverlay ? L"FPS HUD включён" : L"FPS HUD выключен");
        break;
    case OverlayAction::cycleResolution: {
        const uint32_t nativeH = engine_.captureHeight() ? engine_.captureHeight() : 1080;
        int index = resolutionPresetIndex(settings_.width, settings_.height);
        for (int step = 0; step < 5; ++step) {
            index = (index + 1) % 5;
            if (index == 0) break;
            if (index == 1 && nativeH >= 1080) break;
            if (index == 2 && nativeH >= 1440) break;
            if (index == 3 && nativeH >= 2160) break;
            if (index == 4) break;
        }
        applyResolutionIndex(index);
        overlayMenu_.showToast(encodePresetLabel(settings_.width, settings_.height, settings_.fps,
            settings_.videoCodec));
        break;
    }
    case OverlayAction::cycleEncodeFps: {
        const uint32_t next = settings_.fps >= 120 ? 30u : (settings_.fps >= 60 ? 120u : 60u);
        applyEncodeFpsIndex(next);
        overlayMenu_.showToast(L"FPS записи: " + std::to_wstring(settings_.fps));
        break;
    }
    case OverlayAction::cycleHudPosition: {
        settings_.overlayCorner = (settings_.overlayCorner + 1) % 9;
        constexpr const wchar_t* names[] = {
            L"слева сверху", L"сверху по центру", L"справа сверху",
            L"слева", L"по центру", L"справа",
            L"слева снизу", L"снизу по центру", L"справа снизу"
        };
        overlayMenu_.showToast(L"HUD: " + std::wstring(names[settings_.overlayCorner]));
        break;
    }
    case OverlayAction::cycleHudColor: {
        constexpr uint32_t colors[] = {0x76e897, 0xffffff, 0x65b7ff, 0xffd166, 0xff6b81};
        size_t index{};
        for (; index < std::size(colors); ++index) if (colors[index] == settings_.hudTextColor) break;
        settings_.hudTextColor = colors[(index + 1) % std::size(colors)];
        overlayMenu_.showToast(L"Цвет FPS HUD изменён");
        break;
    }
    case OverlayAction::cycleHudOutline:
        settings_.hudOutlineThickness = (settings_.hudOutlineThickness + 1) % 5;
        overlayMenu_.showToast(settings_.hudOutlineThickness == 0 ? L"Обводка выключена" :
            L"Обводка: " + std::to_wstring(settings_.hudOutlineThickness) + L" px");
        break;
    case OverlayAction::cycleHudTemplate: {
        constexpr const wchar_t* templates[] = {
            L"{fps} FPS", L"{fps} FPS  ·  {low} LOW",
            L"{fps} FPS  ·  {frametime} ms", L"FPS {fps}  CPU {cpu}%  GPU {gpu}%"
        };
        size_t index{};
        for (; index < std::size(templates); ++index) if (templates[index] == settings_.hudTemplate) break;
        settings_.hudTemplate = templates[(index + 1) % std::size(templates)];
        overlayMenu_.showToast(L"Шаблон HUD изменён");
        break;
    }
    case OverlayAction::cycleReplayDuration: {
        constexpr uint32_t minutes[] = {1, 2, 3, 5, 10, 15, 20, 30};
        const uint32_t current = std::max(1u, settings_.replaySeconds / 60);
        size_t index{};
        for (; index < std::size(minutes); ++index) if (minutes[index] == current) break;
        settings_.replaySeconds = minutes[(index + 1) % std::size(minutes)] * 60;
        applyReplayLimits();
        overlayMenu_.showToast(L"Replay: последние " +
            std::to_wstring(settings_.replaySeconds / 60) + L" мин");
        break;
    }
    case OverlayAction::cycleHudFont: {
        constexpr const wchar_t* fonts[] = {
            L"Bahnschrift", L"Segoe UI Variable Display", L"Consolas", L"Segoe UI"
        };
        size_t index{};
        for (; index < std::size(fonts); ++index) if (fonts[index] == settings_.hudFontName) break;
        settings_.hudFontName = fonts[(index + 1) % std::size(fonts)];
        overlayMenu_.showToast(L"Шрифт HUD: " + settings_.hudFontName);
        break;
    }
    case OverlayAction::cycleHudSize: {
        constexpr uint32_t sizes[] = {14, 18, 22, 28, 36, 48};
        size_t index{};
        for (; index < std::size(sizes); ++index) if (sizes[index] == settings_.hudFontSize) break;
        settings_.hudFontSize = sizes[(index + 1) % std::size(sizes)];
        overlayMenu_.showToast(L"Размер HUD: " + std::to_wstring(settings_.hudFontSize));
        break;
    }
    }
    persistSettings();
    overlay_.show(settings_.showOverlay && !overlayMenu_.visible());
    overlayMenu_.refresh();
    overlay_.refresh();
    postUiState();
}

PerformanceSnapshot App::performanceSnapshot() const {
    PerformanceSnapshot snapshot;
    snapshot.fps = fpsMonitor_.fps();
    snapshot.onePercentLow = fpsMonitor_.onePercentLow();
    snapshot.frameTimeMs = fpsMonitor_.frameTimeMs();
    snapshot.cpuPercent = fpsMonitor_.cpuPercent();
    snapshot.gpuBusyPercent = fpsMonitor_.gpuBusyPercent();
    snapshot.renderLatencyMs = fpsMonitor_.renderLatencyMs();
    snapshot.gameMetrics = fpsMonitor_.gameFpsActive();
    snapshot.recording = engine_.recording();
    snapshot.replayEnabled = replayEnabled_;
    snapshot.microphoneEnabled = settings_.captureMicrophone;
    snapshot.hudEnabled = settings_.showOverlay;
    snapshot.encodeWidth = settings_.width;
    snapshot.encodeHeight = settings_.height;
    snapshot.encodeFps = settings_.fps;
    snapshot.encodeCodec = engine_.encodeCodec() ? engine_.encodeCodec() : settings_.videoCodec;
    snapshot.hudCorner = settings_.overlayCorner;
    snapshot.hudColor = settings_.hudTextColor;
    snapshot.hudOutline = settings_.hudOutlineThickness;
    snapshot.replaySeconds = settings_.replaySeconds;
    snapshot.hudFontSize = settings_.hudFontSize;
    snapshot.hudFontName = settings_.hudFontName;
    snapshot.overlayHotkey = hotkeyLabel(Hotkey::fromPacked(settings_.hotkeyOverlay));
    return snapshot;
}

void App::drainMixedAudio(int64_t timestamp100ns) {
    const bool systemOn = settings_.captureSystemAudio;
    const bool micOn = settings_.captureMicrophone;
    const auto systemFrames = audioMixer_.availableFrames(AudioMixer::Source::system);
    const auto micFrames = audioMixer_.availableFrames(AudioMixer::Source::microphone);
    std::size_t frames = 0;
    if (systemOn && micOn) {
        frames = (std::min)(systemFrames, micFrames);
        if (frames == 0) {
            const auto queued = (std::max)(systemFrames, micFrames);
            if (queued > 2048) frames = queued;
        }
    } else if (systemOn) {
        frames = systemFrames;
    } else {
        frames = micFrames;
    }
    frames = (std::min)(frames, std::size_t{2048});
    if (frames == 0) return;
    auto mixed = audioMixer_.mix(frames);
    if (mixed.empty()) return;
    engine_.writeAudio(std::as_bytes(std::span<const float>(mixed)), timestamp100ns);
}

void App::persistSettings() {
    settings_.save();
}

void App::applyAudioSettings() {
    audioMixer_.setGain(AudioMixer::Source::system, settings_.systemVolume / 100.0f);
    audioMixer_.setGain(AudioMixer::Source::microphone, settings_.microphoneVolume / 100.0f);
    audioMixer_.setNoiseGate(settings_.noiseGate / 200.0f);
}

void App::applyReplayLimits() {
    settings_.replaySeconds = normalizeReplaySeconds(settings_.replaySeconds);
    replayRecorder_.setLimits({std::chrono::seconds(settings_.replaySeconds), replayByteBudget()});
}

uint64_t App::replayByteBudget() const {
    return estimateBytesPerSecond(settings_.bitrateMbps) *
        static_cast<uint64_t>(settings_.replaySeconds) * 2ull + (4ull << 30);
}

void App::unregisterAppHotkeys() {
    if (!window_) return;
    for (int id = 1; id <= 5; ++id) UnregisterHotKey(window_, id);
}

void App::registerAppHotkeys() {
    unregisterAppHotkeys();
    const uint32_t packed[] = {
        settings_.hotkeyRecord, settings_.hotkeySaveReplay, settings_.hotkeyScreenshot,
        settings_.hotkeyFpsHud, settings_.hotkeyOverlay
    };
    for (int id = 1; id <= 5; ++id) {
        const auto hotkey = Hotkey::fromPacked(packed[id - 1]);
        if (!hotkey.valid()) continue;
        if (!RegisterHotKey(window_, id, hotkey.modifiers | MOD_NOREPEAT, hotkey.vk)) {
            Logger::instance().write(L"ERROR", L"Не удалось зарегистрировать хоткей " + hotkeyLabel(hotkey));
        }
        g_packedHotkeys[id - 1] = packed[id - 1];
    }
}

bool App::assignHotkey(int id, Hotkey hotkey) {
    if (id < 1 || id > 5 || !hotkey.valid()) return false;
    uint32_t* fields[] = {
        &settings_.hotkeyRecord, &settings_.hotkeySaveReplay, &settings_.hotkeyScreenshot,
        &settings_.hotkeyFpsHud, &settings_.hotkeyOverlay
    };
    const auto packed = hotkey.packed();
    for (int i = 0; i < 5; ++i) {
        if (i != id - 1 && *fields[i] == packed) return false;
    }
    *fields[id - 1] = packed;
    if (id == 5) settings_.overlayMenuKey = hotkey.vk;
    return true;
}

void App::beginHotkeyCapture(int id) {
    if (id < 1 || id > 5) return;
    capturingHotkey_ = id;
    unregisterAppHotkeys();
}

void App::finishHotkeyCapture(Hotkey hotkey) {
    const int id = capturingHotkey_;
    capturingHotkey_ = 0;
    if (id != 0 && !assignHotkey(id, hotkey)) {
        MessageBoxW(window_, L"Это сочетание уже занято или недопустимо. Нужен модификатор (Ctrl/Shift/Alt) или F-клавиша.",
                    L"Kanekist Replay", MB_OK | MB_ICONINFORMATION);
    } else {
        persistSettings();
    }
    registerAppHotkeys();
}

void App::addTrayIcon() {
    if (trayAdded_ || !window_) return;
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = window_;
    data.uID = 1;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = TrayMessage;
    data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(data.szTip, L"Kanekist Replay");
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    if (trayAdded_) {
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
    }
}

void App::removeTrayIcon() {
    if (!trayAdded_) return;
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = window_;
    data.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &data);
    trayAdded_ = false;
}

void App::showFromTray() {
    ShowWindow(window_, SW_RESTORE);
    SetForegroundWindow(window_);
    showWebView();
}

void App::hideToTray() {
    persistSettings();
    addTrayIcon();
    ShowWindow(window_, SW_HIDE);
}

void App::quitApplication() {
    overlayMenu_.close();
    overlay_.show(false);
    fpsMonitor_.stop();
    systemAudio_.stop();
    microphone_.stop();
    engine_.stopRecording();
    libraryBusy_ = false;
    if (libraryThread_.joinable()) libraryThread_.join();
    if (saveThread_.joinable()) saveThread_.join();
    if (replayRotateThread_.joinable()) replayRotateThread_.join();
    replayRecorder_.cancelSave();
    engine_.stopPreview();
    if (g_keyboardHook) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
    }
    g_hotkeyHwnd = nullptr;
    removeTrayIcon();
    if (webviewController_) webviewController_->Close();
    webview_.Reset();
    webviewController_.Reset();
    DestroyWindow(window_);
}

void App::showTrayMenu() {
    POINT cursor{};
    GetCursorPos(&cursor);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, TrayOpen, L"Открыть");
    AppendMenuW(menu, MF_STRING, TrayRecord, engine_.recording() ? L"Остановить запись" : L"Начать запись");
    AppendMenuW(menu, MF_STRING, TraySaveReplay, L"Сохранить повтор");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, TrayExit, L"Выход");
    SetForegroundWindow(window_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x, cursor.y, 0, window_, nullptr);
    DestroyMenu(menu);
}

bool App::confirmDiskSpace(bool replay) {
    uint64_t freeBytes{};
    if (!queryFreeDiskBytes(settings_.outputDirectory, freeBytes)) return true;
    const uint64_t perSecond = estimateBytesPerSecond(settings_.bitrateMbps);
    const uint64_t needed = replay
        ? perSecond * settings_.replaySeconds + (1ull << 30)
        : perSecond * 600;
    if (freeBytes > needed && freeBytes > (2ull << 30)) return true;
    const uint32_t minutes = perSecond ? static_cast<uint32_t>(freeBytes / perSecond / 60) : 0;
    wchar_t text[384]{};
    swprintf_s(text,
        L"На диске свободно %.1f ГБ — примерно на %u мин записи.\n%s\nПродолжить?",
        freeBytes / (1024.0 * 1024.0 * 1024.0), minutes,
        replay ? L"Для Instant Replay лучше иметь запас больше длины буфера."
               : L"Рекомендуется минимум 2 ГБ свободного места.");
    return MessageBoxW(window_, text, L"Мало места на диске", MB_YESNO | MB_ICONWARNING) == IDYES;
}

void App::reportFailure(std::wstring_view action, HRESULT result) {
    const auto message = std::wstring(action) + L": " + hresultMessage(result);
    Logger::instance().write(L"ERROR", message);
    status_ = message;
    MessageBoxW(window_, message.c_str(), L"Kanekist Replay", MB_OK | MB_ICONERROR);
    postUiState();
}

LRESULT CALLBACK App::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->dispatch(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT App::dispatch(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            if (webviewController_) webviewController_->put_IsVisible(FALSE);
            hideToTray();
            return 0;
        }
        showWebView();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize = {1100, 760};
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_MINIMIZE) {
            hideToTray();
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(window_, nullptr, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        showWebView();
        return 0;
    }
    case WM_WINDOWPOSCHANGED:
        if (webviewController_ && !IsIconic(window_)) {
            webviewController_->NotifyParentWindowPositionChanged();
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window_, &ps);
        RECT client{};
        GetClientRect(window_, &client);
        HBRUSH brush = CreateSolidBrush(Background);
        FillRect(dc, &client, brush);
        DeleteObject(brush);
        if (!webviewReady_) {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, Text);
            DrawTextW(dc, L"Kanekist", -1, &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        EndPaint(window_, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wParam == StatusTimer) updateStatus();
        return 0;
    case RecordingSavedMessage:
        saving_ = false;
        refreshLibrary();
        postUiState();
        return 0;
    case LibraryPreviewMessage:
        refreshLibrary(false);
        return 0;
    case RecordingStartedMessage:
        starting_ = false;
        if (!wParam) reportFailure(L"Запуск записи", static_cast<HRESULT>(lParam));
        else if (engine_.encodeCodec() != settings_.videoCodec) {
            overlayMenu_.showToast(std::wstring(videoCodecLabel(settings_.videoCodec)) +
                L" недоступен, " + videoCodecLabel(engine_.encodeCodec()));
        }
        postUiState();
        return 0;
    case ReplayRotatedMessage:
        replayRotating_ = false;
        replaySegmentStarted_ = std::chrono::steady_clock::now();
        if (!lParam) {
            replayEnabled_ = false;
            replaySavePending_ = false;
            saving_ = false;
            engine_.setQuietIo(false);
            overlayMenu_.showToast(L"Replay остановлен: ошибка нового сегмента");
        } else if (replaySavePending_ && !wParam) {
            replaySavePending_ = false;
            saving_ = false;
            overlayMenu_.showToast(L"Не удалось запустить сборку Replay");
        }
        postUiState();
        return 0;
    case WM_HOTKEY:
        if (!capturingHotkey_) handleHotkey(wParam);
        return 0;
    case WM_INPUT: {
        if (capturingHotkey_) return 0;
        UINT size = 0;
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size,
            sizeof(RAWINPUTHEADER)) != 0 || size == 0) {
            return 0;
        }
        std::vector<uint8_t> data(size);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, data.data(), &size,
            sizeof(RAWINPUTHEADER)) != size) {
            return 0;
        }
        const auto* raw = reinterpret_cast<RAWINPUT*>(data.data());
        if (raw->header.dwType != RIM_TYPEKEYBOARD) return 0;
        if (raw->data.keyboard.Flags & RI_KEY_BREAK) return 0;
        dispatchPackedHotkey(raw->data.keyboard.VKey);
        return 0;
    }
    case TrayMessage: {
        const UINT tray = LOWORD(lParam);
        if (tray == WM_LBUTTONUP || tray == WM_LBUTTONDBLCLK || tray == NIN_SELECT || tray == NIN_KEYSELECT) {
            showFromTray();
        } else if (tray == WM_RBUTTONUP || tray == WM_CONTEXTMENU) {
            showTrayMenu();
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case TrayOpen: showFromTray(); break;
        case TrayRecord: toggleRecording(); break;
        case TraySaveReplay: saveReplay(); break;
        case TrayExit: quitApplication(); break;
        }
        return 0;
    case WM_CLOSE:
        hideToTray();
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wParam) quitApplication();
        return 0;
    case WM_DESTROY:
        if (g_keyboardHook) {
            UnhookWindowsHookEx(g_keyboardHook);
            g_keyboardHook = nullptr;
        }
        g_hotkeyHwnd = nullptr;
        {
            RAWINPUTDEVICE rid{};
            rid.usUsagePage = 0x01;
            rid.usUsage = 0x06;
            rid.dwFlags = RIDEV_REMOVE;
            rid.hwndTarget = nullptr;
            RegisterRawInputDevices(&rid, 1, sizeof(rid));
        }
        removeTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

} // namespace kanekist
