#include "rx-audio-filter.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

bool RxFilterSettings::valid() const
{
    return lowHz >= minimumHz && highHz <= maximumHz && highHz - lowHz >= minimumBandwidthHz &&
        std::floor(2 * lowHz) == 2 * lowHz && std::floor(2 * highHz) == 2 * highHz;
}

RxAudioFilter::RxAudioFilter() : m_filter(makeFilter(m_settings))
{
}

RxAudioFilter::Fir RxAudioFilter::makeFilter(RxFilterSettings settings)
{
    // The QO100 RX1 filter uses a 513-tap Kaiser-windowed sinc at 12 kHz.
    constexpr double beta = 8.6;
    const double denominator = std::cyl_bessel_i(0.0, beta);
    constexpr double middle = (taps - 1) / 2.0;
    const double width = (settings.highHz - settings.lowHz) / sampleRate;
    const double center = (settings.highHz + settings.lowHz) / 2.0;
    const double radians = 2.0 * std::numbers::pi * center / sampleRate;
    std::array<float, taps> lowpass;
    double sum = 0;
    for (unsigned int i = 0; i < taps; ++i)
    {
        const double time = i - middle;
        const double x = time / middle;
        const double sinc = time == 0 ? width : std::sin(std::numbers::pi * width * time) / (std::numbers::pi * time);
        lowpass[i] = static_cast<float>(sinc * std::cyl_bessel_i(0.0, beta * std::sqrt(std::max(0.0, 1 - x * x))) /
                                        denominator);
        sum += lowpass[i];
    }

    std::array<std::complex<float>, taps> coefficients;
    for (unsigned int i = 0; i < taps; ++i)
    {
        coefficients[i] = lowpass[i] / static_cast<float>(sum) *
            std::polar(1.0f, static_cast<float>(radians * (i - middle)));
    }
    return Fir(firfilt_cccf_create(coefficients.data(), coefficients.size()));
}

bool RxAudioFilter::set(RxFilterSettings settings)
{
    if (!settings.valid())
    {
        return false;
    }
    if (settings == m_settings)
    {
        return ready();
    }

    auto filter = makeFilter(settings);
    if (!filter)
    {
        return false;
    }
    m_pending = std::move(filter);
    m_settings = settings;
    return true;
}

void RxAudioFilter::reset()
{
    if (m_pending)
    {
        m_filter = std::move(m_pending);
    }
    else if (m_replacement)
    {
        m_filter = std::move(m_replacement);
    }
    m_replacement.reset();
    firfilt_cccf_reset(m_filter.get());
    m_fadePosition = 0;
    m_historyPosition = m_historyCount = 0;
}

std::complex<float> RxAudioFilter::execute(std::complex<float> sample)
{
    if (!m_replacement && m_pending)
    {
        m_replacement = std::move(m_pending);
        m_fadePosition = 0;
        auto position = (m_historyPosition + m_history.size() - m_historyCount) % m_history.size();
        for (std::size_t i = 0; i < m_historyCount; ++i)
        {
            firfilt_cccf_push(m_replacement.get(), m_history[position]);
            position = (position + 1) % m_history.size();
        }
    }

    std::complex<float> result;
    firfilt_cccf_execute_block(m_filter.get(), &sample, 1, &result);
    if (m_replacement)
    {
        std::complex<float> next;
        firfilt_cccf_execute_block(m_replacement.get(), &sample, 1, &next);
        const float alpha = static_cast<float>(++m_fadePosition) / fadeSamples;
        result = result * (1 - alpha) + next * alpha;
        if (m_fadePosition == fadeSamples)
        {
            m_filter = std::move(m_replacement);
        }
    }
    m_history[m_historyPosition] = sample;
    m_historyPosition = (m_historyPosition + 1) % m_history.size();
    m_historyCount = std::min(m_historyCount + 1, m_history.size());
    return result;
}
