#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>

namespace wallpaper::audio::dsp
{

inline constexpr std::size_t kFftSize         = 4096;
inline constexpr std::size_t kSpectrumBands   = 64;
inline constexpr std::size_t kHalfFft         = kFftSize / 2;
inline constexpr std::size_t kHopSize         = 1024;
inline constexpr float       kMinFrequencyHz  = 20.0f;
inline constexpr float       kMaxFrequencyHz  = 20000.0f;
inline constexpr float       kTiltPivotHz     = 200.0f;
inline constexpr float       kTiltExponent    = 0.30f;
inline constexpr float       kDbFloor         = -80.0f;
inline constexpr float       kDbCeiling       = -8.0f;
inline constexpr float       kResponsePower   = 1.6f;
inline constexpr float       kResponseScale   = 1.0f;
inline constexpr float       kResponseCeiling = 1.0f;
inline constexpr float       kAttackTimeSec   = 0.030f;
inline constexpr float       kReleaseTimeSec  = 0.140f;

struct BandLayout {
    std::array<std::size_t, kSpectrumBands + 1> edges {};
    std::array<float, kSpectrumBands>           gain {};
};

struct SpectrumBands {
    std::array<float, kSpectrumBands> left {};
    std::array<float, kSpectrumBands> right {};
    std::array<float, kSpectrumBands> average {};
};

inline float HertzToMel(float hz) { return 2595.0f * std::log10(1.0f + hz / 700.0f); }

inline float MelToHertz(float mel) { return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f); }

inline std::size_t HertzToUpperBin(float hz, float sample_rate) {
    if (sample_rate <= 0.0f) return 1;
    const auto bin =
        static_cast<std::size_t>(std::ceil(static_cast<double>(hz) * static_cast<double>(kFftSize) /
                                           static_cast<double>(sample_rate)));
    return std::clamp<std::size_t>(bin, 1, kHalfFft);
}

inline BandLayout MakeMelLayout(float sample_rate) {
    BandLayout layout {};
    if (sample_rate <= 0.0f) return layout;

    const float nyquist = sample_rate * 0.5f;
    const float max_hz  = std::min(kMaxFrequencyHz, nyquist);
    const float min_hz  = std::min(kMinFrequencyHz, max_hz);
    const float min_mel = HertzToMel(min_hz);
    const float max_mel = HertzToMel(max_hz);
    const auto  max_bin = HertzToUpperBin(max_hz, sample_rate);

    layout.edges[0] = HertzToUpperBin(min_hz, sample_rate);
    for (std::size_t band = 1; band < kSpectrumBands; ++band) {
        const float t    = static_cast<float>(band) / static_cast<float>(kSpectrumBands);
        const float hz   = MelToHertz(min_mel + (max_mel - min_mel) * t);
        std::size_t next = HertzToUpperBin(hz, sample_rate);
        if (next <= layout.edges[band - 1]) next = layout.edges[band - 1] + 1;

        const std::size_t remaining = kSpectrumBands - band;
        if (next + remaining > max_bin) next = max_bin - remaining;
        layout.edges[band] = next;
    }
    layout.edges[kSpectrumBands] = max_bin;

    for (std::size_t band = 0; band < kSpectrumBands; ++band) {
        const float center_bin = 0.5f * (static_cast<float>(layout.edges[band]) +
                                         static_cast<float>(layout.edges[band + 1]));
        const float center_hz  = center_bin * sample_rate / static_cast<float>(kFftSize);
        layout.gain[band]      = std::pow(center_hz / kTiltPivotHz, kTiltExponent);
    }
    return layout;
}

inline float BandMagnitude(const std::complex<float>* values, const BandLayout& layout,
                           std::size_t band, float normalization) {
    const std::size_t begin = layout.edges[band];
    const std::size_t end   = layout.edges[band + 1];
    float             peak  = 0.0f;
    for (std::size_t bin = begin; bin < end; ++bin) {
        peak = std::max(peak, std::abs(values[bin]));
    }
    return peak * normalization;
}

inline float ShapeResponse(float unit) {
    const float value = std::clamp(unit, 0.0f, 1.0f);
    if (value <= 0.5f) {
        return 0.5f * std::pow(value * 2.0f, kResponsePower);
    }
    return 1.0f - 0.5f * std::pow((1.0f - value) * 2.0f, kResponsePower);
}

inline float VisualResponse(float magnitude, const BandLayout& layout, std::size_t band) {
    const float compensated = std::max(magnitude * layout.gain[band], 1.0e-12f);
    const float db          = 20.0f * std::log10(compensated);
    const float unit        = std::clamp((db - kDbFloor) / (kDbCeiling - kDbFloor), 0.0f, 1.0f);
    return std::min(ShapeResponse(unit) * kResponseScale, kResponseCeiling);
}

inline SpectrumBands AnalyzeStereoSpectrum(const std::complex<float>* left,
                                           const std::complex<float>* right,
                                           const BandLayout& layout, float normalization) {
    SpectrumBands result {};
    for (std::size_t band = 0; band < kSpectrumBands; ++band) {
        result.left[band] =
            VisualResponse(BandMagnitude(left, layout, band, normalization), layout, band);
        result.right[band] =
            VisualResponse(BandMagnitude(right, layout, band, normalization), layout, band);
        result.average[band] = 0.5f * (result.left[band] + result.right[band]);
    }
    return result;
}

inline float SmoothValue(float previous, float current, float delta_seconds) {
    const float time_constant = current > previous ? kAttackTimeSec : kReleaseTimeSec;
    const float alpha         = 1.0f - std::exp(-std::max(0.0f, delta_seconds) / time_constant);
    return previous + alpha * (current - previous);
}

inline SpectrumBands SmoothSpectrum(const SpectrumBands& input, SpectrumBands& state,
                                    float delta_seconds) {
    SpectrumBands output {};
    for (std::size_t band = 0; band < kSpectrumBands; ++band) {
        state.left[band]     = SmoothValue(state.left[band], input.left[band], delta_seconds);
        state.right[band]    = SmoothValue(state.right[band], input.right[band], delta_seconds);
        output.left[band]    = state.left[band];
        output.right[band]   = state.right[band];
        output.average[band] = 0.5f * (output.left[band] + output.right[band]);
    }
    return output;
}

inline void FftInPlace(std::complex<float>* values, std::size_t count) {
    for (std::size_t i = 1, j = 0; i < count; ++i) {
        std::size_t bit = count >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(values[i], values[j]);
    }

    for (std::size_t length = 2; length <= count; length <<= 1) {
        const float angle = -2.0f * std::numbers::pi_v<float> / static_cast<float>(length);
        const std::complex<float> step(std::cos(angle), std::sin(angle));
        for (std::size_t begin = 0; begin < count; begin += length) {
            std::complex<float> weight(1.0f, 0.0f);
            const std::size_t   half = length >> 1;
            for (std::size_t index = 0; index < half; ++index) {
                const auto even              = values[begin + index];
                const auto odd               = values[begin + index + half] * weight;
                values[begin + index]        = even + odd;
                values[begin + index + half] = even - odd;
                weight *= step;
            }
        }
    }
}

inline float HannWindow(std::size_t index, std::size_t count) {
    return 0.5f * (1.0f - std::cos(2.0f * std::numbers::pi_v<float> * static_cast<float>(index) /
                                   static_cast<float>(count - 1)));
}

} // namespace wallpaper::audio::dsp
