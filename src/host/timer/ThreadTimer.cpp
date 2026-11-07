#include "ThreadTimer.hpp"

#include <cassert>
#include <algorithm>

using namespace wallpaper;
using micros = std::chrono::microseconds;

ThreadTimer::ThreadTimer(std::function<void()> cb): m_callback(cb) {}
ThreadTimer::~ThreadTimer() { Stop(); }

bool ThreadTimer::Running() const { return m_running; }

void ThreadTimer::SetInterval(micros v) { m_interval = std::max(v, micros(1)); }

void ThreadTimer::Start() {
    std::unique_lock<std::mutex> lock(m_op_mutex);

    if (Running()) return;
    m_running = true;
    m_timer_thread = std::thread([this]() {
        using clock = std::chrono::steady_clock;
        auto deadline = clock::now() + m_interval.load();
        while (Running()) {
            {
                std::unique_lock<std::mutex> lock(m_cond_mutex);
                if (m_condition.wait_until(lock, deadline, [this] { return !Running(); }))
                    break;
            }
            if (m_callback) m_callback();
            const auto interval = m_interval.load();
            deadline += interval;
            const auto now = clock::now();
            // Skip missed ticks instead of emitting a burst of overdue draws.
            if (deadline <= now) deadline = now + interval;
        }
    });
}

void ThreadTimer::Stop() {
    std::unique_lock<std::mutex> lock(m_op_mutex);
    assert(std::this_thread::get_id() != m_timer_thread.get_id());

    if (! Running()) return;
    m_running = false;

    {
        std::unique_lock<std::mutex> lock(m_cond_mutex);
        m_condition.notify_all();
    }

    if (m_timer_thread.joinable()) {
        m_timer_thread.join();
    }
}
