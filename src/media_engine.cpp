#include "media_engine.h"
#include "game_window.h"

#include <Mfapi.h>
#include <Mferror.h>
#include <Wincodec.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <mmreg.h>
#include <propidl.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace kanekist {
namespace {

template<typename T>
HRESULT setSize(T* attributes, REFGUID key, uint32_t width, uint32_t height) {
    return MFSetAttributeSize(attributes, key, width, height);
}

void scaleBilinear(const uint8_t* source, uint32_t srcW, uint32_t srcH, uint32_t srcPitch,
                   uint8_t* destination, uint32_t dstW, uint32_t dstH) {
    if (!source || !destination || !srcW || !srcH || !dstW || !dstH) return;

    // Preserve the source aspect ratio. When the requested recording
    // resolution has a different aspect ratio, crop the center rather than
    // stretching the image (e.g. 16:10 -> 16:9).
    uint32_t cropW = srcW;
    uint32_t cropH = srcH;
    const double srcAspect = static_cast<double>(srcW) / static_cast<double>(srcH);
    const double dstAspect = static_cast<double>(dstW) / static_cast<double>(dstH);
    if (srcAspect > dstAspect) {
        cropW = static_cast<uint32_t>(std::floor(static_cast<double>(srcH) * dstAspect));
        cropW = std::clamp(cropW & ~1u, 2u, srcW);
    } else if (srcAspect < dstAspect) {
        cropH = static_cast<uint32_t>(std::floor(static_cast<double>(srcW) / dstAspect));
        cropH = std::clamp(cropH & ~1u, 2u, srcH);
    }
    const uint32_t cropX = (srcW - cropW) / 2;
    const uint32_t cropY = (srcH - cropH) / 2;
    const uint32_t dstPitch = dstW * 4;

    if (cropW == dstW && cropH == dstH) {
        for (uint32_t y = 0; y < dstH; ++y) {
            std::memcpy(destination + static_cast<size_t>(y) * dstPitch,
                source + static_cast<size_t>(cropY + y) * srcPitch +
                    static_cast<size_t>(cropX) * 4, dstPitch);
        }
        return;
    }

    const float xScale = cropW <= 1 || dstW <= 1 ? 0.0f :
        static_cast<float>(cropW - 1) / static_cast<float>(dstW - 1);
    const float yScale = cropH <= 1 || dstH <= 1 ? 0.0f :
        static_cast<float>(cropH - 1) / static_cast<float>(dstH - 1);

    for (uint32_t y = 0; y < dstH; ++y) {
        const float fy = static_cast<float>(y) * yScale;
        const uint32_t y0 = static_cast<uint32_t>(fy);
        const uint32_t y1 = std::min(y0 + 1, cropH - 1);
        const float wy = fy - static_cast<float>(y0);
        const auto* row0 = source + static_cast<size_t>(cropY + y0) * srcPitch +
            static_cast<size_t>(cropX) * 4;
        const auto* row1 = source + static_cast<size_t>(cropY + y1) * srcPitch +
            static_cast<size_t>(cropX) * 4;
        auto* dstRow = destination + static_cast<size_t>(y) * dstPitch;
        for (uint32_t x = 0; x < dstW; ++x) {
            const float fx = static_cast<float>(x) * xScale;
            const uint32_t x0 = static_cast<uint32_t>(fx);
            const uint32_t x1 = std::min(x0 + 1, cropW - 1);
            const float wx = fx - static_cast<float>(x0);
            const auto* p00 = row0 + static_cast<size_t>(x0) * 4;
            const auto* p10 = row0 + static_cast<size_t>(x1) * 4;
            const auto* p01 = row1 + static_cast<size_t>(x0) * 4;
            const auto* p11 = row1 + static_cast<size_t>(x1) * 4;
            for (int c = 0; c < 4; ++c) {
                const float top = static_cast<float>(p00[c]) +
                    (static_cast<float>(p10[c]) - static_cast<float>(p00[c])) * wx;
                const float bottom = static_cast<float>(p01[c]) +
                    (static_cast<float>(p11[c]) - static_cast<float>(p01[c])) * wx;
                dstRow[x * 4 + c] = static_cast<uint8_t>(
                    top + (bottom - top) * wy + 0.5f);
            }
        }
    }
}

void applyEncoderQuality(IMFSinkWriter* writer, DWORD stream, uint32_t fps, uint32_t bitrateMbps) {
    ComPtr<ICodecAPI> codec;
    if (!writer || FAILED(writer->GetServiceForStream(stream, GUID_NULL, IID_PPV_ARGS(&codec)))) return;

    VARIANT value{};
    value.vt = VT_UI4;
    value.ulVal = eAVEncCommonRateControlMode_PeakConstrainedVBR;
    codec->SetValue(&CODECAPI_AVEncCommonRateControlMode, &value);

    const uint32_t bits = std::max(20u, bitrateMbps) * 1'000'000;
    value.ulVal = bits;
    codec->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &value);
    value.ulVal = bits + bits / 2;
    codec->SetValue(&CODECAPI_AVEncCommonMaxBitRate, &value);

    value.ulVal = 0;
    codec->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &value);
    value.ulVal = 90;
    codec->SetValue(&CODECAPI_AVEncCommonQuality, &value);
    value.ulVal = fps * 2;
    codec->SetValue(&CODECAPI_AVEncMPVGOPSize, &value);
    value.ulVal = 0;
    codec->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &value);

    value.vt = VT_BOOL;
    value.boolVal = VARIANT_TRUE;
    codec->SetValue(&CODECAPI_AVEncH264CABACEnable, &value);
}

HRESULT writePngBgra(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t pitch,
                     const std::filesystem::path& target) {
    if (!bgra || !width || !height) return E_INVALIDARG;
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(target.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr)) {
        hr = frame->WritePixels(height, pitch, pitch * height, const_cast<BYTE*>(bgra));
    }
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    return hr;
}

uint8_t pointerMaskBit(const uint8_t* row, UINT x) {
    return static_cast<uint8_t>((row[x / 8] >> (7 - (x % 8))) & 1u);
}

void blendPremultiplied(uint8_t* dst, const uint8_t* src) {
    const unsigned a = src[3];
    if (a == 0) return;
    if (a >= 255) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = 255;
        return;
    }
    const unsigned ia = 255 - a;
    dst[0] = static_cast<uint8_t>((src[0] * a + dst[0] * ia) / 255);
    dst[1] = static_cast<uint8_t>((src[1] * a + dst[1] * ia) / 255);
    dst[2] = static_cast<uint8_t>((src[2] * a + dst[2] * ia) / 255);
    dst[3] = 255;
}

} // namespace

MediaEngine::MediaEngine() = default;

MediaEngine::~MediaEngine() {
    stopPreview();
    if (recording_) stopRecording();
    {
        std::unique_lock lock(encoderMutex_);
        encoderCommand_ = EncoderCommand::Exit;
        encoderCv_.notify_all();
    }
    if (encoderThread_.joinable()) encoderThread_.join();
    MFShutdown();
}

void MediaEngine::configureAudio(const WAVEFORMATEX& format) {
    std::scoped_lock lock(audioMutex_);
    audioFormat_ = format;
    audioConfigured_ = format.nSamplesPerSec > 0 && format.nChannels > 0 && format.nBlockAlign > 0;
}

void MediaEngine::updateEncodingSettings(const Settings& settings) {
    if (recording_) return;
    settings_.width = settings.width;
    settings_.height = settings.height;
    settings_.fps = settings.fps;
    settings_.bitrateMbps = settings.bitrateMbps;
    settings_.normalizeEncodePreset();
    encodeWidth_ = settings_.width;
    encodeHeight_ = settings_.height;
    encodeTexture_.Reset();
    encoderStaging_.Reset();
    videoProcessor_.Reset();
    videoEnum_.Reset();
    scalerInW_ = 0;
    scalerInH_ = 0;
}

HRESULT MediaEngine::initialize(const Settings& settings) {
    settings_ = settings;
    settings_.normalizeEncodePreset();
    encodeWidth_ = settings_.width;
    encodeHeight_ = settings_.height;
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) return hr;
    hr = rebindCapture(true);
    if (FAILED(hr) && !device_) return hr;
    if (!encoderThread_.joinable()) encoderThread_ = std::thread([this] { encoderLoop(); });
    return S_OK;
}

HRESULT MediaEngine::initializeD3D(IDXGIAdapter* adapter) {
    device_.Reset();
    context_.Reset();
    videoDevice_.Reset();
    videoContext_.Reset();
    deviceManager_.Reset();
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL actual{};
    constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0
    };
    const D3D_DRIVER_TYPE type = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
    HRESULT hr = D3D11CreateDevice(adapter, type, nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, &actual, &context_);
    if (FAILED(hr)) {
        flags &= ~D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        hr = D3D11CreateDevice(adapter, type, nullptr, flags,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, &actual, &context_);
    }
    if (FAILED(hr)) return hr;
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(device_.As(&multithread))) multithread->SetMultithreadProtected(TRUE);
    hr = MFCreateDXGIDeviceManager(&resetToken_, &deviceManager_);
    if (SUCCEEDED(hr)) hr = deviceManager_->ResetDevice(device_.Get(), resetToken_);
    device_.As(&videoDevice_);
    context_.As(&videoContext_);
    if (adapter) {
        DXGI_ADAPTER_DESC desc{};
        adapter->GetDesc(&desc);
        adapterLuid_ = desc.AdapterLuid;
        Logger::instance().write(L"INFO", std::wstring(L"Capture GPU: ") + desc.Description);
    }
    return hr;
}

HRESULT MediaEngine::findAdapterForMonitor(HMONITOR monitor, IDXGIAdapter** adapterOut,
                                           IDXGIOutput** outputOut) {
    if (adapterOut) *adapterOut = nullptr;
    if (outputOut) *outputOut = nullptr;
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;

    ComPtr<IDXGIAdapter> fallbackAdapter;
    ComPtr<IDXGIOutput> fallbackOutput;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop) continue;
            if (!fallbackAdapter) {
                fallbackAdapter = adapter;
                fallbackOutput = output;
            }
            if (!monitor || desc.Monitor == monitor) {
                if (adapterOut) *adapterOut = adapter.Detach();
                if (outputOut) *outputOut = output.Detach();
                return S_OK;
            }
        }
    }
    if (!fallbackAdapter) return DXGI_ERROR_NOT_FOUND;
    if (adapterOut) *adapterOut = fallbackAdapter.Detach();
    if (outputOut) *outputOut = fallbackOutput.Detach();
    return S_OK;
}

HWND MediaEngine::resolveTargetWindow() {
    if (const auto game = resolveGameTarget(); game.window) {
        lastForeignWindow_ = game.window;
        return game.window;
    }
    HWND foreground = GetForegroundWindow();
    DWORD processId{};
    if (foreground) GetWindowThreadProcessId(foreground, &processId);
    if (foreground && processId != GetCurrentProcessId() && IsWindowVisible(foreground)) {
        lastForeignWindow_ = foreground;
    }
    if (lastForeignWindow_ && !IsWindow(lastForeignWindow_)) lastForeignWindow_ = nullptr;
    return lastForeignWindow_;
}

HMONITOR MediaEngine::resolveTargetMonitor() {
    if (const auto game = resolveGameTarget(); game.monitor) return game.monitor;
    if (const HWND window = resolveTargetWindow()) {
        return MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
    }
    return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

HRESULT MediaEngine::rebindCapture(bool forceDevice) {
    std::scoped_lock lock(deviceMutex_);
    const auto game = resolveGameTarget();
    const HWND window = game.window ? game.window : resolveTargetWindow();
    const HMONITOR monitor = game.monitor ? game.monitor : resolveTargetMonitor();
    const bool exclusive = game.exclusiveD3d;

    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIOutput> output;
    findAdapterForMonitor(monitor, &adapter, &output);

    LUID luid{};
    if (adapter) {
        DXGI_ADAPTER_DESC desc{};
        adapter->GetDesc(&desc);
        luid = desc.AdapterLuid;
    }
    const bool adapterChanged = luid.LowPart != adapterLuid_.LowPart ||
                                luid.HighPart != adapterLuid_.HighPart;
    const bool targetChanged = monitor != targetMonitor_ || window != targetWindow_ ||
        exclusive != lastExclusive_ || wgc_.closed();
    if (!forceDevice && device_ && !adapterChanged && !targetChanged && wgc_.active()) {
        return S_OK;
    }
    if (!forceDevice && device_ && !adapterChanged && !targetChanged && duplication_ && !wgc_.active()) {
        return S_OK;
    }

    wgc_.stop();
    duplication_.Reset();
    if (!device_ || adapterChanged || forceDevice) {
        {
            std::scoped_lock frameLock(frameMutex_);
            latestFrame_.Reset();
        }
        encoderStaging_.Reset();
        encodeTexture_.Reset();
        scalerSource_.Reset();
        videoProcessor_.Reset();
        videoEnum_.Reset();
        scalerInW_ = 0;
        scalerInH_ = 0;
        const HRESULT deviceHr = initializeD3D(adapter.Get());
        if (FAILED(deviceHr)) return deviceHr;
        adapterLuid_ = luid;
    }

    targetMonitor_ = monitor;
    targetWindow_ = window;
    lastExclusive_ = exclusive;

    const auto bindWindow = [&]() -> HRESULT {
        if (!window) return E_FAIL;
        const HRESULT windowHr = wgc_.startForWindow(device_.Get(), window);
        if (FAILED(windowHr)) return windowHr;
        captureWidth_ = wgc_.width() & ~1u;
        captureHeight_ = wgc_.height() & ~1u;
        Logger::instance().write(L"INFO", std::wstring(L"Display capture: WGC window") +
            (game.processName.empty() ? L"" : (L" (" + game.processName + L")")));
        lastWgcFrame_ = std::chrono::steady_clock::now();
        return S_OK;
    };
    const auto bindMonitor = [&]() -> HRESULT {
        const HRESULT monitorHr = wgc_.startForMonitor(device_.Get(), monitor);
        if (FAILED(monitorHr)) return monitorHr;
        captureWidth_ = wgc_.width() & ~1u;
        captureHeight_ = wgc_.height() & ~1u;
        Logger::instance().write(L"INFO", std::wstring(L"Display capture: WGC monitor") +
            (exclusive ? L" exclusive" : L"") +
            (game.processName.empty() ? L"" : (L" (" + game.processName + L")")));
        lastWgcFrame_ = std::chrono::steady_clock::now();
        return S_OK;
    };

    const bool coverDisplay = exclusive || game.fullscreen || !window;
    HRESULT captureHr = coverDisplay ? bindMonitor() : bindWindow();
    if (FAILED(captureHr)) {
        Logger::instance().write(L"WARN", L"Primary WGC capture failed: " + hresultMessage(captureHr));
        captureHr = coverDisplay ? bindWindow() : bindMonitor();
        if (FAILED(captureHr)) {
            Logger::instance().write(L"WARN", L"Fallback WGC capture failed: " + hresultMessage(captureHr));
        }
    }
    if (SUCCEEDED(captureHr)) return S_OK;
    return initializeDuplication(output.Get());
}

HRESULT MediaEngine::initializeDuplication(IDXGIOutput* output) {
    ComPtr<IDXGIOutput> local = output;
    if (!local) {
        ComPtr<IDXGIAdapter> adapter;
        findAdapterForMonitor(targetMonitor_ ? targetMonitor_ : resolveTargetMonitor(),
            &adapter, &local);
    }
    if (!local) {
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        HRESULT hr = device_.As(&dxgiDevice);
        if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(&adapter);
        if (SUCCEEDED(hr)) hr = adapter->EnumOutputs(0, &local);
        if (FAILED(hr)) return hr;
    }
    ComPtr<IDXGIOutput1> output1;
    HRESULT hr = local.As(&output1);
    if (SUCCEEDED(hr)) hr = output1->DuplicateOutput(device_.Get(), &duplication_);
    if (SUCCEEDED(hr)) {
        DXGI_OUTDUPL_DESC desc{};
        duplication_->GetDesc(&desc);
        captureWidth_ = desc.ModeDesc.Width & ~1u;
        captureHeight_ = desc.ModeDesc.Height & ~1u;
        encoderStaging_.Reset();
        latestFrame_.Reset();
        encodeTexture_.Reset();
        scalerSource_.Reset();
        videoProcessor_.Reset();
        videoEnum_.Reset();
        scalerInW_ = 0;
        scalerInH_ = 0;
        clearPointer();
        Logger::instance().write(L"INFO", L"Display capture: DXGI Desktop Duplication");
    }
    return hr;
}

void MediaEngine::rememberTexture(ID3D11Texture2D* texture) {
    if (!texture) return;
    D3D11_TEXTURE2D_DESC incoming{};
    texture->GetDesc(&incoming);
    captureWidth_ = incoming.Width & ~1u;
    captureHeight_ = incoming.Height & ~1u;
    std::scoped_lock frameLock(frameMutex_);
    D3D11_TEXTURE2D_DESC current{};
    if (latestFrame_) latestFrame_->GetDesc(&current);
    if (!latestFrame_ || current.Width != incoming.Width || current.Height != incoming.Height) {
        latestFrame_.Reset();
        D3D11_TEXTURE2D_DESC desc = incoming;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = 0;
        desc.CPUAccessFlags = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.MipLevels = 1;
        device_->CreateTexture2D(&desc, nullptr, &latestFrame_);
    }
    if (latestFrame_) context_->CopyResource(latestFrame_.Get(), texture);
}

HRESULT MediaEngine::createSinkWriter(const std::filesystem::path& target) {
    struct CodecOption {
        GUID subtype;
        uint32_t index;
        const wchar_t* name;
        bool preferHardware;
    };
    std::vector<CodecOption> codecs;
    if (settings_.videoCodec == 2) {
        codecs.push_back({MFVideoFormat_AV1, 2, L"AV1", true});
    } else if (settings_.videoCodec == 1) {
        codecs.push_back({MFVideoFormat_HEVC, 1, L"HEVC", true});
    }
    codecs.push_back({MFVideoFormat_H264, 0, L"H.264", true});

    const uint32_t sizes[][2] = {
        {settings_.width, settings_.height},
        {1920, 1080},
        {1280, 720}
    };
    const uint32_t fpsList[] = { settings_.fps, 60u, 30u };
    HRESULT lastError = E_FAIL;

    auto trySession = [&](const GUID& subtype, BOOL hardware, uint32_t width, uint32_t height,
                          uint32_t fps, uint32_t bitrateMbps, uint32_t profile,
                          std::shared_ptr<EncodeSession>& session) -> HRESULT {
        session = std::make_shared<EncodeSession>();
        ComPtr<IMFAttributes> attributes;
        HRESULT hr = MFCreateAttributes(&attributes, 3);
        if (SUCCEEDED(hr)) {
            hr = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardware);
        }
        if (SUCCEEDED(hr) && hardware && deviceManager_) {
            hr = attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, deviceManager_.Get());
        }
        if (SUCCEEDED(hr)) hr = attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
        if (SUCCEEDED(hr)) {
            hr = MFCreateSinkWriterFromURL(target.c_str(), nullptr, attributes.Get(), &session->writer);
        }
        ComPtr<IMFMediaType> outputType;
        if (SUCCEEDED(hr)) hr = MFCreateMediaType(&outputType);
        if (SUCCEEDED(hr)) hr = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) hr = outputType->SetGUID(MF_MT_SUBTYPE, subtype);
        if (SUCCEEDED(hr)) hr = outputType->SetUINT32(MF_MT_AVG_BITRATE, bitrateMbps * 1'000'000);
        if (SUCCEEDED(hr)) hr = outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (SUCCEEDED(hr) && profile) hr = outputType->SetUINT32(MF_MT_MPEG2_PROFILE, profile);
        if (SUCCEEDED(hr)) hr = setSize(outputType.Get(), MF_MT_FRAME_SIZE, width, height);
        if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, fps, 1);
        if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (SUCCEEDED(hr)) hr = session->writer->AddStream(outputType.Get(), &session->videoStream);
        ComPtr<IMFMediaType> inputType;
        if (SUCCEEDED(hr)) hr = MFCreateMediaType(&inputType);
        if (SUCCEEDED(hr)) hr = inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) hr = inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (SUCCEEDED(hr)) hr = inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (SUCCEEDED(hr)) hr = setSize(inputType.Get(), MF_MT_FRAME_SIZE, width, height);
        if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, fps, 1);
        if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (SUCCEEDED(hr)) hr = inputType->SetUINT32(MF_MT_DEFAULT_STRIDE, width * 4);
        if (SUCCEEDED(hr)) hr = inputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        if (SUCCEEDED(hr)) {
            hr = session->writer->SetInputMediaType(session->videoStream, inputType.Get(), nullptr);
        }
        if (SUCCEEDED(hr)) hr = session->writer->BeginWriting();
        if (SUCCEEDED(hr)) {
            // The encoder itself remains hardware/GPU-backed. Do not force DXGI
            // surface samples into the Sink Writer: Windows' built-in hardware
            // MFTs are not required to accept arbitrary RGB32 DXGI surfaces,
            // and rejecting every WriteSample results in MF_E_NO_SAMPLES_PROCESSED
            // during Finalize. Frames are still captured/scaled on the GPU, then
            // uploaded through the Sink Writer's supported RGB32 input path.
            session->gpuInput = false;
            applyEncoderQuality(session->writer.Get(), session->videoStream, fps, bitrateMbps);
            return S_OK;
        }
        session.reset();
        std::error_code error;
        std::filesystem::remove(target, error);
        return hr;
    };

    for (const auto& codec : codecs) {
        for (const auto& size : sizes) {
            const uint32_t width = size[0] & ~1u;
            const uint32_t height = size[1] & ~1u;
            if (width < 640 || height < 360) continue;
            const BOOL hardwareOptions[] = { TRUE, FALSE };
            const int hardwareTries = codec.preferHardware || width > 1920 || height > 1080 ? 2 : 1;
            const int hardwareStart = codec.preferHardware || width > 1920 || height > 1080 ? 0 : 1;
            uint32_t profiles[2]{};
            int profileCount = 1;
            if (codec.index == 0) {
                profiles[0] = eAVEncH264VProfile_High;
                profiles[1] = eAVEncH264VProfile_Main;
                profileCount = 2;
            } else if (codec.index == 1) {
                profiles[0] = eAVEncH265VProfile_Main_420_8;
                profiles[1] = 0;
                profileCount = 2;
            }
            for (int hw = hardwareStart; hw < hardwareStart + hardwareTries; ++hw) {
                for (const uint32_t fps : fpsList) {
                    for (int p = 0; p < profileCount; ++p) {
                        const uint32_t bitrateMbps = suggestedBitrateMbps(width, height, fps, codec.index);
                        std::shared_ptr<EncodeSession> session;
                        const HRESULT hr = trySession(codec.subtype, hardwareOptions[hw], width, height,
                            fps, bitrateMbps, profiles[p], session);
                        if (SUCCEEDED(hr)) {
                            encodeFps_ = fps;
                            encodeWidth_ = width;
                            encodeHeight_ = height;
                            encodeCodec_ = codec.index;
                            session_ = std::move(session);
                            if (codec.index != settings_.videoCodec) {
                                Logger::instance().write(L"WARN", std::wstring(L"Requested ") +
                                    videoCodecLabel(settings_.videoCodec) + L", falling back to " + codec.name);
                            }
                            Logger::instance().write(L"INFO", std::wstring(hardwareOptions[hw] ? L"Hardware " : L"Software ") +
                                codec.name + L" encoder ready at " + std::to_wstring(width) + L"x" +
                                std::to_wstring(height) + L" " + std::to_wstring(fps) + L" fps / " +
                                std::to_wstring(bitrateMbps) + L" Mbps");
                            return S_OK;
                        }
                        lastError = hr;
                    }
                }
            }
            if (width == 1920 && height == 1080 && settings_.width <= 1920) break;
        }
    }
    return lastError;
}

HRESULT MediaEngine::startRecording(const std::filesystem::path& target) {
    if (recording_) return S_FALSE;
    recordingTarget_ = target;
    recordingPartial_ = target.parent_path() /
        (target.stem().wstring() + L".partial.mp4");
    std::error_code fileError;
    std::filesystem::remove(recordingPartial_, fileError);
    audioRawPath_ = recordingPartial_;
    audioRawPath_ += L".pcm";
    std::filesystem::remove(audioRawPath_, fileError);
    {
        std::scoped_lock audioLock(audioMutex_);
        if (audioRaw_) {
            fclose(audioRaw_);
            audioRaw_ = nullptr;
        }
        _wfopen_s(&audioRaw_, audioRawPath_.c_str(), L"wb");
    }
    std::unique_lock lock(encoderMutex_);
    frameQueue_.clear();
    encoderResult_ = E_FAIL;
    encoderCommand_ = EncoderCommand::Start;
    encoderCv_.notify_all();
    encoderCv_.wait(lock, [this] { return encoderCommand_ == EncoderCommand::None; });
    if (FAILED(encoderResult_)) {
        Logger::instance().write(L"ERROR", L"Sink writer creation failed: " + hresultMessage(encoderResult_));
        return encoderResult_;
    }
    lastVideoTimestamp100ns_ = -1;
    audioFramesWritten_ = 0;
    encodedFrames_ = 0;
    lastVideoSampleEnd100ns_ = 0;
    recordingOrigin100ns_ = QpcClock::absolute100ns();
    videoErrorLogged_ = false;
    recording_ = true;
    if (!quietIo_) {
        Logger::instance().write(L"INFO", L"Recording started: " + target.wstring());
        publish(L"Recording");
    }
    return S_OK;
}

bool MediaEngine::muxAudioTrack() {
    std::error_code error;
    const uint64_t pcmBytes = audioRawPath_.empty() ? 0 :
        (std::filesystem::exists(audioRawPath_, error) ? std::filesystem::file_size(audioRawPath_, error) : 0);
    if (!quietIo_) {
        Logger::instance().write(L"INFO", L"Audio PCM size: " + std::to_wstring(pcmBytes) +
            L" bytes / " + std::to_wstring(audioFramesWritten_) + L" frames");
    }
    if (pcmBytes < 256) {
        if (!quietIo_) Logger::instance().write(L"WARN", L"Audio mux skipped: PCM too small");
        return false;
    }
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    auto ffmpeg = std::filesystem::path(modulePath).parent_path() / L"ffmpeg.exe";
    if (!std::filesystem::exists(ffmpeg)) {
        wchar_t found[MAX_PATH]{};
        if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, found, nullptr)) ffmpeg = found;
    }
    if (!std::filesystem::exists(ffmpeg)) return muxAudioTrackMediaFoundation();
    const auto mixed = recordingTarget_.parent_path() /
        (recordingTarget_.stem().wstring() + L".mux.mp4");
    std::wstring command = L"\"" + ffmpeg.wstring() + L"\" -hide_banner -y -i \"" +
        recordingPartial_.wstring() + L"\" -f s16le -ar 48000 -ac 2 -i \"" +
        audioRawPath_.wstring() +
        L"\" -c:v copy -c:a aac -b:a 192k -af apad -shortest \"" +
        mixed.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    std::vector<wchar_t> cmd(command.begin(), command.end());
    cmd.push_back(0);
    if (!CreateProcessW(ffmpeg.c_str(), cmd.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, ffmpeg.parent_path().c_str(), &startup, &process)) {
        return muxAudioTrackMediaFoundation();
    }
    WaitForSingleObject(process.hProcess, 30000);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exitCode != 0 || !std::filesystem::exists(mixed)) return muxAudioTrackMediaFoundation();
    std::filesystem::remove(recordingTarget_, error);
    std::filesystem::rename(mixed, recordingTarget_, error);
    return !error;
}

bool MediaEngine::muxAudioTrackMediaFoundation() {
    if (audioRawPath_.empty() || !std::filesystem::exists(audioRawPath_)) return false;
    std::error_code error;
    const auto pcmBytes = std::filesystem::file_size(audioRawPath_, error);
    if (error || pcmBytes < 256) return false;

    auto fail = [](const wchar_t* step, HRESULT status) {
        Logger::instance().write(L"ERROR", std::wstring(L"Audio mux ") + step + L": " +
            hresultMessage(status));
        return false;
    };

    const auto mixed = recordingTarget_.parent_path() /
        (recordingTarget_.stem().wstring() + L".mux.mp4");
    std::filesystem::remove(mixed, error);

    ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(recordingPartial_.c_str(), nullptr, &reader);
    if (FAILED(hr)) return fail(L"open video", hr);
    hr = reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(hr)) hr = reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    ComPtr<IMFMediaType> nativeType;
    if (SUCCEEDED(hr)) {
        hr = reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &nativeType);
    }
    if (FAILED(hr) || !nativeType) return fail(L"video type", hr);
    const HRESULT passthroughHr = reader->SetCurrentMediaType(
        (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, nativeType.Get());
    ComPtr<IMFMediaType> videoInput = nativeType;
    if (FAILED(passthroughHr)) {
        hr = reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &videoInput);
        if (FAILED(hr) || !videoInput) return fail(L"decoded video type", hr);
        Logger::instance().write(L"WARN", L"Audio mux will re-encode video: " +
            hresultMessage(passthroughHr));
    }

    ComPtr<IMFAttributes> attributes;
    hr = MFCreateAttributes(&attributes, 2);
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
    if (SUCCEEDED(hr)) hr = attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    ComPtr<IMFSinkWriter> writer;
    if (SUCCEEDED(hr)) hr = MFCreateSinkWriterFromURL(mixed.c_str(), nullptr, attributes.Get(), &writer);
    if (FAILED(hr)) return fail(L"create writer", hr);

    DWORD videoStream{};
    hr = writer->AddStream(nativeType.Get(), &videoStream);
    if (FAILED(hr)) return fail(L"add video", hr);
    hr = writer->SetInputMediaType(videoStream, videoInput.Get(), nullptr);
    if (FAILED(hr)) return fail(L"video input", hr);

    ComPtr<IMFMediaType> aacType;
    hr = MFCreateMediaType(&aacType);
    if (SUCCEEDED(hr)) hr = aacType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = aacType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    if (SUCCEEDED(hr)) hr = aacType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    if (SUCCEEDED(hr)) hr = aacType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    if (SUCCEEDED(hr)) hr = aacType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr)) hr = aacType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
    if (SUCCEEDED(hr)) hr = aacType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    if (SUCCEEDED(hr)) hr = aacType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    DWORD audioStream{};
    if (SUCCEEDED(hr)) hr = writer->AddStream(aacType.Get(), &audioStream);
    if (FAILED(hr)) return fail(L"add aac", hr);

    WAVEFORMATEX pcmWave{};
    pcmWave.wFormatTag = WAVE_FORMAT_PCM;
    pcmWave.nChannels = 2;
    pcmWave.nSamplesPerSec = 48000;
    pcmWave.wBitsPerSample = 16;
    pcmWave.nBlockAlign = 4;
    pcmWave.nAvgBytesPerSec = 192000;
    ComPtr<IMFMediaType> pcmType;
    hr = MFCreateMediaType(&pcmType);
    if (SUCCEEDED(hr)) {
        hr = MFInitMediaTypeFromWaveFormatEx(pcmType.Get(), &pcmWave, sizeof(pcmWave));
    }
    if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(audioStream, pcmType.Get(), nullptr);
    if (FAILED(hr)) return fail(L"pcm input", hr);
    hr = writer->BeginWriting();
    if (FAILED(hr)) return fail(L"begin writing", hr);

    for (;;) {
        DWORD actual{};
        DWORD flags{};
        LONGLONG timestamp{};
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &actual, &flags,
            &timestamp, &sample);
        if (FAILED(hr)) return fail(L"read video", hr);
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (sample) {
            hr = writer->WriteSample(videoStream, sample.Get());
            if (FAILED(hr)) return fail(L"write video", hr);
        }
    }

    FILE* pcmFile{};
    if (_wfopen_s(&pcmFile, audioRawPath_.c_str(), L"rb") != 0 || !pcmFile) {
        return fail(L"open pcm", E_FAIL);
    }
    constexpr DWORD chunkBytes = 4096 * 4;
    std::vector<BYTE> chunk(chunkBytes);
    int64_t audioTime = 0;
    const int64_t bytesPerSecond = 192000;
    for (;;) {
        const auto read = fread(chunk.data(), 1, chunk.size(), pcmFile);
        if (read < 4) break;
        const DWORD aligned = static_cast<DWORD>(read) & ~3u;
        ComPtr<IMFMediaBuffer> buffer;
        hr = MFCreateMemoryBuffer(aligned, &buffer);
        BYTE* data{};
        if (SUCCEEDED(hr)) hr = buffer->Lock(&data, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            std::memcpy(data, chunk.data(), aligned);
            buffer->Unlock();
            hr = buffer->SetCurrentLength(aligned);
        }
        ComPtr<IMFSample> sample;
        if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
        if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
        if (SUCCEEDED(hr)) hr = sample->SetSampleTime(audioTime);
        const int64_t duration = aligned * 10'000'000 / bytesPerSecond;
        if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(duration);
        if (SUCCEEDED(hr)) hr = writer->WriteSample(audioStream, sample.Get());
        if (FAILED(hr)) {
            fclose(pcmFile);
            return fail(L"write audio", hr);
        }
        audioTime += duration;
    }
    fclose(pcmFile);

    hr = writer->Finalize();
    writer.Reset();
    if (FAILED(hr)) return fail(L"finalize", hr);
    if (!std::filesystem::exists(mixed)) return fail(L"missing output", E_FAIL);
    std::filesystem::remove(recordingTarget_, error);
    std::filesystem::rename(mixed, recordingTarget_, error);
    if (error) {
        Logger::instance().write(L"ERROR", L"Audio mux rename failed");
        return false;
    }
    return true;
}

HRESULT MediaEngine::stopRecording() {
    if (!recording_.exchange(false)) return S_FALSE;
    {
        std::scoped_lock audioLock(audioMutex_);
        if (audioRaw_) {
            fclose(audioRaw_);
            audioRaw_ = nullptr;
        }
    }
    std::unique_lock lock(encoderMutex_);
    encoderCommand_ = EncoderCommand::Stop;
    encoderCv_.notify_all();
    if (!encoderCv_.wait_for(lock, std::chrono::seconds(12),
        [this] { return encoderCommand_ == EncoderCommand::None; })) {
        Logger::instance().write(L"ERROR", L"Encoder finalize hung, abandoning writer");
        abandonEncoderLocked();
        lock.unlock();
        return publishFinishedFile(E_ABORT);
    }
    return encoderResult_;
}

void MediaEngine::abandonEncoderLocked() {
    ++encoderGeneration_;
    session_.reset();
    frameQueue_.clear();
    encoderCommand_ = EncoderCommand::None;
    if (encoderThread_.joinable()) encoderThread_.detach();
    encoderThread_ = std::thread([this] { encoderLoop(); });
    encoderCv_.notify_all();
}

HRESULT MediaEngine::prepareEncodeTexture(ID3D11Texture2D* source) {
    D3D11_TEXTURE2D_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);
    if (encodeTexture_) {
        D3D11_TEXTURE2D_DESC current{};
        encodeTexture_->GetDesc(&current);
        if (current.Width != encodeWidth_ || current.Height != encodeHeight_) encodeTexture_.Reset();
    }
    if (!encodeTexture_) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = encodeWidth_;
        desc.Height = encodeHeight_;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        const HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &encodeTexture_);
        if (FAILED(hr)) return hr;
    }
    if (sourceDesc.Width == encodeWidth_ && sourceDesc.Height == encodeHeight_) {
        context_->CopyResource(encodeTexture_.Get(), source);
        return S_OK;
    }
    return blitScale(source);
}

HRESULT MediaEngine::blitScale(ID3D11Texture2D* source) {
    if (!videoDevice_ || !videoContext_ || !encodeTexture_) return E_NOINTERFACE;
    D3D11_TEXTURE2D_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);
    if (!videoProcessor_ || scalerInW_ != sourceDesc.Width || scalerInH_ != sourceDesc.Height) {
        videoProcessor_.Reset();
        videoEnum_.Reset();
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
        desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        desc.InputWidth = sourceDesc.Width;
        desc.InputHeight = sourceDesc.Height;
        desc.OutputWidth = encodeWidth_;
        desc.OutputHeight = encodeHeight_;
        desc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
        HRESULT hr = videoDevice_->CreateVideoProcessorEnumerator(&desc, &videoEnum_);
        if (FAILED(hr)) return hr;
        hr = videoDevice_->CreateVideoProcessor(videoEnum_.Get(), 0, &videoProcessor_);
        if (FAILED(hr)) return hr;
        videoContext_->VideoProcessorSetStreamAutoProcessingMode(videoProcessor_.Get(), 0, FALSE);
        videoContext_->VideoProcessorSetStreamFilter(videoProcessor_.Get(), 0,
            D3D11_VIDEO_PROCESSOR_FILTER_EDGE_ENHANCEMENT, TRUE, 2);
        scalerInW_ = sourceDesc.Width;
        scalerInH_ = sourceDesc.Height;
    }

    ID3D11Texture2D* input = source;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
    inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorInputView> inputView;
    HRESULT hr = videoDevice_->CreateVideoProcessorInputView(source, videoEnum_.Get(), &inputDesc, &inputView);
    if (FAILED(hr)) {
        if (!scalerSource_ || scalerInW_ != sourceDesc.Width || scalerInH_ != sourceDesc.Height) {
            D3D11_TEXTURE2D_DESC copyDesc = sourceDesc;
            copyDesc.Usage = D3D11_USAGE_DEFAULT;
            copyDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            copyDesc.CPUAccessFlags = 0;
            copyDesc.MiscFlags = 0;
            copyDesc.MipLevels = 1;
            scalerSource_.Reset();
            hr = device_->CreateTexture2D(&copyDesc, nullptr, &scalerSource_);
            if (FAILED(hr)) return hr;
        }
        context_->CopyResource(scalerSource_.Get(), source);
        input = scalerSource_.Get();
        inputView.Reset();
        hr = videoDevice_->CreateVideoProcessorInputView(input, videoEnum_.Get(), &inputDesc, &inputView);
        if (FAILED(hr)) return hr;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
    outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorOutputView> outputView;
    hr = videoDevice_->CreateVideoProcessorOutputView(encodeTexture_.Get(), videoEnum_.Get(),
        &outputDesc, &outputView);
    if (FAILED(hr)) return hr;

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.Get();
    return videoContext_->VideoProcessorBlt(videoProcessor_.Get(), outputView.Get(), 0, 1, &stream);
}

HRESULT MediaEngine::copyFramePixels(ID3D11Texture2D* texture, std::vector<uint8_t>& pixels) {
    const DWORD rowBytes = encodeWidth_ * 4;
    const DWORD bufferBytes = rowBytes * encodeHeight_;
    pixels.resize(bufferBytes);

    ID3D11Texture2D* source = texture;
    if (SUCCEEDED(prepareEncodeTexture(texture)) && encodeTexture_) {
        source = encodeTexture_.Get();
    }

    D3D11_TEXTURE2D_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);
    D3D11_TEXTURE2D_DESC stagingDesc{};
    if (encoderStaging_) encoderStaging_->GetDesc(&stagingDesc);
    if (!encoderStaging_ || stagingDesc.Width != sourceDesc.Width ||
        stagingDesc.Height != sourceDesc.Height) {
        stagingDesc = sourceDesc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;
        encoderStaging_.Reset();
        const HRESULT createHr = device_->CreateTexture2D(&stagingDesc, nullptr, &encoderStaging_);
        if (FAILED(createHr)) return createHr;
    }

    context_->CopyResource(encoderStaging_.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = context_->Map(encoderStaging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return hr;

    const auto* mappedBytes = static_cast<const uint8_t*>(mapped.pData);
    scaleBilinear(mappedBytes, sourceDesc.Width, sourceDesc.Height, mapped.RowPitch,
        pixels.data(), encodeWidth_, encodeHeight_);
    context_->Unmap(encoderStaging_.Get(), 0);
    composeCursor(pixels.data(), encodeWidth_, encodeHeight_, rowBytes);
    return S_OK;
}

HRESULT MediaEngine::enqueueFrame(ID3D11Texture2D* texture) {
    if (!texture) return E_INVALIDARG;

    bool gpuInput = false;
    {
        std::scoped_lock lock(encoderMutex_);
        gpuInput = session_ && session_->gpuInput;
    }

    QueuedFrame frame;
    frame.timestamp100ns = QpcClock::absolute100ns() -
        (recordingOrigin100ns_.load() > 0 ? recordingOrigin100ns_.load() : 0);
    if (frame.timestamp100ns < 0) frame.timestamp100ns = 0;

    HRESULT hr = S_OK;
    if (gpuInput) {
        // Keep the frame on the D3D11 device. The encoder receives the
        // texture through MFCreateDXGISurfaceBuffer, so no full-frame
        // GPU->CPU readback/copy is performed.
        hr = prepareEncodeTexture(texture);
        if (SUCCEEDED(hr) && encodeTexture_) {
            D3D11_TEXTURE2D_DESC desc{};
            encodeTexture_->GetDesc(&desc);
            hr = device_->CreateTexture2D(&desc, nullptr, &frame.texture);
            if (SUCCEEDED(hr)) {
                context_->CopyResource(frame.texture.Get(), encodeTexture_.Get());
            }
        }
    } else {
        hr = copyFramePixels(texture, frame.pixels);
    }
    if (FAILED(hr)) return hr;
    if (!gpuInput && frame.pixels.empty()) return E_FAIL;
    if (gpuInput && !frame.texture) return E_FAIL;

    {
        std::scoped_lock lock(encoderMutex_);
        if (frameQueue_.size() >= 4) {
            frameQueue_.pop_front();
            ++droppedFrames_;
        }
        frameQueue_.push_back(std::move(frame));
    }
    encoderCv_.notify_one();
    return S_OK;
}

HRESULT MediaEngine::writeQueuedFrame(const std::shared_ptr<EncodeSession>& session,
                                      const QueuedFrame& frame) {
    if (!session || !session->writer) return E_FAIL;
    const int64_t duration = 10'000'000 / std::max(1u, encodeFps_);
    int64_t timestamp = frame.timestamp100ns;
    const int64_t previousEnd = lastVideoSampleEnd100ns_.load();
    if (timestamp < previousEnd) timestamp = previousEnd;

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = E_FAIL;
    if (session->gpuInput && frame.texture) {
        hr = MFCreateDXGISurfaceBuffer(
            IID_ID3D11Texture2D, frame.texture.Get(), 0, FALSE, &buffer);
    } else if (!frame.pixels.empty()) {
        const DWORD bufferBytes = static_cast<DWORD>(frame.pixels.size());
        hr = MFCreateMemoryBuffer(bufferBytes, &buffer);
        BYTE* destination{};
        if (SUCCEEDED(hr)) hr = buffer->Lock(&destination, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            std::memcpy(destination, frame.pixels.data(), frame.pixels.size());
            buffer->Unlock();
            hr = buffer->SetCurrentLength(bufferBytes);
        }
    }
    if (FAILED(hr) || !buffer) return FAILED(hr) ? hr : E_FAIL;

    ComPtr<IMFSample> sample;
    if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
    if (SUCCEEDED(hr)) hr = sample->SetSampleTime(timestamp);
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(duration);
    if (SUCCEEDED(hr)) hr = session->writer->WriteSample(session->videoStream, sample.Get());
    if (SUCCEEDED(hr)) {
        ++encodedFrames_;
        lastVideoSampleEnd100ns_ = timestamp + duration;
    }
    return hr;
}

void MediaEngine::encoderLoop() {
    ComApartment apartment;
    for (;;) {
        std::unique_lock lock(encoderMutex_);
        encoderCv_.wait(lock, [this] {
            return encoderCommand_ != EncoderCommand::None || !frameQueue_.empty();
        });

        if (encoderCommand_ == EncoderCommand::Exit) {
            auto session = session_;
            const auto generation = encoderGeneration_.load();
            lock.unlock();
            if (session) (void)finishFile(session, generation);
            lock.lock();
            frameQueue_.clear();
            encoderCommand_ = EncoderCommand::None;
            encoderCv_.notify_all();
            break;
        }

        if (encoderCommand_ == EncoderCommand::Start) {
            encoderResult_ = createSinkWriter(recordingPartial_);
            encoderCommand_ = EncoderCommand::None;
            encoderCv_.notify_all();
            continue;
        }

        if (encoderCommand_ == EncoderCommand::Stop) {
            auto session = session_;
            const auto generation = encoderGeneration_.load();
            while (!frameQueue_.empty()) {
                auto frame = std::move(frameQueue_.front());
                frameQueue_.pop_front();
                lock.unlock();
                const HRESULT writeHr = writeQueuedFrame(session, frame);
                if (FAILED(writeHr) && !videoErrorLogged_.exchange(true)) {
                    Logger::instance().write(L"ERROR", L"Video frame write failed: " +
                        hresultMessage(writeHr));
                }
                lock.lock();
            }
            lock.unlock();
            encoderResult_ = finishFile(session, generation);
            lock.lock();
            encoderCommand_ = EncoderCommand::None;
            encoderCv_.notify_all();
            continue;
        }

        auto session = session_;
        if (frameQueue_.empty() || !session) {
            if (!session) frameQueue_.clear();
            continue;
        }
        auto frame = std::move(frameQueue_.front());
        frameQueue_.pop_front();
        lock.unlock();
        const HRESULT writeHr = writeQueuedFrame(session, frame);
        if (FAILED(writeHr) && !videoErrorLogged_.exchange(true)) {
            Logger::instance().write(L"ERROR", L"Video frame write failed: " +
                hresultMessage(writeHr));
        }
    }
}

HRESULT MediaEngine::finishFile(const std::shared_ptr<EncodeSession>& session, uint64_t generation) {
    gpuPaused_ = true;

    HRESULT finalizeResult = S_OK;
    if (session && session->writer) {
        if (!quietIo_) {
            Logger::instance().write(L"INFO", L"Stopping; encoded frames: " +
                std::to_wstring(encodedFrames_.load()));
            Logger::instance().write(L"INFO", L"Finalizing recording");
        }
        const auto started = std::chrono::steady_clock::now();
        finalizeResult = session->writer->Finalize();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        if (FAILED(finalizeResult)) {
            Logger::instance().write(L"ERROR", L"Finalize failed: " + hresultMessage(finalizeResult));
        } else if (!quietIo_) {
            Logger::instance().write(L"INFO", L"Finalize took " + std::to_wstring(ms) + L" ms");
        }
        session->writer.Reset();
    }
    if (session_ == session) session_.reset();
    gpuPaused_ = false;
    if (generation != encoderGeneration_.load()) return E_ABORT;
    return publishFinishedFile(finalizeResult);
}

HRESULT MediaEngine::publishFinishedFile(HRESULT finalizeResult) {
    std::error_code error;
    const bool haveFrames = encodedFrames_.load() > 0 && !recordingPartial_.empty();
    const bool finalized = SUCCEEDED(finalizeResult);
    bool published = false;
    if (haveFrames && (finalized || std::filesystem::exists(recordingPartial_))) {
        if (finalized && muxAudioTrack()) {
            if (!quietIo_) Logger::instance().write(L"INFO", L"Audio track muxed into recording");
            std::filesystem::remove(recordingPartial_, error);
            published = std::filesystem::exists(recordingTarget_);
        } else {
            std::filesystem::remove(recordingTarget_, error);
            error.clear();
            std::filesystem::rename(recordingPartial_, recordingTarget_, error);
            if (error) {
                Logger::instance().write(L"ERROR", L"Could not publish recording, error " +
                    std::to_wstring(error.value()));
            } else {
                published = std::filesystem::exists(recordingTarget_);
            }
            if (published && !finalized) {
                Logger::instance().write(L"WARN", L"Published recording without a clean finalize");
            }
        }
    } else if (encodedFrames_.load() == 0) {
        Logger::instance().write(L"ERROR", L"No video frames were encoded");
        if (std::filesystem::exists(recordingPartial_, error)) {
            Logger::instance().write(L"WARN", L"Recording kept as partial file: " + recordingPartial_.wstring());
        }
    }
    std::filesystem::remove(audioRawPath_, error);
    audioRawPath_.clear();
    if (!quietIo_) {
        Logger::instance().write(L"INFO", L"Recording stopped; encoded frames: " +
            std::to_wstring(encodedFrames_.load()));
        publish(L"Ready");
    }
    if (published || std::filesystem::exists(recordingTarget_, error)) return S_OK;
    return finalized ? E_FAIL : finalizeResult;
}

HRESULT MediaEngine::writeAudio(std::span<const std::byte> pcm, int64_t timestamp100ns) {
    (void)timestamp100ns;
    std::scoped_lock lock(audioMutex_);
    if (!recording_ || !audioRaw_ || !audioConfigured_ || pcm.empty()) return S_FALSE;
    const auto* samples = reinterpret_cast<const float*>(pcm.data());
    const size_t count = pcm.size() / sizeof(float);
    std::vector<int16_t> converted(count);
    for (size_t i = 0; i < count; ++i) {
        const float value = std::clamp(samples[i], -1.0f, 1.0f);
        converted[i] = static_cast<int16_t>(value * 32767.0f);
    }
    fwrite(converted.data(), sizeof(int16_t), converted.size(), audioRaw_);
    audioFramesWritten_ += count / 2;
    return S_OK;
}

void MediaEngine::startPreview() {
    if (running_.exchange(true)) return;
    captureThread_ = std::jthread([this] { captureLoop(); });
}

void MediaEngine::clearPointer() {
    std::scoped_lock lock(pointerMutex_);
    pointerVisible_ = false;
    pointerPos_ = {};
    pointerHotspot_ = {};
    pointerWidth_ = 0;
    pointerHeight_ = 0;
    pointerPitch_ = 0;
    pointerType_ = 0;
    pointerShape_.clear();
}

void MediaEngine::updatePointer(const DXGI_OUTDUPL_FRAME_INFO& info) {
    std::vector<uint8_t> buffer;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape{};
    if (info.PointerShapeBufferSize > 0 && duplication_) {
        buffer.resize(info.PointerShapeBufferSize);
        UINT written = 0;
        if (FAILED(duplication_->GetFramePointerShape(static_cast<UINT>(buffer.size()),
                buffer.data(), &written, &shape)) || written == 0) {
            buffer.clear();
        } else {
            buffer.resize(written);
        }
    }

    std::scoped_lock lock(pointerMutex_);
    if (info.LastMouseUpdateTime.QuadPart != 0) {
        pointerVisible_ = info.PointerPosition.Visible != FALSE;
        pointerPos_ = info.PointerPosition.Position;
    }
    if (!buffer.empty()) {
        pointerType_ = shape.Type;
        pointerWidth_ = shape.Width;
        pointerHeight_ = shape.Height;
        pointerPitch_ = shape.Pitch;
        pointerHotspot_ = shape.HotSpot;
        pointerShape_ = std::move(buffer);
    }
}

void MediaEngine::composeCursor(uint8_t* dest, uint32_t width, uint32_t height, uint32_t pitch) const {
    if (wgc_.includesCursor()) return;
    if (!dest || !width || !height || !pitch || !captureWidth_ || !captureHeight_) return;
    std::scoped_lock lock(pointerMutex_);
    if (!pointerVisible_ || pointerShape_.empty() || !pointerWidth_ || !pointerHeight_ ||
        !pointerPitch_) {
        return;
    }

    UINT shapeW = pointerWidth_;
    UINT shapeH = pointerHeight_;
    const bool monochrome = pointerType_ == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
    if (monochrome) {
        const size_t oneMask = static_cast<size_t>(pointerPitch_) * pointerHeight_;
        if (pointerShape_.size() >= oneMask * 2) {
            shapeH = pointerHeight_;
        } else {
            shapeH = pointerHeight_ / 2;
        }
        if (!shapeH) return;
        const size_t needed = static_cast<size_t>(pointerPitch_) * shapeH * 2;
        if (pointerShape_.size() < needed) return;
    } else if (pointerShape_.size() < static_cast<size_t>(pointerPitch_) * shapeH) {
        return;
    }

    const float scaleX = static_cast<float>(width) / static_cast<float>(captureWidth_);
    const float scaleY = static_cast<float>(height) / static_cast<float>(captureHeight_);
    const int destW = std::max(1, static_cast<int>(std::lround(static_cast<float>(shapeW) * scaleX)));
    const int destH = std::max(1, static_cast<int>(std::lround(static_cast<float>(shapeH) * scaleY)));
    const int left = static_cast<int>(std::lround(
        (static_cast<float>(pointerPos_.x) - static_cast<float>(pointerHotspot_.x)) * scaleX));
    const int top = static_cast<int>(std::lround(
        (static_cast<float>(pointerPos_.y) - static_cast<float>(pointerHotspot_.y)) * scaleY));

    for (int y = 0; y < destH; ++y) {
        const int dy = top + y;
        if (dy < 0 || dy >= static_cast<int>(height)) continue;
        const UINT sy = std::min(shapeH - 1,
            static_cast<UINT>(y * static_cast<int>(shapeH) / destH));
        for (int x = 0; x < destW; ++x) {
            const int dx = left + x;
            if (dx < 0 || dx >= static_cast<int>(width)) continue;
            const UINT sx = std::min(shapeW - 1,
                static_cast<UINT>(x * static_cast<int>(shapeW) / destW));
            auto* dst = dest + static_cast<size_t>(dy) * pitch + static_cast<size_t>(dx) * 4;

            if (monochrome) {
                const auto* andRow = pointerShape_.data() + static_cast<size_t>(sy) * pointerPitch_;
                const auto* xorRow = andRow + static_cast<size_t>(shapeH) * pointerPitch_;
                const uint8_t andBit = pointerMaskBit(andRow, sx);
                const uint8_t xorBit = pointerMaskBit(xorRow, sx);
                if (andBit && !xorBit) continue;
                if (!andBit && !xorBit) {
                    dst[0] = dst[1] = dst[2] = 0;
                } else if (!andBit && xorBit) {
                    dst[0] = dst[1] = dst[2] = 255;
                } else {
                    dst[0] = static_cast<uint8_t>(dst[0] ^ 255);
                    dst[1] = static_cast<uint8_t>(dst[1] ^ 255);
                    dst[2] = static_cast<uint8_t>(dst[2] ^ 255);
                }
                dst[3] = 255;
                continue;
            }

            const auto* src = pointerShape_.data() + static_cast<size_t>(sy) * pointerPitch_ +
                static_cast<size_t>(sx) * 4;
            if (pointerType_ == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
                if (src[3] == 255) {
                    dst[0] ^= src[0];
                    dst[1] ^= src[1];
                    dst[2] ^= src[2];
                    dst[3] = 255;
                } else {
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst[3] = 255;
                }
            } else {
                blendPremultiplied(dst, src);
            }
        }
    }
}

void MediaEngine::stopPreview() {
    running_ = false;
    if (captureThread_.joinable()) captureThread_.join();
    wgc_.stop();
    duplication_.Reset();
}

void MediaEngine::captureLoop() {
    ComApartment apartment;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const auto shouldEnqueue = [this]() {
        if (!recording_) return false;
        const auto now = QpcClock::absolute100ns();
        const auto frameInterval = 10'000'000 / std::max(1u, encodeFps_);
        if (lastVideoTimestamp100ns_ >= 0 &&
            now - lastVideoTimestamp100ns_ < std::max<int64_t>(0, frameInterval - 10'000)) {
            return false;
        }
        lastVideoTimestamp100ns_ = now;
        return true;
    };
    const auto enqueueLast = [&]() {
        if (!shouldEnqueue()) return;
        ComPtr<ID3D11Texture2D> retained;
        {
            std::scoped_lock frameLock(frameMutex_);
            retained = latestFrame_;
        }
        if (retained && FAILED(enqueueFrame(retained.Get()))) ++droppedFrames_;
    };

    while (running_) {
        const auto now = std::chrono::steady_clock::now();
        if (!gpuPaused_ && (now - lastRebind_ > std::chrono::milliseconds(500) ||
                            (!wgc_.active() && !duplication_))) {
            lastRebind_ = now;
            rebindCapture(false);
        }
        if (gpuPaused_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (wgc_.active()) {
            ComPtr<ID3D11Texture2D> texture;
            const HRESULT hr = wgc_.tryAcquire(&texture);
            if (hr == S_FALSE || !texture) {
                if (lastExclusive_ && now - lastWgcFrame_ > std::chrono::milliseconds(1500)) {
                    wgc_.stop();
                    lastRebind_ = {};
                }
                enqueueLast();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (FAILED(hr)) {
                wgc_.stop();
                ++droppedFrames_;
                continue;
            }
            lastWgcFrame_ = now;
            ++capturedFrames_;
            rememberTexture(texture.Get());
            if (shouldEnqueue()) {
                const auto enqueueHr = enqueueFrame(texture.Get());
                if (FAILED(enqueueHr)) {
                    ++droppedFrames_;
                    if (!videoErrorLogged_.exchange(true)) {
                        Logger::instance().write(L"ERROR", L"Video frame copy failed: " +
                            hresultMessage(enqueueHr));
                    }
                }
            }
            continue;
        }

        if (!duplication_) {
            enqueueLast();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        const auto hr = duplication_->AcquireNextFrame(100, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            enqueueLast();
            continue;
        }
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            duplication_.Reset();
            rebindCapture(true);
            continue;
        }
        if (FAILED(hr)) {
            ++droppedFrames_;
            continue;
        }
        updatePointer(info);
        ComPtr<ID3D11Texture2D> texture;
        resource.As(&texture);
        ++capturedFrames_;
        rememberTexture(texture.Get());
        if (shouldEnqueue()) {
            const auto enqueueHr = enqueueFrame(texture.Get());
            if (FAILED(enqueueHr)) {
                ++droppedFrames_;
                if (!videoErrorLogged_.exchange(true)) {
                    Logger::instance().write(L"ERROR", L"Video frame copy failed: " +
                        hresultMessage(enqueueHr));
                }
            }
        }
        duplication_->ReleaseFrame();
    }
}

HRESULT MediaEngine::takeScreenshot(const std::filesystem::path& target) {
    std::scoped_lock deviceLock(deviceMutex_);
    ComPtr<ID3D11Texture2D> frame;
    {
        std::scoped_lock lock(frameMutex_);
        frame = latestFrame_;
    }
    if (!frame) return MF_E_NOT_AVAILABLE;

    D3D11_TEXTURE2D_DESC desc{};
    frame->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &staging);
    if (FAILED(hr)) return hr;
    context_->CopyResource(staging.Get(), frame.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return hr;

    const UINT pitch = desc.Width * 4;
    std::vector<uint8_t> pixels(static_cast<size_t>(pitch) * desc.Height);
    const auto* mappedBytes = static_cast<const uint8_t*>(mapped.pData);
    for (UINT y = 0; y < desc.Height; ++y) {
        std::memcpy(pixels.data() + static_cast<size_t>(y) * pitch,
            mappedBytes + static_cast<size_t>(y) * mapped.RowPitch, pitch);
    }
    context_->Unmap(staging.Get(), 0);
    composeCursor(pixels.data(), desc.Width, desc.Height, pitch);
    return writePngBgra(pixels.data(), desc.Width, desc.Height, pitch, target);
}

void MediaEngine::publish(std::wstring_view text) {
    if (callback_) callback_(text);
}

std::chrono::microseconds MediaEngine::lastMediaDuration() const {
    const int64_t videoEnd = lastVideoSampleEnd100ns_.load();
    if (videoEnd > 0) return std::chrono::microseconds(videoEnd / 10);
    const auto frames = encodedFrames_.load();
    const auto fps = std::max(1u, encodeFps_);
    return std::chrono::microseconds(frames * 1'000'000ull / fps);
}

namespace {

bool fileIsNewer(const std::filesystem::path& candidate, const std::filesystem::path& source) {
    std::error_code error;
    if (!std::filesystem::exists(candidate, error) || !std::filesystem::exists(source, error)) return false;
    return std::filesystem::last_write_time(candidate, error) >= std::filesystem::last_write_time(source, error);
}

bool writeJpegBgra(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride,
                   const std::filesystem::path& target) {
    if (!bgra || !width || !height) return false;
    std::vector<uint8_t> bgr(static_cast<size_t>(width) * height * 3);
    for (uint32_t y = 0; y < height; ++y) {
        const auto* src = bgra + static_cast<size_t>(y) * stride;
        auto* dst = bgr.data() + static_cast<size_t>(y) * width * 3;
        for (uint32_t x = 0; x < width; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 0];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 2];
        }
    }
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(target.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr)) {
        hr = frame->WritePixels(height, width * 3, static_cast<UINT>(bgr.size()), bgr.data());
    }
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    return SUCCEEDED(hr);
}

double readCachedDuration(const std::filesystem::path& cache) {
    std::ifstream input(cache);
    double seconds = 0;
    if (input) input >> seconds;
    return seconds > 0 ? seconds : 0;
}

void writeCachedDuration(const std::filesystem::path& cache, double seconds) {
    std::ofstream output(cache, std::ios::trunc);
    if (output) output << seconds;
}

    bool grabVideoFrameJpeg(const std::filesystem::path& video, const std::filesystem::path& jpeg) {
    ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(video.c_str(), nullptr, &reader);
    if (FAILED(hr) || !reader) return false;
    hr = reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(hr)) hr = reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    ComPtr<IMFMediaType> rgb;
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&rgb);
    if (SUCCEEDED(hr)) hr = rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (SUCCEEDED(hr)) {
        hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, rgb.Get());
    }
    if (FAILED(hr)) return false;

    PROPVARIANT duration{};
    PropVariantInit(&duration);
    if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration)) &&
        duration.vt == VT_UI8 && duration.uhVal.QuadPart > 30'000'000ull) {
        PROPVARIANT position{};
        PropVariantInit(&position);
        position.vt = VT_I8;
        position.hVal.QuadPart = static_cast<LONGLONG>(std::min<ULONGLONG>(duration.uhVal.QuadPart / 10,
            20'000'000ull));
        reader->SetCurrentPosition(GUID_NULL, position);
        PropVariantClear(&position);
    }
    PropVariantClear(&duration);

    ComPtr<IMFSample> sample;
    for (int attempt = 0; attempt < 8 && !sample; ++attempt) {
        DWORD flags{};
        LONGLONG timestamp{};
        DWORD actual{};
        ComPtr<IMFSample> next;
        hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &actual, &flags,
            &timestamp, &next);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
        sample = next;
    }
    if (!sample) return false;
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer) return false;
    BYTE* data{};
    DWORD maxLen{}, length{};
    if (FAILED(buffer->Lock(&data, &maxLen, &length)) || !data || length < 16) return false;

    ComPtr<IMFMediaType> current;
    UINT32 srcW = 0, srcH = 0;
    GUID subtype{};
    if (FAILED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current)) ||
        FAILED(current->GetGUID(MF_MT_SUBTYPE, &subtype)) ||
        (subtype != MFVideoFormat_RGB32) ||
        FAILED(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &srcW, &srcH)) ||
        srcW < 16 || srcH < 16 || srcW > 7680 || srcH > 4320) {
        buffer->Unlock();
        return false;
    }
    INT32 stride = 0;
    current->GetUINT32(MF_MT_DEFAULT_STRIDE, reinterpret_cast<UINT32*>(&stride));
    if (stride == 0) stride = static_cast<INT32>(srcW * 4);
    const uint32_t absStride = static_cast<uint32_t>(stride < 0 ? -stride : stride);
    if (absStride < srcW * 4 || length < absStride * (srcH - 1) + srcW * 4) {
        buffer->Unlock();
        return false;
    }
    const uint32_t dstW = 320;
    const uint32_t dstH = std::max(2u, (srcH * dstW / srcW) & ~1u);
    std::vector<uint8_t> scaled(static_cast<size_t>(dstW) * dstH * 4);
    const auto* srcBytes = stride < 0 ? data + static_cast<size_t>(absStride) * (srcH - 1) : data;
    scaleBilinear(srcBytes, srcW, srcH, absStride, scaled.data(), dstW, dstH);
    buffer->Unlock();
    return writeJpegBgra(scaled.data(), dstW, dstH, dstW * 4, jpeg);
}

double probeDurationSeconds(const std::filesystem::path& video) {
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(video.c_str(), nullptr, &reader))) return 0;
    PROPVARIANT duration{};
    PropVariantInit(&duration);
    double seconds = 0;
    if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration)) &&
        (duration.vt == VT_UI8 || duration.vt == VT_I8) && duration.uhVal.QuadPart) {
        seconds = static_cast<double>(duration.uhVal.QuadPart) / 10'000'000.0;
    }
    PropVariantClear(&duration);
    return seconds;
}

} // namespace

MediaPreview previewMediaFile(const std::filesystem::path& file,
                              const std::filesystem::path& thumbsDirectory,
                              bool decodeFrames) {
    MediaPreview preview;
    try {
        std::error_code error;
        const auto ext = file.extension().wstring();
        if (_wcsicmp(ext.c_str(), L".png") == 0) {
            preview.hasThumbnail = true;
            return preview;
        }
        if (_wcsicmp(ext.c_str(), L".mp4") != 0) return preview;
        std::filesystem::create_directories(thumbsDirectory, error);
        const auto stamp = thumbsDirectory / (file.filename().wstring() + L".dur");
        const auto jpeg = thumbsDirectory / (file.filename().wstring() + L".jpg");
        if (fileIsNewer(stamp, file)) preview.durationSeconds = readCachedDuration(stamp);
        if (fileIsNewer(jpeg, file)) preview.hasThumbnail = true;
        if (!decodeFrames) return preview;
        if (preview.durationSeconds <= 0) {
            preview.durationSeconds = probeDurationSeconds(file);
            if (preview.durationSeconds > 0) writeCachedDuration(stamp, preview.durationSeconds);
        }
        if (!preview.hasThumbnail) preview.hasThumbnail = grabVideoFrameJpeg(file, jpeg);
    } catch (...) {
        return {};
    }
    return preview;
}

} // namespace kanekist
