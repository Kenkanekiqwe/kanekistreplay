#pragma once

#include <Audioclient.h>
#include <Mmdeviceapi.h>
#include <wrl/client.h>
#include <atomic>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace kanekist {

struct AudioPacket {
    int64_t timestamp100ns{};
    uint32_t sampleRate{};
    uint16_t channels{};
    std::vector<std::byte> pcm;
};

struct AudioDeviceInfo {
    std::wstring id;
    std::wstring name;
};

std::vector<AudioDeviceInfo> listAudioDevices(bool capture);

class AudioCapture {
public:
    using PacketCallback = std::function<void(AudioPacket)>;
    explicit AudioCapture(bool loopback);
    ~AudioCapture();
    HRESULT start(PacketCallback callback, std::wstring_view deviceId = {});
    void stop();
    [[nodiscard]] WAVEFORMATEX format() const noexcept { return format_; }

private:
    HRESULT initializeClient(WAVEFORMATEX* format, DWORD flags, REFERENCE_TIME bufferDuration);
    void convertPacket(const BYTE* data, UINT32 frames, std::vector<std::byte>& pcm) const;
    void run();

    bool loopback_;
    WAVEFORMATEX format_{};
    uint16_t captureChannels_{};
    uint16_t captureBits_{};
    uint16_t captureBlockAlign_{};
    bool captureFloat_{};
    Microsoft::WRL::ComPtr<IMMDevice> device_;
    Microsoft::WRL::ComPtr<IAudioClient> client_;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> capture_;
    HANDLE readyEvent_{};
    std::jthread thread_;
    std::atomic_bool running_{false};
    bool eventDriven_{true};
    PacketCallback callback_;
};

} // namespace kanekist
