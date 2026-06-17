#include "audio/SoundManager.h"
#include "miniaudio-wrapper.hpp"
#include "fs/IBinaryStream.h"
#include "core/Literals.hpp"
#include "utils/Logging.h"

using namespace wallpaper;
using namespace wallpaper::audio;

namespace
{
SoundStream::Desc ToSSDesc(const miniaudio::DeviceDesc& d) {
    return { .channels = d.phyChannels, .sampleRate = d.sampleRate };
}

miniaudio::DeviceDesc ToSSDesc(const SoundStream::Desc& d) {
    return { .phyChannels = d.channels, .sampleRate = d.sampleRate };
}
} // namespace

class Channel_Impl : public miniaudio::Channel {
public:
    explicit Channel_Impl(std::unique_ptr<SoundStream>&& ss): m_ss(std::move(ss)) {}
    virtual ~Channel_Impl() = default;

    ma_uint64 NextPcmData(void* pData, ma_uint32 frameCount) override {
        return m_ss ? m_ss->NextPcmData(pData, frameCount) : 0;
    }

    void PassDeviceDesc(const miniaudio::DeviceDesc& desc) override {
        if (m_ss) m_ss->PassDesc(ToSSDesc(desc));
    }

private:
    std::unique_ptr<SoundStream> m_ss;
};

struct BStreamWrapper {
    std::shared_ptr<wallpaper::fs::IBinaryStream> stream;

    size_t Read(void* pBufferOut, size_t bytesToRead) {
        if (! stream) return 0;
        return stream->Read(pBufferOut, bytesToRead);
    }

    bool Seek(idx offset, ma_seek_origin origin) {
        if (! stream) return false;

        switch (origin) {
        case ma_seek_origin_start: return stream->SeekSet(offset);
        case ma_seek_origin_current: return stream->SeekCur(offset);
        case ma_seek_origin_end: return stream->SeekEnd(offset);
        }
        return false;
    }
};

template<typename T>
class SoundStream_impl : public SoundStream {
public:
    explicit SoundStream_impl(std::unique_ptr<T>&& ss): m_ss(std::move(ss)) {}
    virtual ~SoundStream_impl() = default;

    uint64_t NextPcmData(void* pData, uint32_t frameCount) override {
        return m_ss->NextPcmData(pData, frameCount);
    }

    void PassDesc(const Desc&) override {}

private:
    std::unique_ptr<T> m_ss;
};

std::unique_ptr<SoundStream>
wallpaper::audio::CreateSoundStream(std::shared_ptr<wallpaper::fs::IBinaryStream> stream,
                                    const SoundStream::Desc&                      desc) {
    if (! stream) {
        LOG_ERROR("CreateSoundStream: stream is null");
        return nullptr;
    }
    if (stream->Size() <= 0) {
        LOG_ERROR("CreateSoundStream: stream is empty");
        return nullptr;
    }

    BStreamWrapper sw { stream };
    auto           decoder = std::make_unique<miniaudio::Decoder<BStreamWrapper>>(std::move(sw));
    if (! decoder->Init(ToSSDesc(desc))) {
        return nullptr;
    }

    return std::make_unique<SoundStream_impl<miniaudio::Decoder<BStreamWrapper>>>(
        std::move(decoder));
}

class SoundManager::impl : NoCopy, NoMove {
public:
    impl() = default;
    ~impl() = default;

    miniaudio::Device device {};
};

SoundManager::SoundManager(): pImpl(std::make_unique<impl>()) {}
SoundManager::~SoundManager() {}

void SoundManager::MountStream(std::unique_ptr<SoundStream>&& ss) {
    if (! ss) {
        LOG_ERROR("SoundManager::MountStream failed: null sound stream");
        return;
    }
    pImpl->device.MountChannel(std::make_shared<Channel_Impl>(std::move(ss)));
}

void SoundManager::Test(std::shared_ptr<fs::IBinaryStream> stream) {
    BStreamWrapper sw { stream };
    auto           decoder = std::make_unique<miniaudio::Decoder<BStreamWrapper>>(std::move(sw));
}

bool SoundManager::Init() {
    if (Muted()) {
        LOG_INFO("muted, not init sound device");
        return false;
    }
    return pImpl->device.Init({});
}

bool SoundManager::IsInited() const { return pImpl->device.IsInited(); }
void SoundManager::Play() { pImpl->device.Start(); }
void SoundManager::Pause() { pImpl->device.Stop(); }

void SoundManager::UnMountAll() { pImpl->device.UnmountAll(); }
float SoundManager::Volume() const { return pImpl->device.Volume(); }
bool SoundManager::Muted() const { return pImpl->device.Muted(); }

void SoundManager::SetMuted(bool v) {
    pImpl->device.SetMuted(v);
    if (! Muted() && ! pImpl->device.IsInited()) {
        Init();
    }
}

void SoundManager::SetVolume(float v) { pImpl->device.SetVolume(v); }
