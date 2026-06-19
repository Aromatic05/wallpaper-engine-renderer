#pragma once
#include <cstdint>
#include <vector>
#include <iostream>
#include <cstdint>
#include <array>

#include "core/Literals.hpp"

namespace wallpaper
{
struct SpriteFrame {
    i32   imageId { 0 };
    float frametime { 0 };
    float x { 0 };
    float y { 0 };
    float width { 1 };
    float height { 1 };
    float rate { 1 }; // real h / w

    std::array<float, 2> xAxis { 1, 0 };
    std::array<float, 2> yAxis { 0, 1 };
};

class SpriteAnimation {
public:
    const auto& GetAnimateFrame(double newtime) {
        if (!m_playing) {
            return m_frames.at((usize)m_curFrame);
        }
        if ((m_remainTime -= newtime) < 0.0f) {
            SwitchToNext();
            const auto& frame = m_frames.at((usize)m_curFrame);
            m_remainTime      = frame.frametime;
        }
        const auto& frame = m_frames.at((usize)m_curFrame);
        return frame;
    }
    const auto& GetCurFrame() const { return m_frames.at((usize)m_curFrame); }
    void        AppendFrame(const SpriteFrame& frame) { m_frames.push_back(frame); }
    const auto& Frames() const { return m_frames; }

    usize numFrames() const { return m_frames.size(); }
    idx   CurrentFrameIndex() const { return m_curFrame; }

    double Duration() const {
        double total { 0.0 };
        for (const auto& frame : m_frames) {
            total += frame.frametime;
        }
        return total;
    }

    void SetCurrentFrame(idx frame) {
        if (m_frames.empty()) return;
        const auto count = static_cast<idx>(m_frames.size());
        while (frame < 0) {
            frame += count;
        }
        m_curFrame    = frame % count;
        m_remainTime  = m_frames.at((usize)m_curFrame).frametime;
    }

    void Stop() {
        m_playing = false;
        SetCurrentFrame(0);
    }

    void Pause() { m_playing = false; }
    void Play() { m_playing = true; }
    bool IsPlaying() const noexcept { return m_playing; }

private:
    void SwitchToNext() {
        if (m_curFrame >= std::ssize(m_frames) - 1)
            m_curFrame = 0;
        else
            m_curFrame++;
    }
    idx    m_curFrame { 0 };
    double m_remainTime { 0 };
    bool   m_playing { true };

    std::vector<SpriteFrame> m_frames;
};
} // namespace wallpaper
