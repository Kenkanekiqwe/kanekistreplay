#pragma once

#include "core.h"
#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace kanekist {

class FpsMonitor {
public:
    using Counter = std::function<uint64_t()>;
    FpsMonitor() = default;
    ~FpsMonitor();
    void start(Counter fallbackCounter, HWND ownerWindow);
    void stop();
    [[nodiscard]] double fps() const noexcept { return fps_.load(); }
    [[nodiscard]] bool gameFpsActive() const noexcept { return gameFpsActive_.load(); }
    [[nodiscard]] DWORD targetProcessId() const noexcept { return targetProcessId_.load(); }
    [[nodiscard]] double onePercentLow() const noexcept { return onePercentLow_.load(); }
    [[nodiscard]] double frameTimeMs() const noexcept { return frameTimeMs_.load(); }
    [[nodiscard]] double cpuPercent() const noexcept { return cpuPercent_.load(); }
    [[nodiscard]] double gpuBusyPercent() const noexcept { return gpuBusyPercent_.load(); }
    [[nodiscard]] double renderLatencyMs() const noexcept { return renderLatencyMs_.load(); }

private:
    bool launchPresentMon();
    void readPresentMon();
    std::jthread thread_;
    std::jthread readerThread_;
    std::atomic_bool running_{false};
    std::atomic<double> fps_{0.0};
    std::atomic_bool gameFpsActive_{false};
    std::atomic<DWORD> targetProcessId_{0};
    std::atomic<double> onePercentLow_{0.0};
    std::atomic<double> frameTimeMs_{0.0};
    std::atomic<double> cpuPercent_{0.0};
    std::atomic<double> gpuBusyPercent_{0.0};
    std::atomic<double> renderLatencyMs_{0.0};
    std::atomic<DWORD> foregroundProcessId_{0};
    std::atomic<uint64_t> presentCount_{0};
    std::atomic<double> gpuBusyTotal_{0.0};
    std::atomic<double> latencyTotal_{0.0};
    HANDLE presentMonProcess_{};
    HANDLE presentMonOutput_{};
};

class FpsOverlay {
public:
    FpsOverlay() = default;
    ~FpsOverlay();
    bool create(HINSTANCE instance, const FpsMonitor* monitor, HWND owner, const Settings* settings);
    void show(bool value);
    void refresh();
    void setGameWindow(HWND game);
    [[nodiscard]] HWND window() const noexcept { return window_; }

private:
    bool ensureFactories();
    void present();
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    HWND window_{};
    HWND owner_{};
    HWND gameWindow_{};
    const FpsMonitor* monitor_{};
    const Settings* settings_{};
    bool visibleRequested_{true};
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> renderTarget_;
    Microsoft::WRL::ComPtr<IDWriteFactory> writeFactory_;
};

} // namespace kanekist
