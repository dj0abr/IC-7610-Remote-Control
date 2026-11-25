#include "rx-notch.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace
{
constexpr double bandwidthHz = 400.0;
constexpr int acquireFrames = 6;
constexpr int releaseFrames = 8;
constexpr int candidateToleranceHz = 12;
constexpr int lockRangeHz = 30;
constexpr int retuneThresholdHz = 2;
}

RxNotch::RxNotch()
{
    for (int i = 0; i < fftSize; ++i)
    {
        m_window[i] = 0.5f - 0.5f * std::cos(2.0f * std::numbers::pi_v<float> * i / fftSize);
    }
    m_plan = fftwf_plan_dft_r2c_1d(fftSize, m_fftInput.data(), m_fftOutput, FFTW_ESTIMATE);
}

RxNotch::~RxNotch()
{
    if (m_plan)
    {
        fftwf_destroy_plan(m_plan);
    }
}

RxNotch::Iir RxNotch::makeFilter(int frequency)
{
    if (frequency == 0)
    {
        return {};
    }

    // Cascade two identical biquads for deeper attenuation near the center.
    // Each section contributes half the dB drop at the overall half-power edges.
    const double angle = 2.0 * std::numbers::pi * frequency / sampleRate;
    const double sectionWidth = std::tan(std::numbers::pi * bandwidthHz / sampleRate)
        * std::sqrt(std::numbers::sqrt2 - 1.0);
    const double gain = 1.0 / (1.0 + sectionWidth);
    const float middle = static_cast<float>(-2.0 * gain * std::cos(angle));
    const float edge = static_cast<float>(gain);
    const float feedback = static_cast<float>(2.0 * gain - 1.0);
    float b[]{edge, middle, edge, edge, middle, edge};
    float a[]{1.0f, middle, feedback, 1.0f, middle, feedback};
    return Iir(iirfilt_crcf_create_sos(b, a, 2));
}

std::complex<float> RxNotch::filterSample(const Iir& filter, std::complex<float> sample)
{
    if (filter)
    {
        iirfilt_crcf_execute(filter.get(), sample, &sample);
    }
    return sample;
}

void RxNotch::set(Mode mode, int manualHz)
{
    if (mode != m_mode)
    {
        clearDetection();
        m_mode = mode;
    }
    m_manualHz = manualHz;
}

void RxNotch::setPassband(RxFilterSettings passband)
{
    if (passband != m_passband)
    {
        m_passband = passband;
        clearDetection();
    }
}

void RxNotch::clearDetection()
{
    m_writePosition = 0;
    m_untilAnalysis = fftSize;
    m_candidateHz = 0;
    m_candidateFrames = 0;
    m_missingFrames = 0;
    m_detectedHz = 0;
}

void RxNotch::reset()
{
    clearDetection();
    const int frequency = m_mode == Mode::Manual ? m_manualHz : 0;
    m_filter = makeFilter(frequency);
    m_filterHz = frequency;
    m_nextFilter.reset();
    m_warmupRemaining = 0;
    m_fadeRemaining = 0;
}

void RxNotch::analyze()
{
    for (int i = 0; i < fftSize; ++i)
    {
        m_fftInput[i] = m_history[(m_writePosition + i) % fftSize] * m_window[i];
    }
    fftwf_execute(m_plan);
    followTone(findTone());
}

int RxNotch::findTone() const
{
    constexpr float binHz = static_cast<float>(sampleRate) / fftSize;
    constexpr int noiseRadius = 10;
    constexpr int peakGuard = 3;
    static_assert(minimumHz / binHz >= noiseRadius);
    static_assert(maximumHz / binHz + noiseRadius + 1 <= fftSize / 2);
    const int first = static_cast<int>(std::floor(m_passband.lowHz / binHz));
    const int last = static_cast<int>(std::ceil(m_passband.highHz / binHz));
    std::array<float, fftSize / 2 + 1> power{};
    float bandPower = 0.0f;
    for (int bin = first - noiseRadius; bin <= last + noiseRadius; ++bin)
    {
        const auto& value = m_fftOutput[bin];
        power[bin] = value[0] * value[0] + value[1] * value[1];
        if (bin >= first && bin <= last)
        {
            bandPower += power[bin];
        }
    }

    int strongestHz = 0;
    float strongestPower = 0.0f;
    for (int bin = first; bin <= last; ++bin)
    {
        const float peak = power[bin];
        if (peak < 1e-6f || peak <= strongestPower || peak <= power[bin - 1] || peak < power[bin + 1] ||
            std::max(power[bin - peakGuard], power[bin + peakGuard]) > 0.08f * peak)
        {
            continue;
        }
        // Require a line that stands out both locally and in the receive passband.
        const float linePower = power[bin - 1] + peak + power[bin + 1];
        float nearby = 0.0f;
        for (int step = peakGuard + 1; step <= noiseRadius; ++step)
        {
            nearby += power[bin - step] + power[bin + step];
        }
        if (linePower < 0.12f * bandPower || peak < nearby)
        {
            continue;
        }

        // Parabolic interpolation of the Hann-windowed peak refines the FFT bin.
        const float left = std::log(std::max(1e-20f, power[bin - 1]));
        const float center = std::log(peak);
        const float right = std::log(std::max(1e-20f, power[bin + 1]));
        const float curvature = (left - center) + (right - center);
        const float offset = curvature < 0.0f ? 0.5f * (left - right) / curvature : 0.0f;
        const int frequency = static_cast<int>(std::lround((bin + offset) * binHz));
        if (frequency < m_passband.lowHz || frequency > m_passband.highHz ||
            (m_detectedHz && std::abs(frequency - m_detectedHz) > lockRangeHz))
        {
            continue;
        }
        strongestPower = peak;
        strongestHz = frequency;
    }
    return strongestHz;
}

void RxNotch::followTone(int candidate)
{
    if (m_detectedHz)
    {
        if (candidate)
        {
            m_missingFrames = 0;
            if (std::abs(candidate - m_detectedHz) >= retuneThresholdHz)
            {
                m_detectedHz = candidate;
            }
        }
        else if (++m_missingFrames >= releaseFrames)
        {
            m_detectedHz = 0;
            m_missingFrames = 0;
        }
        return;
    }

    if (!candidate)
    {
        m_candidateFrames = 0;
        return;
    }
    m_candidateFrames = std::abs(candidate - m_candidateHz) <= candidateToleranceHz
        ? m_candidateFrames + 1 : 1;
    m_candidateHz = candidate;
    if (m_candidateFrames >= acquireFrames)
    {
        m_detectedHz = candidate;
        m_candidateFrames = 0;
    }
}

void RxNotch::startTransition(int frequency)
{
    m_nextFilter = makeFilter(frequency);
    if (frequency && !m_nextFilter)
    {
        return;
    }
    m_nextFilterHz = frequency;
    m_warmupRemaining = frequency ? fadeSamples : 0;
    m_fadeRemaining = fadeSamples;
}

std::complex<float> RxNotch::execute(std::complex<float> sample)
{
    if (m_mode == Mode::Auto && m_plan)
    {
        m_history[m_writePosition] = sample.real();
        m_writePosition = (m_writePosition + 1) % fftSize;
        if (--m_untilAnalysis == 0)
        {
            analyze();
            m_untilAnalysis = hop;
        }
    }

    const int frequency = m_mode == Mode::Manual ? m_manualHz : m_detectedHz;
    if (!m_fadeRemaining && frequency != m_filterHz)
    {
        startTransition(frequency);
    }

    const auto current = filterSample(m_filter, sample);
    if (!m_fadeRemaining)
    {
        return current;
    }

    const auto next = filterSample(m_nextFilter, sample);
    if (m_warmupRemaining)
    {
        --m_warmupRemaining;
        return current;
    }

    const float mix = 1.0f - static_cast<float>(--m_fadeRemaining) / fadeSamples;
    if (!m_fadeRemaining)
    {
        m_filter = std::move(m_nextFilter);
        m_filterHz = m_nextFilterHz;
    }
    return current + mix * (next - current);
}
