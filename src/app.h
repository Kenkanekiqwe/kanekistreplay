#pragma once

#include "audio_capture.h"
#include "audio_mixer.h"
#include "fps_monitor.h"
#include "media_engine.h"
#include "overlay_menu.h"
#include "replay_recorder.h"

#include <Windows.h>
#include <WebView2.h>
#include <wrl/client.h>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace kanekist {

class App {
public:
    explicit App(HINSTANCE instance);
    int run(int showCommand);

private:
    enum TrayCommand {
        TrayOpen = 4001, TrayRecord, TraySaveReplay, TrayExit
    };

    bool createMainWindow(int showCommand);
    void createWebView();
    void showWebView();
    void resizeWebView();
    void postUiState(bool includeLibrary = true);
    void handleUiMessage(const std::wstring& json);
    void updateStatus();
    void toggleRecording();
    void toggleReplay();
    void setReplayEnabled(bool enable);
    void setMicrophoneEnabled(bool enable);
    void setSystemAudioEnabled(bool enable);
    void wireMicrophone();
    void wireSystemAudio();
    void applyQualityIndex(int index);
    void applyResolutionIndex(int index);
    void applyEncodeFpsIndex(uint32_t fps);
    int qualityIndex() const;
    void saveReplay();
    void takeScreenshot();
    void refreshLibrary(bool kickPreviews = true);
    void queueLibraryPreviews();
    void openMedia(const std::wstring& name);
    void deleteMedia(const std::wstring& name);
    void renameMedia(const std::wstring& name, const std::wstring& nextName);
    void copyMediaPath(const std::wstring& name);
    void mapMediaHosts();
    std::filesystem::path mediaFile(const std::wstring& name) const;
    void handleHotkey(WPARAM id);
    void handleOverlayAction(OverlayAction action);
    PerformanceSnapshot performanceSnapshot() const;
    void drainMixedAudio(int64_t timestamp100ns);
    void persistSettings();
    void applyAudioSettings();
    void applyReplayLimits();
    void registerAppHotkeys();
    void unregisterAppHotkeys();
    bool assignHotkey(int id, Hotkey hotkey);
    void beginHotkeyCapture(int id);
    void finishHotkeyCapture(Hotkey hotkey);
    void addTrayIcon();
    void removeTrayIcon();
    void showFromTray();
    void hideToTray();
    void quitApplication();
    void showTrayMenu();
    bool confirmDiskSpace(bool replay);
    uint64_t replayByteBudget() const;
    void rotateReplaySegment(bool saveAfterRotation);
    std::filesystem::path nextReplaySegmentPath();
    void reportFailure(std::wstring_view action, HRESULT result);
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT dispatch(UINT message, WPARAM wParam, LPARAM lParam);

    HINSTANCE instance_{};
    HWND window_{};
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> webviewController_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
    bool webviewReady_{};
    bool hideAfterUiReady_{};
    std::wstring libraryJson_{L"[]"};
    Settings settings_;
    MediaEngine engine_;
    AudioCapture systemAudio_{true};
    AudioCapture microphone_{false};
    AudioMixer audioMixer_;
    FpsMonitor fpsMonitor_;
    FpsOverlay overlay_;
    OverlayMenu overlayMenu_;
    bool replayEnabled_{};
    bool saving_{};
    bool starting_{};
    bool trayAdded_{};
    int capturingHotkey_{};
    std::jthread saveThread_;
    std::jthread libraryThread_;
    std::atomic_bool libraryBusy_{false};
    std::jthread replayRotateThread_;
    std::atomic_bool replayRotating_{false};
    bool replaySavePending_{};
    ReplayRecorder replayRecorder_;
    std::filesystem::path replaySegmentPath_;
    std::chrono::steady_clock::time_point replaySegmentStarted_{};
    uint64_t replaySegmentIndex_{};
    std::filesystem::path replayTemporary_;
    std::wstring status_{L"Инициализация"};
};

} // namespace kanekist
