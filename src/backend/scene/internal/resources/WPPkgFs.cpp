#include "WPPkgFs.hpp"
#include "WPPkgIndex.hpp"
#include "utils/Logging.h"
#include "fs/LimitedBinaryStream.h"
#include "fs/CBinaryStream.h"
#include "utils/Platform.hpp"
#include "utils/Sha.hpp"
#include "WPCommon.hpp"
#include <filesystem>
#include <limits>
#include <span>
#include <vector>

using namespace wallpaper;
using namespace wallpaper::fs;

namespace
{
constexpr std::string_view PKG_CACHE_DIR { "hanabi-scene/pkg-index01" };

struct CachePkgFile {
    std::string path;
    idx         offset { 0 };
    idx         length { 0 };
};

constexpr i32 MAX_CACHE_ENTRY_COUNT = 1'000'000;
constexpr i32 MAX_CACHE_STRING_SIZE = 16 * 1024 * 1024;

bool CanRead(const IBinaryStream& file, idx bytes) {
    if (bytes < 0) return false;
    const auto position = file.Tell();
    const auto size     = file.Size();
    return position >= 0 && size >= position && bytes <= size - position;
}

bool ReadSizedString(IBinaryStream& file, std::string& result) {
    if (! CanRead(file, static_cast<idx>(sizeof(i32)))) return false;
    const auto length = file.ReadInt32();
    if (length < 0 || length > MAX_CACHE_STRING_SIZE) return false;
    if (! CanRead(file, static_cast<idx>(length))) return false;

    result.assign(static_cast<size_t>(length), '\0');
    return length == 0
           || file.Read(result.data(), static_cast<usize>(length)) == static_cast<usize>(length);
}

template<typename T>
bool ReadPod(IBinaryStream& file, T& value) {
    return file.Read(&value, sizeof(value)) == sizeof(value);
}

template<typename T>
void WritePod(IBinaryStreamW& file, const T& value) {
    file.Write(&value, sizeof(value));
}

struct PkgFileStamp {
    uint64_t size { 0 };
    int64_t  mtime { 0 };
};

bool QueryPkgFileStamp(std::string_view pkgpath, PkgFileStamp& stamp) {
    std::error_code         ec;
    const std::filesystem::path path { pkgpath };

    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return false;

    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (ec) return false;

    stamp.size  = static_cast<uint64_t>(size);
    stamp.mtime = static_cast<int64_t>(mtime.time_since_epoch().count());
    return true;
}

std::filesystem::path GetPkgCachePath(std::string_view pkgpath) {
    const auto sha = utils::genSha1(std::span<const char>(pkgpath.data(), pkgpath.size()));
    return platform::GetCachePath(PKG_CACHE_DIR) / (sha + ".wpkg");
}

bool LoadPkgFilesFromCache(std::string_view pkgpath, const PkgFileStamp& stamp, std::string& version,
                           std::vector<CachePkgFile>& files) {
    auto cache_file = CreateCBinaryStream(GetPkgCachePath(pkgpath).string());
    if (! cache_file) return false;

    if (ReadVersion("WPKG", *cache_file) != 1) return false;

    uint64_t cached_size { 0 };
    int64_t  cached_mtime { 0 };
    if (! ReadPod(*cache_file, cached_size) || ! ReadPod(*cache_file, cached_mtime)) return false;
    if (cached_size != stamp.size || cached_mtime != stamp.mtime) return false;

    if (! ReadSizedString(*cache_file, version)) return false;

    if (! CanRead(*cache_file, static_cast<idx>(sizeof(i32)))) return false;
    const auto entry_count = cache_file->ReadInt32();
    if (entry_count < 0 || entry_count > MAX_CACHE_ENTRY_COUNT) return false;

    files.clear();
    files.reserve(static_cast<size_t>(entry_count));
    for (i32 i = 0; i < entry_count; i++) {
        CachePkgFile file;
        if (! ReadSizedString(*cache_file, file.path)) return false;
        if (! CanRead(*cache_file, static_cast<idx>(sizeof(i32) * 2))) return false;
        file.offset = cache_file->ReadInt32();
        file.length = cache_file->ReadInt32();
        if (file.offset < 0 || file.length < 0) return false;
        files.push_back(std::move(file));
    }
    return true;
}

void SavePkgFilesToCache(std::string_view pkgpath, const PkgFileStamp& stamp, std::string_view version,
                         std::span<const CachePkgFile> files) {
    const auto max_i32 = static_cast<idx>(std::numeric_limits<i32>::max());
    if (version.size() > static_cast<size_t>(std::numeric_limits<i32>::max())
        || files.size() > static_cast<size_t>(std::numeric_limits<i32>::max())) {
        return;
    }
    for (const auto& file : files) {
        if (file.path.size() > static_cast<size_t>(std::numeric_limits<i32>::max())
            || file.offset < 0 || file.offset > max_i32
            || file.length < 0 || file.length > max_i32) {
            return;
        }
    }
    const auto cache_path = GetPkgCachePath(pkgpath);
    std::error_code ec;
    std::filesystem::create_directories(cache_path.parent_path(), ec);

    auto cache_file = CreateCBinaryStreamW(cache_path.string());
    if (! cache_file) return;

    WriteVersion("WPKG", *cache_file, 1);
    WritePod(*cache_file, stamp.size);
    WritePod(*cache_file, stamp.mtime);

    cache_file->WriteInt32(static_cast<i32>(version.size()));
    if (! version.empty()) cache_file->Write(version.data(), version.size());

    cache_file->WriteInt32(static_cast<i32>(files.size()));
    for (const auto& file : files) {
        cache_file->WriteInt32(static_cast<i32>(file.path.size()));
        if (! file.path.empty()) cache_file->Write(file.path.data(), file.path.size());
        cache_file->WriteInt32(static_cast<i32>(file.offset));
        cache_file->WriteInt32(static_cast<i32>(file.length));
    }
}
} // namespace

std::unique_ptr<WPPkgFs> WPPkgFs::CreatePkgFs(std::string_view pkgpath) {
    auto pkgfs       = std::unique_ptr<WPPkgFs>(new WPPkgFs());
    pkgfs->m_pkgPath = pkgpath;

    PkgFileStamp              stamp;
    std::string               version;
    std::vector<CachePkgFile> pkgfiles;
    if (QueryPkgFileStamp(pkgpath, stamp) && LoadPkgFilesFromCache(pkgpath, stamp, version, pkgfiles)) {
        LOG_INFO("pkg version: %s", version.c_str());
        for (const auto& el : pkgfiles) {
            pkgfs->m_files.insert({ el.path, { el.path, el.offset, el.length } });
        }
        return pkgfs;
    }

    auto indexResult = ReadWPPkgIndex(pkgpath);
    if (! indexResult) {
        const std::string packagePath(pkgpath);
        LOG_ERROR("invalid pkg file %s: %s",
                  packagePath.c_str(),
                  indexResult.error().message.c_str());
        return nullptr;
    }

    auto index = std::move(indexResult.value());
    LOG_INFO("pkg version: %s", index.version.c_str());
    pkgfiles.clear();
    pkgfiles.reserve(index.entries.size());
    for (auto& entry : index.entries) {
        pkgfiles.push_back({ entry.path, entry.offset, entry.length });
        pkgfs->m_files.insert(
            { entry.path, { entry.path, entry.offset, entry.length } });
    }

    if (QueryPkgFileStamp(pkgpath, stamp)) {
        SavePkgFilesToCache(pkgpath, stamp, index.version, pkgfiles);
    }
    return pkgfs;
}

bool WPPkgFs::Contains(std::string_view path) const { return m_files.count(std::string(path)) > 0; }

std::shared_ptr<IBinaryStream> WPPkgFs::Open(std::string_view path) {
    auto pkg = fs::CreateCBinaryStream(m_pkgPath);
    if (! pkg) return nullptr;
    if (Contains(path)) {
        const auto& file = m_files.at(std::string(path));
        return std::make_shared<LimitedBinaryStream>(pkg, file.offset, file.length);
    }
    return nullptr;
}

std::shared_ptr<IBinaryStreamW> WPPkgFs::OpenW(std::string_view) { return nullptr; }
