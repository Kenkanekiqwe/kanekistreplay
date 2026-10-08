#pragma once

#include <Windows.h>
#include <objbase.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>

namespace kanekist {

struct Hotkey {
    uint32_t modifiers = 0;
    uint32_t vk = 0;
    [[nodiscard]] uint32_t packed() const noexcept { return (modifiers << 16) | (vk & 0xffffu); }
    static Hotkey fromPacked(uint32_t packed) noexcept {
        return Hotkey{packed >> 16, packed & 0xffffu};
    }
    [[nodiscard]] bool valid() const noexcept;
};

constexpr uint32_t packHotkey(uint32_t modifiers, uint32_t vk) noexcept {
    return (modifiers << 16) | (vk & 0xffffu);
}

struct Settings {
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 60;
    uint32_t bitrateMbps = 60;
    uint32_t replaySeconds = 60;
    bool captureSystemAudio = true;
    bool captureMicrophone = false;
    bool showOverlay = true;
    bool instantReplayEnabled = false;
    bool showAdvancedStats = true;
    bool startMinimized = false;
    uint32_t overlayMenuKey = 'K';
    uint32_t hotkeyRecord = packHotkey(MOD_CONTROL | MOD_SHIFT, 'R');
    uint32_t hotkeySaveReplay = packHotkey(MOD_CONTROL | MOD_SHIFT, 'S');
    uint32_t hotkeyScreenshot = packHotkey(MOD_CONTROL | MOD_SHIFT, 'P');
    uint32_t hotkeyFpsHud = packHotkey(MOD_CONTROL | MOD_SHIFT, 'O');
    uint32_t hotkeyOverlay = packHotkey(MOD_ALT, 'K');
    uint32_t systemVolume = 100;
    uint32_t microphoneVolume = 100;
    uint32_t noiseGate = 0;
    uint32_t overlayCorner = 1;
    uint32_t hudOffsetX = 18;
    uint32_t hudOffsetY = 18;
    uint32_t hudFontSize = 28;
    uint32_t hudTextColor = 0x76e897;
    uint32_t hudOutlineColor = 0x08080a;
    uint32_t hudOutlineThickness = 2;
    uint32_t overlayMenuX = UINT32_MAX;
    uint32_t overlayMenuY = UINT32_MAX;
    std::wstring hudTemplate = L"{fps} FPS";
    std::wstring hudFontName = L"Bahnschrift";
    std::wstring microphoneDeviceId;
    std::wstring systemAudioDeviceId;
    uint32_t videoCodec = 0; // 0 H.264, 1 HEVC, 2 AV1
    std::filesystem::path outputDirectory;

    static Settings load();
    void save() const;
    void normalizeEncodePreset();
};

std::wstring encodePresetLabel(uint32_t width, uint32_t height, uint32_t fps, uint32_t codec = 0);
const wchar_t* videoCodecLabel(uint32_t codec) noexcept;
uint32_t suggestedBitrateMbps(uint32_t width, uint32_t height, uint32_t fps, uint32_t codec = 0) noexcept;
int resolutionPresetIndex(uint32_t width, uint32_t height) noexcept;
void applyResolutionPreset(Settings& settings, int index, uint32_t nativeWidth, uint32_t nativeHeight);
void applyEncodeFps(Settings& settings, uint32_t fps);

class Logger {
public:
    static Logger& instance();
    void write(std::wstring_view level, std::wstring_view message);

private:
    Logger();
    std::mutex mutex_;
    std::wofstream stream_;
};

class ComApartment {
public:
    explicit ComApartment(DWORD model = COINIT_MULTITHREADED);
    ~ComApartment();
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    [[nodiscard]] HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_;
};

class QpcClock {
public:
    QpcClock();
    [[nodiscard]] int64_t now100ns() const;
    [[nodiscard]] static int64_t absolute100ns();
    [[nodiscard]] static int64_t ticksTo100ns(int64_t ticks, int64_t frequency);

private:
    int64_t origin_{};
    int64_t frequency_{};
};

std::filesystem::path appDataDirectory();
std::filesystem::path uniqueMediaPath(std::wstring_view prefix, std::wstring_view extension);
std::filesystem::path uniqueMediaPath(const std::filesystem::path& directory,
                                      std::wstring_view prefix, std::wstring_view extension);
std::wstring hresultMessage(HRESULT value);
std::wstring hotkeyLabel(Hotkey hotkey);
uint32_t normalizeReplaySeconds(uint32_t seconds) noexcept;
uint64_t estimateBytesPerSecond(uint32_t bitrateMbps) noexcept;
bool queryFreeDiskBytes(const std::filesystem::path& path, uint64_t& freeBytes);
void excludeWindowFromCapture(HWND window) noexcept;

} // namespace kanekist
