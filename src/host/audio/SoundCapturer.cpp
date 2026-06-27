#include "SoundCapturer.hpp"
#include "miniaudio-wrapper.hpp"

#include "utils/Logging.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <mutex>
#include <numbers>
#include <optional>
#include <string_view>

namespace wallpaper::audio
{
namespace
{
constexpr ma_uint32 kSpectrumBandCount   = 64;
constexpr ma_uint32 kCaptureChannelCount = 2;
constexpr ma_uint32 kCaptureSampleRate   = 48000;

bool ContainsAsciiCaseInsensitive(std::string_view text, std::string_view pattern) {
    return std::search(text.begin(),
                       text.end(),
                       pattern.begin(),
                       pattern.end(),
                       [](char lhs, char rhs) {
                           return std::tolower(static_cast<unsigned char>(lhs)) ==
                                  std::tolower(static_cast<unsigned char>(rhs));
                       }) != text.end();
}

float GoertzelMagnitude(const float* samples,
                        ma_uint32    frame_count,
                        ma_uint32    channels,
                        ma_uint32    channel_index,
                        ma_uint32    sample_rate,
                        double       target_frequency) {
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

std::optional<ma_device_id> FindMonitorCaptureDevice(ma_context& context) {
    ma_device_info* playback_infos { nullptr };
    ma_uint32       playback_count { 0 };
    ma_device_info* capture_infos { nullptr };
    ma_uint32       capture_count { 0 };
    if (ma_context_get_devices(&context,
                               &playback_infos,
                               &playback_count,
                               &capture_infos,
                               &capture_count) != MA_SUCCESS) {
        LOG_ERROR("SoundCapturer: failed to enumerate audio devices");
        return std::nullopt;
    }

    for (ma_uint32 i = 0; i < capture_count; i++) {
        const std::string_view name { capture_infos[i].name };
        if (!ContainsAsciiCaseInsensitive(name, "monitor")) continue;
        LOG_INFO("SoundCapturer: selected monitor source '%s'", capture_infos[i].name);
        return capture_infos[i].id;
    }

    LOG_ERROR("SoundCapturer: no monitor capture device found");
    return std::nullopt;
}
} // namespace

class SoundCapturer::impl {
public:
    bool Init() {
        if (m_inited) return true;

        constexpr ma_backend backends[] = {
            ma_backend_pulseaudio,
            ma_backend_alsa,
        };

        ma_context_config context_config = ma_context_config_init();
        context_config.pulse.pApplicationName = "wallpaper-engine-renderer";
        if (ma_context_init(backends,
                            static_cast<ma_uint32>(std::size(backends)),
                            &context_config,
                            &m_context) != MA_SUCCESS) {
            LOG_ERROR("SoundCapturer: failed to init miniaudio context");
            return false;
        }
        m_context_inited = true;

        const auto device_id = FindMonitorCaptureDevice(m_context);
        if (!device_id.has_value()) {
            UnInit();
            return false;
        }
        m_capture_device_id = *device_id;
        m_has_capture_device_id = true;

        ma_device_config config = ma_device_config_init(ma_device_type_capture);
        config.capture.pDeviceID = &m_capture_device_id;
        config.capture.format    = ma_format_f32;
        config.capture.channels  = kCaptureChannelCount;
        config.sampleRate        = kCaptureSampleRate;
        config.dataCallback      = DataCallback;
        config.pUserData         = this;

        if (ma_device_init(&m_context, &config, &m_device) != MA_SUCCESS) {
            LOG_ERROR("SoundCapturer: failed to init capture device");
            UnInit();
            return false;
        }
        m_device_inited = true;

        if (ma_device_start(&m_device) != MA_SUCCESS) {
            LOG_ERROR("SoundCapturer: failed to start capture device");
            UnInit();
            return false;
        }

        m_inited = true;
        return true;
    }

    ~impl() { UnInit(); }

    bool IsInited() const { return m_inited; }

    void GetSpectrum(uint32_t resolution,
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
            (*left)[i]    = left_sum / count;
            (*right)[i]   = right_sum / count;
            (*average)[i] = average_sum / count;
        }
    }

private:
    static void DataCallback(ma_device* device, void* output, const void* input, ma_uint32 frames) {
        (void)output;
        auto* self = static_cast<impl*>(device->pUserData);
        if (self == nullptr || input == nullptr || frames == 0) return;
        self->AnalyzeSpectrum(static_cast<const float*>(input),
                              frames,
                              device->capture.channels,
                              device->sampleRate);
    }

    void AnalyzeSpectrum(const float* samples,
                         ma_uint32    frame_count,
                         ma_uint32    channels,
                         ma_uint32    sample_rate) {
        if (samples == nullptr || frame_count == 0 || channels == 0 || sample_rate == 0) return;

        constexpr double min_frequency = 20.0;
        const double nyquist = std::max(min_frequency, static_cast<double>(sample_rate) * 0.5);
        std::array<float, kSpectrumBandCount> left {};
        std::array<float, kSpectrumBandCount> right {};
        std::array<float, kSpectrumBandCount> average {};

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
        m_spectrum_left    = left;
        m_spectrum_right   = right;
        m_spectrum_average = average;
    }

    void UnInit() {
        m_inited = false;
        if (m_device_inited) {
            ma_device_uninit(&m_device);
            m_device_inited = false;
        }
        if (m_context_inited) {
            ma_context_uninit(&m_context);
            m_context_inited = false;
        }
        m_has_capture_device_id = false;
    }

    ma_context                            m_context {};
    ma_device                             m_device {};
    ma_device_id                          m_capture_device_id {};
    bool                                  m_has_capture_device_id { false };
    bool                                  m_context_inited { false };
    bool                                  m_device_inited { false };
    bool                                  m_inited { false };
    mutable std::mutex                    m_spectrum_mutex;
    std::array<float, kSpectrumBandCount> m_spectrum_left {};
    std::array<float, kSpectrumBandCount> m_spectrum_right {};
    std::array<float, kSpectrumBandCount> m_spectrum_average {};
};

SoundCapturer::SoundCapturer(): m_impl(std::make_unique<impl>()) {}
SoundCapturer::~SoundCapturer() = default;

bool SoundCapturer::Init() { return m_impl->Init(); }

bool SoundCapturer::IsInited() const { return m_impl->IsInited(); }

bool SoundCapturer::EnsureInit() const { return m_impl->IsInited() || m_impl->Init(); }

void SoundCapturer::GetSpectrum(uint32_t            resolution,
                                std::vector<float>* left,
                                std::vector<float>* right,
                                std::vector<float>* average) const {
    if (!EnsureInit()) {
        if (left != nullptr) left->assign(static_cast<size_t>(resolution), 0.0f);
        if (right != nullptr) right->assign(static_cast<size_t>(resolution), 0.0f);
        if (average != nullptr) average->assign(static_cast<size_t>(resolution), 0.0f);
        return;
    }
    m_impl->GetSpectrum(resolution, left, right, average);
}

} // namespace wallpaper::audio
