#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>

namespace wallpaper
{
template<typename T>
class TripleSwapchain {
public:
    virtual ~TripleSwapchain() = default;

    TripleSwapchain(const TripleSwapchain&)            = delete;
    TripleSwapchain& operator=(const TripleSwapchain&) = delete;
    TripleSwapchain(TripleSwapchain&&)                 = delete;
    TripleSwapchain& operator=(TripleSwapchain&&)      = delete;

    T* eatFrame() {
        if (! dirty().exchange(false)) {
            return nullptr;
        }
        presented() = ready().exchange(presented());
        return presented();
    }

    void renderFrame() {
        inprogress() = ready().exchange(inprogress());
        dirty().exchange(true);
        std::function<void()> on_ready;
        {
            std::scoped_lock lock(m_callback_mutex);
            on_ready = m_on_ready;
        }
        if (on_ready) on_ready();
    }

    void setOnReady(std::function<void()> callback) {
        std::scoped_lock lock(m_callback_mutex);
        m_on_ready = std::move(callback);
    }

    T* getInprogress() { return inprogress(); }

    virtual std::uint32_t width() const  = 0;
    virtual std::uint32_t height() const = 0;

protected:
    TripleSwapchain() = default;

    virtual std::atomic<T*>& presented()  = 0;
    virtual std::atomic<T*>& ready()      = 0;
    virtual std::atomic<T*>& inprogress() = 0;

private:
    std::atomic<bool>& dirty() { return m_dirty; }

    std::atomic<bool>    m_dirty { false };
    std::mutex            m_callback_mutex;
    std::function<void()> m_on_ready;
};
} // namespace wallpaper
