#include "AudioSink.hpp"

#include <Windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace op77::WebHost
{
namespace
{
// Sub-format GUIDs, declared here rather than pulled from ksmedia.h: that
// header's definitions need ksuser.lib on the link line for two constants whose
// values are fixed by the Windows ABI anyway.
constexpr GUID kSubFormatIeeeFloat{0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr GUID kSubFormatPcm{0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

// PKEY_Device_FriendlyName, spelled out for the same reason as the sub-formats
// above: the header that declares it needs initguid/uuid handling that would
// leak into every translation unit that includes this file.
constexpr PROPERTYKEY kDeviceFriendlyName{
    {0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};

// Exit codes of the audio self-test, kept clear of the values Main.cpp already
// returns for its other modes.
constexpr int kExitAudioSinkOpenFailed = 20;
constexpr int kExitAudioUnhealthy = 21;

// About 0.7 s of stereo audio at 48 kHz. Large enough that a page compositing
// its frames late does not starve the device, small enough that a paused TV
// does not keep playing for a second after the user stops it.
constexpr uint64_t kRingFrames = 1U << 15U;

// How long the render thread waits for the device before re-checking whether it
// is shutting down. WASAPI's event fires far more often than this; the timeout
// only bounds how long Close() can block behind a dead device.
constexpr DWORD kRenderWaitMilliseconds = 200;

class ComScope
{
public:
    ComScope()
    {
        m_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_usable = SUCCEEDED(m_result) || m_result == RPC_E_CHANGED_MODE;
    }
    ~ComScope()
    {
        if (SUCCEEDED(m_result)) CoUninitialize();
    }
    [[nodiscard]] bool Usable() const { return m_usable; }

private:
    HRESULT m_result{};
    bool m_usable{};
};

// The device's channel count, sample rate and sample type, read from the mix
// format the engine will hand us.
struct MixFormat
{
    uint32_t sampleRate{};
    uint16_t channels{};
    bool isFloat{};
};

[[nodiscard]] bool DescribeMixFormat(const WAVEFORMATEX& aFormat, MixFormat& aOut, std::string& aError)
{
    aOut.channels = aFormat.nChannels;
    aOut.sampleRate = aFormat.nSamplesPerSec;
    if (aFormat.wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
    {
        aOut.isFloat = true;
    }
    else if (aFormat.wFormatTag == WAVE_FORMAT_PCM)
    {
        aOut.isFloat = false;
    }
    else if (aFormat.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             aFormat.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
    {
        const auto* const extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(&aFormat);
        if (IsEqualGUID(extended->SubFormat, kSubFormatIeeeFloat)) aOut.isFloat = true;
        else if (IsEqualGUID(extended->SubFormat, kSubFormatPcm)) aOut.isFloat = false;
        else
        {
            aError = "unsupported_extensible_subformat";
            return false;
        }
    }
    else
    {
        aError = "unsupported_sample_format:" + std::to_string(aFormat.wFormatTag);
        return false;
    }
    if (!aOut.isFloat && aFormat.wBitsPerSample != 16)
    {
        aError = "unsupported_pcm_width:" + std::to_string(aFormat.wBitsPerSample);
        return false;
    }
    return true;
}
} // namespace

/// Everything that only exists once a device is open. Kept out of the header
/// so the WASAPI headers do not leak into every translation unit that wants to
/// ask a sink about its volume.
struct AudioSink::Impl
{
    ComScope com;
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    Microsoft::WRL::ComPtr<IMMDevice> device;
    Microsoft::WRL::ComPtr<IAudioClient> client;
    Microsoft::WRL::ComPtr<IAudioRenderClient> render;
    HANDLE event{};
    WAVEFORMATEX* mixFormat{};
    UINT32 bufferFrames{};
    bool isFloat{};
    std::wstring deviceName;
    // Interleaved device-format frames, written by the browser's audio thread
    // and read by the render thread. Monotonic counters with a mask, so a
    // reader and a writer never touch the same index variable.
    std::vector<float> ring;
    std::vector<int16_t> ring16;
    std::atomic<uint64_t> writeFrames{};
    std::atomic<uint64_t> readFrames{};
    std::mutex ringMutex;
};

AudioSink::AudioSink() = default;

AudioSink::~AudioSink()
{
    Close();
}

bool AudioSink::ProbeDeviceFormat(Format& aFormat, std::string& aError)
{
    ComScope com;
    if (!com.Usable())
    {
        aError = "com_unavailable";
        return false;
    }
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(enumerator.GetAddressOf()))))
    {
        aError = "no_device_enumerator";
        return false;
    }
    Microsoft::WRL::ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.GetAddressOf())))
    {
        aError = "no_render_endpoint";
        return false;
    }
    Microsoft::WRL::ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(client.GetAddressOf()))))
    {
        aError = "audio_client_activate_failed";
        return false;
    }
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix)) || mix == nullptr)
    {
        aError = "mix_format_unavailable";
        return false;
    }
    MixFormat described;
    const bool describedOk = DescribeMixFormat(*mix, described, aError);
    CoTaskMemFree(mix);
    if (!describedOk) return false;
    aFormat.sampleRate = described.sampleRate;
    aFormat.channels = described.channels;
    return true;
}

bool AudioSink::Open(std::string& aError)
{
    if (m_opened.load()) return true;
    m_impl = std::make_unique<Impl>();
    if (!m_impl->com.Usable())
    {
        aError = "com_unavailable";
        m_impl.reset();
        return false;
    }
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(m_impl->enumerator.GetAddressOf()))))
    {
        aError = "no_device_enumerator";
        m_impl.reset();
        return false;
    }
    if (FAILED(m_impl->enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                                           m_impl->device.GetAddressOf())))
    {
        aError = "no_render_endpoint";
        m_impl.reset();
        return false;
    }
    {
        Microsoft::WRL::ComPtr<IPropertyStore> store;
        if (SUCCEEDED(m_impl->device->OpenPropertyStore(STGM_READ, store.GetAddressOf())))
        {
            PROPVARIANT value{};
            PropVariantInit(&value);
            if (SUCCEEDED(store->GetValue(kDeviceFriendlyName, &value)) && value.vt == VT_LPWSTR)
            {
                m_impl->deviceName = value.pwszVal;
            }
            PropVariantClear(&value);
        }
    }
    if (FAILED(m_impl->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                        reinterpret_cast<void**>(m_impl->client.GetAddressOf()))))
    {
        aError = "audio_client_activate_failed";
        m_impl.reset();
        return false;
    }
    if (FAILED(m_impl->client->GetMixFormat(&m_impl->mixFormat)) || m_impl->mixFormat == nullptr)
    {
        aError = "mix_format_unavailable";
        m_impl.reset();
        return false;
    }
    MixFormat described;
    if (!DescribeMixFormat(*m_impl->mixFormat, described, aError))
    {
        m_impl.reset();
        return false;
    }
    m_impl->isFloat = described.isFloat;
    m_format.sampleRate = described.sampleRate;
    m_format.channels = described.channels;
    // Event-driven shared mode: the engine wakes this thread when it wants
    // frames, which is what keeps latency at one buffer instead of one polling
    // interval. 100 ms is the conventional request.
    constexpr REFERENCE_TIME kBufferDuration = 1000000; // 100 ms in 100 ns units
    if (FAILED(m_impl->client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                          kBufferDuration, 0, m_impl->mixFormat, nullptr)))
    {
        aError = "audio_client_initialize_failed";
        m_impl.reset();
        return false;
    }
    if (FAILED(m_impl->client->GetBufferSize(&m_impl->bufferFrames)))
    {
        aError = "buffer_size_unavailable";
        m_impl.reset();
        return false;
    }
    m_impl->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (m_impl->event == nullptr || FAILED(m_impl->client->SetEventHandle(m_impl->event)))
    {
        aError = "event_handle_failed";
        m_impl.reset();
        return false;
    }
    if (FAILED(m_impl->client->GetService(__uuidof(IAudioRenderClient),
                                          reinterpret_cast<void**>(m_impl->render.GetAddressOf()))))
    {
        aError = "render_client_unavailable";
        m_impl.reset();
        return false;
    }
    // Prime the device with silence: the engine plays whatever the buffer holds
    // as soon as Start() returns, and uninitialised audio is noise.
    BYTE* initial = nullptr;
    if (SUCCEEDED(m_impl->render->GetBuffer(m_impl->bufferFrames, &initial)) && initial != nullptr)
    {
        std::memset(initial, 0, static_cast<size_t>(m_impl->bufferFrames) * m_format.channels *
                                   (m_impl->isFloat ? sizeof(float) : sizeof(int16_t)));
        m_impl->render->ReleaseBuffer(m_impl->bufferFrames, 0);
    }
    m_impl->ring.assign(static_cast<size_t>(kRingFrames) * m_format.channels, 0.0F);
    if (!m_impl->isFloat)
    {
        m_impl->ring16.assign(static_cast<size_t>(kRingFrames) * m_format.channels, 0);
    }
    if (FAILED(m_impl->client->Start()))
    {
        aError = "audio_client_start_failed";
        m_impl.reset();
        return false;
    }
    m_stopping.store(false);
    m_opened.store(true);
    m_thread = std::thread([this]() { RenderLoop(); });
    return true;
}

void AudioSink::Close()
{
    if (!m_opened.load() && !m_thread.joinable()) return;
    m_stopping.store(true);
    if (m_impl && m_impl->event != nullptr) SetEvent(m_impl->event);
    if (m_thread.joinable()) m_thread.join();
    if (m_impl)
    {
        if (m_impl->client != nullptr) m_impl->client->Stop();
        if (m_impl->event != nullptr)
        {
            CloseHandle(m_impl->event);
            m_impl->event = nullptr;
        }
        if (m_impl->mixFormat != nullptr)
        {
            CoTaskMemFree(m_impl->mixFormat);
            m_impl->mixFormat = nullptr;
        }
        m_impl.reset();
    }
    m_opened.store(false);
}

void AudioSink::SetGain(const float aGain)
{
    m_gain.store(std::clamp(aGain, 0.0F, 2.0F));
}

void AudioSink::SetMuted(const bool aMuted)
{
    m_muted.store(aMuted);
}

AudioSink::Stats AudioSink::Snapshot() const
{
    Stats stats;
    stats.pushedFrames = m_pushedFrames.load();
    stats.playedFrames = m_playedFrames.load();
    stats.underrunFrames = m_underrunFrames.load();
    stats.droppedFrames = m_droppedFrames.load();
    stats.streams = m_streams.load();
    stats.streamErrors = m_streamErrors.load();
    return stats;
}

std::string AudioSink::Describe() const
{
    const Format format = m_format;
    const Stats stats = Snapshot();
    std::string name;
    if (m_impl)
    {
        // The device name is wide; only the tail after the last backslash is
        // worth logging, and it is the part that identifies the endpoint.
        const std::wstring& wide = m_impl->deviceName;
        const size_t split = wide.find_last_of(L'\\');
        const std::wstring tail = split == std::wstring::npos ? wide : wide.substr(split + 1);
        name.assign(tail.begin(), tail.end());
    }
    const bool muted = m_muted.load();
    const float gain = m_gain.load();
    return "device=\"" + name + "\" rate=" + std::to_string(format.sampleRate) + " channels=" +
           std::to_string(format.channels) + (m_impl && !m_impl->isFloat ? " pcm16" : " float32") +
           " gain=" + std::to_string(gain) + (muted ? " muted" : "") +
           " streams=" + std::to_string(stats.streams) + " streamErrors=" + std::to_string(stats.streamErrors) +
           " pushed=" + std::to_string(stats.pushedFrames) + " played=" + std::to_string(stats.playedFrames) +
           " underrun=" + std::to_string(stats.underrunFrames) + " dropped=" + std::to_string(stats.droppedFrames);
}

void AudioSink::Push(const float* const* const aData, const int aFrames, const int aChannels)
{
    if (!m_opened.load() || m_impl == nullptr || aData == nullptr || aFrames <= 0 || aChannels <= 0) return;
    const int deviceChannels = m_format.channels;
    const uint64_t write = m_impl->writeFrames.load(std::memory_order_relaxed);
    const uint64_t read = m_impl->readFrames.load(std::memory_order_acquire);
    const uint64_t used = write - read;
    const uint64_t space = kRingFrames - std::min<uint64_t>(used, kRingFrames);
    const uint64_t accepted = std::min<uint64_t>(space, static_cast<uint64_t>(aFrames));
    if (accepted < static_cast<uint64_t>(aFrames))
    {
        m_droppedFrames.fetch_add(static_cast<uint64_t>(aFrames) - accepted);
    }
    if (accepted == 0) return;
    m_streaming.store(true);
    {
        std::lock_guard<std::mutex> guard(m_impl->ringMutex);
        for (uint64_t frame = 0; frame < accepted; ++frame)
        {
            const uint64_t slot = (write + frame) % kRingFrames;
            for (int channel = 0; channel < deviceChannels; ++channel)
            {
                // Mono source to a stereo device is duplicated rather than
                // half-silenced; anything else maps by index.
                const int source = aChannels == 1 ? 0 : std::min(channel, aChannels - 1);
                const float* const samples = aData[source];
                m_impl->ring[static_cast<size_t>(slot) * deviceChannels + channel] =
                    samples == nullptr ? 0.0F : samples[frame];
            }
        }
    }
    m_impl->writeFrames.store(write + accepted, std::memory_order_release);
    m_pushedFrames.fetch_add(accepted);
}

uint64_t AudioSink::ReadRing(float* const aDestination, const uint64_t aFrames)
{
    const uint64_t write = m_impl->writeFrames.load(std::memory_order_acquire);
    uint64_t read = m_impl->readFrames.load(std::memory_order_relaxed);
    const uint64_t available = write - read;
    const uint64_t taken = std::min<uint64_t>(available, aFrames);
    if (taken > 0)
    {
        const int deviceChannels = m_format.channels;
        std::lock_guard<std::mutex> guard(m_impl->ringMutex);
        for (uint64_t frame = 0; frame < taken; ++frame)
        {
            const uint64_t slot = (read + frame) % kRingFrames;
            std::memcpy(aDestination + frame * deviceChannels,
                        m_impl->ring.data() + static_cast<size_t>(slot) * deviceChannels,
                        static_cast<size_t>(deviceChannels) * sizeof(float));
        }
        read += taken;
        m_impl->readFrames.store(read, std::memory_order_release);
    }
    return taken;
}

void AudioSink::RenderLoop()
{
    const int deviceChannels = m_format.channels;
    std::vector<float> scratch(static_cast<size_t>(m_impl->bufferFrames) * deviceChannels, 0.0F);
    while (!m_stopping.load())
    {
        if (WaitForSingleObject(m_impl->event, kRenderWaitMilliseconds) != WAIT_OBJECT_0) continue;
        if (m_stopping.load()) break;
        UINT32 padding = 0;
        if (FAILED(m_impl->client->GetCurrentPadding(&padding))) continue;
        UINT32 available = m_impl->bufferFrames > padding ? m_impl->bufferFrames - padding : 0;
        if (available == 0) continue;
        BYTE* destination = nullptr;
        if (FAILED(m_impl->render->GetBuffer(available, &destination)) || destination == nullptr) continue;
        uint64_t written = 0;
        while (written < available)
        {
            const uint64_t chunk = std::min<uint64_t>(available - written, m_impl->bufferFrames);
            const uint64_t taken = ReadRing(scratch.data(), chunk);
            if (taken < chunk)
            {
                std::memset(scratch.data() + static_cast<size_t>(taken) * deviceChannels, 0,
                            static_cast<size_t>(chunk - taken) * deviceChannels * sizeof(float));
                if (m_streaming.load()) m_underrunFrames.fetch_add(chunk - taken);
            }
            const float gain = m_muted.load() ? 0.0F : m_gain.load();
            const size_t sampleCount = static_cast<size_t>(chunk) * deviceChannels;
            for (size_t index = 0; index < sampleCount; ++index)
            {
                scratch[index] = std::clamp(scratch[index] * gain, -1.0F, 1.0F);
            }
            if (m_impl->isFloat)
            {
                std::memcpy(destination + written * deviceChannels * sizeof(float), scratch.data(),
                            sampleCount * sizeof(float));
            }
            else
            {
                auto* const samples = reinterpret_cast<int16_t*>(destination + written * deviceChannels *
                                                                                  sizeof(int16_t));
                for (size_t index = 0; index < sampleCount; ++index)
                {
                    samples[index] = static_cast<int16_t>(std::lround(scratch[index] * 32767.0F));
                }
            }
            written += chunk;
        }
        m_impl->render->ReleaseBuffer(available, 0);
        if (m_streaming.load()) m_playedFrames.fetch_add(written);
    }
}

int AudioSink::SelfTest(const double aSeconds)
{
    AudioSink sink;
    std::string error;
    if (!sink.Open(error))
    {
        std::fprintf(stderr, "Open77 audio self-test: sink open failed: %s\n", error.c_str());
        return kExitAudioSinkOpenFailed;
    }
    const Format format = sink.DeviceFormat();
    std::fprintf(stderr, "Open77 audio self-test: %s\n", sink.Describe().c_str());
    const int channels = format.channels;
    const auto sampleRate = static_cast<double>(format.sampleRate);
    // A 440 Hz tone, delivered in the same 10 ms cadence a browser stream uses
    // (1024 frames per packet is Chromium's own chunk), so the ring sees the
    // same burstiness it will see in production.
    const int framesPerChunk = 1024;
    std::vector<float> storage(static_cast<size_t>(framesPerChunk) * channels, 0.0F);
    std::vector<const float*> planes(static_cast<size_t>(channels), nullptr);
    for (int channel = 0; channel < channels; ++channel)
    {
        planes[static_cast<size_t>(channel)] = storage.data() + static_cast<size_t>(channel) * framesPerChunk;
    }
    const auto started = std::chrono::steady_clock::now();
    uint64_t pushed = 0;
    double phase = 0.0;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() < aSeconds)
    {
        // Pace against the clock, not "sleep one chunk's worth": a sleep that
        // overruns by a millisecond per 23 ms chunk starves the device by 4%
        // and the run reports underruns that the sink invented rather than saw.
        // (pacing happens after the push, against the frames pushed so far)
        for (int frame = 0; frame < framesPerChunk; ++frame)
        {
            const float value = static_cast<float>(std::sin(phase) * 0.25);
            for (int channel = 0; channel < channels; ++channel)
            {
                storage[static_cast<size_t>(channel) * framesPerChunk + frame] = value;
            }
            phase += 2.0 * 3.14159265358979323846 * 440.0 / sampleRate;
            if (phase > 2.0 * 3.14159265358979323846) phase -= 2.0 * 3.14159265358979323846;
        }
        sink.Push(planes.data(), framesPerChunk, channels);
        pushed += framesPerChunk;
        const auto nextDue = started + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                           std::chrono::duration<double>(static_cast<double>(pushed) / sampleRate));
        if (nextDue > std::chrono::steady_clock::now())
        {
            std::this_thread::sleep_until(nextDue);
        }
    }
    // Let the device drain what is already buffered before measuring.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const Stats stats = sink.Snapshot();
    std::fprintf(stderr, "Open77 audio self-test: %s\n", sink.Describe().c_str());
    const double expectedFrames = aSeconds * sampleRate;
    const bool healthy = stats.playedFrames > 0 &&
                         static_cast<double>(stats.playedFrames) >= expectedFrames * 0.5 &&
                         stats.underrunFrames < stats.playedFrames / 2;
    std::fprintf(stderr, "Open77 audio self-test: %s (pushed=%llu played=%llu expected~%.0f)\n",
                 healthy ? "ok" : "failed", static_cast<unsigned long long>(pushed),
                 static_cast<unsigned long long>(stats.playedFrames), expectedFrames);
    sink.Close();
    return healthy ? 0 : kExitAudioUnhealthy;
}
} // namespace op77::WebHost
