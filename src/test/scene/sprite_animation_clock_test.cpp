#include "scene/SpriteAnimation.hpp"

#include <initializer_list>
#include <iostream>

using wallpaper::SpriteAnimation;
using wallpaper::SpriteFrame;

namespace
{
int failures = 0;

SpriteAnimation MakeAnimation(std::initializer_list<float> durations) {
    SpriteAnimation animation;
    int image_id = 0;
    for (float duration : durations) {
        SpriteFrame frame;
        frame.imageId = image_id++;
        frame.frametime = duration;
        animation.AppendFrame(frame);
    }
    return animation;
}

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void ExpectFrame(SpriteAnimation& animation, double delta, int expected, const char* message) {
    const int actual = animation.GetAnimateFrame(delta).imageId;
    if (actual != expected) {
        std::cerr << "FAIL: " << message << ": expected frame " << expected
                  << ", got " << actual << '\n';
        ++failures;
    }
}

void FirstFrameAndExactBoundaries() {
    auto animation = MakeAnimation({0.125f, 0.25f, 0.125f});
    ExpectFrame(animation, 0.0, 0, "initial sampling retains the first frame");
    ExpectFrame(animation, 0.0625, 0, "first frame receives its authored duration");
    ExpectFrame(animation, 0.0625, 1, "exact first boundary advances immediately");
    ExpectFrame(animation, 0.25, 2, "exact variable-duration boundary advances");
    ExpectFrame(animation, 0.125, 0, "exact cycle boundary wraps to the first frame");
}

void UnevenAndUniformClocksAgree() {
    auto uniform = MakeAnimation({0.125f, 0.25f, 0.125f});
    auto uneven = uniform;
    // Both schedules total 0.8125 s: one 0.5 s cycle, then 0.3125 s.
    for (int i = 0; i < 13; ++i) uniform.GetAnimateFrame(0.0625);
    for (double delta : {0.03125, 0.28125, 0.0625, 0.1875, 0.25}) {
        uneven.GetAnimateFrame(delta);
    }
    ExpectFrame(uniform, 0.0, 1, "uniform clock reaches the expected frame");
    ExpectFrame(uneven, 0.0, 1, "uneven clock reaches the same elapsed position");
    ExpectFrame(uniform, 0.0625, 2, "uniform clock preserves its fractional remainder");
    ExpectFrame(uneven, 0.0625, 2, "uneven clock preserves the same remainder");
}

void OverdueVariableFramesAndHugeCycles() {
    auto animation = MakeAnimation({0.125f, 0.25f, 0.125f});
    ExpectFrame(animation, 0.4375, 2, "one update crosses all overdue variable frames");
    ExpectFrame(animation, 0.0625, 0, "multi-frame update retains the remainder");

    // Over two trillion cycles must be skipped without iterating through them.
    ExpectFrame(animation, 1099511627776.3125, 1, "huge delta skips complete cycles");
    ExpectFrame(animation, 0.0625, 2, "huge delta retains its sub-frame remainder");
    ExpectFrame(animation, 0x1p80, 2, "huge exact cycles preserve the current frame phase");
    ExpectFrame(animation, 0.0625, 2, "huge exact cycles preserve the remaining duration");
    ExpectFrame(animation, 0.0625, 0, "remaining duration still reaches the exact boundary");
}

void PlaybackControlsPreserveClockState() {
    auto animation = MakeAnimation({0.125f, 0.25f, 0.125f});
    animation.SetCurrentFrame(1);
    ExpectFrame(animation, 0.1875, 1, "selecting a frame initializes its own duration");
    animation.Pause();
    Require(!animation.IsPlaying(), "Pause stops playback");
    ExpectFrame(animation, 1000000.0, 1, "paused time does not advance the clock");
    animation.Play();
    Require(animation.IsPlaying(), "Play resumes playback");
    ExpectFrame(animation, 0.0625, 2, "resume retains the pre-pause remainder");

    animation.Pause();
    animation.SetCurrentFrame(-1);
    Require(!animation.IsPlaying(), "selecting a frame preserves the paused state");
    ExpectFrame(animation, 1.0, 2, "negative frame selection wraps while paused");
    animation.Play();
    ExpectFrame(animation, 0.125, 0, "selected last frame receives its full duration");

    animation.Stop();
    Require(!animation.IsPlaying(), "Stop stops playback");
    ExpectFrame(animation, 1000000.0, 0, "Stop resets to the first frame and stays there");
    animation.Play();
    ExpectFrame(animation, 0.0625, 0, "Play after Stop restarts the first frame duration");
    ExpectFrame(animation, 0.0625, 1, "Play after Stop crosses the first exact boundary");
}

void ZeroDurationFramesDoNotLoopForever() {
    auto mixed = MakeAnimation({0.0f, 0.125f, 0.0f, 0.25f, 0.0f});
    ExpectFrame(mixed, 0.0, 1, "leading zero-duration frames are skipped");
    ExpectFrame(mixed, 0.125, 3, "zero-duration frames at a boundary are skipped");
    ExpectFrame(mixed, 0.25, 1, "trailing zero-duration frames wrap without consuming time");
    mixed.SetCurrentFrame(2);
    ExpectFrame(mixed, 0.0, 3, "selected zero-duration frame advances when playing");

    auto zero = MakeAnimation({0.0f, 0.0f, 0.0f});
    ExpectFrame(zero, 0.0, 0, "all-zero animation remains on its current frame");
    ExpectFrame(zero, 1000000000000.0, 0, "huge delta on all-zero frames terminates");
    zero.SetCurrentFrame(1);
    ExpectFrame(zero, 0.125, 1, "all-zero animation preserves an explicitly selected frame");

    auto single = MakeAnimation({0.0f});
    ExpectFrame(single, 1.0, 0, "single zero-duration frame terminates");
}
} // namespace

int main() {
    FirstFrameAndExactBoundaries();
    UnevenAndUniformClocksAgree();
    OverdueVariableFramesAndHugeCycles();
    PlaybackControlsPreserveClockState();
    ZeroDurationFramesDoNotLoopForever();
    if (failures != 0) return 1;
    std::cout << "sprite animation clock tests passed\n";
}
