#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace kanekist {

struct ReplaySegment {
    std::filesystem::path path;
    std::chrono::microseconds duration{};
    uint64_t bytes{};
};

class ReplayRecorder {
public:
    struct Limits {
        std::chrono::seconds duration{60};
        uint64_t maxBytes{2ull * 1024 * 1024 * 1024};
    };

    class Snapshot {
    public:
        Snapshot() = default;

        [[nodiscard]] const std::vector<ReplaySegment>& segments() const noexcept {
            return segments_;
        }
        [[nodiscard]] std::chrono::microseconds duration() const noexcept {
            return duration_;
        }
        [[nodiscard]] uint64_t bytes() const noexcept { return bytes_; }
        [[nodiscard]] bool empty() const noexcept { return segments_.empty(); }

    private:
        friend class ReplayRecorder;
        std::vector<ReplaySegment> segments_;
        std::chrono::microseconds duration_{};
        uint64_t bytes_{};
        std::shared_ptr<const void> pins_;
    };

    explicit ReplayRecorder(Limits limits = {});
    ~ReplayRecorder();
    ReplayRecorder(const ReplayRecorder&) = delete;
    ReplayRecorder& operator=(const ReplayRecorder&) = delete;

    void setLimits(Limits limits);
    [[nodiscard]] Limits limits() const;

    // The path must name a completed segment. Ownership of that file transfers
    // to the recorder; it is deleted when evicted and no snapshot pins it.
    [[nodiscard]] bool addCompletedSegment(
        std::filesystem::path path,
        std::chrono::microseconds duration,
        uint64_t bytes);

    [[nodiscard]] Snapshot snapshot() const;
    void clear();

    [[nodiscard]] size_t segmentCount() const;
    [[nodiscard]] std::chrono::microseconds bufferedDuration() const;
    [[nodiscard]] uint64_t bufferedBytes() const;

    // Starts one background concat/remux operation. Returns false if another
    // save is active or the arguments/snapshot are invalid.
    [[nodiscard]] bool saveAsync(
        Snapshot snapshot,
        std::filesystem::path ffmpegExecutable,
        std::filesystem::path finalMp4);
    [[nodiscard]] bool saveAsync(
        std::filesystem::path ffmpegExecutable,
        std::filesystem::path finalMp4);
    void cancelSave();

    [[nodiscard]] bool saving() const noexcept { return saving_.load(); }
    [[nodiscard]] double progress() const noexcept { return progress_.load(); }
    [[nodiscard]] std::wstring lastError() const;
    [[nodiscard]] std::filesystem::path lastSavedPath() const;

    // Removes abandoned files created by this class. Active files newer than
    // maxAge are left untouched.
    static size_t cleanupStaleTempFiles(
        const std::filesystem::path& directory,
        std::chrono::hours maxAge = std::chrono::hours(24));

private:
    struct Record;

    void trimLocked();
    void saveWorker(
        std::stop_token stop,
        Snapshot snapshot,
        std::filesystem::path ffmpegExecutable,
        std::filesystem::path finalMp4);
    void finishSave(std::wstring error, std::filesystem::path savedPath = {});

    mutable std::mutex mutex_;
    std::deque<std::shared_ptr<Record>> segments_;
    Limits limits_;
    std::chrono::microseconds bufferedDuration_{};
    uint64_t bufferedBytes_{};

    mutable std::mutex stateMutex_;
    std::mutex launchMutex_;
    std::jthread saveThread_;
    std::atomic_bool saving_{false};
    std::atomic<double> progress_{0.0};
    std::wstring lastError_;
    std::filesystem::path lastSavedPath_;
};

} // namespace kanekist
