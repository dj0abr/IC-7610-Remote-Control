#include "../iq-utc-clock.hpp"

#include <cstdlib>
#include <iostream>

namespace
{
using namespace std::chrono_literals;

void check(bool ok, const char* message)
{
    if (!ok)
    {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

ft8::Time utc(std::chrono::milliseconds elapsed)
{
    return ft8::Time{} + 1000s + elapsed;
}

IqUtcClock::SteadyTime steady(std::chrono::milliseconds elapsed)
{
    return IqUtcClock::SteadyTime{} + elapsed;
}
}

int main()
{
    IqUtcClock clock;
    constexpr std::int64_t rate = ft8::sample_rate;
    constexpr std::int64_t initialOrigin = 1000 * rate;

    check(!clock.observeSystemClock(utc(1s), steady(1s)), "Initial observation");
    clock.establishOrigin(utc(1s), rate);
    check(clock.sampleZero() == initialOrigin, "Initial anchor uses UTC and the sample count");

    check(!clock.observeSystemClock(utc(2500ms), steady(2500ms)), "Delay is not a clock change");
    clock.establishOrigin(utc(2500ms), 2 * rate);
    check(!clock.observeSystemClock(utc(4500ms), steady(4500ms)), "Sustained delay is not a clock change");
    clock.establishOrigin(utc(4500ms), 4 * rate);
    check(clock.sampleZero() == initialOrigin, "A sustained 500 ms arrival offset leaves the anchor fixed");

    check(clock.observeSystemClock(utc(5300ms), steady(5s)), "Forward UTC step detected");
    clock.establishOrigin(utc(5300ms), 9 * rate / 2);
    check(clock.sampleZero() == initialOrigin + 3 * rate / 10, "Clock step translates the anchor by 300 ms");
    check(!clock.observeSystemClock(utc(6300ms), steady(6s)), "Forward UTC step is reported once");

    check(clock.observeSystemClock(utc(6950ms), steady(7s)), "Backward UTC step detected");
    check(clock.sampleZero() == initialOrigin - rate / 20, "Backward step translates the anchor by -350 ms");

    clock.resetOrigin();
    check(!clock.sampleZero(), "Capture restart clears the anchor");
    check(!clock.observeSystemClock(utc(9950ms), steady(10s)), "Retune keeps track of the system clock");
    clock.establishOrigin(utc(9950ms), 6 * rate);
    check(clock.sampleZero() == initialOrigin + 395 * rate / 100, "First batch after retune sets a new anchor");

    clock = {};
    check(!clock.observeSystemClock(utc(12s), steady(12s)), "A new connection starts with no clock history");
    clock.establishOrigin(utc(12s), rate);
    check(clock.sampleZero() == initialOrigin + 11 * rate, "A new connection anchors its own sample count");
}
