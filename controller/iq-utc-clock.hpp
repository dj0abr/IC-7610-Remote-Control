#pragma once

#include <libft8/libft8.hpp>

#include <chrono>
#include <cstdint>
#include <optional>

class IqUtcClock
{
public:
    using SteadyTime = std::chrono::steady_clock::time_point;

    bool observeSystemClock(ft8::Time utc, SteadyTime steady)
    {
        using namespace std::chrono_literals;
        const auto step = m_last ? (utc - m_last->utc) - (steady - m_last->steady) : ft8::Time::duration{};
        m_last = Observation{utc, steady};
        if (step >= -150ms && step <= 150ms)
        {
            return false;
        }

        if (m_sampleZero)
        {
            // Translate the existing anchor to the new system time without re-anchoring to a TCP read.
            *m_sampleZero += ft8::sample_index(ft8::Time{step});
        }
        return true;
    }

    void establishOrigin(ft8::Time received, std::int64_t endAudioSample)
    {
        if (!m_sampleZero)
        {
            m_sampleZero = ft8::sample_index(received) - endAudioSample;
        }
    }

    void resetOrigin() { m_sampleZero.reset(); }

    std::optional<std::int64_t> sampleZero() const { return m_sampleZero; }

private:
    struct Observation
    {
        ft8::Time utc;
        SteadyTime steady;
    };

    std::optional<Observation> m_last;
    std::optional<std::int64_t> m_sampleZero;
};
