#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace wallpaper
{
class OwnedFileDescriptor final {
public:
    OwnedFileDescriptor() = default;
    explicit OwnedFileDescriptor(int descriptor);
    ~OwnedFileDescriptor();

    OwnedFileDescriptor(const OwnedFileDescriptor&) = delete;
    OwnedFileDescriptor& operator=(const OwnedFileDescriptor&) = delete;
    OwnedFileDescriptor(OwnedFileDescriptor&& other) noexcept;
    OwnedFileDescriptor& operator=(OwnedFileDescriptor&& other) noexcept;

    int  get() const { return m_descriptor; }
    bool valid() const { return m_descriptor >= 0; }
    int  release();
    void reset(int descriptor = -1);

private:
    int m_descriptor { -1 };
};

enum class TextureExportKind
{
    DmaBuf,
    SharedMemory,
};

enum class TexturePixelFormat
{
    Rgba8Unorm,
    Bgra8Unorm,
};

enum class TextureStorageLayout
{
    DrmModifier,
    LinearRows,
};

enum class TextureOwnership
{
    OwnedFileDescriptors,
};

enum class TextureSyncMode
{
    // Producers publish a swapchain slot only after rendering or host-side frame delivery has
    // completed. No explicit fence or semaphore is currently exported with the frame.
    Implicit,
};

struct TextureExtent {
    std::uint32_t width { 0 };
    std::uint32_t height { 0 };
};

struct TexturePlane {
    OwnedFileDescriptor descriptor;
    std::uint32_t       offset { 0 };
    std::uint32_t       stride { 0 };
};

struct TextureFrame final {
    static constexpr std::size_t MaxPlanes { 4 };
    static constexpr std::uint64_t InvalidDrmModifier {
        std::numeric_limits<std::uint64_t>::max()
    };

    TextureExportKind    exportKind { TextureExportKind::SharedMemory };
    TexturePixelFormat   format { TexturePixelFormat::Bgra8Unorm };
    TextureStorageLayout storageLayout { TextureStorageLayout::LinearRows };
    TextureOwnership     ownership { TextureOwnership::OwnedFileDescriptors };
    TextureSyncMode      acquireSync { TextureSyncMode::Implicit };
    TextureSyncMode      releaseSync { TextureSyncMode::Implicit };
    TextureExtent        extent;
    std::uint32_t        drmFourcc { 0 };
    std::uint64_t        drmModifier { InvalidDrmModifier };
    std::uint32_t        planeCount { 0 };
    std::uint32_t        bufferId { std::numeric_limits<std::uint32_t>::max() };
    std::uint64_t        shmSize { 0 };
    bool                 premultiplied { false };
    bool                 descriptorsOmitted { false };
    std::uint64_t        revision { 0 };
    std::array<TexturePlane, MaxPlanes> planes {};

    TextureFrame() = default;
    TextureFrame(const TextureFrame&) = delete;
    TextureFrame& operator=(const TextureFrame&) = delete;
    TextureFrame(TextureFrame&&) noexcept = default;
    TextureFrame& operator=(TextureFrame&&) noexcept = default;

    bool valid() const;
};
} // namespace wallpaper
