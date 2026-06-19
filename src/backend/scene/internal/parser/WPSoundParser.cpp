#include "WPSoundParser.hpp"
#include "audio/SoundManager.h"
#include "fs/VFS.h"
#include "wpscene/WPSoundObject.h"
#include "utils/Logging.h"
#include "core/Random.hpp"

#include <limits>
#include <string>
#include <string_view>

using namespace wallpaper;

enum class PlaybackMode
{
    Single,
    Random,
    Loop
};

static PlaybackMode ToPlaybackMode(std::string_view s) {
    if (s == "single")
        return PlaybackMode::Single;
    else if (s == "loop")
        return PlaybackMode::Loop;
    else if (s == "random")
        return PlaybackMode::Random;
    return PlaybackMode::Loop;
};

class WPSoundStream : public audio::SoundStream {
public:
    struct Config {
        float        maxtime { 10.0f };
        float        mintime { 0.0f };
        float        volume { 1.0f };
        PlaybackMode mode { PlaybackMode::Loop };
    };
    WPSoundStream(const std::vector<std::string>& paths, fs::VFS& vfs, Config c)
        : vfs(vfs), m_config(c), m_soundPaths(paths) {};
    virtual ~WPSoundStream() = default;

    uint64_t NextPcmData(void* pData, uint32_t frameCount) override {
        if (m_soundPaths.empty()) return 0;

        // first
        if (! m_curActive) {
            Switch();
        }
        if (! m_curActive) return 0;

        uint64_t frameReads = m_curActive->NextPcmData(pData, frameCount);
        if (frameReads == 0) {
            if (m_config.mode == PlaybackMode::Single) {
                m_curActive.reset();
                return 0;
            }
            Switch();
            if (! m_curActive) return 0;
            frameReads = m_curActive->NextPcmData(pData, frameCount);
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
        }
    }
    uint32_t LoopIndex() {
        if (m_config.mode == PlaybackMode::Random) {
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

private:
    fs::VFS& vfs;
    Config   m_config;
    Desc     m_desc;
    uint32_t m_curIndex { std::numeric_limits<uint32_t>::max() };

    const std::vector<std::string> m_soundPaths;
    std::unique_ptr<SoundStream>   m_curActive;
};

audio::SoundHandle WPSoundParser::Parse(const wpscene::WPSoundObject& obj, fs::VFS& vfs,
                                        audio::SoundManager& sm) {
    WPSoundStream::Config config { .maxtime = obj.maxtime,
                                   .mintime = obj.mintime,
                                   .volume  = 1.0f,
                                   .mode    = ToPlaybackMode(obj.playbackmode) };

    auto ss = std::make_unique<WPSoundStream>(obj.sound, vfs, config);
    const bool autoplay = obj.visible && ! obj.startsilent;
    const auto handle = sm.MountStream(std::move(ss), obj.volume, autoplay);
    LOG_INFO("SceneSoundMount: layer=%d name='%s' handle=%u sounds=%zu volume=%.3f visible=%s "
             "startsilent=%s autoplay=%s playbackmode='%s'",
             obj.id,
             obj.name.c_str(),
             handle,
             obj.sound.size(),
             obj.volume,
             obj.visible ? "true" : "false",
             obj.startsilent ? "true" : "false",
             autoplay ? "true" : "false",
             obj.playbackmode.c_str());
    return handle;
}
