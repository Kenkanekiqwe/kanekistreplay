#include "replay_buffer.h"

#include <algorithm>

namespace kanekist {

ReplayBuffer::ReplayBuffer(int64_t duration100ns, size_t memoryLimitBytes)
    : duration100ns_(std::max<int64_t>(duration100ns, 10'000'000)),
      memoryLimitBytes_(std::max<size_t>(memoryLimitBytes, 1024 * 1024)) {}

void ReplayBuffer::push(EncodedSample sample) {
    std::scoped_lock lock(mutex_);
    bytes_ += sample.bytes.size();
    samples_.push_back(std::move(sample));
    trim();
}

void ReplayBuffer::trim() {
    if (samples_.empty()) return;
    const auto newest = samples_.back().timestamp100ns;
    while (samples_.size() > 1 &&
           (bytes_ > memoryLimitBytes_ ||
            newest - samples_.front().timestamp100ns > duration100ns_)) {
        bytes_ -= samples_.front().bytes.size();
        samples_.pop_front();
    }

    // A replay must begin at a decodable video key frame. Keep preceding audio
    // only when a video key frame is available in the retained interval.
    const auto firstKey = std::find_if(samples_.begin(), samples_.end(), [](const auto& sample) {
        return sample.kind == SampleKind::video && sample.keyFrame;
    });
    if (firstKey != samples_.end()) {
        const auto start = firstKey->timestamp100ns;
        while (!samples_.empty() && samples_.front().timestamp100ns < start) {
            bytes_ -= samples_.front().bytes.size();
            samples_.pop_front();
        }
    }
}

std::vector<EncodedSample> ReplayBuffer::snapshot() const {
    std::scoped_lock lock(mutex_);
    std::vector<EncodedSample> result(samples_.begin(), samples_.end());
    normalizeTimestamps(result);
    return result;
}

size_t ReplayBuffer::bytes() const {
    std::scoped_lock lock(mutex_);
    return bytes_;
}

size_t ReplayBuffer::sampleCount() const {
    std::scoped_lock lock(mutex_);
    return samples_.size();
}

void ReplayBuffer::clear() {
    std::scoped_lock lock(mutex_);
    samples_.clear();
    bytes_ = 0;
}

void ReplayBuffer::normalizeTimestamps(std::vector<EncodedSample>& samples) {
    if (samples.empty()) return;
    const auto origin = std::min_element(samples.begin(), samples.end(), [](const auto& a, const auto& b) {
        return a.timestamp100ns < b.timestamp100ns;
    })->timestamp100ns;
    for (auto& sample : samples) sample.timestamp100ns = std::max<int64_t>(0, sample.timestamp100ns - origin);
}

} // namespace kanekist
