#pragma once

#include <Windows.h>
#include <string>

namespace kanekist {

struct GameTarget {
    HWND window{};
    DWORD processId{};
    HMONITOR monitor{};
    bool fullscreen{};
    bool exclusiveD3d{};
    std::wstring processName;
};

[[nodiscard]] GameTarget resolveGameTarget();
[[nodiscard]] bool isD3dExclusiveFullscreen() noexcept;
bool tryMakeBorderless(HWND window);
void pinOverlayToGame(HWND overlay, HWND game);

} // namespace kanekist
