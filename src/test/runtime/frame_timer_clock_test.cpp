#include "timer/FrameTimer.hpp"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace std::chrono;
using namespace std::chrono_literals;

static void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

int main() {
    wallpaper::FrameTimer frames;
    frames.SetRequiredFps(60);
    frames.FrameBegin();
    frames.FrameEnd();
    const auto start = steady_clock::now();
    double animated = 0.0;
    for (auto delay : {10ms, 40ms, 15ms, 35ms, 10ms, 30ms}) {
        std::this_thread::sleep_for(delay);
        frames.FrameBegin();
        animated += frames.DeltaTime();
        frames.FrameEnd();
    }
    const double elapsed = duration<double>(steady_clock::now() - start).count();
    Require(std::abs(animated - elapsed) < 0.008,
            "animation time must follow elapsed time across uneven frame intervals");

    std::this_thread::sleep_for(80ms);
    frames.ResetClock();
    frames.FrameBegin();
    Require(frames.DeltaTime() < 0.04, "scene setup time must not advance a newly loaded scene");
    frames.FrameEnd();

    frames.Run();
    frames.FrameBegin();
    frames.FrameEnd();
    frames.Stop();
    std::this_thread::sleep_for(120ms);
    frames.Run();
    frames.FrameBegin();
    Require(frames.DeltaTime() < 0.04, "paused time must not jump the animation on resume");
    frames.FrameEnd();
    frames.Stop();

    std::atomic<int> calls{0};
    wallpaper::ThreadTimer timer([&] { ++calls; });
    timer.SetInterval(2s);
    for (int i = 0; i < 30; ++i) {
        timer.Start();
        timer.Stop();
    }
    Require(calls == 0, "stopping a timer must not invoke its pending callback");

    timer.SetInterval(1ms);
    timer.Start();
    const auto timeout = steady_clock::now() + 1s;
    while (calls == 0 && steady_clock::now() < timeout) std::this_thread::yield();
    timer.Stop();
    Require(calls > 0, "restarting the timer must reliably start its worker");

    std::atomic<int> queued_draws{0};
    wallpaper::FrameTimer blocked([&] { ++queued_draws; });
    blocked.SetRequiredFps(1000);
    blocked.Run();
    const auto draw_timeout = steady_clock::now() + 1s;
    while (queued_draws == 0 && steady_clock::now() < draw_timeout) std::this_thread::yield();
    std::this_thread::sleep_for(20ms);
    Require(queued_draws == 1, "a blocked renderer must not accumulate a burst of queued draws");
    blocked.FrameBegin(); // One active draw may have one successor waiting.
    std::this_thread::sleep_for(20ms);
    blocked.Stop();
    Require(queued_draws == 2, "an active draw must not occupy the pending successor slot");
    blocked.FrameEnd();

    std::atomic<int> paced_calls{0};
    wallpaper::ThreadTimer paced([&] {
        ++paced_calls;
        std::this_thread::sleep_for(6ms);
    });
    paced.SetInterval(10ms);
    const auto pacing_start = steady_clock::now();
    paced.Start();
    std::this_thread::sleep_for(260ms);
    paced.Stop();
    const double pacing_seconds = duration<double>(steady_clock::now() - pacing_start).count();
    Require(paced_calls / pacing_seconds > 80.0,
            "callback execution time must not accumulate into the timer period");
    std::cout << "elapsed=" << elapsed << " animation=" << animated
              << " paced_callbacks=" << paced_calls << '\n';
}
