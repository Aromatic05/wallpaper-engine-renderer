#include "host/audio/SoundSpectrumDsp.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <cmath>
#include <complex>
#include <numbers>

namespace
{
constexpr float kSampleRate = 48000.0f;
using Buffer                = std::array<std::complex<float>, wallpaper::audio::dsp::kFftSize>;

void FillSine(Buffer& left, Buffer& right, float hz, float left_amplitude, float right_amplitude) {
    for (std::size_t index = 0; index < left.size(); ++index) {
        const float time   = static_cast<float>(index) / kSampleRate;
        const float sine   = std::sin(2.0f * std::numbers::pi_v<float> * hz * time);
        const float window = wallpaper::audio::dsp::HannWindow(index, left.size());
        left[index]        = std::complex<float>(left_amplitude * sine * window, 0.0f);
        right[index]       = std::complex<float>(right_amplitude * sine * window, 0.0f);
    }
    wallpaper::audio::dsp::FftInPlace(left.data(), left.size());
    wallpaper::audio::dsp::FftInPlace(right.data(), right.size());
}

std::size_t ExpectedBand(const wallpaper::audio::dsp::BandLayout& layout, float hz) {
    const auto bin = wallpaper::audio::dsp::HertzToUpperBin(hz, kSampleRate);
    for (std::size_t band = 0; band < wallpaper::audio::dsp::kSpectrumBands; ++band) {
        if (bin >= layout.edges[band] && bin < layout.edges[band + 1]) return band;
    }
    return wallpaper::audio::dsp::kSpectrumBands - 1;
}

std::size_t PeakBand(const std::array<float, wallpaper::audio::dsp::kSpectrumBands>& values) {
    std::size_t peak_band = 0;
    for (std::size_t band = 1; band < values.size(); ++band) {
        if (values[band] > values[peak_band]) peak_band = band;
    }
    return peak_band;
}

bool NearBand(std::size_t actual, std::size_t expected, std::size_t tolerance) {
    return actual <= expected + tolerance && expected <= actual + tolerance;
}

void Require(bool condition, const char* message) {
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}
} // namespace

int main() {
    const auto  layout        = wallpaper::audio::dsp::MakeMelLayout(kSampleRate);
    const float normalization = 2.0f / static_cast<float>(wallpaper::audio::dsp::kFftSize);

    for (const float hz : { 60.0f, 250.0f, 1000.0f, 6000.0f, 12000.0f }) {
        Buffer left {};
        Buffer right {};
        FillSine(left, right, hz, 0.5f, 0.5f);
        const auto spectrum = wallpaper::audio::dsp::AnalyzeStereoSpectrum(
            left.data(), right.data(), layout, normalization);
        Require(NearBand(PeakBand(spectrum.average), ExpectedBand(layout, hz), 2),
                "Mel-band frequency mapping mismatch");
    }

    Buffer left {};
    Buffer right {};
    FillSine(left, right, 440.0f, 0.03f, 0.03f);
    const auto quiet_tone = wallpaper::audio::dsp::AnalyzeStereoSpectrum(
        left.data(), right.data(), layout, normalization);
    const float response = quiet_tone.average[PeakBand(quiet_tone.average)];
    Require(response > 0.60f, "quiet-tone visual response is too small");
    Require(response < 0.80f, "quiet-tone visual response is too large");

    FillSine(left, right, 1000.0f, 1.0f, 0.0f);
    const auto channel_split = wallpaper::audio::dsp::AnalyzeStereoSpectrum(
        left.data(), right.data(), layout, normalization);
    const auto band = PeakBand(channel_split.left);
    Require(channel_split.left[band] > 0.0f, "active channel produced no spectrum");
    Require(channel_split.left[band] <= wallpaper::audio::dsp::kResponseCeiling,
            "spectrum response exceeded its ceiling");
    Require(channel_split.right[band] == 0.0f, "silent channel produced spectrum");
    Require(std::abs(channel_split.average[band] - channel_split.left[band] * 0.5f) < 1.0e-5f,
            "stereo average mismatch");

    wallpaper::audio::dsp::SpectrumBands state {};
    const auto                           attacked = wallpaper::audio::dsp::SmoothSpectrum(
        channel_split, state, static_cast<float>(wallpaper::audio::dsp::kHopSize) / kSampleRate);
    Require(attacked.left[band] > 0.0f, "attack smoothing suppressed the signal");
    Require(attacked.left[band] < channel_split.left[band], "attack smoothing did not interpolate");

    wallpaper::audio::dsp::SpectrumBands silence {};
    const auto                           released = wallpaper::audio::dsp::SmoothSpectrum(
        silence, state, static_cast<float>(wallpaper::audio::dsp::kHopSize) / kSampleRate);
    Require(released.left[band] > 0.0f, "release smoothing dropped immediately to zero");
    Require(released.left[band] < attacked.left[band], "release smoothing did not decay");
    return 0;
}
