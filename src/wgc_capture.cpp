#include "wgc_capture.h"

#include "core.h"

#include <atomic>
#include <roapi.h>
#include <wrl/wrappers/corewrappers.h>
#include <windows.foundation.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;

using ABI::Windows::Graphics::SizeInt32;
using ABI::Windows::Graphics::Capture::IDirect3D11CaptureFrame;
using ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool;
using ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePoolStatics2;
using ABI::Windows::Graphics::Capture::IGraphicsCaptureItem;
using ABI::Windows::Graphics::Capture::IGraphicsCaptureSession;
using ABI::Windows::Graphics::Capture::IGraphicsCaptureSession2;
using ABI::Windows::Graphics::Capture::IGraphicsCaptureSession3;
using ABI::Windows::Graphics::Capture::IGraphicsCaptureSession6;
using ABI::Windows::Graphics::DirectX::DirectXPixelFormat;
using ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;

namespace kanekist {
namespace {

constexpr DirectXPixelFormat kCaptureFormat =
    DirectXPixelFormat::DirectXPixelFormat_B8G8R8A8UIntNormalized;

HRESULT wrapDevice(ID3D11Device* device, IDirect3DDevice** wrapped) {
    if (!device || !wrapped) return E_POINTER;
    *wrapped = nullptr;
    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    ComPtr<IInspectable> inspectable;
    if (SUCCEEDED(hr)) {
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), &inspectable);
    }
    ComPtr<IDirect3DDevice> winrtDevice;
    if (SUCCEEDED(hr)) hr = inspectable.As(&winrtDevice);
    if (SUCCEEDED(hr)) *wrapped = winrtDevice.Detach();
    return hr;
}

HRESULT textureFromSurface(ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface* surface,
                           ID3D11Texture2D** texture) {
    if (!surface || !texture) return E_POINTER;
    *texture = nullptr;
    ComPtr<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
    HRESULT hr = surface->QueryInterface(IID_PPV_ARGS(&access));
    if (SUCCEEDED(hr)) hr = access->GetInterface(IID_PPV_ARGS(texture));
    return hr;
}

} // namespace

struct WgcCapture::Impl {
    ComPtr<IDirect3DDevice> winrtDevice;
    ComPtr<IGraphicsCaptureItem> item;
    ComPtr<IDirect3D11CaptureFramePool> pool;
    ComPtr<IGraphicsCaptureSession> session;
    SizeInt32 size{};
    HWND window{};
    HMONITOR monitor{};
    UINT width{};
    UINT height{};
    bool includesCursor{};
    bool active{};
    std::atomic_bool closed{false};

    void stop() {
        ComPtr<ABI::Windows::Foundation::IClosable> closable;
        if (session && SUCCEEDED(session.As(&closable))) closable->Close();
        closable.Reset();
        if (pool && SUCCEEDED(pool.As(&closable))) closable->Close();
        session.Reset();
        pool.Reset();
        item.Reset();
        winrtDevice.Reset();
        window = nullptr;
        monitor = nullptr;
        width = 0;
        height = 0;
        includesCursor = false;
        active = false;
        closed = false;
        size = {};
    }

    HRESULT startWithItem(ID3D11Device* device, IGraphicsCaptureItem* captureItem,
                          HWND hwnd, HMONITOR hmon) {
        stop();
        if (!device || !captureItem) return E_POINTER;

        HRESULT hr = wrapDevice(device, &winrtDevice);
        if (FAILED(hr)) return hr;

        item = captureItem;
        hr = item->get_Size(&size);
        if (FAILED(hr)) return hr;
        if (size.Width < 2 || size.Height < 2) return E_UNEXPECTED;
        size.Width &= ~1;
        size.Height &= ~1;

        ComPtr<IDirect3D11CaptureFramePoolStatics2> poolFactory;
        hr = RoGetActivationFactory(
            HStringReference(RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool).Get(),
            IID_PPV_ARGS(&poolFactory));
        if (FAILED(hr)) return hr;
        hr = poolFactory->CreateFreeThreaded(winrtDevice.Get(), kCaptureFormat, 3, size, &pool);
        if (FAILED(hr)) return hr;
        hr = pool->CreateCaptureSession(item.Get(), &session);
        if (FAILED(hr)) return hr;

        ComPtr<IGraphicsCaptureSession2> session2;
        if (SUCCEEDED(session.As(&session2))) {
            if (SUCCEEDED(session2->put_IsCursorCaptureEnabled(TRUE))) includesCursor = true;
        }
        ComPtr<IGraphicsCaptureSession3> session3;
        if (SUCCEEDED(session.As(&session3))) {
            session3->put_IsBorderRequired(FALSE);
        }
        ComPtr<IGraphicsCaptureSession6> session6;
        if (SUCCEEDED(session.As(&session6))) {
            session6->put_IncludeSecondaryWindows(TRUE);
        }
        hr = session->StartCapture();
        if (FAILED(hr)) {
            stop();
            return hr;
        }

        window = hwnd;
        monitor = hmon;
        width = static_cast<UINT>(size.Width);
        height = static_cast<UINT>(size.Height);
        active = true;
        return S_OK;
    }
};

WgcCapture::WgcCapture() : impl_(std::make_unique<Impl>()) {}
WgcCapture::~WgcCapture() { stop(); }

HRESULT WgcCapture::startForWindow(ID3D11Device* device, HWND window) {
    if (!window || !IsWindow(window)) return E_INVALIDARG;
    ComPtr<IGraphicsCaptureItemInterop> interop;
    HRESULT hr = RoGetActivationFactory(
        HStringReference(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem).Get(),
        IID_PPV_ARGS(&interop));
    if (FAILED(hr)) return hr;
    ComPtr<IGraphicsCaptureItem> item;
    hr = interop->CreateForWindow(window, IID_PPV_ARGS(&item));
    if (FAILED(hr)) return hr;
    return impl_->startWithItem(device, item.Get(), window, MonitorFromWindow(window, MONITOR_DEFAULTTONULL));
}

HRESULT WgcCapture::startForMonitor(ID3D11Device* device, HMONITOR monitor) {
    if (!monitor) return E_INVALIDARG;
    ComPtr<IGraphicsCaptureItemInterop> interop;
    HRESULT hr = RoGetActivationFactory(
        HStringReference(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem).Get(),
        IID_PPV_ARGS(&interop));
    if (FAILED(hr)) return hr;
    ComPtr<IGraphicsCaptureItem> item;
    hr = interop->CreateForMonitor(monitor, IID_PPV_ARGS(&item));
    if (FAILED(hr)) return hr;
    return impl_->startWithItem(device, item.Get(), nullptr, monitor);
}

void WgcCapture::stop() {
    if (impl_) impl_->stop();
}

HRESULT WgcCapture::tryAcquire(ID3D11Texture2D** texture) {
    if (!texture) return E_POINTER;
    *texture = nullptr;
    if (!impl_ || !impl_->active || !impl_->pool) return E_FAIL;

    ComPtr<IDirect3D11CaptureFrame> frame;
    HRESULT hr = impl_->pool->TryGetNextFrame(&frame);
    if (FAILED(hr)) {
        if (hr == static_cast<HRESULT>(0x80000013L)) impl_->closed = true;
        return hr;
    }
    if (!frame) return S_FALSE;

    SizeInt32 content{};
    if (SUCCEEDED(frame->get_ContentSize(&content)) &&
        content.Width >= 2 && content.Height >= 2 &&
        (content.Width != impl_->size.Width || content.Height != impl_->size.Height)) {
        content.Width &= ~1;
        content.Height &= ~1;
        hr = impl_->pool->Recreate(impl_->winrtDevice.Get(), kCaptureFormat, 3, content);
        if (SUCCEEDED(hr)) {
            impl_->size = content;
            impl_->width = static_cast<UINT>(content.Width);
            impl_->height = static_cast<UINT>(content.Height);
        }
        return S_FALSE;
    }

    ComPtr<ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface> surface;
    hr = frame->get_Surface(&surface);
    if (FAILED(hr)) return hr;
    hr = textureFromSurface(surface.Get(), texture);
    if (SUCCEEDED(hr) && *texture) {
        D3D11_TEXTURE2D_DESC desc{};
        (*texture)->GetDesc(&desc);
        impl_->width = desc.Width;
        impl_->height = desc.Height;
    }
    return hr;
}

bool WgcCapture::active() const noexcept { return impl_ && impl_->active && !impl_->closed; }
bool WgcCapture::closed() const noexcept { return impl_ && impl_->closed.load(); }
bool WgcCapture::includesCursor() const noexcept { return impl_ && impl_->includesCursor; }
UINT WgcCapture::width() const noexcept { return impl_ ? impl_->width : 0; }
UINT WgcCapture::height() const noexcept { return impl_ ? impl_->height : 0; }
HWND WgcCapture::window() const noexcept { return impl_ ? impl_->window : nullptr; }
HMONITOR WgcCapture::monitor() const noexcept { return impl_ ? impl_->monitor : nullptr; }

} // namespace kanekist
