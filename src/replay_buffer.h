#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <vector>

namespace kanekist {

enum class SampleKind { video, audio };

struct EncodedSample {
    SampleKind kind{};
    int64_t timestamp100ns{};
    int64_t duration100ns{};
    bool keyFrame{};
    std::vector<uint8_t> bytes;
};

class ReplayBuffer {
public:
    explicit ReplayBuffer(int64_t duration100ns, size_t memoryLimitBytes = 512ull * 1024 * 1024);
    void push(EncodedSample sample);
    [[nodiscard]] std::vector<EncodedSample> snapshot() const;
    [[nodiscard]] size_t bytes() const;
    [[nodiscard]] size_t sampleCount() const;
    void clear();

    static void normalizeTimestamps(std::vector<EncodedSample>& samples);

private:
    void trim();
    mutable std::mutex mutex_;
    std::deque<EncodedSample> samples_;
    int64_t duration100ns_;
    size_t memoryLimitBytes_;
    size_t bytes_{};
};

} // namespace kanekist
