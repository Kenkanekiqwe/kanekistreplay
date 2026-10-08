#pragma once

#include <Windows.h>
#include <mmreg.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <vector>

namespace kanekist {

class AudioMixer {
public:
    static constexpr std::uint32_t outputSampleRate = 48'000;
    static constexpr std::uint16_t outputChannels = 2;

    enum class Source {
        system,
        microphone,
    };

    enum class Limiter {
        none,
        hard,
        soft,
    };

    enum class SubmitResult {
        ok,
        empty,
        unsupportedFormat,
        malformedBuffer,
    };

    AudioMixer() = default;

    // The format object must include the WAVEFORMATEXTENSIBLE tail when its
    // wFormatTag is WAVE_FORMAT_EXTENSIBLE.
    [[nodiscard]] SubmitResult submit(Source source,
                                      std::span<const std::byte> pcm,
                                      const WAVEFORMATEX& format);

    void setGain(Source source, float gain) noexcept;
    [[nodiscard]] float gain(Source source) const noexcept;

    void setNoiseGate(float threshold) noexcept;
    [[nodiscard]] float noiseGate() const noexcept;

    void setLimiter(Limiter limiter) noexcept;
    [[nodiscard]] Limiter limiter() const noexcept;

    // Returns exactly frameCount interleaved stereo frames. A source whose
    // queue is short contributes silence.
    [[nodiscard]] std::vector<float> mix(std::size_t frameCount);

    // Mixes at most maxFrameCount frames currently queued by either source.
    [[nodiscard]] std::vector<float> mixAvailable(
        std::size_t maxFrameCount = static_cast<std::size_t>(-1));

    [[nodiscard]] std::size_t availableFrames(Source source) const noexcept;
    void clear(Source source) noexcept;
    void clear() noexcept;

    [[nodiscard]] static WAVEFORMATEX outputFormat() noexcept;

private:
    struct SourceState {
        float gain{1.0F};
        float envelope{};
        std::deque<float> output;
        std::vector<float> resampleInput;
        double resamplePosition{};
        std::uint32_t inputSampleRate{};
    };

    [[nodiscard]] SourceState& state(Source source) noexcept;
    [[nodiscard]] const SourceState& state(Source source) const noexcept;
    void appendResampled(SourceState& destination,
                         std::span<const float> stereo,
                         std::uint32_t sampleRate);
    [[nodiscard]] std::vector<float> mixLocked(std::size_t frameCount);
    [[nodiscard]] static float applyLimiter(float sample, Limiter limiter) noexcept;

    mutable std::mutex mutex_;
    SourceState system_;
    SourceState microphone_;
    Limiter limiter_{Limiter::hard};
    float noiseGate_{};
};

} // namespace kanekist
