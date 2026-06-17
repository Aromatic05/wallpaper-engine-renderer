#include "common/fs/include/fs/IBinaryStream.h"
#include "common/fs/include/fs/LimitedBinaryStream.h"
#include "common/fs/include/fs/MemBinaryStream.h"
#include "common/fs/include/fs/VFS.h"
#include "common/fs/include/fs/Fs.h"
#include "host/audio/include/audio/SoundManager.h"

#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
using wallpaper::fs::Fs;
using wallpaper::fs::IBinaryStream;
using wallpaper::fs::LimitedBinaryStream;
using wallpaper::fs::MemBinaryStream;
using wallpaper::fs::VFS;
using wallpaper::idx;
using wallpaper::isize;
using wallpaper::usize;
using wallpaper::audio::CreateSoundStream;
using wallpaper::audio::SoundStream;

class ShortReadStream final : public IBinaryStream {
public:
    explicit ShortReadStream(std::vector<uint8_t> data): m_data(std::move(data)) {}

    usize Read(void* buffer, usize sizeInByte) override {
        if (sizeInByte == 0 || m_pos >= static_cast<idx>(m_data.size())) return 0;

        const usize remaining = static_cast<usize>(m_data.size() - static_cast<size_t>(m_pos));
        const usize read      = std::min<usize>(remaining, std::min<usize>(sizeInByte, 2));
        std::memcpy(buffer, m_data.data() + m_pos, read);
        m_pos += static_cast<idx>(read);
        return read;
    }

    char* Gets(char* buffer, usize sizeStr) override {
        Read(buffer, sizeStr);
        return buffer;
    }

    idx Tell() const override { return m_pos; }

    bool SeekSet(idx offset) override {
        if (offset < 0 || offset > Size()) return false;
        m_pos = offset;
        return true;
    }

    bool SeekCur(idx offset) override { return SeekSet(m_pos + offset); }
    bool SeekEnd(idx offset) override { return SeekSet(Size() + offset); }
    isize Size() const override { return static_cast<isize>(m_data.size()); }

protected:
    usize Write_impl(const void*, usize) override { return 0; }

private:
    idx                  m_pos { 0 };
    std::vector<uint8_t> m_data;
};

class MemoryFs final : public Fs {
public:
    explicit MemoryFs(std::unordered_map<std::string, std::string> files)
        : m_files(std::move(files)) {}

    std::shared_ptr<IBinaryStream> Open(std::string_view path) override {
        auto it = m_files.find(std::string(path));
        if (it == m_files.end()) return nullptr;
        return std::make_shared<MemBinaryStream>(
            std::vector<uint8_t>(it->second.begin(), it->second.end()));
    }

    std::shared_ptr<wallpaper::fs::IBinaryStreamW> OpenW(std::string_view) override {
        return nullptr;
    }

    bool Contains(std::string_view path) const override {
        return m_files.count(std::string(path)) > 0;
    }

private:
    std::unordered_map<std::string, std::string> m_files;
};

void TestLimitedBinaryStream() {
    auto base = std::make_shared<ShortReadStream>(std::vector<uint8_t> { 1, 2, 3, 4, 5, 6 });
    LimitedBinaryStream limited(base, 1, 4);

    assert(limited.SeekSet(0));
    assert(limited.Tell() == 0);
    assert(limited.SeekSet(limited.Size()));
    assert(limited.Tell() == limited.Size());
    assert(limited.SeekEnd(0));
    assert(limited.Tell() == limited.Size());
    assert(limited.SeekEnd(-2));
    assert(limited.Tell() == limited.Size() - 2);
    assert(! limited.SeekSet(-1));
    assert(! limited.SeekSet(limited.Size() + 1));
    assert(! limited.SeekEnd(1));

    uint8_t bytes[8] {};
    assert(limited.SeekSet(0));
    const usize short_read = limited.Read(bytes, 4);
    assert(short_read == 2);
    assert(limited.Tell() == 2);

    assert(limited.SeekEnd(0));
    assert(limited.Read(bytes, sizeof(bytes)) == 0);
    assert(limited.Tell() == limited.Size());

    assert(limited.SeekEnd(0));
    assert(limited.Rewind());
    assert(limited.Tell() == 0);
}

void TestVfsIdentityAndCacheIsolation() {
    auto vfs1 = std::make_unique<VFS>();
    auto vfs2 = std::make_unique<VFS>();
    assert(vfs1->Identity() != vfs2->Identity());

    vfs1->Mount("/assets",
                std::make_unique<MemoryFs>(
                    std::unordered_map<std::string, std::string> { { "/same.txt", "first" } }),
                "mem1");
    vfs2->Mount("/assets",
                std::make_unique<MemoryFs>(
                    std::unordered_map<std::string, std::string> { { "/same.txt", "second" } }),
                "mem2");

    const std::string path = "/assets/same.txt";
    auto file1 = vfs1->Open(path);
    auto file2 = vfs2->Open(path);
    assert(file1 && file2);
    assert(file1->ReadAllStr() == "first");
    assert(file2->ReadAllStr() == "second");

    const std::string key1 = vfs1->ScopedResourceKey(path);
    const std::string key2 = vfs2->ScopedResourceKey(path);
    assert(key1 != key2);

    std::unordered_map<std::string, std::string> cache;
    cache.emplace(key1, "first");
    cache.emplace(key2, "second");
    vfs1.reset();
    assert(cache.at(key2) == "second");
}

void TestDecoderFailureHandling() {
    const SoundStream::Desc desc { .channels = 2, .sampleRate = 44100 };
    assert(CreateSoundStream(nullptr, desc) == nullptr);

    auto empty = std::make_shared<MemBinaryStream>(std::vector<uint8_t> {});
    assert(CreateSoundStream(empty, desc) == nullptr);
}
} // namespace

int main() {
    TestLimitedBinaryStream();
    TestVfsIdentityAndCacheIsolation();
    TestDecoderFailureHandling();
    return 0;
}
