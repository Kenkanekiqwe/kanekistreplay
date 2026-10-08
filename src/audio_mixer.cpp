#include "audio_mixer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace kanekist {
namespace {

enum class SampleEncoding {
    pcm16,
    pcm24,
    pcm32,
    float32,
};

[[nodiscard]] bool isWaveSubtype(const GUID& subtype, WORD formatTag) noexcept {
    static constexpr BYTE waveTail[8]{
        0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71,
    };
    return subtype.Data1 == formatTag && subtype.Data2 == 0x0000 &&
           subtype.Data3 == 0x0010 &&
           std::memcmp(subtype.Data4, waveTail, sizeof(waveTail)) == 0;
}

[[nodiscard]] bool encodingFor(const WAVEFORMATEX& format,
                               SampleEncoding& encoding) noexcept {
    WORD formatTag = format.wFormatTag;
    if (formatTag == WAVE_FORMAT_EXTENSIBLE) {
        if (format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            return false;
        }
        const auto& extensible =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        if (isWaveSubtype(extensible.SubFormat, WAVE_FORMAT_PCM)) {
            formatTag = WAVE_FORMAT_PCM;
        } else if (isWaveSubtype(extensible.SubFormat, WAVE_FORMAT_IEEE_FLOAT)) {
            formatTag = WAVE_FORMAT_IEEE_FLOAT;
        } else {
            return false;
        }
    }

    if (formatTag == WAVE_FORMAT_PCM && format.wBitsPerSample == 16) {
        encoding = SampleEncoding::pcm16;
        return true;
    }
    if (formatTag == WAVE_FORMAT_PCM && format.wBitsPerSample == 24) {
        encoding = SampleEncoding::pcm24;
        return true;
    }
    if (formatTag == WAVE_FORMAT_PCM && format.wBitsPerSample == 32) {
        encoding = SampleEncoding::pcm32;
        return true;
    }
    if (formatTag == WAVE_FORMAT_IEEE_FLOAT && format.wBitsPerSample == 32) {
        encoding = SampleEncoding::float32;
        return true;
    }
    return false;
}

[[nodiscard]] float readSample(const std::byte* bytes,
                               SampleEncoding encoding) noexcept {
    if (encoding == SampleEncoding::pcm16) {
        std::int16_t value{};
        std::memcpy(&value, bytes, sizeof(value));
        return static_cast<float>(value) / 32'768.0F;
    }
    if (encoding == SampleEncoding::pcm24) {
        const auto* src = reinterpret_cast<const unsigned char*>(bytes);
        const std::int32_t value = static_cast<std::int32_t>(src[0]) |
            (static_cast<std::int32_t>(src[1]) << 8) |
            (static_cast<std::int32_t>(static_cast<std::int8_t>(src[2])) << 16);
        return static_cast<float>(value) / 8'388'608.0F;
    }
    if (encoding == SampleEncoding::pcm32) {
        std::int32_t value{};
        std::memcpy(&value, bytes, sizeof(value));
        return static_cast<float>(value) / 2'147'483'648.0F;
    }

    float value{};
    std::memcpy(&value, bytes, sizeof(value));
    return std::isfinite(value) ? value : 0.0F;
}

} // namespace

AudioMixer::SubmitResult AudioMixer::submit(Source source,
                                            std::span<const std::byte> pcm,
                                            const WAVEFORMATEX& format) {
    if (pcm.empty()) return SubmitResult::empty;

    SampleEncoding encoding{};
    if (!encodingFor(format, encoding) || format.nSamplesPerSec == 0 ||
        format.nChannels == 0) {
        return SubmitResult::unsupportedFormat;
    }

    const std::size_t bytesPerSample =
        encoding == SampleEncoding::pcm16 ? sizeof(std::int16_t) :
        encoding == SampleEncoding::pcm24 ? 3 : sizeof(float);
    const std::size_t minimumBlockAlign =
        static_cast<std::size_t>(format.nChannels) * bytesPerSample;
    if (format.nBlockAlign < minimumBlockAlign ||
        pcm.size() % format.nBlockAlign != 0) {
        return SubmitResult::malformedBuffer;
    }

    const std::size_t frameCount = pcm.size() / format.nBlockAlign;
    if (frameCount >
        (std::numeric_limits<std::size_t>::max)() / outputChannels) {
        return SubmitResult::malformedBuffer;
    }

    std::vector<float> stereo;
    stereo.resize(frameCount * outputChannels);
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        const auto* input = pcm.data() + frame * format.nBlockAlign;
        const float left = readSample(input, encoding);
        const float right = format.nChannels == 2
                                ? readSample(input + bytesPerSample, encoding)
                                : left;
        stereo[frame * outputChannels] = left;
        stereo[frame * outputChannels + 1] = right;
    }

    std::scoped_lock lock(mutex_);
    appendResampled(state(source), stereo, format.nSamplesPerSec);
    return SubmitResult::ok;
}

void AudioMixer::setGain(Source source, float gainValue) noexcept {
    std::scoped_lock lock(mutex_);
    state(source).gain = std::isfinite(gainValue) ? gainValue : 0.0F;
}

float AudioMixer::gain(Source source) const noexcept {
    std::scoped_lock lock(mutex_);
    return state(source).gain;
}

void AudioMixer::setNoiseGate(float threshold) noexcept {
    std::scoped_lock lock(mutex_);
    if (!std::isfinite(threshold) || threshold <= 0.0F) {
        noiseGate_ = 0.0F;
        return;
    }
    noiseGate_ = std::clamp(threshold, 0.0F, 0.5F);
}

float AudioMixer::noiseGate() const noexcept {
    std::scoped_lock lock(mutex_);
    return noiseGate_;
}

void AudioMixer::setLimiter(Limiter limiterValue) noexcept {
    std::scoped_lock lock(mutex_);
    limiter_ = limiterValue;
}

AudioMixer::Limiter AudioMixer::limiter() const noexcept {
    std::scoped_lock lock(mutex_);
    return limiter_;
}

std::vector<float> AudioMixer::mix(std::size_t frameCount) {
    std::scoped_lock lock(mutex_);
    return mixLocked(frameCount);
}

std::vector<float> AudioMixer::mixAvailable(std::size_t maxFrameCount) {
    std::scoped_lock lock(mutex_);
    const std::size_t queuedFrames =
        (std::max)(system_.output.size(), microphone_.output.size()) /
        outputChannels;
    return mixLocked((std::min)(queuedFrames, maxFrameCount));
}

std::size_t AudioMixer::availableFrames(Source source) const noexcept {
    std::scoped_lock lock(mutex_);
    return state(source).output.size() / outputChannels;
}

void AudioMixer::clear(Source source) noexcept {
    std::scoped_lock lock(mutex_);
    auto& sourceState = state(source);
    sourceState.output.clear();
    sourceState.resampleInput.clear();
    sourceState.resamplePosition = 0.0;
    sourceState.inputSampleRate = 0;
    sourceState.envelope = 0.0F;
}

void AudioMixer::clear() noexcept {
    std::scoped_lock lock(mutex_);
    system_.output.clear();
    system_.resampleInput.clear();
    system_.resamplePosition = 0.0;
    system_.inputSampleRate = 0;
    microphone_.output.clear();
    microphone_.resampleInput.clear();
    microphone_.resamplePosition = 0.0;
    microphone_.inputSampleRate = 0;
    system_.envelope = 0.0F;
    microphone_.envelope = 0.0F;
}

WAVEFORMATEX AudioMixer::outputFormat() noexcept {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = outputChannels;
    format.nSamplesPerSec = outputSampleRate;
    format.wBitsPerSample = 32;
    format.nBlockAlign =
        static_cast<WORD>(outputChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    return format;
}

AudioMixer::SourceState& AudioMixer::state(Source source) noexcept {
    return source == Source::system ? system_ : microphone_;
}

const AudioMixer::SourceState& AudioMixer::state(Source source) const noexcept {
    return source == Source::system ? system_ : microphone_;
}

void AudioMixer::appendResampled(SourceState& destination,
                                 std::span<const float> stereo,
                                 std::uint32_t sampleRate) {
    if (destination.inputSampleRate != 0 &&
        destination.inputSampleRate != sampleRate) {
        destination.resampleInput.clear();
        destination.resamplePosition = 0.0;
    }
    destination.inputSampleRate = sampleRate;

    if (sampleRate == outputSampleRate) {
        destination.resampleInput.clear();
        destination.resamplePosition = 0.0;
        destination.output.insert(destination.output.end(), stereo.begin(), stereo.end());
        return;
    }

    destination.resampleInput.insert(destination.resampleInput.end(),
                                     stereo.begin(), stereo.end());
    const double step =
        static_cast<double>(sampleRate) / static_cast<double>(outputSampleRate);
    const std::size_t inputFrames =
        destination.resampleInput.size() / outputChannels;

    while (destination.resamplePosition + 1.0 <
           static_cast<double>(inputFrames)) {
        const auto firstFrame =
            static_cast<std::size_t>(destination.resamplePosition);
        const std::size_t secondFrame = firstFrame + 1;
        const float fraction = static_cast<float>(
            destination.resamplePosition - static_cast<double>(firstFrame));
        for (std::size_t channel = 0; channel < outputChannels; ++channel) {
            const float first =
                destination.resampleInput[firstFrame * outputChannels + channel];
            const float second =
                destination.resampleInput[secondFrame * outputChannels + channel];
            destination.output.push_back(
                first + (second - first) * fraction);
        }
        destination.resamplePosition += step;
    }

    const std::size_t consumedFrames = (std::min)(
        static_cast<std::size_t>(destination.resamplePosition), inputFrames);
    if (consumedFrames != 0) {
        destination.resampleInput.erase(
            destination.resampleInput.begin(),
            destination.resampleInput.begin() +
                static_cast<std::ptrdiff_t>(consumedFrames * outputChannels));
        destination.resamplePosition -= static_cast<double>(consumedFrames);
    }
}

std::vector<float> AudioMixer::mixLocked(std::size_t frameCount) {
    if (frameCount > std::vector<float>{}.max_size() / outputChannels) {
        throw std::length_error("AudioMixer output block is too large");
    }

    std::vector<float> mixed(frameCount * outputChannels, 0.0F);
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        for (std::size_t channel = 0; channel < outputChannels; ++channel) {
            float sample = 0.0F;
            if (!system_.output.empty()) {
                sample += system_.output.front() * system_.gain;
                system_.output.pop_front();
            }
            if (!microphone_.output.empty()) {
                const float raw = microphone_.output.front();
                microphone_.output.pop_front();
                const float magnitude = std::abs(raw);
                float& envelope = microphone_.envelope;
                const float coeff = magnitude > envelope ? 0.40F : 0.035F;
                envelope += (magnitude - envelope) * coeff;
                float gate = 1.0F;
                if (noiseGate_ > 0.0F) {
                    const float open = noiseGate_ * 1.4F;
                    if (envelope <= noiseGate_) gate = 0.0F;
                    else if (envelope < open) gate = (envelope - noiseGate_) / (open - noiseGate_);
                }
                sample += raw * microphone_.gain * gate;
            }
            mixed[frame * outputChannels + channel] =
                applyLimiter(sample, limiter_);
        }
    }
    return mixed;
}

float AudioMixer::applyLimiter(float sample, Limiter limiterValue) noexcept {
    if (std::isnan(sample)) return 0.0F;
    if (limiterValue == Limiter::none) return sample;
    if (std::isinf(sample)) return std::copysign(1.0F, sample);
    if (limiterValue == Limiter::hard) {
        return std::clamp(sample, -1.0F, 1.0F);
    }

    constexpr float threshold = 0.8F;
    const float magnitude = std::abs(sample);
    if (magnitude <= threshold) return sample;
    const float limited =
        threshold + (1.0F - threshold) *
                        std::tanh((magnitude - threshold) /
                                  (1.0F - threshold));
    return std::copysign(limited, sample);
}

} // namespace kanekist
