#include "replay_recorder.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <sstream>
#include <string_view>

namespace kanekist {
namespace {

constexpr std::wstring_view tempPrefix = L".kanekist-replay-";

void removeNoThrow(const std::filesystem::path& path) noexcept {
    if (path.empty()) return;
    std::error_code error;
    std::filesystem::remove(path, error);
}

bool hasLineBreak(const std::filesystem::path& path) {
    const auto text = path.wstring();
    return text.find_first_of(L"\r\n") != std::wstring::npos;
}

bool isMp4Path(const std::filesystem::path& path) {
    auto extension = path.extension().wstring();
    std::ranges::transform(extension, extension.begin(),
        [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    return extension == L".mp4";
}

std::wstring quoteWindowsArgument(std::wstring_view argument) {
    std::wstring result(1, L'"');
    size_t backslashes = 0;
    for (const wchar_t value : argument) {
        if (value == L'\\') {
            ++backslashes;
            continue;
        }
        if (value == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
        } else {
            result.append(backslashes, L'\\');
            result.push_back(value);
        }
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::wstring makeCommandLine(const std::vector<std::wstring>& arguments) {
    std::wstring command;
    for (const auto& argument : arguments) {
        if (!command.empty()) command.push_back(L' ');
        command += quoteWindowsArgument(argument);
    }
    return command;
}

std::string ffconcatPath(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    std::string result;
    result.reserve(utf8.size() + 16);
    result += "file '";
    for (const char8_t value : utf8) {
        if (value == u8'\'') {
            result += "'\\''";
        } else {
            result.push_back(static_cast<char>(value));
        }
    }
    result += "'\n";
    return result;
}

std::string secondsText(std::chrono::microseconds duration) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(6)
           << static_cast<double>(duration.count()) / 1'000'000.0;
    return stream.str();
}

std::wstring utf8ToWide(std::string_view text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return std::wstring(text.begin(), text.end());
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}

std::wstring readErrorLog(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    std::string text((std::istreambuf_iterator<char>(stream)),
                     std::istreambuf_iterator<char>());
    constexpr size_t maxErrorLength = 4096;
    if (text.size() > maxErrorLength) text.erase(0, text.size() - maxErrorLength);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) {
        text.pop_back();
    }
    return utf8ToWide(text);
}

double readProgress(
    const std::filesystem::path& path,
    std::chrono::microseconds expectedDuration) {
    if (expectedDuration.count() <= 0) return 0.0;
    std::ifstream stream(path);
    std::string line;
    int64_t elapsedUs = 0;
    while (std::getline(stream, line)) {
        constexpr std::string_view currentKey = "out_time_us=";
        constexpr std::string_view legacyKey = "out_time_ms=";
        std::string_view value;
        if (line.starts_with(currentKey)) {
            value = std::string_view(line).substr(currentKey.size());
        } else if (line.starts_with(legacyKey)) {
            // Despite its historical name, ffmpeg reports microseconds here.
            value = std::string_view(line).substr(legacyKey.size());
        } else {
            continue;
        }
        try {
            elapsedUs = std::max(elapsedUs, std::stoll(std::string(value)));
        } catch (...) {
        }
    }
    return std::clamp(
        static_cast<double>(elapsedUs) / expectedDuration.count(), 0.0, 0.99);
}

std::filesystem::path absolutePath(const std::filesystem::path& path) {
    std::error_code error;
    auto result = std::filesystem::absolute(path, error);
    return error ? path : result;
}

} // namespace

struct ReplayRecorder::Record {
    ReplaySegment segment;
    std::atomic_bool deleteWhenReleased{false};

    ~Record() {
        if (deleteWhenReleased.load()) removeNoThrow(segment.path);
    }
};

ReplayRecorder::ReplayRecorder(Limits limits) : limits_(limits) {
    if (limits_.duration.count() < 0) limits_.duration = {};
}

ReplayRecorder::~ReplayRecorder() {
    {
        std::lock_guard launchLock(launchMutex_);
        if (saveThread_.joinable()) {
            saveThread_.request_stop();
            saveThread_.join();
        }
    }
    clear();
}

void ReplayRecorder::setLimits(Limits limits) {
    if (limits.duration.count() < 0) limits.duration = {};
    std::lock_guard lock(mutex_);
    limits_ = limits;
    trimLocked();
}

ReplayRecorder::Limits ReplayRecorder::limits() const {
    std::lock_guard lock(mutex_);
    return limits_;
}

bool ReplayRecorder::addCompletedSegment(
    std::filesystem::path path,
    std::chrono::microseconds duration,
    uint64_t bytes) {
    if (path.empty() || duration.count() <= 0 || hasLineBreak(path)) return false;
    path = absolutePath(path);
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) return false;

    auto record = std::make_shared<Record>();
    record->segment = {std::move(path), duration, bytes};

    std::lock_guard lock(mutex_);
    if (bufferedDuration_.count() >
            std::numeric_limits<int64_t>::max() - duration.count() ||
        bufferedBytes_ > std::numeric_limits<uint64_t>::max() - bytes) {
        return false;
    }
    if (std::ranges::any_of(segments_, [&](const auto& existing) {
            return existing->segment.path == record->segment.path;
        })) {
        return false;
    }
    segments_.push_back(std::move(record));
    bufferedDuration_ += duration;
    bufferedBytes_ += bytes;
    trimLocked();
    return true;
}

ReplayRecorder::Snapshot ReplayRecorder::snapshot() const {
    Snapshot result;
    auto pins = std::make_shared<std::vector<std::shared_ptr<Record>>>();
    std::lock_guard lock(mutex_);
    pins->reserve(segments_.size());
    result.segments_.reserve(segments_.size());
    for (const auto& record : segments_) {
        pins->push_back(record);
        result.segments_.push_back(record->segment);
    }
    result.duration_ = bufferedDuration_;
    result.bytes_ = bufferedBytes_;
    result.pins_ = std::move(pins);
    return result;
}

void ReplayRecorder::clear() {
    std::lock_guard lock(mutex_);
    for (const auto& record : segments_) record->deleteWhenReleased = true;
    segments_.clear();
    bufferedDuration_ = {};
    bufferedBytes_ = 0;
}

size_t ReplayRecorder::segmentCount() const {
    std::lock_guard lock(mutex_);
    return segments_.size();
}

std::chrono::microseconds ReplayRecorder::bufferedDuration() const {
    std::lock_guard lock(mutex_);
    return bufferedDuration_;
}

uint64_t ReplayRecorder::bufferedBytes() const {
    std::lock_guard lock(mutex_);
    return bufferedBytes_;
}

void ReplayRecorder::trimLocked() {
    const auto durationLimit =
        std::chrono::duration_cast<std::chrono::microseconds>(limits_.duration);
    while (!segments_.empty() &&
           ((durationLimit.count() > 0 && bufferedDuration_ > durationLimit) ||
            (limits_.maxBytes > 0 && bufferedBytes_ > limits_.maxBytes))) {
        const auto record = std::move(segments_.front());
        segments_.pop_front();
        bufferedDuration_ -= record->segment.duration;
        bufferedBytes_ -= record->segment.bytes;
        record->deleteWhenReleased = true;
    }
}

bool ReplayRecorder::saveAsync(
    std::filesystem::path ffmpegExecutable,
    std::filesystem::path finalMp4) {
    return saveAsync(snapshot(), std::move(ffmpegExecutable), std::move(finalMp4));
}

bool ReplayRecorder::saveAsync(
    Snapshot snapshotValue,
    std::filesystem::path ffmpegExecutable,
    std::filesystem::path finalMp4) {
    if (snapshotValue.empty() || ffmpegExecutable.empty() || finalMp4.empty() ||
        !isMp4Path(finalMp4) || hasLineBreak(finalMp4)) {
        return false;
    }
    ffmpegExecutable = absolutePath(ffmpegExecutable);
    finalMp4 = absolutePath(finalMp4);
    std::error_code error;
    if (!std::filesystem::is_regular_file(ffmpegExecutable, error) || error) return false;

    std::lock_guard launchLock(launchMutex_);
    if (saving_.load()) return false;
    if (saveThread_.joinable()) saveThread_.join();
    {
        std::lock_guard stateLock(stateMutex_);
        lastError_.clear();
        lastSavedPath_.clear();
    }
    progress_ = 0.0;
    saving_ = true;
    try {
        saveThread_ = std::jthread(
            [this, snapshotValue = std::move(snapshotValue),
             ffmpegExecutable = std::move(ffmpegExecutable),
             finalMp4 = std::move(finalMp4)](std::stop_token stop) mutable {
                try {
                    saveWorker(stop, std::move(snapshotValue),
                               std::move(ffmpegExecutable), std::move(finalMp4));
                } catch (const std::exception& exception) {
                    finishSave(utf8ToWide(exception.what()));
                } catch (...) {
                    finishSave(L"Unexpected replay save failure.");
                }
            });
    } catch (const std::exception& exception) {
        finishSave(utf8ToWide(exception.what()));
        return false;
    } catch (...) {
        finishSave(L"Unable to start replay save worker.");
        return false;
    }
    return true;
}

void ReplayRecorder::cancelSave() {
    std::lock_guard launchLock(launchMutex_);
    if (saveThread_.joinable()) saveThread_.request_stop();
}

std::wstring ReplayRecorder::lastError() const {
    std::lock_guard lock(stateMutex_);
    return lastError_;
}

std::filesystem::path ReplayRecorder::lastSavedPath() const {
    std::lock_guard lock(stateMutex_);
    return lastSavedPath_;
}

void ReplayRecorder::finishSave(
    std::wstring error,
    std::filesystem::path savedPath) {
    const bool succeeded = error.empty();
    {
        std::lock_guard lock(stateMutex_);
        lastError_ = std::move(error);
        lastSavedPath_ = std::move(savedPath);
    }
    if (succeeded) progress_ = 1.0;
    saving_ = false;
}

void ReplayRecorder::saveWorker(
    std::stop_token stop,
    Snapshot snapshotValue,
    std::filesystem::path ffmpegExecutable,
    std::filesystem::path finalMp4) {
    const auto directory = finalMp4.parent_path();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        finishSave(L"Unable to create the replay output directory.");
        return;
    }
    cleanupStaleTempFiles(directory);

    static std::atomic_uint64_t sequence{0};
    const auto unique = std::wstring(tempPrefix) +
        std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64()) + L"-" +
        std::to_wstring(sequence.fetch_add(1));
    const auto concatPath = directory / (unique + L".concat.txt");
    const auto progressPath = directory / (unique + L".progress.txt");
    const auto errorPath = directory / (unique + L".stderr.log");
    const auto partialPath = directory / (unique + L".partial.mp4");
    const auto cleanup = [&] {
        removeNoThrow(concatPath);
        removeNoThrow(progressPath);
        removeNoThrow(errorPath);
        removeNoThrow(partialPath);
    };

    {
        std::ofstream concat(concatPath, std::ios::binary | std::ios::trunc);
        if (!concat) {
            finishSave(L"Unable to create the ffmpeg concat list.");
            cleanup();
            return;
        }
        concat << "ffconcat version 1.0\n";
        for (const auto& segment : snapshotValue.segments()) {
            concat << ffconcatPath(segment.path);
            concat << "duration " << secondsText(segment.duration) << '\n';
        }
        if (!concat) {
            finishSave(L"Unable to write the ffmpeg concat list.");
            cleanup();
            return;
        }
    }

    const auto configuredDuration = std::chrono::duration_cast<std::chrono::microseconds>(
        limits().duration);
    const auto outputDuration =
        configuredDuration.count() > 0
            ? std::min(snapshotValue.duration(), configuredDuration)
            : snapshotValue.duration();
    const auto trimStart = snapshotValue.duration() - outputDuration;

    std::vector<std::wstring> arguments{
        ffmpegExecutable.wstring(), L"-hide_banner", L"-y", L"-f", L"concat",
        L"-safe", L"0"};
    if (trimStart.count() > 0) {
        arguments.emplace_back(L"-ss");
        arguments.emplace_back(utf8ToWide(secondsText(trimStart)));
    }
    arguments.insert(arguments.end(), {L"-i", concatPath.wstring()});
    if (outputDuration.count() > 0) {
        arguments.emplace_back(L"-t");
        arguments.emplace_back(utf8ToWide(secondsText(outputDuration)));
    }
    arguments.insert(arguments.end(), {
        L"-map", L"0", L"-c", L"copy", L"-avoid_negative_ts", L"make_zero",
        L"-movflags", L"+faststart", L"-progress", progressPath.wstring(),
        L"-nostats", partialPath.wstring()});
    auto command = makeCommandLine(arguments);

    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE errorHandle = CreateFileW(
        errorPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
        &security, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    HANDLE nullInput = CreateFileW(
        L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (errorHandle == INVALID_HANDLE_VALUE || nullInput == INVALID_HANDLE_VALUE) {
        if (errorHandle != INVALID_HANDLE_VALUE) CloseHandle(errorHandle);
        if (nullInput != INVALID_HANDLE_VALUE) CloseHandle(nullInput);
        finishSave(L"Unable to prepare ffmpeg process handles.");
        cleanup();
        return;
    }

    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = nullInput;
    startup.hStdOutput = errorHandle;
    startup.hStdError = errorHandle;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(
        ffmpegExecutable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, ffmpegExecutable.parent_path().c_str(),
        &startup, &process);
    const DWORD createError = started ? ERROR_SUCCESS : GetLastError();
    CloseHandle(nullInput);
    CloseHandle(errorHandle);
    if (!started) {
        finishSave(L"Unable to start ffmpeg (Windows error " +
                   std::to_wstring(createError) + L").");
        cleanup();
        return;
    }
    CloseHandle(process.hThread);

    bool cancelled = false;
    for (;;) {
        const DWORD wait = WaitForSingleObject(process.hProcess, 100);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_FAILED) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, INFINITE);
            break;
        }
        progress_ = readProgress(progressPath, outputDuration);
        if (stop.stop_requested()) {
            cancelled = true;
            TerminateProcess(process.hProcess, ERROR_CANCELLED);
            WaitForSingleObject(process.hProcess, INFINITE);
            break;
        }
    }

    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    if (cancelled) {
        finishSave(L"Replay save was cancelled.");
        cleanup();
        return;
    }
    if (exitCode != 0) {
        auto detail = readErrorLog(errorPath);
        std::wstring message =
            L"ffmpeg failed with exit code " + std::to_wstring(exitCode) + L".";
        if (!detail.empty()) message += L"\n" + detail;
        finishSave(std::move(message));
        cleanup();
        return;
    }

    if (!MoveFileExW(
            partialPath.c_str(), finalMp4.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        finishSave(L"Unable to publish the final replay (Windows error " +
                   std::to_wstring(GetLastError()) + L").");
        cleanup();
        return;
    }
    removeNoThrow(concatPath);
    removeNoThrow(progressPath);
    removeNoThrow(errorPath);
    finishSave({}, std::move(finalMp4));
}

size_t ReplayRecorder::cleanupStaleTempFiles(
    const std::filesystem::path& directory,
    std::chrono::hours maxAge) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error) return 0;
    const auto now = std::filesystem::file_time_type::clock::now();
    size_t removed = 0;
    std::filesystem::directory_iterator iterator(directory, error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end) {
        const auto& entry = *iterator;
        const auto name = entry.path().filename().wstring();
        if (name.starts_with(tempPrefix) && entry.is_regular_file(error) && !error) {
            const auto modified = entry.last_write_time(error);
            if (!error && now - modified >= maxAge) {
                if (std::filesystem::remove(entry.path(), error) && !error) ++removed;
            }
        }
        error.clear();
        iterator.increment(error);
    }
    return removed;
}

} // namespace kanekist
