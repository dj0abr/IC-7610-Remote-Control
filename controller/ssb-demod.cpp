#include "ssb-demod.hpp"

#include <QtEndian>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <cstdint>

namespace
{
constexpr unsigned int firstFactor = 10;
constexpr unsigned int secondFactor = 4;
constexpr unsigned int thirdFactor = 4;
constexpr unsigned int decimation = firstFactor * secondFactor * thirdFactor;
constexpr unsigned int voiceRate = SsbDemodulator::inputRate / decimation;
constexpr unsigned int interpolation = SsbDemodulator::audioRate / voiceRate;
constexpr float rejectionDb = 80.0f;
static_assert(voiceRate == RxAudioFilter::sampleRate);
constexpr float radiansPerCycle = 2.0f * std::numbers::pi_v<float>;
constexpr float targetAudioRms = 0.38f;
constexpr float targetVoicePower = 2.0f * targetAudioRms * targetAudioRms;
constexpr float maxReceiveGain = 4000.0f;
constexpr float minimumVoicePower = targetVoicePower / (maxReceiveGain * maxReceiveGain);
constexpr float outputPeakLimit = 0.96f;

float decayFor(float seconds, unsigned int sampleRate)
{
    return std::exp(-1.0f / (seconds * sampleRate));
}

const float voiceAttack = 1.0f - decayFor(0.030f, voiceRate);
const float voiceRelease = 1.0f - decayFor(0.060f, voiceRate);
const float limiterRelease = decayFor(0.012f, SsbDemodulator::audioRate);

}

SsbDemodulator::SsbDemodulator()
    : m_oscillator(nco_crcf_create(LIQUID_VCO)),
      m_first(firdecim_crcf_create_kaiser(firstFactor, 8, rejectionDb)),
      m_second(firdecim_crcf_create_kaiser(secondFactor, 8, rejectionDb)),
      m_third(firdecim_crcf_create_kaiser(thirdFactor, 8, rejectionDb)),
      m_interpolator(firinterp_rrrf_create_kaiser(interpolation, 12, rejectionDb))
{
    resetNoiseReduction();
    if (ready())
    {
        firdecim_crcf_set_scale(m_first, 1.0f / firstFactor);
        firdecim_crcf_set_scale(m_second, 1.0f / secondFactor);
        firdecim_crcf_set_scale(m_third, 1.0f / thirdFactor);
    }
}

SsbDemodulator::~SsbDemodulator()
{
    if (m_interpolator)
    {
        firinterp_rrrf_destroy(m_interpolator);
    }
    if (m_third)
    {
        firdecim_crcf_destroy(m_third);
    }
    if (m_second)
    {
        firdecim_crcf_destroy(m_second);
    }
    if (m_first)
    {
        firdecim_crcf_destroy(m_first);
    }
    if (m_oscillator)
    {
        nco_crcf_destroy(m_oscillator);
    }
}

bool SsbDemodulator::ready() const
{
    return m_oscillator && m_first && m_second && m_third && m_filter.ready() && m_interpolator && m_noiseReduction;
}

void SsbDemodulator::tune(int offsetHz, Sideband sideband)
{
    nco_crcf_reset(m_oscillator);
    setFrequencyOffset(offsetHz);
    firdecim_crcf_reset(m_first);
    firdecim_crcf_reset(m_second);
    firdecim_crcf_reset(m_third);
    firinterp_rrrf_reset(m_interpolator);
    m_sideband = sideband;
    m_filter.reset();
    resetNoiseReduction();
    m_voiceCount = 0;
    m_voicePower = minimumVoicePower;
    m_outputPeak = outputPeakLimit;
    m_iq.clear();
    m_at192k.clear();
}

void SsbDemodulator::setFrequencyOffset(int offsetHz)
{
    nco_crcf_set_frequency(m_oscillator, radiansPerCycle * offsetHz / inputRate);
}

bool SsbDemodulator::setRxFilter(RxFilterSettings settings)
{
    return m_filter.set(settings);
}

void SsbDemodulator::setNoiseReduction(int percent)
{
    m_nrPercent = std::clamp(percent, 0, 100);
    if (m_noiseReduction)
    {
        int attenuation = -std::max(1, static_cast<int>(std::lround(0.3 * m_nrPercent)));
        speex_preprocess_ctl(m_noiseReduction.get(), SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &attenuation);
    }
}

void SsbDemodulator::resetNoiseReduction()
{
    auto* state = speex_preprocess_state_init(voiceBlockSamples, voiceRate);
    if (!state)
    {
        return;
    }
    m_noiseReduction.reset(state);
    int enabled = 1;
    int disabled = 0;
    speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_DENOISE, &enabled);
    speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_AGC, &disabled);
    m_nrMix = 0.0f;
    setNoiseReduction(m_nrPercent);
}

void SsbDemodulator::processVoiceBlock()
{
    std::array<spx_int16_t, voiceBlockSamples> wet;
    for (unsigned int i = 0; i < voiceBlockSamples; ++i)
    {
        wet[i] = static_cast<spx_int16_t>(std::clamp(std::lround(m_voiceBlock[i] * 32768), -32768L, 32767L));
    }
    // Keep the noise estimate warm even when the unprocessed audio is selected.
    speex_preprocess_run(m_noiseReduction.get(), wet.data());
    const float target = m_nrPercent ? 1.0f : 0.0f;
    constexpr float step = 1.0f / voiceBlockSamples;
    for (unsigned int i = 0; i < voiceBlockSamples; ++i)
    {
        m_nrMix += std::clamp(target - m_nrMix, -step, step);
        if (std::abs(target - m_nrMix) < 1e-5f)
        {
            m_nrMix = target;
        }
        m_voiceBlock[i] += m_nrMix * (wet[i] / 32768.0f - m_voiceBlock[i]);
    }

    const auto start = m_audio.size();
    m_audio.resize(start + voiceBlockSamples * interpolation);
    auto* output = m_audio.data() + start;
    firinterp_rrrf_execute_block(m_interpolator, m_voiceBlock.data(), voiceBlockSamples, output);
    for (float& sample : std::span(output, voiceBlockSamples * interpolation))
    {
        sample = limitAudio(sample);
    }
}

float SsbDemodulator::detectAudio(const std::complex<float>& sample)
{
    // Estimate audio power from the complex envelope, independent of audio phase.
    const float power = 4.0f * std::norm(sample);
    const float weight = power > m_voicePower ? voiceAttack : voiceRelease;
    m_voicePower += weight * (power - m_voicePower);
    m_voicePower = std::max(m_voicePower, minimumVoicePower);

    const float gain = std::sqrt(targetVoicePower / m_voicePower);
    return 2.0f * sample.real() * gain;
}

float SsbDemodulator::limitAudio(float sample)
{
    m_outputPeak = std::max(m_outputPeak * limiterRelease, std::abs(sample));
    m_outputPeak = std::max(m_outputPeak, outputPeakLimit);
    return sample * (outputPeakLimit / m_outputPeak);
}

std::span<const float> SsbDemodulator::process(const char* data, int bytes)
{
    const auto* raw = reinterpret_cast<const uchar*>(data);
    const auto retained = m_iq.size();
    const unsigned int samples = bytes / 4;
    m_iq.resize(retained + samples);
    for (unsigned int i = 0; i < samples; ++i)
    {
        constexpr float normalize = 1.0f / 32768.0f;
        const float inPhase = normalize * qFromLittleEndian<qint16>(raw + 4 * i);
        const float quadrature = normalize * qFromLittleEndian<qint16>(raw + 4 * i + 2);
        m_iq[retained + i] = {inPhase, quadrature};
    }

    // Keep the fractional decimation block for the next TCP frame.
    const unsigned int voiceSamples = m_iq.size() / decimation;
    if (voiceSamples == 0)
    {
        m_at192k.clear();
        return {};
    }
    const unsigned int used = voiceSamples * decimation;
    m_at192k.resize(used / firstFactor);
    m_at48k.resize(m_at192k.size() / secondFactor);
    m_at12k.resize(voiceSamples);
    m_audio.clear();
    m_audio.reserve((m_voiceCount + voiceSamples) / voiceBlockSamples * voiceBlockSamples * interpolation);

    nco_crcf_mix_block_down(m_oscillator, m_iq.data(), m_iq.data(), used);
    firdecim_crcf_execute_block(m_first, m_iq.data(), m_at192k.size(), m_at192k.data());
    firdecim_crcf_execute_block(m_second, m_at192k.data(), m_at48k.size(), m_at48k.data());
    firdecim_crcf_execute_block(m_third, m_at48k.data(), voiceSamples, m_at12k.data());
    for (auto sample : m_at12k)
    {
        // Conjugating LSB mirrors its negative-frequency passband into the same FIR.
        if (m_sideband == Sideband::Lsb)
        {
            sample = std::conj(sample);
        }
        m_voiceBlock[m_voiceCount++] = detectAudio(m_filter.execute(sample));
        if (m_voiceCount == voiceBlockSamples)
        {
            processVoiceBlock();
            m_voiceCount = 0;
        }
    }

    const auto tail = std::move(m_iq.begin() + used, m_iq.end(), m_iq.begin());
    m_iq.resize(tail - m_iq.begin());
    return m_audio;
}
