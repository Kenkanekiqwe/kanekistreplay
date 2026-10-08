#include "audio_mixer.h"
#include "core.h"
#include "overlay_menu.h"
#include "replay_buffer.h"
#include "replay_recorder.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <span>
#include <string>

using namespace kanekist;

void testTimestampConversion() {
    assert(QpcClock::ticksTo100ns(1'000, 1'000) == 10'000'000);
    assert(QpcClock::ticksTo100ns(0, 1'000) == 0);
    assert(QpcClock::ticksTo100ns(5, 0) == 0);
}

void testReplayDurationAndKeyFrame() {
    ReplayBuffer buffer(20'000'000, 16 * 1024 * 1024);
    for (int i = 0; i < 8; ++i) {
        EncodedSample sample;
        sample.kind = SampleKind::video;
        sample.timestamp100ns = i * 5'000'000;
        sample.duration100ns = 5'000'000;
        sample.keyFrame = i % 2 == 0;
        sample.bytes.resize(1024, static_cast<uint8_t>(i));
        buffer.push(std::move(sample));
    }
    const auto snapshot = buffer.snapshot();
    assert(!snapshot.empty());
    assert(snapshot.front().keyFrame);
    assert(snapshot.front().timestamp100ns == 0);
    assert(snapshot.back().timestamp100ns <= 20'000'000);
}

void testReplayMemoryLimit() {
    ReplayBuffer buffer(600'000'000, 1024 * 1024);
    for (int i = 0; i < 5; ++i) {
        EncodedSample sample;
        sample.kind = SampleKind::video;
        sample.timestamp100ns = i * 1'000'000;
        sample.keyFrame = true;
        sample.bytes.resize(400 * 1024);
        buffer.push(std::move(sample));
    }
    assert(buffer.bytes() <= 1024 * 1024);
}

void testTimestampNormalization() {
    std::vector<EncodedSample> samples(2);
    samples[0].timestamp100ns = 55;
    samples[1].timestamp100ns = 85;
    ReplayBuffer::normalizeTimestamps(samples);
    assert(samples[0].timestamp100ns == 0);
    assert(samples[1].timestamp100ns == 30);
}

void testOverlayHitTesting() {
    assert(OverlayMenu::hitTestCard(50, 130, 1.0f) == 0);
    assert(OverlayMenu::hitTestCard(50, 188, 1.0f) == 1);
    assert(OverlayMenu::hitTestCard(50, 420, 1.0f) == 5);
    assert(OverlayMenu::hitTestCard(50, 478, 1.0f) == 6);
    assert(OverlayMenu::hitTestCard(430, 420, 1.0f) == 12);
    assert(OverlayMenu::hitTestCard(430, 478, 1.0f) == 13);
    assert(OverlayMenu::hitTestCard(430, 536, 1.0f) == 14);
    assert(OverlayMenu::hitTestCard(430, 594, 1.0f) == 15);
    assert(OverlayMenu::hitTestCard(430, 140, 1.0f) == 7);
    assert(OverlayMenu::hitTestCard(430, 198, 1.0f) == 8);
    assert(OverlayMenu::hitTestCard(10, 20, 1.0f) == -1);
    assert(OverlayMenu::hitTestCard(100, 260, 2.0f) == 0);
}

void testAudioMixer() {
    AudioMixer mixer;
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = 24'000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = 48'000;
    const std::array<int16_t, 4> samples{16'000, -16'000, 8'000, -8'000};
    const auto bytes = std::as_bytes(std::span(samples));
    assert(mixer.submit(AudioMixer::Source::system, bytes, format) == AudioMixer::SubmitResult::ok);
    auto output = mixer.mixAvailable();
    assert(!output.empty());
    assert(output.size() % 2 == 0);
    assert(output[0] == output[1]);
    for (float sample : output) {
        assert(sample >= -1.0f && sample <= 1.0f);
        (void)sample;
    }
}

void testMicrophoneNoiseGate() {
    AudioMixer mixer;
    mixer.setNoiseGate(0.2f);
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = 48'000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = 96'000;
    const std::array<int16_t, 8> quiet{200, -180, 160, -140, 120, -100, 80, -60};
    assert(mixer.submit(AudioMixer::Source::microphone, std::as_bytes(std::span(quiet)), format) ==
           AudioMixer::SubmitResult::ok);
    const auto gated = mixer.mixAvailable();
    assert(!gated.empty());
    for (float sample : gated) {
        assert(std::abs(sample) < 0.01f);
        (void)sample;
    }
}

void testAudioMixerTwoSourcesStayRealtime() {
    AudioMixer mixer;
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = 48'000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = 96'000;
    const std::array<int16_t, 8> samples{1'000, -1'000, 2'000, -2'000, 3'000, -3'000, 4'000, -4'000};
    const auto bytes = std::as_bytes(std::span(samples));
    assert(mixer.submit(AudioMixer::Source::system, bytes, format) == AudioMixer::SubmitResult::ok);
    assert(mixer.submit(AudioMixer::Source::microphone, bytes, format) == AudioMixer::SubmitResult::ok);
    const auto frames = (std::min)(mixer.availableFrames(AudioMixer::Source::system),
                                   mixer.availableFrames(AudioMixer::Source::microphone));
    const auto mixed = mixer.mix(frames);
    assert(mixed.size() == frames * 2);
    assert(frames == samples.size());
}

void testReplayMinutesNormalization() {
    assert(normalizeReplaySeconds(15) == 60);
    assert(normalizeReplaySeconds(60) == 60);
    assert(normalizeReplaySeconds(1200) == 1200);
    assert(normalizeReplaySeconds(1800) == 1800);
    assert(normalizeReplaySeconds(4000) == 1800);
    assert(Hotkey::fromPacked(packHotkey(MOD_ALT, 'K')).valid());
}

void testVideoCodecBitrate() {
    const auto h264 = suggestedBitrateMbps(1920, 1080, 60, 0);
    const auto hevc = suggestedBitrateMbps(1920, 1080, 60, 1);
    const auto av1 = suggestedBitrateMbps(1920, 1080, 60, 2);
    assert(hevc < h264);
    assert(av1 < hevc);
    (void)h264; (void)hevc; (void)av1;
    Settings settings;
    settings.videoCodec = 9;
    settings.normalizeEncodePreset();
    assert(settings.videoCodec == 2);
    assert(std::wstring(videoCodecLabel(1)) == L"HEVC");
}

void testReplaySegmentRing() {
    const auto directory = std::filesystem::temp_directory_path() / L"kanekist-replay-test";
    std::filesystem::create_directories(directory);
    ReplayRecorder recorder({std::chrono::seconds(2), 1024});
    for (int i = 0; i < 3; ++i) {
        const auto path = directory / (std::to_wstring(i) + L".mp4");
        std::ofstream(path).write("test", 4);
        assert(recorder.addCompletedSegment(path, std::chrono::seconds(1), 4));
    }
    assert(recorder.segmentCount() == 2);
    assert(recorder.bufferedDuration() == std::chrono::seconds(2));
    recorder.clear();
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

int main() {
    testTimestampConversion();
    testReplayDurationAndKeyFrame();
    testReplayMemoryLimit();
    testTimestampNormalization();
    testOverlayHitTesting();
    testAudioMixer();
    testMicrophoneNoiseGate();
    testAudioMixerTwoSourcesStayRealtime();
    testReplayMinutesNormalization();
    testVideoCodecBitrate();
    testReplaySegmentRing();
    std::cout << "All Kanekist tests passed\n";
    return 0;
}
