#include "audio_capture.h"

#include "core.h"
#include <Avrt.h>
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <propidl.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace kanekist {
namespace {

std::wstring endpointName(IMMDevice* device) {
    if (!device) return L"(unknown)";
    ComPtr<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) return L"(unknown)";
    PROPVARIANT value{};
    PropVariantInit(&value);
    std::wstring name = L"(unknown)";
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value)) &&
        value.vt == VT_LPWSTR && value.pwszVal) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    return name;
}

void describeFormat(WAVEFORMATEX* mix, uint16_t& channels, uint16_t& bits, bool& isFloat) {
    channels = mix ? mix->nChannels : 0;
    bits = mix ? mix->wBitsPerSample : 0;
    isFloat = mix && mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (mix && mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        mix->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
        isFloat = extended->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        if (extended->Samples.wValidBitsPerSample) bits = extended->Samples.wValidBitsPerSample;
    }
}

float readNativeSample(const BYTE* frame, UINT32 channel, UINT32 channels, UINT16 bits, bool isFloat) {
    if (channel >= channels) return 0.0f;
    if (isFloat && bits == 32) {
        float value{};
        std::memcpy(&value, frame + channel * 4, sizeof(value));
        return std::isfinite(value) ? value : 0.0f;
    }
    if (bits == 16) {
        std::int16_t value{};
        std::memcpy(&value, frame + channel * 2, sizeof(value));
        return static_cast<float>(value) / 32768.0f;
    }
    if (bits == 24) {
        const BYTE* src = frame + channel * 3;
        const std::int32_t value = static_cast<std::int32_t>(src[0]) |
            (static_cast<std::int32_t>(src[1]) << 8) |
            (static_cast<std::int32_t>(static_cast<std::int8_t>(src[2])) << 16);
        return static_cast<float>(value) / 8388608.0f;
    }
    if (bits == 32) {
        std::int32_t value{};
        std::memcpy(&value, frame + channel * 4, sizeof(value));
        return static_cast<float>(value) / 2147483648.0f;
    }
    return 0.0f;
}

} // namespace

AudioCapture::AudioCapture(bool loopback) : loopback_(loopback) {}
AudioCapture::~AudioCapture() { stop(); }

HRESULT AudioCapture::initializeClient(WAVEFORMATEX* format, DWORD flags, REFERENCE_TIME bufferDuration) {
    client_.Reset();
    capture_.Reset();
    HRESULT hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(client_.GetAddressOf()));
    if (SUCCEEDED(hr)) {
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, bufferDuration, 0, format, nullptr);
    }
    return hr;
}

HRESULT AudioCapture::start(PacketCallback callback, std::wstring_view deviceId) {
    if (running_) return S_FALSE;
    eventDriven_ = true;
    ComApartment apartment;
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr) && !deviceId.empty()) {
        hr = enumerator->GetDevice(std::wstring(deviceId).c_str(), &device_);
        if (FAILED(hr)) {
            Logger::instance().write(L"WARN", L"Audio device not found, using default");
            device_.Reset();
            hr = S_OK;
        }
    }
    if (SUCCEEDED(hr) && !device_) {
        hr = enumerator->GetDefaultAudioEndpoint(loopback_ ? eRender : eCapture,
                                                 eConsole, &device_);
    }
    WAVEFORMATEX* mix{};
    const REFERENCE_TIME bufferDuration = 20 * 10'000;
    if (SUCCEEDED(hr)) {
        ComPtr<IAudioClient> probe;
        hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(probe.GetAddressOf()));
        if (SUCCEEDED(hr)) hr = probe->GetMixFormat(&mix);
    }

    WAVEFORMATEXTENSIBLE float48{};
    float48.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    float48.Format.nChannels = 2;
    float48.Format.nSamplesPerSec = 48000;
    float48.Format.wBitsPerSample = 32;
    float48.Format.nBlockAlign = 8;
    float48.Format.nAvgBytesPerSec = 384000;
    float48.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    float48.Samples.wValidBitsPerSample = 32;
    float48.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    float48.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    if (SUCCEEDED(hr) && !loopback_) {
        const DWORD convertedFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        hr = initializeClient(&float48.Format, convertedFlags, bufferDuration);
        if (SUCCEEDED(hr)) {
            format_ = float48.Format;
            format_.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
            format_.cbSize = 0;
            captureChannels_ = 2;
            captureBits_ = 32;
            captureBlockAlign_ = 8;
            captureFloat_ = true;
        }
    }

    if ((FAILED(hr) || loopback_) && mix) {
        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            (loopback_ ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
        hr = initializeClient(mix, flags, bufferDuration);
        eventDriven_ = SUCCEEDED(hr);
        if (FAILED(hr) && !loopback_) {
            flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            hr = initializeClient(mix, flags, bufferDuration);
            eventDriven_ = false;
        } else if (FAILED(hr) && loopback_) {
            flags = AUDCLNT_STREAMFLAGS_LOOPBACK;
            hr = initializeClient(mix, flags, bufferDuration);
            eventDriven_ = false;
        }
        if (SUCCEEDED(hr)) {
            describeFormat(mix, captureChannels_, captureBits_, captureFloat_);
            captureBlockAlign_ = mix->nBlockAlign;
            format_ = *mix;
            format_.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
            format_.nChannels = captureChannels_ == 1 ? 1 : 2;
            format_.wBitsPerSample = 32;
            format_.nBlockAlign = static_cast<WORD>(format_.nChannels * 4);
            format_.nAvgBytesPerSec = format_.nSamplesPerSec * format_.nBlockAlign;
            format_.cbSize = 0;
        }
    }
    CoTaskMemFree(mix);

    if (SUCCEEDED(hr)) hr = client_->GetService(IID_PPV_ARGS(&capture_));
    if (eventDriven_) {
        readyEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (SUCCEEDED(hr) && !readyEvent_) hr = HRESULT_FROM_WIN32(GetLastError());
        if (SUCCEEDED(hr)) hr = client_->SetEventHandle(readyEvent_);
    }
    if (SUCCEEDED(hr)) hr = client_->Start();
    if (FAILED(hr)) {
        if (readyEvent_) CloseHandle(readyEvent_);
        readyEvent_ = nullptr;
        capture_.Reset();
        client_.Reset();
        device_.Reset();
        return hr;
    }
    callback_ = std::move(callback);
    running_ = true;
    thread_ = std::jthread([this] { run(); });
    Logger::instance().write(L"INFO", std::wstring(loopback_ ? L"System audio capture started: " :
        L"Microphone capture started: ") + endpointName(device_.Get()) + L" (" +
        std::to_wstring(format_.nSamplesPerSec) + L" Hz, " +
        std::to_wstring(format_.nChannels) + L" ch)");
    return S_OK;
}

void AudioCapture::stop() {
    if (!running_.exchange(false)) return;
    if (readyEvent_) SetEvent(readyEvent_);
    if (thread_.joinable()) thread_.join();
    if (client_) client_->Stop();
    capture_.Reset();
    client_.Reset();
    device_.Reset();
    if (readyEvent_) CloseHandle(readyEvent_);
    readyEvent_ = nullptr;
}

void AudioCapture::convertPacket(const BYTE* data, UINT32 frames, std::vector<std::byte>& pcm) const {
    const UINT32 outChannels = format_.nChannels;
    pcm.resize(static_cast<size_t>(frames) * outChannels * sizeof(float));
    auto* destination = reinterpret_cast<float*>(pcm.data());
    if (!data) {
        std::memset(destination, 0, pcm.size());
        return;
    }
    for (UINT32 frame = 0; frame < frames; ++frame) {
        const BYTE* source = data + static_cast<size_t>(frame) * captureBlockAlign_;
        const float left = readNativeSample(source, 0, captureChannels_, captureBits_, captureFloat_);
        const float right = captureChannels_ == 1
            ? left
            : readNativeSample(source, 1, captureChannels_, captureBits_, captureFloat_);
        if (outChannels == 1) {
            destination[frame] = left;
        } else {
            destination[frame * 2] = left;
            destination[frame * 2 + 1] = right;
        }
    }
}

void AudioCapture::run() {
    ComApartment apartment;
    DWORD taskIndex{};
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
    while (running_) {
        if (eventDriven_ && readyEvent_) {
            if (WaitForSingleObject(readyEvent_, 250) != WAIT_OBJECT_0) continue;
        } else {
            Sleep(10);
        }
        if (!capture_) break;
        UINT32 packets{};
        if (FAILED(capture_->GetNextPacketSize(&packets))) break;
        while (packets && running_) {
            BYTE* data{};
            UINT32 frames{};
            DWORD flags{};
            UINT64 devicePosition{}, qpcPosition{};
            if (FAILED(capture_->GetBuffer(&data, &frames, &flags, &devicePosition, &qpcPosition))) break;
            AudioPacket packet;
            packet.timestamp100ns = QpcClock::absolute100ns();
            packet.sampleRate = format_.nSamplesPerSec;
            packet.channels = format_.nChannels;
            if (frames) {
                const BYTE* source = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : data;
                convertPacket(source, frames, packet.pcm);
            }
            capture_->ReleaseBuffer(frames);
            if (callback_ && !packet.pcm.empty()) callback_(std::move(packet));
            if (FAILED(capture_->GetNextPacketSize(&packets))) packets = 0;
        }
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

std::vector<AudioDeviceInfo> listAudioDevices(bool capture) {
    std::vector<AudioDeviceInfo> devices;
    devices.push_back({L"", L"По умолчанию"});
    ComApartment apartment;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator)))) {
        return devices;
    }
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(capture ? eCapture : eRender,
        DEVICE_STATE_ACTIVE, &collection))) {
        return devices;
    }
    UINT count{};
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device)) || !device) continue;
        LPWSTR id{};
        if (FAILED(device->GetId(&id)) || !id) continue;
        AudioDeviceInfo info;
        info.id = id;
        CoTaskMemFree(id);
        info.name = endpointName(device.Get());
        if (info.name.empty()) info.name = info.id;
        devices.push_back(std::move(info));
    }
    return devices;
}

} // namespace kanekist
