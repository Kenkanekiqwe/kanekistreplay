#include "overlay_menu.h"

#include "core.h"
#include "game_window.h"

#include <Windowsx.h>
#include <timeapi.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <string_view>

using Microsoft::WRL::ComPtr;

namespace kanekist {
namespace {

constexpr float MenuLeft = 28.0f;
constexpr float MenuTop = 118.0f;
constexpr float ColWidth = 360.0f;
constexpr float ColGap = 20.0f;
constexpr float RowHeight = 58.0f;
constexpr float ContentWidth = 796.0f;
constexpr float ContentHeight = 712.0f;
constexpr float CloseLeft = 744.0f;
constexpr float CloseTop = 20.0f;
constexpr float CloseSize = 32.0f;
constexpr UINT AnimationTimer = 1;
constexpr float ShowMs = 420.0f;
constexpr float HideMs = 340.0f;

constexpr const wchar_t* HudAnchorNames[] = {
    L"слева сверху", L"сверху", L"справа сверху",
    L"слева", L"центр", L"справа",
    L"слева снизу", L"снизу", L"справа снизу"
};

D2D1_COLOR_F color(uint32_t rgb, float alpha = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f,
                        (rgb & 0xff) / 255.0f, alpha);
}

void drawText(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush, IDWriteTextFormat* format,
              const D2D1_RECT_F& rect, std::wstring_view text) {
    if (!format || text.empty()) return;
    target->DrawTextW(text.data(), static_cast<UINT32>(text.size()), format, rect, brush);
}

const wchar_t* rowIcon(int index) {
    switch (index) {
    case 0: return L"\uE7C8";
    case 1: return L"\uE72C";
    case 2: return L"\uE74E";
    case 3: return L"\uE722";
    case 4: return L"\uE8B7";
    case 5: return L"\uE720";
    case 6: return L"\uECAD";
    case 7: return L"\uE7F4";
    case 8: return L"\uE916";
    case 9: return L"\uE7B8";
    case 10: return L"\uE790";
    case 11: return L"\uE8E5";
    case 12: return L"\uE8A5";
    case 13: return L"\uE916";
    case 14: return L"\uE8D2";
    case 15: return L"\uE8A3";
    default: return L"";
    }
}

std::wstring rowTitle(int index) {
    switch (index) {
    case 0: return L"Запись";
    case 1: return L"Instant Replay";
    case 2: return L"Сохранить повтор";
    case 3: return L"Скриншот";
    case 4: return L"Галерея";
    case 5: return L"Микрофон";
    case 6: return L"FPS HUD";
    case 7: return L"Разрешение";
    case 8: return L"FPS записи";
    case 9: return L"Позиция HUD";
    case 10: return L"Цвет HUD";
    case 11: return L"Обводка HUD";
    case 12: return L"Шаблон HUD";
    case 13: return L"Длительность Replay";
    case 14: return L"Шрифт HUD";
    case 15: return L"Размер HUD";
    default: return L"";
    }
}

std::wstring rowValue(int index, const PerformanceSnapshot& snapshot) {
    switch (index) {
    case 0: return snapshot.recording ? L"идёт" : L"стоп";
    case 1: return snapshot.replayEnabled ? L"вкл" : L"выкл";
    case 5: return snapshot.microphoneEnabled ? L"вкл" : L"выкл";
    case 6: return snapshot.hudEnabled ? L"вкл" : L"выкл";
    case 7: {
        std::wstring label;
        if (snapshot.encodeHeight <= 720) label = L"720p";
        else if (snapshot.encodeHeight <= 1080) label = L"1080p";
        else if (snapshot.encodeHeight <= 1440) label = L"1440p";
        else if (snapshot.encodeHeight <= 2160) label = L"4K";
        else label = L"экран";
        return label + L" · " + videoCodecLabel(snapshot.encodeCodec);
    }
    case 8: return std::to_wstring(snapshot.encodeFps);
    case 9: return HudAnchorNames[snapshot.hudCorner % 9];
    case 10: return L"сменить";
    case 11: return snapshot.hudOutline == 0 ? L"выкл" : L"вкл";
    case 12: return L"сменить";
    case 13: return std::to_wstring(std::max(1u, snapshot.replaySeconds / 60)) + L" мин";
    case 14: return snapshot.hudFontName.empty() ? L"по умолчанию" : snapshot.hudFontName;
    case 15: return std::to_wstring(snapshot.hudFontSize) + L" px";
    default: return L"";
    }
}

bool rowOn(int index, const PerformanceSnapshot& snapshot) {
    return (index == 0 && snapshot.recording) || (index == 1 && snapshot.replayEnabled) ||
           (index == 5 && snapshot.microphoneEnabled) || (index == 6 && snapshot.hudEnabled);
}

float smootherstep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

} // namespace

OverlayMenu::~OverlayMenu() {
    if (timerPeriodSet_) timeEndPeriod(1);
    releaseBuffers();
    if (window_) DestroyWindow(window_);
    if (dimmer_) DestroyWindow(dimmer_);
}

bool OverlayMenu::create(HINSTANCE instance, HWND owner, ActionHandler actions, MetricsProvider metrics,
                         POINT panelPos, MoveHandler moved) {
    instance_ = instance;
    owner_ = owner;
    actions_ = std::move(actions);
    metrics_ = std::move(metrics);
    moved_ = std::move(moved);
    savedPos_ = panelPos;

    D2D1_FACTORY_OPTIONS options{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, options,
        factory_.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(writeFactory_.GetAddressOf())))) return false;

    writeFactory_->CreateTextFormat(L"Segoe UI Variable Display", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        26.0f, L"ru-RU", &titleFormat_);
    if (!titleFormat_) {
        writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 26.0f, L"ru-RU", &titleFormat_);
    }
    writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"ru-RU", &bodyFormat_);
    writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"ru-RU", &valueFormat_);
    writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"ru-RU", &captionFormat_);
    writeFactory_->CreateTextFormat(L"Segoe MDL2 Assets", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"en-US", &iconFormat_);
    if (!iconFormat_) {
        writeFactory_->CreateTextFormat(L"Segoe Fluent Icons", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"en-US", &iconFormat_);
    }
    if (titleFormat_) titleFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (bodyFormat_) bodyFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (valueFormat_) {
        valueFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        valueFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    if (captionFormat_) captionFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (iconFormat_) {
        iconFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    WNDCLASSEXW panelClass{sizeof(panelClass)};
    panelClass.hInstance = instance_;
    panelClass.lpfnWndProc = windowProc;
    panelClass.lpszClassName = L"KanekistGameOverlayMenu";
    panelClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&panelClass);

    WNDCLASSEXW dimmerClass{sizeof(dimmerClass)};
    dimmerClass.hInstance = instance_;
    dimmerClass.lpfnWndProc = dimmerProc;
    dimmerClass.lpszClassName = L"KanekistOverlayDimmer";
    dimmerClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    dimmerClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassExW(&dimmerClass);

    dimmer_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        dimmerClass.lpszClassName, L"", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, instance_, this);
    if (dimmer_) {
        SetLayeredWindowAttributes(dimmer_, 0, 0, LWA_ALPHA);
        excludeWindowFromCapture(dimmer_);
    }
    window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        panelClass.lpszClassName, L"Kanekist Overlay", WS_POPUP, 0, 0, 100, 100,
        nullptr, nullptr, instance_, this);
    if (window_) excludeWindowFromCapture(window_);
    return window_ != nullptr && dimmer_ != nullptr;
}

bool OverlayMenu::createDeviceResources() {
    if (target_) return true;
    const auto properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(factory_->CreateDCRenderTarget(&properties, &target_))) return false;
    if (FAILED(target_->CreateSolidColorBrush(color(0xffffff), &brush_))) return false;
    const auto stroke = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
        D2D1_LINE_JOIN_ROUND);
    factory_->CreateStrokeStyle(stroke, nullptr, 0, &stroke_);
    return true;
}

void OverlayMenu::discardDeviceResources() {
    stroke_.Reset();
    brush_.Reset();
    target_.Reset();
}

void OverlayMenu::layoutForMonitor() {
    const auto game = resolveGameTarget();
    previousForeground_ = game.window ? game.window : GetForegroundWindow();
    HMONITOR monitor = game.monitor ? game.monitor :
        MonitorFromWindow(previousForeground_ ? previousForeground_ : owner_,
        MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    monitorRect_ = info.rcMonitor;
    dpiScale_ = GetDpiForWindow(owner_ ? owner_ : window_) / 96.0f;
    if (dpiScale_ <= 0) dpiScale_ = 1.0f;

    const int width = static_cast<int>(ContentWidth * dpiScale_);
    const int height = static_cast<int>(ContentHeight * dpiScale_);
    const int monitorW = monitorRect_.right - monitorRect_.left;
    const int monitorH = monitorRect_.bottom - monitorRect_.top;
    int x = monitorRect_.left + (monitorW - width) / 2;
    int y = monitorRect_.top + (monitorH - height) / 2;
    if (savedPos_.x != -1 && savedPos_.y != -1) {
        x = savedPos_.x;
        y = savedPos_.y;
    }
    x = std::clamp<LONG>(x, monitorRect_.left, monitorRect_.right - width);
    y = std::clamp<LONG>(y, monitorRect_.top, monitorRect_.bottom - height);
    panelRect_ = {x, y, x + width, y + height};

    SetWindowPos(dimmer_, HWND_TOPMOST, monitorRect_.left, monitorRect_.top, monitorW, monitorH,
        SWP_SHOWWINDOW | SWP_NOACTIVATE);
    SetWindowPos(window_, HWND_TOPMOST, panelRect_.left, panelRect_.top, width, height,
        SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void OverlayMenu::startPhase(Phase phase) {
    phase_ = phase;
    animStart_ = std::chrono::steady_clock::now();
    contentDirty_ = true;
    if (!timerPeriodSet_) {
        timeBeginPeriod(1);
        timerPeriodSet_ = true;
    }
    SetTimer(window_, AnimationTimer, 8, nullptr);
}

void OverlayMenu::toggle() {
    if (phase_ == Phase::Shown || phase_ == Phase::In) {
        close();
        return;
    }
    const auto game = resolveGameTarget();
    if (game.window && (game.exclusiveD3d || game.fullscreen)) {
        tryMakeBorderless(game.window);
    }
    layoutForMonitor();
    ShowWindow(dimmer_, SW_SHOWNA);
    ShowWindow(window_, SW_SHOWNA);
    pinOverlayToGame(dimmer_, nullptr);
    pinOverlayToGame(window_, nullptr);
    SetForegroundWindow(window_);
    startPhase(Phase::In);
    tickAnimation();
}

void OverlayMenu::close() {
    if (phase_ == Phase::Hidden || phase_ == Phase::Out) return;
    startPhase(Phase::Out);
    tickAnimation();
}

float OverlayMenu::eased() const {
    const auto elapsed = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - animStart_).count();
    if (phase_ == Phase::In) return smootherstep(elapsed / ShowMs);
    if (phase_ == Phase::Out) return 1.0f - smootherstep(elapsed / HideMs);
    if (phase_ == Phase::Shown) return 1.0f;
    return 0.0f;
}

void OverlayMenu::tickAnimation() {
    const float t = eased();
    anim_ = t;
    presentDimmer(0.55f * t);
    blitAnimated();
    if (phase_ == Phase::In && t >= 0.999f) {
        phase_ = Phase::Shown;
        KillTimer(window_, AnimationTimer);
        if (timerPeriodSet_) {
            timeEndPeriod(1);
            timerPeriodSet_ = false;
        }
        anim_ = 1.0f;
        presentDimmer(0.55f);
        blitAnimated();
    } else if (phase_ == Phase::Out && t <= 0.001f) {
        phase_ = Phase::Hidden;
        KillTimer(window_, AnimationTimer);
        if (timerPeriodSet_) {
            timeEndPeriod(1);
            timerPeriodSet_ = false;
        }
        ShowWindow(window_, SW_HIDE);
        ShowWindow(dimmer_, SW_HIDE);
        if (previousForeground_ && IsWindow(previousForeground_)) SetForegroundWindow(previousForeground_);
    }
}

void OverlayMenu::refresh() {
    if (phase_ == Phase::Hidden) return;
    contentDirty_ = true;
    blitAnimated();
}

void OverlayMenu::showToast(std::wstring message) {
    toast_ = std::move(message);
    toastUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    if (phase_ != Phase::Hidden) {
        contentDirty_ = true;
        blitAnimated();
    }
}

D2D1_RECT_F OverlayMenu::cardRect(int index) const {
    const bool right = index >= 7;
    const int row = right ? index - 7 : index;
    const float left = MenuLeft + (right ? ColWidth + ColGap : 0.0f);
    const float top = MenuTop + static_cast<float>(row) * RowHeight;
    return D2D1::RectF(left, top, left + ColWidth, top + RowHeight);
}

int OverlayMenu::hitTestCard(float x, float y, float scale) {
    if (scale <= 0) return -1;
    x /= scale;
    y /= scale;
    if (y < MenuTop) return -1;
    const int row = static_cast<int>((y - MenuTop) / RowHeight);
    if (row < 0) return -1;
    const float rightLeft = MenuLeft + ColWidth + ColGap;
    if (x >= MenuLeft && x <= MenuLeft + ColWidth && row < 7) return row;
    if (x >= rightLeft && x <= rightLeft + ColWidth && row < 9) return 7 + row;
    return -1;
}

bool OverlayMenu::inHeader(float x, float y) const {
    return x >= 0 && x <= ContentWidth && y >= 0 && y < MenuTop;
}

bool OverlayMenu::inClose(float x, float y) const {
    return x >= CloseLeft && x <= CloseLeft + CloseSize &&
           y >= CloseTop && y <= CloseTop + CloseSize;
}

void OverlayMenu::invokeCard(int card) {
    static constexpr OverlayAction actions[] = {
        OverlayAction::toggleRecording, OverlayAction::toggleReplay, OverlayAction::saveReplay,
        OverlayAction::screenshot, OverlayAction::gallery, OverlayAction::toggleMicrophone,
        OverlayAction::toggleFpsHud, OverlayAction::cycleResolution, OverlayAction::cycleEncodeFps,
        OverlayAction::cycleHudPosition, OverlayAction::cycleHudColor, OverlayAction::cycleHudOutline,
        OverlayAction::cycleHudTemplate, OverlayAction::cycleReplayDuration,
        OverlayAction::cycleHudFont, OverlayAction::cycleHudSize
    };
    if (card >= 0 && card < 16 && actions_) actions_(actions[card]);
}

void OverlayMenu::presentDimmer(float alpha) {
    if (!dimmer_) return;
    const BYTE value = static_cast<BYTE>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
    SetLayeredWindowAttributes(dimmer_, 0, value, LWA_ALPHA);
}

void OverlayMenu::releaseBuffers() {
    if (contentDc_ && contentOld_) SelectObject(contentDc_, contentOld_);
    if (presentDc_ && presentOld_) SelectObject(presentDc_, presentOld_);
    if (contentBmp_) DeleteObject(contentBmp_);
    if (presentBmp_) DeleteObject(presentBmp_);
    if (contentDc_) DeleteDC(contentDc_);
    if (presentDc_) DeleteDC(presentDc_);
    contentDc_ = presentDc_ = nullptr;
    contentBmp_ = presentBmp_ = contentOld_ = presentOld_ = nullptr;
    presentBits_ = nullptr;
    bufferWidth_ = bufferHeight_ = 0;
}

void OverlayMenu::ensureBuffers(int width, int height) {
    if (contentDc_ && width == bufferWidth_ && height == bufferHeight_) return;
    releaseBuffers();
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    contentDc_ = CreateCompatibleDC(screen);
    presentDc_ = CreateCompatibleDC(screen);
    void* contentBits{};
    contentBmp_ = CreateDIBSection(contentDc_, &info, DIB_RGB_COLORS, &contentBits, nullptr, 0);
    presentBmp_ = CreateDIBSection(presentDc_, &info, DIB_RGB_COLORS, &presentBits_, nullptr, 0);
    if (contentBmp_) contentOld_ = static_cast<HBITMAP>(SelectObject(contentDc_, contentBmp_));
    if (presentBmp_) presentOld_ = static_cast<HBITMAP>(SelectObject(presentDc_, presentBmp_));
    ReleaseDC(nullptr, screen);
    bufferWidth_ = width;
    bufferHeight_ = height;
    contentDirty_ = true;
}

void OverlayMenu::renderContent() {
    if (!createDeviceResources() || !contentDc_) return;
    RECT bind{0, 0, bufferWidth_, bufferHeight_};
    if (SUCCEEDED(target_->BindDC(contentDc_, &bind))) {
        target_->SetDpi(96.0f * dpiScale_, 96.0f * dpiScale_);
        render(target_.Get());
        contentDirty_ = false;
    }
}

void OverlayMenu::blitAnimated() {
    if (!window_ || phase_ == Phase::Hidden) return;
    RECT client{};
    GetClientRect(window_, &client);
    const int width = std::max(1L, client.right);
    const int height = std::max(1L, client.bottom);
    ensureBuffers(width, height);
    if (!contentDc_ || !presentDc_) return;
    if (contentDirty_) renderContent();

    const float t = std::clamp(anim_, 0.0f, 1.0f);
    const float scale = 0.78f + 0.22f * t;
    if (presentBits_) {
        std::memset(presentBits_, 0, static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    }
    const int dw = std::max(1, static_cast<int>(width * scale + 0.5f));
    const int dh = std::max(1, static_cast<int>(height * scale + 0.5f));
    const int ox = (width - dw) / 2;
    const int oy = (height - dh) / 2;
    SetStretchBltMode(presentDc_, COLORONCOLOR);
    StretchBlt(presentDc_, ox, oy, dw, dh, contentDc_, 0, 0, width, height, SRCCOPY);

    HDC screen = GetDC(nullptr);
    POINT pos{};
    SIZE size{width, height};
    POINT source{0, 0};
    const BYTE alpha = static_cast<BYTE>(std::clamp(t * t * (3.0f - 2.0f * t), 0.0f, 1.0f) * 255.0f);
    BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(window_, screen, nullptr, &size, presentDc_, &source, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

void OverlayMenu::present() {
    contentDirty_ = true;
    blitAnimated();
}

void OverlayMenu::render(ID2D1RenderTarget* target) {
    const auto snapshot = metrics_ ? metrics_() : PerformanceSnapshot{};
    target->BeginDraw();
    target->SetTransform(D2D1::Matrix3x2F::Identity());
    target->Clear(D2D1::ColorF(0, 0, 0, 0));

    const auto panel = D2D1::RoundedRect(D2D1::RectF(0, 0, ContentWidth, ContentHeight), 22, 22);
    brush_->SetColor(color(0x121416, 0.97f));
    target->FillRoundedRectangle(panel, brush_.Get());
    brush_->SetColor(color(0xffffff, 0.08f));
    target->DrawRoundedRectangle(panel, brush_.Get(), 1.0f);

    brush_->SetColor(color(0xffffff, 0.22f));
    for (int i = 0; i < 6; ++i) {
        const float x = 22.0f + (i % 2) * 6.0f;
        const float y = 28.0f + (i / 2) * 6.0f;
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), 1.6f, 1.6f), brush_.Get());
    }

    brush_->SetColor(color(0xf4f6f8));
    drawText(target, brush_.Get(), titleFormat_.Get(), D2D1::RectF(48, 22, 360, 56), L"Kanekist");
    brush_->SetColor(color(0x8a8f98));
    const auto hint = snapshot.overlayHotkey.empty() ? std::wstring(L"Alt+K") : snapshot.overlayHotkey;
    drawText(target, brush_.Get(), captionFormat_.Get(), D2D1::RectF(48, 60, 420, 84),
        hint + L"  ·  перетащи  ·  Esc закрыть");

    const bool live = snapshot.recording;
    brush_->SetColor(live ? color(0xff5c6a) : (snapshot.replayEnabled ? color(0x5ee9a3) : color(0x8a8f98)));
    drawText(target, brush_.Get(), valueFormat_.Get(), D2D1::RectF(480, 28, 720, 58),
        live ? L"запись" : (snapshot.replayEnabled ? L"replay" : L"готово"));

    const auto closeRect = D2D1::RectF(CloseLeft, CloseTop, CloseLeft + CloseSize, CloseTop + CloseSize);
    brush_->SetColor(color(0xffffff, hotCard_ == -2 ? 0.16f : 0.08f));
    target->FillRoundedRectangle(D2D1::RoundedRect(closeRect, 8, 8), brush_.Get());
    brush_->SetColor(color(0xf4f6f8, 0.92f));
    const float pad = 10.0f;
    const D2D1_POINT_2F a{closeRect.left + pad, closeRect.top + pad};
    const D2D1_POINT_2F b{closeRect.right - pad, closeRect.bottom - pad};
    const D2D1_POINT_2F c{closeRect.right - pad, closeRect.top + pad};
    const D2D1_POINT_2F d{closeRect.left + pad, closeRect.bottom - pad};
    target->DrawLine(a, b, brush_.Get(), 1.8f, stroke_.Get());
    target->DrawLine(c, d, brush_.Get(), 1.8f, stroke_.Get());

    brush_->SetColor(color(0x8a8f98));
    drawText(target, brush_.Get(), captionFormat_.Get(), D2D1::RectF(36, 94, 240, 114), L"ЗАХВАТ");
    drawText(target, brush_.Get(), captionFormat_.Get(),
        D2D1::RectF(MenuLeft + ColWidth + ColGap + 8, 94, 760, 114), L"НАСТРОЙКИ");

    for (int i = 0; i < 16; ++i) {
        const auto rect = cardRect(i);
        const bool hot = i == hotCard_;
        const bool on = rowOn(i, snapshot);
        if (hot) {
            brush_->SetColor(color(0xffffff, 0.07f));
            target->FillRoundedRectangle(D2D1::RoundedRect(rect, 10, 10), brush_.Get());
        }
        const auto accent = on ? (i == 0 && live ? color(0xff8a96) : color(0x5ee9a3)) : color(0xf4f6f8);
        brush_->SetColor(color(0xffffff, 0.05f));
        target->FillRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(rect.left + 10, rect.top + 14, rect.left + 42, rect.bottom - 14), 8, 8), brush_.Get());
        brush_->SetColor(accent);
        drawText(target, brush_.Get(), iconFormat_.Get(),
            D2D1::RectF(rect.left + 10, rect.top + 12, rect.left + 42, rect.bottom - 12), rowIcon(i));
        drawText(target, brush_.Get(), bodyFormat_.Get(),
            D2D1::RectF(rect.left + 52, rect.top + 16, rect.right - 130, rect.bottom - 12), rowTitle(i));
        const auto value = rowValue(i, snapshot);
        if (!value.empty()) {
            brush_->SetColor(on ? (i == 0 && live ? color(0xff8a96) : color(0x5ee9a3)) : color(0x8a8f98));
            drawText(target, brush_.Get(), valueFormat_.Get(),
                D2D1::RectF(rect.right - 168, rect.top + 18, rect.right - 16, rect.bottom - 12), value);
        }
    }

    if (!toast_.empty() && std::chrono::steady_clock::now() < toastUntil_) {
        brush_->SetColor(color(0x1c3a28, 0.96f));
        target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(28, 656, 768, 694), 10, 10), brush_.Get());
        brush_->SetColor(color(0x8ef0b0));
        drawText(target, brush_.Get(), bodyFormat_.Get(), D2D1::RectF(44, 664, 752, 688), toast_);
    }

    const HRESULT hr = target->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) discardDeviceResources();
}

LRESULT CALLBACK OverlayMenu::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<OverlayMenu*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<OverlayMenu*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->dispatch(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK OverlayMenu::dimmerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<OverlayMenu*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<OverlayMenu*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, wParam, lParam);
    if (message == WM_LBUTTONUP) {
        self->close();
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT OverlayMenu::dispatch(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_TIMER:
        if (wParam == AnimationTimer) tickAnimation();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(window_, &ps);
        present();
        EndPaint(window_, &ps);
        return 0;
    }
    case WM_DPICHANGED: {
        dpiScale_ = HIWORD(wParam) / 96.0f;
        const auto* rect = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(window_, nullptr, rect->left, rect->top, rect->right - rect->left,
            rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (phase_ != Phase::Hidden) present();
        return 0;
    }
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
        TrackMouseEvent(&track);
        const float x = GET_X_LPARAM(lParam) / dpiScale_;
        const float y = GET_Y_LPARAM(lParam) / dpiScale_;
        if (dragging_) {
            POINT now{};
            GetCursorPos(&now);
            const int dx = now.x - dragMouse_.x;
            const int dy = now.y - dragMouse_.y;
            RECT current{};
            GetWindowRect(window_, &current);
            const int width = current.right - current.left;
            const int height = current.bottom - current.top;
            int nx = current.left + dx;
            int ny = current.top + dy;
            nx = std::clamp<LONG>(nx, monitorRect_.left, monitorRect_.right - width);
            ny = std::clamp<LONG>(ny, monitorRect_.top, monitorRect_.bottom - height);
            SetWindowPos(window_, nullptr, nx, ny, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            dragMouse_ = now;
            return 0;
        }
        int next = hitTestCard(x, y, 1.0f);
        if (inClose(x, y)) next = -2;
        if (next != hotCard_) {
            hotCard_ = next;
            present();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (hotCard_ != -1) {
            hotCard_ = -1;
            present();
        }
        return 0;
    case WM_LBUTTONDOWN: {
        const float x = GET_X_LPARAM(lParam) / dpiScale_;
        const float y = GET_Y_LPARAM(lParam) / dpiScale_;
        pressedCard_ = hotCard_;
        if (inClose(x, y)) {
            close();
            return 0;
        }
        if (inHeader(x, y) && !inClose(x, y)) {
            dragging_ = true;
            GetCursorPos(&dragMouse_);
            SetCapture(window_);
            return 0;
        }
        SetCapture(window_);
        return 0;
    }
    case WM_LBUTTONUP:
        ReleaseCapture();
        if (dragging_) {
            dragging_ = false;
            RECT current{};
            GetWindowRect(window_, &current);
            savedPos_ = {current.left, current.top};
            if (moved_) moved_(current.left, current.top);
            return 0;
        }
        if (pressedCard_ >= 0 && pressedCard_ == hotCard_) invokeCard(pressedCard_);
        pressedCard_ = -1;
        present();
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) close();
        return 0;
    case WM_CLOSE:
        close();
        return 0;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

} // namespace kanekist
