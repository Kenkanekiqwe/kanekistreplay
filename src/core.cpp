#include "core.h"

#include <KnownFolders.h>
#include <ShlObj.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace kanekist {
namespace {

std::wstring jsonBool(bool value) { return value ? L"true" : L"false"; }

std::optional<uint32_t> readNumber(std::wstring_view text, std::wstring_view key) {
    const auto pos = text.find(L"\"" + std::wstring(key) + L"\"");
    if (pos == std::wstring_view::npos) return std::nullopt;
    const auto colon = text.find(L':', pos);
    if (colon == std::wstring_view::npos) return std::nullopt;
    wchar_t* end{};
    const auto value = wcstoul(text.data() + colon + 1, &end, 10);
    return end == text.data() + colon + 1 ? std::nullopt : std::optional<uint32_t>(value);
}

bool readBool(std::wstring_view text, std::wstring_view key, bool fallback) {
    const auto pos = text.find(L"\"" + std::wstring(key) + L"\"");
    if (pos == std::wstring_view::npos) return fallback;
    const auto colon = text.find(L':', pos);
    if (colon == std::wstring_view::npos) return fallback;
    const auto rest = text.substr(colon + 1);
    if (rest.find(L"true") < rest.find_first_of(L",}")) return true;
    if (rest.find(L"false") < rest.find_first_of(L",}")) return false;
    return fallback;
}

std::wstring readString(std::wstring_view text, std::wstring_view key) {
    const auto pos = text.find(L"\"" + std::wstring(key) + L"\"");
    if (pos == std::wstring_view::npos) return {};
    const auto colon = text.find(L':', pos);
    const auto first = text.find(L'"', colon + 1);
    if (first == std::wstring_view::npos) return {};
    std::wstring result;
    for (size_t i = first + 1; i < text.size(); ++i) {
        const wchar_t ch = text[i];
        if (ch == L'\\' && i + 1 < text.size() &&
            (text[i + 1] == L'\\' || text[i + 1] == L'"')) {
            result.push_back(text[++i]);
        } else if (ch == L'"') {
            return result;
        } else {
            result.push_back(ch);
        }
    }
    return {};
}

std::wstring jsonEscape(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size() + 8);
    for (const auto ch : value) {
        if (ch == L'\\' || ch == L'"') result.push_back(L'\\');
        result.push_back(ch);
    }
    return result;
}

} // namespace

std::filesystem::path appDataDirectory() {
    PWSTR raw{};
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw))) {
        std::filesystem::path result(raw);
        CoTaskMemFree(raw);
        result /= L"KanekistReplay";
        std::filesystem::create_directories(result);
        return result;
    }
    return std::filesystem::temp_directory_path() / L"KanekistReplay";
}

Settings Settings::load() {
    Settings result;
    result.outputDirectory = appDataDirectory() / L"Media";
    std::filesystem::create_directories(result.outputDirectory);
    std::wifstream input(appDataDirectory() / L"settings.json");
    if (!input) return result;
    const std::wstring text((std::istreambuf_iterator<wchar_t>(input)), {});
    result.width = readNumber(text, L"width").value_or(result.width);
    result.height = readNumber(text, L"height").value_or(result.height);
    result.fps = readNumber(text, L"fps").value_or(result.fps);
    result.bitrateMbps = readNumber(text, L"bitrateMbps").value_or(result.bitrateMbps);
    if (const auto minutes = readNumber(text, L"replayMinutes")) {
        result.replaySeconds = normalizeReplaySeconds(*minutes * 60);
    } else {
        result.replaySeconds = normalizeReplaySeconds(
            readNumber(text, L"replaySeconds").value_or(result.replaySeconds));
    }
    result.captureSystemAudio = readBool(text, L"captureSystemAudio", true);
    result.captureMicrophone = readBool(text, L"captureMicrophone", false);
    result.showOverlay = readBool(text, L"showOverlay", true);
    result.instantReplayEnabled = readBool(text, L"instantReplayEnabled", false);
    result.showAdvancedStats = readBool(text, L"showAdvancedStats", true);
    result.startMinimized = readBool(text, L"startMinimized", false);
    result.overlayMenuKey = readNumber(text, L"overlayMenuKey").value_or(result.overlayMenuKey);
    result.hotkeyOverlay = packHotkey(MOD_ALT, result.overlayMenuKey);
    result.hotkeyRecord = readNumber(text, L"hotkeyRecord").value_or(result.hotkeyRecord);
    result.hotkeySaveReplay = readNumber(text, L"hotkeySaveReplay").value_or(result.hotkeySaveReplay);
    result.hotkeyScreenshot = readNumber(text, L"hotkeyScreenshot").value_or(result.hotkeyScreenshot);
    result.hotkeyFpsHud = readNumber(text, L"hotkeyFpsHud").value_or(result.hotkeyFpsHud);
    result.hotkeyOverlay = readNumber(text, L"hotkeyOverlay").value_or(result.hotkeyOverlay);
    result.overlayMenuKey = Hotkey::fromPacked(result.hotkeyOverlay).vk;
    result.systemVolume = readNumber(text, L"systemVolume").value_or(result.systemVolume);
    result.microphoneVolume = readNumber(text, L"microphoneVolume").value_or(result.microphoneVolume);
    result.noiseGate = readNumber(text, L"noiseGate").value_or(result.noiseGate);
    result.systemVolume = std::clamp(result.systemVolume, 0u, 200u);
    result.microphoneVolume = std::clamp(result.microphoneVolume, 0u, 200u);
    result.noiseGate = std::clamp(result.noiseGate, 0u, 40u);
    result.overlayCorner = readNumber(text, L"overlayCorner").value_or(result.overlayCorner);
    if (const auto anchor = readNumber(text, L"hudAnchor")) {
        result.overlayCorner = *anchor % 9;
    } else {
        constexpr uint32_t legacy[] = {0, 2, 6, 8};
        result.overlayCorner = legacy[result.overlayCorner % 4];
    }
    result.hudOffsetX = readNumber(text, L"hudOffsetX").value_or(result.hudOffsetX);
    result.hudOffsetY = readNumber(text, L"hudOffsetY").value_or(result.hudOffsetY);
    result.hudFontSize = readNumber(text, L"hudFontSize").value_or(result.hudFontSize);
    result.hudTextColor = readNumber(text, L"hudTextColor").value_or(result.hudTextColor);
    result.hudOutlineColor = readNumber(text, L"hudOutlineColor").value_or(result.hudOutlineColor);
    result.hudOutlineThickness = readNumber(text, L"hudOutlineThickness").value_or(result.hudOutlineThickness);
    result.overlayMenuX = readNumber(text, L"overlayMenuX").value_or(result.overlayMenuX);
    result.overlayMenuY = readNumber(text, L"overlayMenuY").value_or(result.overlayMenuY);
    if (auto value = readString(text, L"hudTemplate"); !value.empty()) result.hudTemplate = value;
    if (auto value = readString(text, L"hudFontName"); !value.empty()) result.hudFontName = value;
    if (auto path = readString(text, L"outputDirectory"); !path.empty()) result.outputDirectory = path;
    result.microphoneDeviceId = readString(text, L"microphoneDeviceId");
    result.systemAudioDeviceId = readString(text, L"systemAudioDeviceId");
    result.videoCodec = std::min(2u, readNumber(text, L"videoCodec").value_or(0));
    result.normalizeEncodePreset();
    return result;
}

void Settings::normalizeEncodePreset() {
    width &= ~1u;
    height &= ~1u;
    if (height < 720) {
        width = 1280;
        height = 720;
    }
    if (height > 2160) {
        width = 3840;
        height = 2160;
    }
    if (fps < 45) fps = 30;
    else if (fps < 90) fps = 60;
    else fps = 120;
    videoCodec = std::min(2u, videoCodec);
    bitrateMbps = suggestedBitrateMbps(width, height, fps, videoCodec);
}

uint32_t suggestedBitrateMbps(uint32_t width, uint32_t height, uint32_t fps, uint32_t codec) noexcept {
    const double pixels = std::max(1u, width) * static_cast<double>(std::max(1u, height));
    const double scale = pixels / (1920.0 * 1080.0) * (std::max(30u, fps) / 60.0);
    double factor = 60.0;
    uint32_t minMbps = 25;
    uint32_t maxMbps = 150;
    if (codec == 1) {
        factor = 34.0;
        minMbps = 12;
        maxMbps = 80;
    } else if (codec == 2) {
        factor = 24.0;
        minMbps = 8;
        maxMbps = 55;
    }
    const uint32_t mbps = static_cast<uint32_t>(std::lround(factor * scale));
    return std::clamp(mbps, minMbps, maxMbps);
}

const wchar_t* videoCodecLabel(uint32_t codec) noexcept {
    if (codec == 1) return L"HEVC";
    if (codec == 2) return L"AV1";
    return L"H.264";
}

int resolutionPresetIndex(uint32_t width, uint32_t height) noexcept {
    if (height <= 720 && width <= 1280) return 0;
    if (height <= 1080 && width <= 1920) return 1;
    if (height <= 1440 && width <= 2560) return 2;
    if (height <= 2160 && width <= 3840) return 3;
    return 4;
}

void applyResolutionPreset(Settings& settings, int index, uint32_t nativeWidth, uint32_t nativeHeight) {
    nativeWidth &= ~1u;
    nativeHeight &= ~1u;
    if (nativeWidth < 1280) nativeWidth = 1920;
    if (nativeHeight < 720) nativeHeight = 1080;
    switch (index) {
    case 0: settings.width = 1280; settings.height = 720; break;
    case 2: settings.width = 2560; settings.height = 1440; break;
    case 3: settings.width = 3840; settings.height = 2160; break;
    case 4: settings.width = nativeWidth; settings.height = nativeHeight; break;
    default: settings.width = 1920; settings.height = 1080; break;
    }
    // Encoding resolution is independent from the physical capture resolution.
    // Higher presets are valid: the GPU scaler/encoder can upscale the captured frame.
    settings.width &= ~1u;
    settings.height &= ~1u;
    settings.bitrateMbps = suggestedBitrateMbps(settings.width, settings.height, settings.fps, settings.videoCodec);
}

void applyEncodeFps(Settings& settings, uint32_t fps) {
    if (fps < 45) settings.fps = 30;
    else if (fps < 90) settings.fps = 60;
    else     settings.fps = 120;
    settings.bitrateMbps = suggestedBitrateMbps(settings.width, settings.height, settings.fps, settings.videoCodec);
}

std::wstring encodePresetLabel(uint32_t width, uint32_t height, uint32_t fps, uint32_t codec) {
    const wchar_t* res = L"экран";
    if (height <= 720 && width <= 1280) res = L"720p";
    else if (height <= 1080 && width <= 1920) res = L"1080p";
    else if (height <= 1440 && width <= 2560) res = L"1440p";
    else if (height <= 2160 && width <= 3840) res = L"4K";
    return std::wstring(res) + L" " + std::to_wstring(fps) + L" " + videoCodecLabel(codec);
}

void Settings::save() const {
    const auto target = appDataDirectory() / L"settings.json";
    const auto temporary = target.wstring() + L".tmp";
    std::wofstream output(temporary, std::ios::trunc);
    output << L"{\n"
           << L"  \"width\": " << width << L",\n"
           << L"  \"height\": " << height << L",\n"
           << L"  \"fps\": " << fps << L",\n"
           << L"  \"bitrateMbps\": " << bitrateMbps << L",\n"
           << L"  \"replaySeconds\": " << replaySeconds << L",\n"
           << L"  \"replayMinutes\": " << (replaySeconds / 60) << L",\n"
           << L"  \"captureSystemAudio\": " << jsonBool(captureSystemAudio) << L",\n"
           << L"  \"captureMicrophone\": " << jsonBool(captureMicrophone) << L",\n"
           << L"  \"showOverlay\": " << jsonBool(showOverlay) << L",\n"
           << L"  \"instantReplayEnabled\": " << jsonBool(instantReplayEnabled) << L",\n"
           << L"  \"showAdvancedStats\": " << jsonBool(showAdvancedStats) << L",\n"
           << L"  \"startMinimized\": " << jsonBool(startMinimized) << L",\n"
           << L"  \"overlayMenuKey\": " << overlayMenuKey << L",\n"
           << L"  \"hotkeyRecord\": " << hotkeyRecord << L",\n"
           << L"  \"hotkeySaveReplay\": " << hotkeySaveReplay << L",\n"
           << L"  \"hotkeyScreenshot\": " << hotkeyScreenshot << L",\n"
           << L"  \"hotkeyFpsHud\": " << hotkeyFpsHud << L",\n"
           << L"  \"hotkeyOverlay\": " << hotkeyOverlay << L",\n"
           << L"  \"systemVolume\": " << systemVolume << L",\n"
           << L"  \"microphoneVolume\": " << microphoneVolume << L",\n"
           << L"  \"noiseGate\": " << noiseGate << L",\n"
           << L"  \"overlayCorner\": " << overlayCorner << L",\n"
           << L"  \"hudAnchor\": " << overlayCorner << L",\n"
           << L"  \"hudOffsetX\": " << hudOffsetX << L",\n"
           << L"  \"hudOffsetY\": " << hudOffsetY << L",\n"
           << L"  \"hudFontSize\": " << hudFontSize << L",\n"
           << L"  \"hudTextColor\": " << hudTextColor << L",\n"
           << L"  \"hudOutlineColor\": " << hudOutlineColor << L",\n"
           << L"  \"hudOutlineThickness\": " << hudOutlineThickness << L",\n"
           << L"  \"overlayMenuX\": " << overlayMenuX << L",\n"
           << L"  \"overlayMenuY\": " << overlayMenuY << L",\n"
           << L"  \"hudTemplate\": \"" << jsonEscape(hudTemplate) << L"\",\n"
           << L"  \"hudFontName\": \"" << jsonEscape(hudFontName) << L"\",\n"
           << L"  \"microphoneDeviceId\": \"" << jsonEscape(microphoneDeviceId) << L"\",\n"
           << L"  \"systemAudioDeviceId\": \"" << jsonEscape(systemAudioDeviceId) << L"\",\n"
           << L"  \"videoCodec\": " << videoCodec << L",\n"
           << L"  \"outputDirectory\": \"" << jsonEscape(outputDirectory.wstring()) << L"\"\n"
           << L"}\n";
    output.flush();
    output.close();
    if (!output) {
        Logger::instance().write(L"ERROR", L"Failed to write settings.json");
        std::error_code cleanup;
        std::filesystem::remove(temporary, cleanup);
        return;
    }
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    if (error) {
        std::filesystem::remove(target, error);
        error.clear();
        std::filesystem::rename(temporary, target, error);
    }
    if (error) {
        Logger::instance().write(L"ERROR", L"Failed to replace settings.json");
        std::filesystem::remove(temporary, error);
    }
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::Logger() {
    stream_.open(appDataDirectory() / L"kanekist.log", std::ios::app);
}

void Logger::write(std::wstring_view level, std::wstring_view message) {
    std::scoped_lock lock(mutex_);
    SYSTEMTIME time{};
    GetLocalTime(&time);
    stream_ << std::setfill(L'0') << std::setw(2) << time.wHour << L':'
            << std::setw(2) << time.wMinute << L':' << std::setw(2) << time.wSecond
            << L" [" << level << L"] " << message << L'\n';
    stream_.flush();
}

ComApartment::ComApartment(DWORD model) : result_(CoInitializeEx(nullptr, model)) {}
ComApartment::~ComApartment() { if (SUCCEEDED(result_)) CoUninitialize(); }

QpcClock::QpcClock() {
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&origin_));
    QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&frequency_));
}

int64_t QpcClock::now100ns() const {
    int64_t current{};
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&current));
    return ticksTo100ns(current - origin_, frequency_);
}

int64_t QpcClock::absolute100ns() {
    LARGE_INTEGER ticks{}, frequency{};
    QueryPerformanceCounter(&ticks);
    QueryPerformanceFrequency(&frequency);
    return ticksTo100ns(ticks.QuadPart, frequency.QuadPart);
}

int64_t QpcClock::ticksTo100ns(int64_t ticks, int64_t frequency) {
    if (frequency <= 0) return 0;
    return static_cast<int64_t>((static_cast<long double>(ticks) * 10'000'000.0L) / frequency);
}

std::filesystem::path uniqueMediaPath(const std::filesystem::path& directory,
                                      std::wstring_view prefix, std::wstring_view extension) {
    std::filesystem::create_directories(directory);
    SYSTEMTIME t{};
    GetLocalTime(&t);
    std::wostringstream name;
    name << prefix << L'_' << t.wYear << std::setfill(L'0') << std::setw(2) << t.wMonth
         << std::setw(2) << t.wDay << L'_' << std::setw(2) << t.wHour << std::setw(2)
         << t.wMinute << std::setw(2) << t.wSecond << extension;
    return directory / name.str();
}

std::filesystem::path uniqueMediaPath(std::wstring_view prefix, std::wstring_view extension) {
    return uniqueMediaPath(Settings::load().outputDirectory, prefix, extension);
}

bool Hotkey::valid() const noexcept {
    if (vk == 0 || vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU ||
        vk == VK_LWIN || vk == VK_RWIN || vk == VK_ESCAPE) {
        return false;
    }
    const bool functionKey = vk >= VK_F1 && vk <= VK_F24;
    return modifiers != 0 || functionKey;
}

std::wstring hotkeyLabel(Hotkey hotkey) {
    if (!hotkey.valid()) return L"не задано";
    std::wstring text;
    if (hotkey.modifiers & MOD_CONTROL) text += L"Ctrl+";
    if (hotkey.modifiers & MOD_SHIFT) text += L"Shift+";
    if (hotkey.modifiers & MOD_ALT) text += L"Alt+";
    if (hotkey.modifiers & MOD_WIN) text += L"Win+";
    UINT scan = MapVirtualKeyW(hotkey.vk, MAPVK_VK_TO_VSC) << 16;
    if (hotkey.vk == VK_LEFT || hotkey.vk == VK_RIGHT || hotkey.vk == VK_UP ||
        hotkey.vk == VK_DOWN || hotkey.vk == VK_INSERT || hotkey.vk == VK_DELETE ||
        hotkey.vk == VK_HOME || hotkey.vk == VK_END || hotkey.vk == VK_PRIOR ||
        hotkey.vk == VK_NEXT || hotkey.vk == VK_NUMLOCK || hotkey.vk == VK_DIVIDE) {
        scan |= 1u << 24;
    }
    wchar_t name[64]{};
    if (GetKeyNameTextW(static_cast<LONG>(scan), name, ARRAYSIZE(name)) > 0) {
        text += name;
    } else {
        text.push_back(static_cast<wchar_t>(hotkey.vk));
    }
    return text;
}

uint32_t normalizeReplaySeconds(uint32_t seconds) noexcept {
    uint32_t minutes = seconds == 0 ? 1 : (seconds + 30) / 60;
    minutes = std::clamp(minutes, 1u, 30u);
    return minutes * 60;
}

uint64_t estimateBytesPerSecond(uint32_t bitrateMbps) noexcept {
    const uint64_t mbps = std::max(1u, bitrateMbps);
    return mbps * 125'000ull + mbps * 15'000ull;
}

bool queryFreeDiskBytes(const std::filesystem::path& path, uint64_t& freeBytes) {
    std::error_code error;
    auto probe = path;
    if (!std::filesystem::exists(probe, error)) probe = probe.parent_path();
    if (probe.empty()) probe = path.root_path();
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(probe.c_str(), &available, nullptr, nullptr)) return false;
    freeBytes = available.QuadPart;
    return true;
}

void excludeWindowFromCapture(HWND window) noexcept {
    if (!window) return;
#ifndef WDA_EXCLUDEFROMCAPTURE
    constexpr DWORD WDA_EXCLUDEFROMCAPTURE = 0x00000011;
#endif
    if (!SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE)) {
        SetWindowDisplayAffinity(window, WDA_MONITOR);
    }
}

std::wstring hresultMessage(HRESULT value) {
    wchar_t* buffer{};
    const auto length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(value), 0,
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring result = length ? std::wstring(buffer, length) : L"Unknown error";
    LocalFree(buffer);
    return result;
}

} // namespace kanekist
