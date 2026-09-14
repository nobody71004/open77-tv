#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace op77::WebHost
{
/// The browser's decoded audio, played through the machine's default render
/// device.
///
/// Windowless CEF has no audio device of its own: it decodes a stream and hands
/// the PCM to the client, and if the client drops it the page plays silently.
/// The probe run attached to this feature measured exactly that path -- 256
/// packets / 262,144 frames of stereo 44.1 kHz float audio for a six-second
/// clip, with the peak amplitude matching the page's 0.5 volume -- so the
/// remaining work is only to consume it.
///
/// One sink per surface. Chromium's audio engine mixes clients, so N TVs are N
/// clients on one device rather than a mixer here, and each surface keeps its
/// own gain and mute instead of sharing a global volume.
///
/// The device's shared-mode format is fixed by the audio engine: a client may
/// not choose its own sample rate. So the format is probed FIRST and handed
/// back to CEF in `CefAudioHandler::GetAudioParameters`, which CEF documents as
/// configurable. That keeps this sink a pure format converter and takes
/// resampling off the audio thread -- 44.1 kHz PCM on a 48 kHz device is the
/// normal case, not the exception.
class AudioSink
{
public:
    struct Format
    {
        uint32_t sampleRate{};
        uint16_t channels{};
    };

    struct Stats
    {
        uint64_t pushedFrames{};
        uint64_t playedFrames{};
        /// Frames the render thread had to fill with silence because the ring
        /// ran dry. A device that keeps up reports zero; anything else means
        /// the page is not producing audio at the rate it claims.
        uint64_t underrunFrames{};
        /// Frames discarded because the ring was full: the page produced audio
        /// faster than real time (a seek, a burst after a stall).
        uint64_t droppedFrames{};
        uint32_t streams{};
        uint32_t streamErrors{};
    };

    AudioSink();
    ~AudioSink();

    AudioSink(const AudioSink&) = delete;
    AudioSink& operator=(const AudioSink&) = delete;

    /// The default render device's shared-mode format. No device open, so this
    /// is safe to call before a browser exists -- which is the order that
    /// matters, because the format has to be known before CEF starts a stream.
    [[nodiscard]] static bool ProbeDeviceFormat(Format& aFormat, std::string& aError);

    [[nodiscard]] bool Open(std::string& aError);
    void Close();

    /// Planar float frames, exactly as CEF delivers them. `aChannels` may
    /// differ from the device's count: extra device channels are filled with
    /// silence, extra source channels are dropped.
    void Push(const float* const* aData, int aFrames, int aChannels);

    void SetGain(float aGain);
    void SetMuted(bool aMuted);
    [[nodiscard]] bool Opened() const { return m_opened.load(); }
    [[nodiscard]] Format DeviceFormat() const { return m_format; }
    [[nodiscard]] Stats Snapshot() const;
    /// One line for the host trace: what the device is, and what has been
    /// played through it. Silent audio and a silent device look identical from
    /// the page, so this is the line that tells them apart.
    [[nodiscard]] std::string Describe() const;

    /// Plays a generated tone through this sink and reports what the device
    /// accepted, so the audio path can be exercised on a machine with no game
    /// running. Returns a process exit code.
    static int SelfTest(double aSeconds);

private:
    void RenderLoop();
    void FillSilence(float* aDestination, int aFrames, int aChannels);
    [[nodiscard]] uint64_t ReadRing(float* aDestination, uint64_t aFrames);

    struct Impl;

    std::unique_ptr<Impl> m_impl;
    Format m_format{};
    std::atomic<bool> m_opened{false};
    std::atomic<bool> m_stopping{false};
    std::atomic<float> m_gain{1.0F};
    std::atomic<bool> m_muted{false};
    // Counting starts at the first Push, not at Open: the device is primed
    // with a full buffer of silence so it never plays uninitialised memory, and
    // the drain after the last Push is silence too. Counting those as "played"
    // would make every healthy stream look like it had 0.2 s of underrun.
    std::atomic<bool> m_streaming{false};
    std::atomic<uint64_t> m_pushedFrames{};
    std::atomic<uint64_t> m_playedFrames{};
    std::atomic<uint64_t> m_underrunFrames{};
    std::atomic<uint64_t> m_droppedFrames{};
    std::atomic<uint32_t> m_streams{};
    std::atomic<uint32_t> m_streamErrors{};
    std::thread m_thread;
};
} // namespace op77::WebHost
