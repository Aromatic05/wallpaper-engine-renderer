#include "common/fs/include/fs/IBinaryStream.h"
#include "common/fs/include/fs/LimitedBinaryStream.h"
#include "common/fs/include/fs/MemBinaryStream.h"
#include "common/fs/include/fs/VFS.h"
#include "common/fs/include/fs/Fs.h"
#include "host/audio/include/audio/SoundManager.h"
#include "backend/scene/internal/parser/WPTexImageParser.hpp"
#include "backend/scene/internal/scene/include/scene/SceneVertexArray.h"
#include "backend/scene/internal/scene/include/scene/SceneIndexArray.h"
#include "render/vulkan/include/vulkan/Device.hpp"
#include "render/vulkan/include/vulkan/Instance.hpp"
#include "render/vulkanrender/CopyPass.hpp"
#include "render/vulkanrender/PassCommon.hpp"
#include "render/vulkanrender/Resource.hpp"

#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <array>
#include <memory>
#include <string>
#include <type_traits>
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
using wallpaper::TextureFormat;
namespace vk = wallpaper::vulkan;

[[noreturn]] void FailTest(const char* message) {
    std::fprintf(stderr, "resource-correctness-test: %s\n", message);
    std::abort();
}

void RequireTest(bool condition, const char* message) {
    if (! condition) FailTest(message);
}

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

class FailingSeekStream final : public IBinaryStream {
public:
    explicit FailingSeekStream(std::vector<uint8_t> data): m_data(std::move(data)) {}

    usize Read(void* buffer, usize sizeInByte) override {
        if (m_pos < 0 || m_pos >= Size() || sizeInByte == 0) return 0;
        const usize remaining = static_cast<usize>(Size() - m_pos);
        const usize read      = std::min(sizeInByte, remaining);
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
        m_seek_attempts.push_back(offset);
        if (m_fail_seek) return false;
        if (offset < 0 || offset > Size()) return false;
        m_pos = offset;
        return true;
    }

    bool SeekCur(idx offset) override { return SeekSet(m_pos + offset); }
    bool SeekEnd(idx offset) override { return SeekSet(Size() + offset); }
    isize Size() const override { return static_cast<isize>(m_data.size()); }

    void SetFailSeek(bool fail_seek) { m_fail_seek = fail_seek; }
    const std::vector<idx>& SeekAttempts() const { return m_seek_attempts; }

protected:
    usize Write_impl(const void*, usize) override { return 0; }

private:
    idx                  m_pos { 0 };
    bool                 m_fail_seek { false };
    std::vector<uint8_t> m_data;
    std::vector<idx>     m_seek_attempts;
};

class TrackingStream final : public IBinaryStream {
public:
    enum class OpKind {
        Read,
        SeekSet,
        SeekCur,
        SeekEnd,
    };

    struct Op {
        OpKind kind;
        idx    before;
        idx    after;
        idx    offset;
        usize  requested;
        usize  actual;
    };

    explicit TrackingStream(std::shared_ptr<IBinaryStream> inner): m_inner(std::move(inner)) {}

    usize Read(void* buffer, usize sizeInByte) override {
        const idx   before = m_inner->Tell();
        const usize actual = m_inner->Read(buffer, sizeInByte);
        m_ops.push_back(
            { OpKind::Read, before, m_inner->Tell(), 0, sizeInByte, actual });
        return actual;
    }

    char* Gets(char* buffer, usize sizeStr) override {
        Read(buffer, sizeStr);
        return buffer;
    }

    idx Tell() const override { return m_inner->Tell(); }

    bool SeekSet(idx offset) override {
        const idx before = m_inner->Tell();
        const bool ok    = m_inner->SeekSet(offset);
        m_ops.push_back({ OpKind::SeekSet, before, m_inner->Tell(), offset, 0, 0 });
        return ok;
    }

    bool SeekCur(idx offset) override {
        const idx before = m_inner->Tell();
        const bool ok    = m_inner->SeekCur(offset);
        m_ops.push_back({ OpKind::SeekCur, before, m_inner->Tell(), offset, 0, 0 });
        return ok;
    }

    bool SeekEnd(idx offset) override {
        const idx before = m_inner->Tell();
        const bool ok    = m_inner->SeekEnd(offset);
        m_ops.push_back({ OpKind::SeekEnd, before, m_inner->Tell(), offset, 0, 0 });
        return ok;
    }

    isize Size() const override { return m_inner->Size(); }

    const std::vector<Op>& Ops() const { return m_ops; }

protected:
    usize Write_impl(const void*, usize) override { return 0; }

private:
    std::shared_ptr<IBinaryStream> m_inner;
    std::vector<Op>                m_ops;
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

std::vector<uint8_t> DecodeBase64(std::string_view input) {
    auto decodeChar = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    std::vector<uint8_t> output;
    int                  bits  = 0;
    int                  value = 0;
    for (const char c : input) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        if (c == '=') break;

        const int decoded = decodeChar(c);
        assert(decoded >= 0);
        value = (value << 6) | decoded;
        bits += 6;
        if (bits < 8) continue;

        bits -= 8;
        output.push_back(static_cast<uint8_t>((value >> bits) & 0xFF));
    }
    return output;
}

template<typename T>
void AppendLE(std::vector<uint8_t>& bytes, T value) {
    for (usize i = 0; i < sizeof(T); ++i) {
        bytes.push_back(static_cast<uint8_t>((static_cast<std::make_unsigned_t<T>>(value) >>
                                              (8 * i)) &
                                             0xFF));
    }
}

void AppendTexVersion(std::vector<uint8_t>& bytes, const char prefix, int version) {
    bytes.push_back('T');
    bytes.push_back('E');
    bytes.push_back('X');
    bytes.push_back(static_cast<uint8_t>(prefix));
    bytes.push_back(static_cast<uint8_t>('0' + ((version / 1000) % 10)));
    bytes.push_back(static_cast<uint8_t>('0' + ((version / 100) % 10)));
    bytes.push_back(static_cast<uint8_t>('0' + ((version / 10) % 10)));
    bytes.push_back(static_cast<uint8_t>('0' + (version % 10)));
    bytes.push_back('\0');
}

std::string BuildMinimalTexAsset(std::array<uint8_t, 4> rgba) {
    std::vector<uint8_t> bytes;
    AppendTexVersion(bytes, 'V', 1);
    AppendTexVersion(bytes, 'I', 1);
    AppendLE<int32_t>(bytes, 0); // RGBA8
    AppendLE<uint32_t>(bytes, 0); // flags
    AppendLE<int32_t>(bytes, 1);  // width
    AppendLE<int32_t>(bytes, 1);  // height
    AppendLE<int32_t>(bytes, 1);  // map width
    AppendLE<int32_t>(bytes, 1);  // map height
    AppendLE<int32_t>(bytes, 0);  // unknown
    AppendTexVersion(bytes, 'B', 1);
    AppendLE<int32_t>(bytes, 1); // image count
    AppendLE<int32_t>(bytes, 1); // mipmap count
    AppendLE<int32_t>(bytes, 1); // mip width
    AppendLE<int32_t>(bytes, 1); // mip height
    AppendLE<int32_t>(bytes, 4); // src size
    bytes.insert(bytes.end(), rgba.begin(), rgba.end());
    return std::string(bytes.begin(), bytes.end());
}

struct VulkanFixture {
    vk::Instance instance;
    vk::Device   device;

    VulkanFixture() {
        const std::array<vk::Extension, 0>     inst_exts {};
        const std::array<vk::InstanceLayer, 0> inst_layers {};
        const std::array<vk::Extension, 0>     device_exts {};

        RequireTest(vk::Instance::Create(instance, inst_exts, inst_layers),
                    "failed to create Vulkan instance");
        RequireTest(instance.ChoosePhysicalDevice([&](auto gpu) {
                        return vk::Device::CheckGPU(gpu, device_exts, {});
                    }),
                    "failed to choose Vulkan physical device");
        RequireTest(vk::Device::Create(instance, device_exts, VkExtent2D { 1, 1 }, device),
                    "failed to create Vulkan device");
    }
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

    auto failing = std::make_shared<FailingSeekStream>(std::vector<uint8_t> { 10, 11, 12, 13 });
    LimitedBinaryStream guarded(failing, 1, 2);

    assert(guarded.SeekSet(0));
    assert(guarded.Tell() == 0);
    assert(failing->Tell() == 1);

    failing->SetFailSeek(true);
    assert(! guarded.SeekSet(1));
    assert(guarded.Tell() == 0);
    assert(failing->Tell() == 1);

    uint8_t byte { 0 };
    assert(guarded.Read(&byte, 1) == 0);
    assert(byte == 0);
    assert(guarded.Tell() == 0);
    assert(failing->Tell() == 1);

    assert(! guarded.SeekCur(1));
    assert(guarded.Tell() == 0);
    assert(failing->Tell() == 1);

    failing->SetFailSeek(false);
    assert(guarded.SeekSet(1));
    assert(guarded.Tell() == 1);
    assert(failing->Tell() == 2);
    assert(guarded.Read(&byte, 1) == 1);
    assert(byte == 12);
    assert(guarded.Tell() == 2);
    assert(failing->Tell() == 3);
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

void TestVfsTextureCacheIsolation() {
    auto vfs1 = std::make_unique<VFS>();
    auto vfs2 = std::make_unique<VFS>();

    const auto tex_blob = BuildMinimalTexAsset({ 255, 0, 0, 255 });
    vfs1->Mount("/assets",
                std::make_unique<MemoryFs>(std::unordered_map<std::string, std::string> {
                    { "/materials/same.tex", tex_blob },
                }),
                "mem1");
    vfs2->Mount("/assets",
                std::make_unique<MemoryFs>(std::unordered_map<std::string, std::string> {
                    { "/materials/same.tex", tex_blob },
                }),
                "mem2");

    wallpaper::WPTexImageParser parser1(vfs1.get());
    wallpaper::WPTexImageParser parser2(vfs2.get());
    auto                        image1 = parser1.Parse("same");
    auto                        image2 = parser2.Parse("same");

    assert(image1 != nullptr);
    assert(image2 != nullptr);
    assert(image1->header.format == TextureFormat::RGBA8);
    assert(image2->header.format == TextureFormat::RGBA8);

    VulkanFixture vk;
    auto          tex1 = vk.device.tex_cache().CreateTex(*image1);
    auto          tex2 = vk.device.tex_cache().CreateTex(*image2);

    assert(! tex1.slots.empty());
    assert(! tex2.slots.empty());
    assert(tex1.slots.front().handle);
    assert(tex2.slots.front().handle);
    assert(tex1.slots.front().view);
    assert(tex2.slots.front().view);
}


void TestCopyPassKeepsPreparedSourceResidentUntilExecution() {
    VulkanFixture vk;
    wallpaper::Scene scene;

    constexpr std::string_view source_key = "_rt_copy_source";
    constexpr std::string_view target_key = "_rt_copy_target";
    constexpr std::string_view interloper_key = "_rt_copy_interloper";

    wallpaper::SceneRenderTarget source_target {
        .width = 4,
        .height = 4,
        .allowReuse = true,
    };
    scene.renderTargets[std::string(source_key)] = source_target;
    scene.renderTargets[std::string(target_key)] = source_target;

    vk::CopyPass::Desc desc {
        .src = std::string(source_key),
        .dst = std::string(target_key),
    };
    vk::CopyPass copy(desc);
    copy.addReleaseTexs(std::array<std::string_view, 1> { source_key });

    vk::RenderingResources resources {};
    copy.prepare(scene, vk.device, resources);
    RequireTest(copy.prepared(), "copy pass did not prepare");
    RequireTest(copy.desc().vk_src.handle != VK_NULL_HANDLE, "copy source image is missing");

    const auto texture_key = vk::ToTexKey(scene.renderTargets.at(std::string(source_key)));
    auto& cache = vk.device.tex_cache();

    auto interloper = cache.Query(interloper_key, texture_key);
    RequireTest(interloper.has_value(), "failed to allocate interloper image");
    RequireTest(interloper->handle != copy.desc().vk_src.handle,
                "copy source was released during prepare and reused by an interloper");

    auto producer = cache.Query(source_key, texture_key);
    RequireTest(producer.has_value(), "failed to query producer image");
    RequireTest(producer->handle == copy.desc().vk_src.handle,
                "producer and prepared copy pass bound different source images");
    RequireTest(producer->view == copy.desc().vk_src.view,
                "producer and prepared copy pass bound different source views");
}

void TestTextureCacheDeferredGraphActivation() {
    VulkanFixture vk;
    auto&         cache = vk.device.tex_cache();

    const vk::TextureKey key {
        .width = 1,
        .height = 1,
        .usage = vk::TexUsage::COLOR,
        .format = TextureFormat::RGBA8,
        .sample = {},
        .mipmap_level = 1,
    };

    auto first = cache.Query("rt/first", key);
    assert(first.has_value());

    cache.BeginDeferredGraphActivation();
    cache.MarkShareReady("rt/first");

    auto second = cache.Query("rt/second", key);
    assert(second.has_value());
    assert(second->handle != first->handle);
    assert(second->view != first->view);

    cache.EndDeferredGraphActivation();

    auto third = cache.Query("rt/third", key);
    assert(third.has_value());
    assert(third->handle == first->handle);
    assert(third->view == first->view);

    const vk::TextureKey cancel_key {
        .width = 2,
        .height = 1,
        .usage = vk::TexUsage::COLOR,
        .format = TextureFormat::RGBA8,
        .sample = {},
        .mipmap_level = 1,
    };

    auto cancel_first = cache.Query("rt/cancel-first", cancel_key);
    assert(cancel_first.has_value());

    cache.BeginDeferredGraphActivation();
    cache.MarkShareReady("rt/cancel-first");
    cache.CancelDeferredGraphActivation();

    auto cancel_second = cache.Query("rt/cancel-second", cancel_key);
    assert(cancel_second.has_value());
    assert(cancel_second->handle != cancel_first->handle);
    assert(cancel_second->view != cancel_first->view);
}

void TestDecoderFailureHandling() {
    const SoundStream::Desc desc { .channels = 2, .sampleRate = 44100 };
    assert(CreateSoundStream(nullptr, desc) == nullptr);

    auto empty = std::make_shared<MemBinaryStream>(std::vector<uint8_t> {});
    assert(CreateSoundStream(empty, desc) == nullptr);
}

void TestDecoderProbeAndRewind() {
    constexpr std::string_view tinyVorbisBase64 =
        "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"
        "dVeWdV22bd2XNM00NVFUVU0UVddUVds2VdW2NVF0XVFVZVlUVVl2XVnXVVfWfU0UVdVTTdkVVVWWVdnVZVWWdV90Vd1WXdnXVVnW"
        "fdvWhV/WfcKoqrpuyq6uq7Ks+7Iu+7rt65RJ00xTE0VV1URRVU1XtW1TdW1bE0XXFVXVlkVTdWVVln1fdWXZ10TRdUVVlWVRVWVZ"
        "lWVdd2VXt0VV1W1Vdn3fdF1dl3VdWGZb94XTdXVdlWXfV2VZ92Vdx9Z13/dM07ZN19V101V139Z15Zlt2/hFVdV1VZaFX5Vl39eF"
        "4Xlu3ReeUVV13ZRdX1dlWRduXzfavm48r21j2z6yryMMR76wLF3bNrq+TZh13egbQ+E3hjTTtG3TVXXddF1fl3XdaOu6UFRVXVdl"
        "2fdVV/Z9W/eF4fZ93xhV1/dVWRaG1ZadYfd9pe4LlVW2hd/WdeeYbV1YfuPo/L4ydHVbaOu6scy+rjy7cXSGPgIAAAYcAAACTCgD"
        "hYasCADiBAAYhJxDTEGIFIMQQkgphJBSxBiEzDkpGXNSQimphVJSixiDkDkmJXNOSiihpVBKS6GE1kIpsYVSWmyt1ZpaizWE0loo"
        "pbVQSouppRpbazVGjEHInJOSOSellNJaKKW1zDkqnYOUOggppZRaLCnFWDknJYOOSgchpZJKTCWlGEMqsZWUYiwpxdhabLnFmHMo"
        "pcWSSmwlpVhbTDm2GHOOGIOQOSclc05KKKW1UlJrlXNSOggpZQ5KKinFWEpKMXNOSgchpQ5CSiWlGFNKsYVSYisp1VhKarHFmHNL"
        "MdZQUoslpRhLSjG2GHNuseXWQWgtpBJjKCXGFmOurbUaQymxlZRiLCnVFmOtvcWYcyglxpJKjSWlWFuNucYYc06x5ZparLnF2Gtt"
        "ufWac9CptVpTTLm2GHOOuQVZc+69g9BaKKXFUEqMrbVaW4w5h1JiKynVWEqKtcWYc2ux9lBKjCWlWEtKNbYYa4419ppaq7XFmGtq"
        "seaac+8x5thTazW3GGtOseVac+695tZjAQAAAw4AAAEmlIFCQ1YCAFEAAAQhSjEGoUGIMeekNAgx5pyUijHnIKRSMeYchFIy5yCU"
        "klLmHIRSUgqlpJJSa6GUUlJqrQAAgAIHAIAAGzQlFgcoNGQlAJAKAGBwHMvyPFE0Vdl2LMnzRNE0VdW2HcvyPFE0TVW1bcvzRNE0"
        "VdV1dd3yPFE0VVV1XV33RFE1VdV1ZVn3PVE0VVV1XVn2fdNUVdV1ZVm2hV80VVd1XVmWZd9YXdV1ZVm2dVsYVtV1XVmWbVs3hlvX"
        "dd33hWE5Ordu67rv+8LxO8cAAPAEBwCgAhtWRzgpGgssNGQlAJABAEAYg5BBSCGDEFJIIaUQUkoJAAAYcAAACDChDBQashIAiAIA"
        "AAiRUkopjZRSSimlkVJKKaWUEkIIIYQQQgghhBBCCCGEEEIIIYQQQgghhBBCCCGEEEIIBQD4TzgA+D/YoCmxOEChISsBgHAAAMAY"
        "pZhyDDoJKTWMOQahlJRSaq1hjDEIpaTUWkuVcxBKSam12GKsnINQUkqtxRpjByGl1lqssdaaOwgppRZrrDnYHEppLcZYc86995BS"
        "azHWWnPvvZfWYqw159yDEMK0FGOuufbge+8ptlprzT34IIRQsdVac/BBCCGEizH33IPwPQghXIw55x6E8MEHYQAAd4MDAESCjTOs"
        "JJ0VjgYXGrISAAgJACAQYoox55yDEEIIkVKMOecchBBCKCVSijHnnIMOQgglZIw55xyEEEIopZSMMeecgxBCCaWUkjnnHIQQQiil"
        "lFIy56CDEEIJpZRSSucchBBCCKWUUkrpoIMQQgmllFJKKSGEEEIJpZRSSiklhBBCCaWUUkoppYQQSiillFJKKaWUEEIppZRSSiml"
        "lBJCKKWUUkoppZSSQimllFJKKaWUUlIopZRSSimllFJKCaWUUkoppZSUUkkFAAAcOAAABBhBJxlVFmGjCRcegEJDVgIAQAAAFMRW"
        "U4mdQcwxZ6khCDGoqUJKKYYxQ8ogpilTCiGFIXOKIQKhxVZLxQAAABAEAAgICQAwQFAwAwAMDhA+B0EnQHC0AQAIQmSGSDQsBIcH"
        "lQARMRUAJCYo5AJAhcVF2sUFdBnggi7uOhBCEIIQxOIACkjAwQk3PPGGJ9zgBJ2iUgcBAAAAAHAAAA8AAMcFEBHRHEaGxgZHh8cH"
        "SEgAAAAAAMgAwAcAwCECREQ0h5GhscHR4fEBEhIAAAAAAAAAAAAEBAQAAAAAAAIAAAAEBE9nZ1MABHIDAAAAAAAAairP2AIAAADr"
        "7KLRAx5EddReKOewyiUahvNTQAABgHHsxcvruq7ruq7ruiIUADq4ncrD7/AQd7xJW7hM5bDqyPLmZkxOjoa+zhJAEAAAAAAAAAAA"
        "kOdN1rvTdbbh9sg2VpIkWarH6V7fBCtWHMt2lrkA/pWs7kOE/3Ur3pwNHGPnGLlzj9yA6GEOICojKAEAAAAAoM2aZJ/DNJXkn4e+"
        "v6vNqUdtzV22Wlkio5rq94W1hOX3hbWE5Pf5dJLf59NJ/lh66bOvVr5MtK9W5iKkpjKX2Lr21UukdU31srDmJL/P6pTvDccE";

    auto tracking = std::make_shared<TrackingStream>(
        std::make_shared<MemBinaryStream>(DecodeBase64(tinyVorbisBase64)));
    auto limited = std::make_shared<LimitedBinaryStream>(tracking, 0, tracking->Size());

    const SoundStream::Desc desc { .channels = 2, .sampleRate = 44100 };
    auto                    stream = CreateSoundStream(limited, desc);
    assert(stream != nullptr);

    bool sawProbeRead   = false;
    bool sawRewind      = false;
    bool sawRestartRead = false;
    for (const auto& op : tracking->Ops()) {
        if (! sawProbeRead && op.kind == TrackingStream::OpKind::Read && op.before == 0 &&
            op.actual == static_cast<usize>(limited->Size())) {
            sawProbeRead = true;
            continue;
        }
        if (sawProbeRead && ! sawRewind && op.kind == TrackingStream::OpKind::SeekSet &&
            op.offset == 0 && op.after == 0) {
            sawRewind = true;
            continue;
        }
        if (sawRewind && op.kind == TrackingStream::OpKind::Read && op.before == 0 &&
            op.actual > 0) {
            sawRestartRead = true;
            break;
        }
    }

    assert(sawProbeRead);
    assert(sawRewind);
    assert(sawRestartRead);
}
void TestSceneResourceIdsStartUnassigned() {
    std::vector<wallpaper::SceneVertexArray::SceneVertexAttribute> attrs {
        { .name = "a_Position", .type = wallpaper::VertexType::FLOAT3 },
    };
    wallpaper::SceneVertexArray vertices(attrs, 1);
    RequireTest(vertices.ID() == std::numeric_limits<uint32_t>::max(),
                "scene vertex array id must start unassigned");
    wallpaper::SceneIndexArray indices(1);
    RequireTest(indices.ID() == std::numeric_limits<uint32_t>::max(),
                "scene index array id must start unassigned");
}

void TestSceneVertexArrayAppendAndMove() {
    std::vector<wallpaper::SceneVertexArray::SceneVertexAttribute> attrs {
        { .name = "a_Position", .type = wallpaper::VertexType::FLOAT3 },
        { .name = "a_TexCoord", .type = wallpaper::VertexType::FLOAT2 },
    };
    wallpaper::SceneVertexArray vertices(attrs, 2);
    vertices.SetOption("dynamic", true);
    vertices.SetFloatOption("scale", 2.0f);

    const std::array<float, 5> first { 1.0f, 2.0f, 3.0f, 0.25f, 0.5f };
    const std::array<float, 5> second { 4.0f, 5.0f, 6.0f, 0.75f, 1.0f };
    RequireTest(vertices.AddVertex(first.data()), "first vertex append failed");
    RequireTest(vertices.AddVertex(second.data()), "last-capacity vertex append failed");

    wallpaper::SceneVertexArray moved(std::move(vertices));
    RequireTest(moved.VertexCount() == 2, "move construction lost vertices");
    RequireTest(moved.GetOption("dynamic"), "move construction lost bool options");
    RequireTest(moved.GetFloatOption("scale") == 2.0f,
                "move construction lost float options");
    const auto offsets = moved.GetAttrOffsetMap();
    const auto position = offsets.at("a_Position").offset / sizeof(float);
    const auto uv = offsets.at("a_TexCoord").offset / sizeof(float);
    RequireTest(moved.Data()[position] == 1.0f, "first vertex position was overwritten");
    RequireTest(moved.Data()[position + moved.OneSize()] == 4.0f,
                "second vertex position was not appended");
    RequireTest(moved.Data()[uv] == 0.25f, "first vertex UV was overwritten");
    RequireTest(moved.Data()[uv + moved.OneSize()] == 0.75f,
                "second vertex UV was not appended");

    wallpaper::SceneVertexArray assigned(attrs, 1);
    assigned.SetOption("stale", true);
    assigned = std::move(moved);
    RequireTest(assigned.VertexCount() == 2, "move assignment lost vertices");
    RequireTest(assigned.GetOption("dynamic") && !assigned.GetOption("stale"),
                "move assignment did not replace bool options");
    RequireTest(assigned.GetFloatOption("scale") == 2.0f,
                "move assignment lost float options");
}

} // namespace

int main() {
    TestLimitedBinaryStream();
    TestVfsIdentityAndCacheIsolation();
    TestVfsTextureCacheIsolation();
    TestCopyPassKeepsPreparedSourceResidentUntilExecution();
    TestTextureCacheDeferredGraphActivation();
    TestDecoderFailureHandling();
    TestDecoderProbeAndRewind();
    TestSceneResourceIdsStartUnassigned();
    TestSceneVertexArrayAppendAndMove();
    return 0;
}
