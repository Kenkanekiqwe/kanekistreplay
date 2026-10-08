#pragma once

#include "core.h"
#include "wgc_capture.h"

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace kanekist {

class MediaEngine {
public:
    using StatusCallback = std::function<void(std::wstring_view)>;

    MediaEngine();
    ~MediaEngine();
    MediaEngine(const MediaEngine&) = delete;
    MediaEngine& operator=(const MediaEngine&) = delete;

    HRESULT initialize(const Settings& settings);
    void updateEncodingSettings(const Settings& settings);
    void configureAudio(const WAVEFORMATEX& format);
    HRESULT writeAudio(std::span<const std::byte> pcm, int64_t timestamp100ns);
    HRESULT startRecording(const std::filesystem::path& target);
    HRESULT stopRecording();
    void setQuietIo(bool quiet) noexcept { quietIo_ = quiet; }
    HRESULT takeScreenshot(const std::filesystem::path& target);
    void startPreview();
    void stopPreview();

    [[nodiscard]] bool recording() const noexcept { return recording_; }
    [[nodiscard]] uint32_t captureWidth() const noexcept { return captureWidth_; }
    [[nodiscard]] uint32_t captureHeight() const noexcept { return captureHeight_; }
    [[nodiscard]] uint64_t capturedFrames() const noexcept { return capturedFrames_; }
    [[nodiscard]] uint64_t droppedFrames() const noexcept { return droppedFrames_; }
    [[nodiscard]] uint64_t encodedFrames() const noexcept { return encodedFrames_; }
    [[nodiscard]] uint32_t encodeCodec() const noexcept { return encodeCodec_; }
    [[nodiscard]] std::chrono::microseconds lastMediaDuration() const;
    void setStatusCallback(StatusCallback callback) { callback_ = std::move(callback); }

private:
    enum class EncoderCommand { None, Start, Stop, Exit };

    HRESULT initializeD3D(IDXGIAdapter* adapter = nullptr);
    HRESULT initializeDuplication(IDXGIOutput* output = nullptr);
    HRESULT findAdapterForMonitor(HMONITOR monitor, IDXGIAdapter** adapter, IDXGIOutput** output);
    HMONITOR resolveTargetMonitor();
    HWND resolveTargetWindow();
    HRESULT rebindCapture(bool forceDevice);
    void rememberTexture(ID3D11Texture2D* texture);
    struct EncodeSession {
        Microsoft::WRL::ComPtr<IMFSinkWriter> writer;
        DWORD videoStream{};
        bool gpuInput{};
    };

    HRESULT createSinkWriter(const std::filesystem::path& target);
    bool muxAudioTrack();
    bool muxAudioTrackMediaFoundation();
    HRESULT copyFramePixels(ID3D11Texture2D* texture, std::vector<uint8_t>& pixels);
    HRESULT prepareEncodeTexture(ID3D11Texture2D* source);
    HRESULT blitScale(ID3D11Texture2D* source);
    HRESULT enqueueFrame(ID3D11Texture2D* texture);
    void updatePointer(const DXGI_OUTDUPL_FRAME_INFO& info);
    void clearPointer();
    void composeCursor(uint8_t* dest, uint32_t width, uint32_t height, uint32_t pitch) const;
    struct QueuedFrame {
        std::vector<uint8_t> pixels;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        int64_t timestamp100ns{};
    };
    HRESULT writeQueuedFrame(const std::shared_ptr<EncodeSession>& session,
                             const QueuedFrame& frame);
    void encoderLoop();
    void finishFile(const std::shared_ptr<EncodeSession>& session, uint64_t generation);
    void abandonEncoderLocked();
    HRESULT publishFinishedFile(HRESULT finalizeResult);
    void captureLoop();
    void publish(std::wstring_view text);

    Settings settings_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    WgcCapture wgc_;
    std::mutex deviceMutex_;
    HMONITOR targetMonitor_{};
    HWND targetWindow_{};
    HWND lastForeignWindow_{};
    bool lastExclusive_{};
    LUID adapterLuid_{};
    std::chrono::steady_clock::time_point lastRebind_{};
    std::chrono::steady_clock::time_point lastWgcFrame_{};
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> deviceManager_;
    std::shared_ptr<EncodeSession> session_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> latestFrame_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> encoderStaging_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> encodeTexture_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> scalerSource_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> videoContext_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> videoEnum_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> videoProcessor_;
    UINT scalerInW_{};
    UINT scalerInH_{};
    UINT captureWidth_{};
    UINT captureHeight_{};
    UINT resetToken_{};
    WAVEFORMATEX audioFormat_{};
    bool audioConfigured_{};
    int64_t lastVideoTimestamp100ns_{-1};
    uint64_t audioFramesWritten_{};
    FILE* audioRaw_{};
    std::mutex audioMutex_;
    std::mutex frameMutex_;
    std::mutex encoderMutex_;
    std::condition_variable encoderCv_;
    std::deque<QueuedFrame> frameQueue_;
    EncoderCommand encoderCommand_{EncoderCommand::None};
    HRESULT encoderResult_{S_OK};
    std::jthread captureThread_;
    std::thread encoderThread_;
    std::atomic_bool running_{false};
    std::atomic_bool recording_{false};
    std::atomic_bool gpuPaused_{false};
    std::atomic_bool quietIo_{false};
    std::atomic_uint64_t encoderGeneration_{0};
    std::atomic_int64_t recordingOrigin100ns_{-1};
    std::atomic_int64_t lastVideoSampleEnd100ns_{0};
    uint32_t encodeFps_{60};
    uint32_t encodeWidth_{1920};
    uint32_t encodeHeight_{1080};
    uint32_t encodeCodec_{0};
    std::atomic_uint64_t capturedFrames_{0};
    std::atomic_uint64_t droppedFrames_{0};
    std::atomic_uint64_t encodedFrames_{0};
    std::atomic_bool videoErrorLogged_{false};
    QpcClock clock_;
    StatusCallback callback_;
    std::filesystem::path recordingTarget_;
    std::filesystem::path recordingPartial_;
    std::filesystem::path audioRawPath_;
    mutable std::mutex pointerMutex_;
    bool pointerVisible_{};
    POINT pointerPos_{};
    POINT pointerHotspot_{};
    UINT pointerWidth_{};
    UINT pointerHeight_{};
    UINT pointerPitch_{};
    UINT pointerType_{};
    std::vector<uint8_t> pointerShape_;
};

struct MediaPreview {
    double durationSeconds{};
    bool hasThumbnail{};
};

MediaPreview previewMediaFile(const std::filesystem::path& file,
                              const std::filesystem::path& thumbsDirectory,
                              bool decodeFrames = false);

} // namespace kanekist
