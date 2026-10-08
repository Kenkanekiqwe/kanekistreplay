#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <memory>

namespace kanekist {

class WgcCapture {
public:
    WgcCapture();
    ~WgcCapture();
    WgcCapture(const WgcCapture&) = delete;
    WgcCapture& operator=(const WgcCapture&) = delete;

    HRESULT startForWindow(ID3D11Device* device, HWND window);
    HRESULT startForMonitor(ID3D11Device* device, HMONITOR monitor);
    void stop();
    HRESULT tryAcquire(ID3D11Texture2D** texture);

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool includesCursor() const noexcept;
    [[nodiscard]] UINT width() const noexcept;
    [[nodiscard]] UINT height() const noexcept;
    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] HWND window() const noexcept;
    [[nodiscard]] HMONITOR monitor() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kanekist
