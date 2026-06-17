#pragma once
#include <memory>
#include <vector>
#include "IBinaryStream.h"

#include "core/Literals.hpp"

namespace wallpaper
{
namespace fs
{

class LimitedBinaryStream : public IBinaryStream {
public:
    LimitedBinaryStream(std::shared_ptr<IBinaryStream> infs, idx start, isize size)
        : m_pos(0), m_start(start), m_end(start + size), m_infs(infs) {}
    virtual ~LimitedBinaryStream() = default;

private:
    bool CanSeekTo(idx pos) const { return pos >= 0 && pos <= Size(); }

    bool SeekInMPos(void) { return m_infs->SeekSet(m_start + m_pos); }
    bool SeekInPos(idx pos) {
        if (CanSeekTo(pos)) {
            m_pos = pos;
            return SeekInMPos();
        }
        return false;
    }
    bool End() const { return m_pos < 0 || m_pos == Size(); };

protected:
    virtual usize Write_impl(const void*, usize) { return 0; }

public:
    virtual usize Read(void* buffer, usize sizeInByte) {
        if (End() || sizeInByte == 0) return 0;

        const isize available = Size() - m_pos;
        if (available <= 0) return 0;

        isize isizeInByte = static_cast<isize>(sizeInByte);
        if (isizeInByte > available) isizeInByte = available;
        SeekInMPos();
        const usize read = m_infs->Read(buffer, static_cast<usize>(isizeInByte));
        m_pos += static_cast<isize>(read);
        return read;
    }
    virtual char* Gets(char* buffer, usize sizeStr) {
        Read(buffer, sizeStr);
        return buffer;
    }
    virtual idx  Tell() const { return m_pos; }
    virtual bool SeekSet(idx offset) {
        idx pos = offset;
        return SeekInPos(pos);
    }
    virtual bool SeekCur(idx offset) {
        idx pos = m_pos + offset;
        return SeekInPos(pos);
    }
    virtual bool SeekEnd(idx offset) {
        idx pos = Size() + offset;
        return SeekInPos(pos);
    }
    virtual isize Size() const { return m_end - m_start; }

private:
    idx                            m_pos;
    const idx                      m_start;
    const idx                      m_end; // end if m_pos == m_end - m_start
    std::shared_ptr<IBinaryStream> m_infs;
};

} // namespace fs
} // namespace wallpaper
