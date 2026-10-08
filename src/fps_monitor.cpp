#include "fps_monitor.h"
#include "game_window.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace kanekist {
namespace {

uint64_t fileTimeValue(const FILETIME& value) {
    ULARGE_INTEGER result{};
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
}

std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> fields;
    size_t begin = 0;
    bool quoted = false;
    for (size_t i = 0; i <= line.size(); ++i) {
        if (i < line.size() && line[i] == '"') quoted = !quoted;
        if (i == line.size() || (line[i] == ',' && !quoted)) {
            fields.emplace_back(line.substr(begin, i - begin));
            begin = i + 1;
        }
    }
    return fields;
}

void replaceToken(std::wstring& text, std::wstring_view token, std::wstring value) {
    size_t position{};
    while ((position = text.find(token, position)) != std::wstring::npos) {
        text.replace(position, token.size(), value);
        position += value.size();
    }
}

std::wstring formatHudText(const Settings& settings, const FpsMonitor& monitor) {
    std::wstring text = settings.hudTemplate;
    replaceToken(text, L"{fps}", std::to_wstring(static_cast<int>(std::round(monitor.fps()))));
    replaceToken(text, L"{low}", std::to_wstring(static_cast<int>(std::round(monitor.onePercentLow()))));
    wchar_t value[32]{};
    swprintf_s(value, L"%.1f", monitor.frameTimeMs());
    replaceToken(text, L"{frametime}", value);
    swprintf_s(value, L"%.0f", monitor.cpuPercent());
    replaceToken(text, L"{cpu}", value);
    swprintf_s(value, L"%.0f", monitor.gpuBusyPercent());
    replaceToken(text, L"{gpu}", value);
    return text;
}

D2D1_COLOR_F hudColor(uint32_t rgb, float alpha = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f,
                        (rgb & 0xff) / 255.0f, alpha);
}

} // namespace

FpsMonitor::~FpsMonitor() { stop(); }

void FpsMonitor::start(Counter counter, HWND ownerWindow) {
    if (running_.exchange(true)) return;
    if (launchPresentMon()) readerThread_ = std::jthread([this] { readPresentMon(); });
    thread_ = std::jthread([this, counter = std::move(counter), ownerWindow] {
        auto previousTime = std::chrono::steady_clock::now();
        uint64_t previousFrames = counter();
        FILETIME idle{}, kernel{}, user{};
        GetSystemTimes(&idle, &kernel, &user);
        uint64_t previousIdle = fileTimeValue(idle);
        uint64_t previousTotal = fileTimeValue(kernel) + fileTimeValue(user);
        std::vector<double> fpsHistory;
        while (running_) {
            const auto game = resolveGameTarget();
            DWORD processId = game.processId;
            if (!processId) {
                const HWND foreground = GetForegroundWindow();
                GetWindowThreadProcessId(foreground, &processId);
            }
            DWORD ownerProcessId{};
            GetWindowThreadProcessId(ownerWindow, &ownerProcessId);
            foregroundProcessId_ = processId != ownerProcessId ? processId : 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            const auto now = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(now - previousTime).count();
            const uint64_t presents = presentCount_.exchange(0);
            if (presents && foregroundProcessId_) {
                const double measuredFps = presents / std::max(0.001, seconds);
                fps_ = measuredFps;
                frameTimeMs_ = 1000.0 / measuredFps;
                const double busy = gpuBusyTotal_.exchange(0.0);
                const double latency = latencyTotal_.exchange(0.0);
                gpuBusyPercent_ = std::clamp((busy / presents) / frameTimeMs_.load() * 100.0, 0.0, 100.0);
                renderLatencyMs_ = latency / presents;
                fpsHistory.push_back(measuredFps);
                if (fpsHistory.size() > 120) fpsHistory.erase(fpsHistory.begin());
                auto sorted = fpsHistory;
                std::ranges::sort(sorted);
                onePercentLow_ = sorted[std::min(sorted.size() - 1,
                    static_cast<size_t>(std::floor(sorted.size() * 0.01)))];
                targetProcessId_ = foregroundProcessId_.load();
                gameFpsActive_ = true;
            } else {
                gameFpsActive_ = false;
                targetProcessId_ = 0;
                gpuBusyTotal_ = 0.0;
                latencyTotal_ = 0.0;
                const auto frames = counter();
                fps_ = seconds > 0 ? static_cast<double>(frames - previousFrames) / seconds : 0.0;
                frameTimeMs_ = fps_ > 0 ? 1000.0 / fps_.load() : 0.0;
                onePercentLow_ = fps_.load();
                gpuBusyPercent_ = 0.0;
                renderLatencyMs_ = 0.0;
                previousFrames = frames;
            }
            FILETIME currentIdle{}, currentKernel{}, currentUser{};
            if (GetSystemTimes(&currentIdle, &currentKernel, &currentUser)) {
                const uint64_t idleValue = fileTimeValue(currentIdle);
                const uint64_t totalValue = fileTimeValue(currentKernel) + fileTimeValue(currentUser);
                const uint64_t totalDelta = totalValue - previousTotal;
                const uint64_t idleDelta = idleValue - previousIdle;
                cpuPercent_ = totalDelta ? 100.0 * (totalDelta - idleDelta) / totalDelta : 0.0;
                previousIdle = idleValue;
                previousTotal = totalValue;
            }
            previousTime = now;
        }
    });
}

bool FpsMonitor::launchPresentMon() {
    wchar_t modulePath[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, modulePath, MAX_PATH)) return false;
    const auto presentMon = std::filesystem::path(modulePath).parent_path() / L"PresentMon.exe";
    if (!std::filesystem::exists(presentMon)) return false;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE writePipe{};
    if (!CreatePipe(&presentMonOutput_, &writePipe, &security, 256 * 1024)) return false;
    SetHandleInformation(presentMonOutput_, HANDLE_FLAG_INHERIT, 0);
    std::wstring command = L"\"" + presentMon.wstring() +
        L"\" --output_stdout --no_console_stats --v2_metrics --exclude_dropped "
        L"--session_name KanekistReplayFPS --stop_existing_session";
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const bool created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, presentMon.parent_path().c_str(), &startup, &process);
    CloseHandle(writePipe);
    if (!created) {
        CloseHandle(presentMonOutput_);
        presentMonOutput_ = nullptr;
        return false;
    }
    CloseHandle(process.hThread);
    presentMonProcess_ = process.hProcess;
    return true;
}

void FpsMonitor::readPresentMon() {
    std::string pending;
    std::array<char, 64 * 1024> buffer{};
    bool headerFound = false;
    size_t pidColumn = SIZE_MAX;
    size_t gpuBusyColumn = SIZE_MAX;
    size_t latencyColumn = SIZE_MAX;
    while (running_) {
        DWORD read{};
        if (!ReadFile(presentMonOutput_, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) ||
            read == 0) break;
        pending.append(buffer.data(), read);
        size_t newline{};
        while ((newline = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            const auto fields = splitCsv(line);
            if (!headerFound) {
                for (size_t i = 0; i < fields.size(); ++i) {
                    if (fields[i].find("ProcessID") != std::string::npos) pidColumn = i;
                    if (fields[i].find("GPUBusy") != std::string::npos) gpuBusyColumn = i;
                    if (fields[i].find("DisplayLatency") != std::string::npos ||
                        fields[i].find("GPULatency") != std::string::npos) latencyColumn = i;
                }
                headerFound = pidColumn != SIZE_MAX;
                continue;
            }
            try {
                if (pidColumn >= fields.size() ||
                    std::stoul(fields[pidColumn]) != foregroundProcessId_.load()) continue;
                ++presentCount_;
                if (gpuBusyColumn < fields.size()) {
                    double current = gpuBusyTotal_.load();
                    while (!gpuBusyTotal_.compare_exchange_weak(current,
                        current + std::stod(fields[gpuBusyColumn]))) {}
                }
                if (latencyColumn < fields.size()) {
                    double current = latencyTotal_.load();
                    while (!latencyTotal_.compare_exchange_weak(current,
                        current + std::stod(fields[latencyColumn]))) {}
                }
            } catch (...) {
            }
        }
    }
}

void FpsMonitor::stop() {
    running_ = false;
    if (presentMonProcess_) TerminateProcess(presentMonProcess_, 0);
    if (presentMonOutput_) CloseHandle(presentMonOutput_);
    presentMonOutput_ = nullptr;
    if (readerThread_.joinable()) readerThread_.join();
    if (presentMonProcess_) CloseHandle(presentMonProcess_);
    presentMonProcess_ = nullptr;
    if (thread_.joinable()) thread_.join();
}

FpsOverlay::~FpsOverlay() {
    if (window_) DestroyWindow(window_);
}

bool FpsOverlay::ensureFactories() {
    if (factory_ && writeFactory_ && renderTarget_) return true;
    D2D1_FACTORY_OPTIONS options{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, options, factory_.GetAddressOf()))) {
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(writeFactory_.GetAddressOf())))) {
        return false;
    }
    const auto properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    return SUCCEEDED(factory_->CreateDCRenderTarget(&properties, &renderTarget_));
}

bool FpsOverlay::create(HINSTANCE instance, const FpsMonitor* monitor, HWND owner,
                        const Settings* settings) {
    monitor_ = monitor;
    owner_ = owner;
    settings_ = settings;
    if (!ensureFactories()) return false;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = instance;
    wc.lpfnWndProc = windowProc;
    wc.lpszClassName = L"KanekistFpsOverlay";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP,
        24, 24, 8, 8, nullptr, nullptr, instance, this);
    if (!window_) return false;
    SetTimer(window_, 1, 250, nullptr);
    return true;
}

void FpsOverlay::show(bool value) {
    visibleRequested_ = value;
    if (!value) ShowWindow(window_, SW_HIDE);
    else present();
}

void FpsOverlay::refresh() {
    present();
}

void FpsOverlay::setGameWindow(HWND game) {
    gameWindow_ = game && IsWindow(game) ? game : nullptr;
    if (window_ && visibleRequested_) pinOverlayToGame(window_, nullptr);
}

void FpsOverlay::present() {
    if (!window_ || !visibleRequested_ || !ensureFactories() || !monitor_ || !settings_) return;

    const auto text = formatHudText(*settings_, *monitor_);
    const float fontSize = static_cast<float>(std::clamp(settings_->hudFontSize, 12u, 72u));
    const int outline = static_cast<int>(std::clamp(settings_->hudOutlineThickness, 0u, 6u));
    const bool condensed = settings_->hudFontName == L"Bahnschrift";
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(writeFactory_->CreateTextFormat(settings_->hudFontName.c_str(), nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        condensed ? DWRITE_FONT_STRETCH_CONDENSED : DWRITE_FONT_STRETCH_NORMAL,
        fontSize, L"en-us", &format))) {
        writeFactory_->CreateTextFormat(L"Bahnschrift", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_CONDENSED, fontSize, L"en-us", &format);
    }
    if (!format) return;
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(writeFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
        format.Get(), 2400.0f, 240.0f, &layout))) return;
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    const int pad = std::max(8, outline + 6);
    const int width = std::max(8, static_cast<int>(std::ceil(metrics.width)) + pad * 2);
    const int height = std::max(8, static_cast<int>(std::ceil(metrics.height)) + pad * 2);

    HMONITOR monitor = MonitorFromWindow(
        gameWindow_ ? gameWindow_ : (window_ ? window_ : owner_), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    GetMonitorInfoW(monitor, &monitorInfo);
    const RECT& area = monitorInfo.rcMonitor;
    const int padX = static_cast<int>(settings_->hudOffsetX);
    const int padY = static_cast<int>(settings_->hudOffsetY);
    const uint32_t anchor = settings_->overlayCorner % 9;
    const int column = static_cast<int>(anchor % 3);
    const int row = static_cast<int>(anchor / 3);
    const int x = column == 0 ? area.left + padX :
                  column == 2 ? area.right - width - padX :
                  area.left + (area.right - area.left - width) / 2;
    const int y = row == 0 ? area.top + padY :
                  row == 2 ? area.bottom - height - padY :
                  area.top + (area.bottom - area.top - height) / 2;
    POINT pos{x, y};

    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = width;
    bitmap.bmiHeader.biHeight = -height;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    void* bits{};
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(memory, &bitmap, DIB_RGB_COLORS, &bits, nullptr, 0);
    const auto old = SelectObject(memory, dib);
    RECT bind{0, 0, width, height};
    if (SUCCEEDED(renderTarget_->BindDC(memory, &bind))) {
        renderTarget_->BeginDraw();
        renderTarget_->Clear(D2D1::ColorF(0, 0, 0, 0));
        ComPtr<ID2D1SolidColorBrush> brush;
        renderTarget_->CreateSolidColorBrush(hudColor(0), &brush);
        const auto capsule = D2D1::RoundedRect(
            D2D1::RectF(1.0f, 1.0f, static_cast<float>(width) - 1.0f, static_cast<float>(height) - 1.0f),
            static_cast<float>(height) * 0.45f, static_cast<float>(height) * 0.45f);
        brush->SetColor(D2D1::ColorF(0.02f, 0.04f, 0.05f, 0.48f));
        renderTarget_->FillRoundedRectangle(capsule, brush.Get());
        brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f));
        renderTarget_->DrawRoundedRectangle(capsule, brush.Get(), 1.0f);
        const auto originText = D2D1::Point2F(static_cast<float>(pad) - metrics.left,
                                              static_cast<float>(pad) - metrics.top);
        if (outline > 0) {
            brush->SetColor(hudColor(settings_->hudOutlineColor, 0.92f));
            for (int dx = -outline; dx <= outline; ++dx) {
                for (int dy = -outline; dy <= outline; ++dy) {
                    if (dx == 0 && dy == 0) continue;
                    if (dx * dx + dy * dy > outline * outline) continue;
                    renderTarget_->DrawTextLayout(
                        D2D1::Point2F(originText.x + dx, originText.y + dy),
                        layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
                }
            }
        } else {
            brush->SetColor(D2D1::ColorF(0, 0, 0, 0.22f));
            const D2D1_POINT_2F halo[] = {{ -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }};
            for (const auto offset : halo) {
                renderTarget_->DrawTextLayout(
                    D2D1::Point2F(originText.x + offset.x, originText.y + offset.y),
                    layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
            }
            brush->SetColor(D2D1::ColorF(0, 0, 0, 0.40f));
            renderTarget_->DrawTextLayout(D2D1::Point2F(originText.x + 1.0f, originText.y + 1.5f),
                layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
        }
        brush->SetColor(hudColor(settings_->hudTextColor));
        renderTarget_->DrawTextLayout(originText, layout.Get(), brush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
        renderTarget_->EndDraw();
    }
    SIZE size{width, height};
    POINT source{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(window_, screen, &pos, &size, memory, &source, 0, &blend, ULW_ALPHA);
    pinOverlayToGame(window_, nullptr);
    ShowWindow(window_, SW_SHOWNOACTIVATE);
    SelectObject(memory, old);
    DeleteObject(dib);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK FpsOverlay::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<FpsOverlay*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<FpsOverlay*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    switch (message) {
    case WM_TIMER:
        if (self) {
            if (self->visibleRequested_) self->present();
            else ShowWindow(self->window_, SW_HIDE);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(window, &ps);
        EndPaint(window, &ps);
        return 0;
    }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace kanekist
