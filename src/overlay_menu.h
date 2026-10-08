#pragma once

#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <chrono>
#include <functional>
#include <string>

namespace kanekist {

enum class OverlayAction {
    toggleRecording,
    toggleReplay,
    saveReplay,
    screenshot,
    gallery,
    toggleMicrophone,
    toggleFpsHud,
    cycleResolution,
    cycleEncodeFps,
    cycleHudPosition,
    cycleHudColor,
    cycleHudOutline,
    cycleHudTemplate,
    cycleReplayDuration,
    cycleHudFont,
    cycleHudSize
};

struct PerformanceSnapshot {
    double fps{};
    double onePercentLow{};
    double frameTimeMs{};
    double cpuPercent{};
    double gpuBusyPercent{};
    double renderLatencyMs{};
    bool gameMetrics{};
    bool recording{};
    bool replayEnabled{};
    bool microphoneEnabled{};
    bool hudEnabled{};
    uint32_t encodeWidth{};
    uint32_t encodeHeight{};
    uint32_t encodeFps{};
    uint32_t encodeCodec{};
    uint32_t hudCorner{};
    uint32_t hudColor{};
    uint32_t hudOutline{};
    uint32_t replaySeconds{};
    uint32_t hudFontSize{};
    std::wstring hudFontName;
    std::wstring overlayHotkey;
};

class OverlayMenu {
public:
    using ActionHandler = std::function<void(OverlayAction)>;
    using MetricsProvider = std::function<PerformanceSnapshot()>;
    using MoveHandler = std::function<void(int x, int y)>;

    OverlayMenu() = default;
    ~OverlayMenu();
    bool create(HINSTANCE instance, HWND owner, ActionHandler actions, MetricsProvider metrics,
                POINT panelPos, MoveHandler moved);
    void toggle();
    void close();
    void refresh();
    void showToast(std::wstring message);
    [[nodiscard]] bool visible() const noexcept { return phase_ != Phase::Hidden; }
    [[nodiscard]] HWND window() const noexcept { return window_; }

    static int hitTestCard(float x, float y, float scale);

private:
    enum class Phase { Hidden, In, Shown, Out };

    bool createDeviceResources();
    void discardDeviceResources();
    void present();
    void presentDimmer(float alpha);
    void render(ID2D1RenderTarget* target);
    void ensureBuffers(int width, int height);
    void releaseBuffers();
    void renderContent();
    void blitAnimated();
    void layoutForMonitor();
    void invokeCard(int card);
    void startPhase(Phase phase);
    void tickAnimation();
    float eased() const;
    D2D1_RECT_F cardRect(int index) const;
    bool inHeader(float x, float y) const;
    bool inClose(float x, float y) const;
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK dimmerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT dispatch(UINT message, WPARAM wParam, LPARAM lParam);

    HINSTANCE instance_{};
    HWND owner_{};
    HWND window_{};
    HWND dimmer_{};
    HWND previousForeground_{};
    Phase phase_{Phase::Hidden};
    float dpiScale_{1.0f};
    float anim_{};
    int hotCard_{-1};
    int pressedCard_{-1};
    bool dragging_{};
    bool pressedOutside_{};
    POINT dragMouse_{};
    RECT panelRect_{};
    RECT monitorRect_{};
    POINT savedPos_{ -1, -1 };
    ActionHandler actions_;
    MetricsProvider metrics_;
    MoveHandler moved_;
    std::wstring toast_;
    std::chrono::steady_clock::time_point toastUntil_{};
    std::chrono::steady_clock::time_point animStart_{};
    bool contentDirty_{true};
    bool timerPeriodSet_{};
    int bufferWidth_{};
    int bufferHeight_{};
    HDC contentDc_{};
    HDC presentDc_{};
    HBITMAP contentBmp_{};
    HBITMAP presentBmp_{};
    HBITMAP contentOld_{};
    HBITMAP presentOld_{};
    void* presentBits_{};

    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> target_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<IDWriteFactory> writeFactory_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> titleFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> bodyFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> valueFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> captionFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> iconFormat_;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> stroke_;
};

} // namespace kanekist
