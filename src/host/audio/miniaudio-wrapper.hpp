#pragma once
#include <memory>
#include <array>
#include <vector>
#include <mutex>
#include <cstdint>
#include <algorithm>
#include <functional>
#include <cstring>
#include <atomic>
#include <cmath>
#include <numbers>
#include <utility>

#include "utils/Logging.h"
#include "core/NoCopyMove.hpp"

#define MA_NO_WASAPI
#define MA_NO_DSOUND
#define MA_NO_WINMM
#define MA_NO_COREAUDIO
#define MA_NO_ENCODING
#define STB_VORBIS_HEADER_ONLY
#include <miniaudio/extras/stb_vorbis.c> /* Enables Vorbis decoding. */
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio/miniaudio.h>
/* stb_vorbis implementation must come after the implementation of miniaudio. */
#undef STB_VORBIS_HEADER_ONLY
#include <miniaudio/extras/stb_vorbis.c> /* Enables Vorbis decoding. */

namespace miniaudio
{

struct DeviceDesc {
    ma_uint32              phyChannels;
    ma_uint32              sampleRate;
    static const ma_format format { ma_format_f32 };
};

template<typename TStream>
class Decoder : NoCopy {
public:
    Decoder(TStream&& s): m_stream(std::move(s)) {}
    ~Decoder() { ma_decoder_uninit(&m_decoder); }
    Decoder(Decoder&& o) noexcept
        : m_decoder(std::exchange(o.m_decoder, ma_decoder()), m_stream(std::move(m_stream))) {}
    Decoder& operator=(Decoder&& o) noexcept {
        m_decoder = std::exchange(o.m_decoder, ma_decoder());
        m_stream  = std::move(m_stream);
        return *this;
    }

    bool Init(const DeviceDesc& d) {
        ma_decoder_config config =
            ma_decoder_config_init(DeviceDesc::format, d.phyChannels, d.sampleRate);
        ma_result result = ma_decoder_init(Read, Seek, this, &config, &m_decoder);
        m_inited         = result == MA_SUCCESS;
        if (! m_inited) {
            LOG_ERROR("init decoder failed");
        }
        return m_inited;
    }
    ma_uint64 NextPcmData(void* pData, ma_uint64 frameCount) {
        if (! m_inited) return 0;
        decltype(frameCount) readed { 0 };
        ma_result result = ma_decoder_read_pcm_frames(&m_decoder, pData, frameCount, &readed);
        return result == MA_SUCCESS ? readed : 0;
    }
    bool IsInited() { return m_inited; }
    void Reset() {
        if (! m_inited) return;
        ma_decoder_seek_to_pcm_frame(&m_decoder, 0);
    }

private:
    static ma_result Read(ma_decoder* pMaDecoder, void* pBufferOut, size_t bytesToRead,
                          size_t* pBytesRead) {
        auto* pDecoder = static_cast<Decoder<TStream>*>(pMaDecoder->pUserData);
        *pBytesRead    = pDecoder->m_stream.Read(pBufferOut, bytesToRead);
        return MA_SUCCESS;
    }
    static ma_result Seek(ma_decoder* pMaDecoder, ma_int64 byteOffset, ma_seek_origin origin) {
        auto* pDecoder = static_cast<Decoder<TStream>*>(pMaDecoder->pUserData);
        bool  ok       = pDecoder->m_stream.Seek(byteOffset, origin);
        return ok ? MA_SUCCESS : MA_ERROR;
    }
    bool       m_inited { false };
    ma_decoder m_decoder {};
    TStream    m_stream;
};

class Channel : NoCopy {
public:
    Channel()          = default;
    virtual ~Channel() = default;

    virtual ma_uint64 NextPcmData(void* pData, ma_uint32 frameCount) = 0;
    virtual void      PassDeviceDesc(const DeviceDesc&)              = 0;
    virtual bool      IsPlaying() const                              = 0;
    virtual bool      IsEnded() const                                = 0;
    virtual bool      ShouldRemove() const                           = 0;
    virtual float     Volume() const                                 = 0;
};

class Device : NoCopy {
public:
    Device() {}
    ~Device() { UnInit(); }
    Device(Device&& o) noexcept: m_device(std::exchange(o.m_device, ma_device())) {}
    Device& operator=(Device&& o) noexcept {
        m_device = std::exchange(o.m_device, ma_device());
        return *this;
    }

public:
    bool Init(const DeviceDesc& d) {
        if (IsInited()) return true; // already inited
        ma_result result;
        auto      config = GenMaDeviceConfig(d);
        Stop();
        result = ma_device_init(NULL, &config, &m_device);
        if (result == MA_SUCCESS) {
            LOG_INFO("sound device inited");
        }
        if (result != MA_SUCCESS || ! IsInited()) {
            LOG_ERROR("can't init sound device");
            UnInit();
            return false;
        }
        if (m_device.playback.format != ma_format_f32) {
            LOG_ERROR("wrong playback format");
            UnInit();
            return false;
        }
        if (ma_device_start(&m_device) != MA_SUCCESS) {
            LOG_ERROR("can't start sound device");
            UnInit();
            return false;
        }
        {
            std::unique_lock<std::mutex> lock { m_mutex };
            for (auto& el : m_channels) {
                el.chn->PassDeviceDesc(GetDesc());
            }
        }
        Start();
        return true;
    }
    bool IsInited() const { return m_device.state.value != ma_device_state_uninitialized; }
    void UnInit() {
        if (IsInited()) {
            LOG_INFO("uninit sound device");
        }
        UnmountAll();
        ma_device_uninit(&m_device); // always do it
    }
    // bool IsStarted() const { return ma_device_is_started(&m_device); }
    // bool IsStopped() const { return ma_device_get_state(&m_device) == MA_STATE_STOPPED; }
    void Start() {
        m_running = true;
        /*
        if(!IsStopped()) return;
        LOG_INFO("state: %d", ma_device_get_state(&m_device));
        if (ma_device_start(&m_device) != MA_SUCCESS) {
            LOG_ERROR("can't start sound device");
            //ma_device_uninit(&m_device);
        }
        */
    }
    void Stop() {
        m_running = false;
        /*
        if(!IsStarted()) return;
        LOG_INFO("state: %d", ma_device_get_state(&m_device));
        if(ma_device_stop(&m_device) != MA_SUCCESS){
            LOG_ERROR("can't stop sound device");
        }*/
    }
    float Volume() const { return m_volume; }
    bool  Muted() const { return m_muted; }
    void  SetMuted(bool v) { m_muted = v; }

    void SetVolume(float v) { m_volume = v; };
    void GetSpectrum(ma_uint32 resolution,
                     std::vector<float>* left,
                     std::vector<float>* right,
                     std::vector<float>* average) const {
        if (left == nullptr || right == nullptr || average == nullptr) return;

        const auto size = static_cast<size_t>(resolution);
        left->assign(size, 0.0f);
        right->assign(size, 0.0f);
        average->assign(size, 0.0f);
        if (size == 0) return;

        std::lock_guard<std::mutex> lock { m_spectrum_mutex };
        if (size >= m_spectrum_left.size()) {
            left->assign(m_spectrum_left.begin(), m_spectrum_left.end());
            right->assign(m_spectrum_right.begin(), m_spectrum_right.end());
            average->assign(m_spectrum_average.begin(), m_spectrum_average.end());
            return;
        }

        const size_t source_size = m_spectrum_left.size();
        for (size_t i = 0; i < size; i++) {
            const size_t begin = (i * source_size) / size;
            const size_t end   = std::max(begin + 1, ((i + 1) * source_size) / size);
            float left_sum { 0.0f };
            float right_sum { 0.0f };
            float average_sum { 0.0f };
            for (size_t j = begin; j < std::min(end, source_size); j++) {
                left_sum += m_spectrum_left[j];
                right_sum += m_spectrum_right[j];
                average_sum += m_spectrum_average[j];
            }
            const auto count =
                static_cast<float>(std::max<size_t>(1, std::min(end, source_size) - begin));
            (*left)[i] = left_sum / count;
            (*right)[i] = right_sum / count;
            (*average)[i] = average_sum / count;
        }
    }
    void MountChannel(std::shared_ptr<Channel> chn) {
        ChannelWrap chnw;
        chnw.chn = chn;
        chnw.chn->PassDeviceDesc(GetDesc());
        {
            std::unique_lock<std::mutex> lock { m_mutex };
            m_channels.push_back(chnw);
        }
    }
    void UnmountAll() {
        {
            std::unique_lock<std::mutex> lock { m_mutex };
            m_channels.clear();
        }
    }
    DeviceDesc GetDesc() const {
        return DeviceDesc { .phyChannels = m_device.playback.channels,
                            .sampleRate  = m_device.sampleRate };
    }

private:
    static void data_callback(ma_device* pMaDevice, void* pOutput, const void* pInput,
                              ma_uint32 frameCount) {
        Device* pDevice = static_cast<Device*>(pMaDevice->pUserData);
        if (! pDevice->IsInited()) return;
        pDevice->data_callback(pOutput, pInput, frameCount);
    }
    void data_callback(void* pOutput, const void* pInput, ma_uint32 frameCount) {
        (void)pInput;
        if (! m_running || m_muted) return;
        const auto phyChannels    = m_device.playback.channels;
        const auto framesSize     = frameCount * phyChannels;
        const auto framesByteSize = framesSize * sizeof(float);
        {
            if (m_frameBuffer.size() < framesByteSize) m_frameBuffer.resize(framesByteSize);
            // std::memset(pOutput, 0, framesByteSize);
        }
        {
            std::unique_lock<std::mutex> lock { m_mutex };

            float* pOutput_float = static_cast<float*>(pOutput);
            float* pBuffer_float = reinterpret_cast<float*>(m_frameBuffer.data());
            std::fill_n(pOutput_float, framesSize, 0.0f);
            for (ma_uint32 i = 0; i < m_channels.size(); i++) {
                if (! m_channels[i].chn->IsPlaying()) continue;
                ma_uint64 framesReaded =
                    m_channels[i].chn->NextPcmData(m_frameBuffer.data(), frameCount);
                if (framesReaded != 0) {
                    const float channel_volume = m_channels[i].chn->Volume();
                    for (size_t i = 0; i < framesSize; i++)
                        pOutput_float[i] += m_volume * channel_volume * pBuffer_float[i];
                }
            }
            m_channels.erase(std::remove_if(m_channels.begin(),
                                            m_channels.end(),
                                            [](auto& c) {
                                                return c.chn == nullptr || c.chn->ShouldRemove();
                                            }),
                             m_channels.end());
        }
        AnalyzeSpectrum(
            static_cast<const float*>(pOutput), frameCount, phyChannels, m_device.sampleRate);
    }
    ma_device_config GenMaDeviceConfig(const DeviceDesc& d) {
        ma_device_config config  = ma_device_config_init(ma_device_type_playback);
        config.sampleRate        = d.sampleRate;
        config.playback.format   = ma_format_f32;
        config.playback.channels = d.phyChannels;
        config.dataCallback      = data_callback;
        config.pUserData         = (void*)this;
        return config;
    }

    static float GoertzelMagnitude(const float* samples, ma_uint32 frame_count, ma_uint32 channels,
                                   ma_uint32 channel_index, ma_uint32 sample_rate,
                                   double target_frequency) {
        if (samples == nullptr || frame_count == 0 || channels == 0 || sample_rate == 0) {
            return 0.0f;
        }

        const auto actual_channel = std::min(channel_index, channels - 1);
        const double omega =
            (2.0 * std::numbers::pi * target_frequency) / static_cast<double>(sample_rate);
        const double coeff = 2.0 * std::cos(omega);
        double s_prev { 0.0 };
        double s_prev2 { 0.0 };

        for (ma_uint32 i = 0; i < frame_count; i++) {
            const double sample = samples[i * channels + actual_channel];
            const double s      = sample + coeff * s_prev - s_prev2;
            s_prev2             = s_prev;
            s_prev              = s;
        }

        const double power =
            std::max(0.0, s_prev2 * s_prev2 + s_prev * s_prev - coeff * s_prev * s_prev2);
        return static_cast<float>(std::sqrt(power) / static_cast<double>(frame_count));
    }

    void AnalyzeSpectrum(const float* samples, ma_uint32 frame_count, ma_uint32 channels,
                         ma_uint32 sample_rate) {
        if (samples == nullptr || frame_count == 0 || channels == 0 || sample_rate == 0) return;

        constexpr double min_frequency = 20.0;
        const double nyquist = std::max(min_frequency, static_cast<double>(sample_rate) * 0.5);
        std::array<float, 64> left {};
        std::array<float, 64> right {};
        std::array<float, 64> average {};

        for (size_t band = 0; band < average.size(); band++) {
            const double t = (static_cast<double>(band) + 0.5) / static_cast<double>(average.size());
            const double frequency = min_frequency * std::pow(nyquist / min_frequency, t);
            left[band] = std::min(2.0f,
                                  GoertzelMagnitude(samples,
                                                    frame_count,
                                                    channels,
                                                    0,
                                                    sample_rate,
                                                    frequency) *
                                      4.0f);
            right[band] = std::min(2.0f,
                                   GoertzelMagnitude(samples,
                                                     frame_count,
                                                     channels,
                                                     channels > 1 ? 1u : 0u,
                                                     sample_rate,
                                                     frequency) *
                                       4.0f);
            average[band] = (left[band] + right[band]) * 0.5f;
        }

        std::lock_guard<std::mutex> lock { m_spectrum_mutex };
        m_spectrum_left = left;
        m_spectrum_right = right;
        m_spectrum_average = average;
    }

private:
    struct ChannelWrap {
        std::shared_ptr<Channel> chn;
    };
    ma_device         m_device {}; // must init c struct
    std::mutex        m_mutex;     // for operating channel vector
    std::atomic<bool> m_running { false };

    float m_volume { 1.0f };
    bool  m_muted { false };

    std::vector<ChannelWrap> m_channels;
    std::vector<uint8_t>     m_frameBuffer;
    mutable std::mutex       m_spectrum_mutex;
    std::array<float, 64>    m_spectrum_left {};
    std::array<float, 64>    m_spectrum_right {};
    std::array<float, 64>    m_spectrum_average {};
};

} // namespace miniaudio
