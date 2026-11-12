#include "FrameTimer.hpp"
#include "utils//Logging.h"

#include <algorithm>
#include <numeric>

using namespace wallpaper;
using micros = std::chrono::microseconds;
using namespace std::chrono;

FrameTimer::FrameTimer(std::function<void()> cb)
    : m_callback(cb), m_frame_busy_count(0), m_timer([this]() {
          // Keep one pending draw: a slow renderer must not replay stale timer
          // ticks in a burst after it catches up.
          i32 idle = 0;
          if (m_callback && m_frame_busy_count.compare_exchange_strong(idle, 1)) {
              m_callback();
          }
      }) {
    SetRequiredFps(15);
}

FrameTimer::~FrameTimer() {};

u16 FrameTimer::RequiredFps() const { return m_req_fps; }

double FrameTimer::FrameTime() const {
    return duration_cast<duration<double>>(m_frametime.load()).count();
}

double FrameTimer::IdeaTime() const {
    auto frametime = m_frametime.load();
    auto ideatime  = m_ideatime.load();
    auto time      = frametime > ideatime ? frametime : ideatime;
    return duration_cast<duration<double>>(time).count();
}

double FrameTimer::DeltaTime() const { return m_delta_time; }

void FrameTimer::UpdateFrametime() {
    m_frametime.store(std::accumulate(m_frametime_queue.begin(),
                                      m_frametime_queue.end(),
                                      duration_cast<microseconds>(0s)) /
                      m_frametime_queue.size());
}

void FrameTimer::SetRequiredFps(u16 value) {
    m_req_fps = std::max<u16>(1, value);
    microseconds ideatime { 1'000'000 / m_req_fps };
    m_ideatime = ideatime;
    m_timer.SetInterval(ideatime);
    for (usize i = 0; i < FrameTimer::FRAMETIME_QUEUE_SIZE; i++) {
        AddFrametime(ideatime);
    }
    UpdateFrametime();
}

void FrameTimer::AddFrametime(micros t) {
    m_frametime_queue.push_back(t);
    while (m_frametime_queue.size() > FrameTimer::FRAMETIME_QUEUE_SIZE) {
        m_frametime_queue.pop_front();
    }
}

void FrameTimer::FrameBegin() {
    const auto now = steady_clock::now();
    // This draw has left the queue. Allow one successor while it renders,
    // without queuing a backlog or forcing a slow renderer to skip every other tick.
    m_frame_busy_count.store(0);
    // The first frame after startup/resume has no active predecessor. Never
    // include time spent paused in the scene's animation clock.
    m_delta_time = m_reset_clock.exchange(false)
                       ? duration<double>(m_ideatime.load()).count()
                       : duration<double>(now - m_clock).count();
    m_clock = now;
}
void FrameTimer::FrameEnd() {
    auto now = steady_clock::now();
    AddFrametime(duration_cast<microseconds>(now - m_clock));
    UpdateFrametime();

}

void FrameTimer::SetCallback(const std::function<void()>& cb) {
    if (! Running()) m_callback = cb;
}
void FrameTimer::Run() {
    if (!Running()) ResetClock();
    m_timer.Start();
}
void FrameTimer::ResetClock() { m_reset_clock = true; }
void FrameTimer::Stop() { m_timer.Stop(); }
bool FrameTimer::Running() const { return m_timer.Running(); }
