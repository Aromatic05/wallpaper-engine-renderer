#include "WPSoundParser.hpp"
#include "audio/SoundManager.h"
#include "fs/VFS.h"
#include "wpscene/WPSoundObject.h"
#include "utils/Logging.h"
#include "core/Random.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <string_view>

using namespace wallpaper;

static WPSoundPlaybackMode ToPlaybackMode(std::string_view s) {
    if (s == "single")
        return WPSoundPlaybackMode::Single;
    else if (s == "loop")
        return WPSoundPlaybackMode::Loop;
    else if (s == "random")
        return WPSoundPlaybackMode::Random;
    return WPSoundPlaybackMode::Loop;
};

WPSoundPlaybackPolicy WPSoundParser::ResolvePlaybackPolicy(
    const wpscene::WPSoundObject& obj, bool autoplay, bool force_audio_loop) {
    const auto authored_mode = ToPlaybackMode(obj.playbackmode);
    if (force_audio_loop && autoplay && authored_mode == WPSoundPlaybackMode::Single) {
        return { .autoplay = true, .mode = WPSoundPlaybackMode::Loop };
    }
    return { .autoplay = autoplay, .mode = authored_mode };
}

static const char* PlaybackModeName(WPSoundPlaybackMode mode) {
    switch (mode) {
    case WPSoundPlaybackMode::Single: return "single";
    case WPSoundPlaybackMode::Random: return "random";
    case WPSoundPlaybackMode::Loop: return "loop";
    }
    return "loop";
}

class WPSoundStream : public audio::SoundStream {
public:
    struct Config {
        float        maxtime { 10.0f };
        float        mintime { 0.0f };
        float        volume { 1.0f };
        WPSoundPlaybackMode mode { WPSoundPlaybackMode::Loop };
    };
    WPSoundStream(const std::vector<std::string>& paths, fs::VFS& vfs, Config c)
        : vfs(vfs), m_config(c), m_soundPaths(paths) {};
    virtual ~WPSoundStream() = default;

    uint64_t NextPcmData(void* pData, uint32_t frameCount) override {
        if (m_soundPaths.empty()) return 0;

        if (m_config.mode == WPSoundPlaybackMode::Random && ! m_curActive) {
            if (! m_randomDelayScheduled) ScheduleRandomDelay();
            if (m_randomDelayFramesRemaining > 0) return DrainRandomDelay(pData, frameCount);
            m_randomDelayScheduled = false;
        }

        // first
        if (! m_curActive) {
            Switch();
        }
        if (! m_curActive) return 0;

        uint64_t frameReads = m_curActive->NextPcmData(pData, frameCount);
        if (frameReads == 0) {
            if (m_config.mode == WPSoundPlaybackMode::Single) {
                LOG_INFO("SceneSoundEnd: mode='single' paths=%zu", m_soundPaths.size());
                m_curActive.reset();
                return 0;
            }
            if (m_config.mode == WPSoundPlaybackMode::Random) {
                m_curActive.reset();
                ScheduleRandomDelay();
                if (m_randomDelayFramesRemaining > 0) return DrainRandomDelay(pData, frameCount);
                m_randomDelayScheduled = false;
            }
            Switch();
            if (! m_curActive) return 0;
            frameReads = m_curActive->NextPcmData(pData, frameCount);
        }
        if (frameReads < frameCount) {
            FillSilence(static_cast<float*>(pData) + frameReads * m_desc.channels,
                        frameCount - static_cast<uint32_t>(frameReads));
        }
        // volume
        {
            float*     pData_float = static_cast<float*>(pData);
            const auto num         = frameReads * m_desc.channels;
            for (uint i = 0; i < num; i++, pData_float++) {
                (*pData_float) *= m_config.volume;
            }
        }
        return frameReads;
    };
    void PassDesc(const Desc& d) override { m_desc = d; }
    void Reset() override {
        m_curActive.reset();
        m_curIndex = std::numeric_limits<uint32_t>::max();
        m_randomDelayFramesRemaining = 0;
        m_randomDelayScheduled       = false;
    }

    void Switch() {
        if (m_soundPaths.empty()) return;

        const std::string path = m_soundPaths[LoopIndex()];
        auto              stream = vfs.Open("/assets/" + path);
        if (! stream) {
            LOG_ERROR("SceneSoundSwitch: asset-open-failed path='%s'", path.c_str());
            m_curActive.reset();
            return;
        }

        m_curActive = audio::CreateSoundStream(std::move(stream), m_desc);
        if (! m_curActive) {
            LOG_ERROR("SceneSoundSwitch: decoder-create-failed path='%s' channels=%u sample-rate=%u",
                      path.c_str(),
                      m_desc.channels,
                      m_desc.sampleRate);
            return;
        }
        LOG_INFO("SceneSoundSwitch: path='%s' channels=%u sample-rate=%u",
                 path.c_str(),
                 m_desc.channels,
                 m_desc.sampleRate);
    }
    uint32_t LoopIndex() {
        if (m_config.mode == WPSoundPlaybackMode::Random) {
            if (m_soundPaths.size() == 1) {
                m_curIndex = 0;
                return m_curIndex;
            }
            if (m_curIndex >= m_soundPaths.size()) {
                m_curIndex = Random::get<uint32_t>(0, static_cast<uint32_t>(m_soundPaths.size() - 1));
                return m_curIndex;
            }
            uint32_t next = Random::get<uint32_t>(0, static_cast<uint32_t>(m_soundPaths.size() - 2));
            if (next >= m_curIndex) next++;
            m_curIndex = next;
            return m_curIndex;
        }
        m_curIndex++;
        if (m_curIndex == m_soundPaths.size()) m_curIndex = 0;
        return m_curIndex;
    }
    void ScheduleRandomDelay() {
        const float lower = std::max(0.0f, std::min(m_config.mintime, m_config.maxtime));
        const float upper = std::max(lower, std::max(m_config.mintime, m_config.maxtime));
        const double delaySeconds =
            Random::get<double>(static_cast<double>(lower), static_cast<double>(upper));

        m_randomDelayFramesRemaining =
            static_cast<uint64_t>(delaySeconds * static_cast<double>(m_desc.sampleRate));
        m_randomDelayScheduled = true;
        LOG_INFO("SceneSoundRandomDelay: delay=%.3f frames=%llu",
                 delaySeconds,
                 static_cast<unsigned long long>(m_randomDelayFramesRemaining));
    }
    void FillSilence(float* pData, uint32_t frameCount) {
        if (pData == nullptr || frameCount == 0 || m_desc.channels == 0) return;
        std::fill_n(pData, static_cast<size_t>(frameCount) * m_desc.channels, 0.0f);
    }
    uint64_t DrainRandomDelay(void* pData, uint32_t frameCount) {
        FillSilence(static_cast<float*>(pData), frameCount);
        const auto drained = std::min<uint64_t>(m_randomDelayFramesRemaining, frameCount);
        m_randomDelayFramesRemaining -= drained;
        return frameCount;
    }

private:
    fs::VFS& vfs;
    Config   m_config;
    Desc     m_desc;
    uint32_t m_curIndex { std::numeric_limits<uint32_t>::max() };
    uint64_t m_randomDelayFramesRemaining { 0 };
    bool     m_randomDelayScheduled { false };

    const std::vector<std::string> m_soundPaths;
    std::unique_ptr<SoundStream>   m_curActive;
};

audio::SoundHandle WPSoundParser::Parse(const wpscene::WPSoundObject& obj, fs::VFS& vfs,
                                        audio::SoundManager& sm, bool autoplay,
                                        bool force_audio_loop) {
    const auto policy = ResolvePlaybackPolicy(obj, autoplay, force_audio_loop);
    WPSoundStream::Config config { .maxtime = obj.maxtime,
                                   .mintime = obj.mintime,
                                   .volume  = 1.0f,
                                   .mode    = policy.mode };

    auto ss = std::make_unique<WPSoundStream>(obj.sound, vfs, config);
    const auto handle = sm.MountStream(std::move(ss), obj.volume, policy.autoplay);
    LOG_VERBOSE("SceneSoundMount: layer=%d name='%s' handle=%u sounds=%zu volume=%.3f visible=%s "
                "startsilent=%s autoplay=%s playbackmode='%s' effective-playbackmode='%s'",
                obj.id,
                obj.name.c_str(),
                handle,
                obj.sound.size(),
                obj.volume,
                obj.visible ? "true" : "false",
                obj.startsilent ? "true" : "false",
                policy.autoplay ? "true" : "false",
                obj.playbackmode.c_str(),
                PlaybackModeName(policy.mode));
    return handle;
}
