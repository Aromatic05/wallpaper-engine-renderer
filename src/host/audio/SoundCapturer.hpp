#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace wallpaper::audio
{

class SoundCapturer {
public:
    SoundCapturer();
    ~SoundCapturer();

    SoundCapturer(const SoundCapturer&)            = delete;
    SoundCapturer& operator=(const SoundCapturer&) = delete;

    bool Init();
    bool IsInited() const;
    bool EnsureInit() const;
    void GetSpectrum(uint32_t resolution,
                     std::vector<float>* left,
                     std::vector<float>* right,
                     std::vector<float>* average) const;

private:
    class impl;
    std::unique_ptr<impl> m_impl;
};

} // namespace wallpaper::audio
