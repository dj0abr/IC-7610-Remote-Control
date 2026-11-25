#pragma once

#include "rx-audio-filter.hpp"

#include <speex/speex_preprocess.h>

#include <span>
#include <vector>

class SsbDemodulator
{
public:
    enum class Sideband
    {
        Usb,
        Lsb
    };

    static constexpr int inputRate = 1920000;
    static constexpr int audioRate = 48000;

    SsbDemodulator();
    ~SsbDemodulator();
    SsbDemodulator(const SsbDemodulator&) = delete;
    SsbDemodulator& operator=(const SsbDemodulator&) = delete;

    bool ready() const;
    void tune(int offsetHz, Sideband sideband);
    void setFrequencyOffset(int offsetHz);
    bool setRxFilter(RxFilterSettings settings);
    void setNoiseReduction(int percent);
    std::span<const float> process(const char* data, int bytes);

private:
    float detectAudio(const std::complex<float>& sample);
    float limitAudio(float sample);
    void resetNoiseReduction();
    void processVoiceBlock();

    nco_crcf m_oscillator;
    firdecim_crcf m_first;
    firdecim_crcf m_second;
    firdecim_crcf m_third;
    RxAudioFilter m_filter;
    firinterp_rrrf m_interpolator;
    Sideband m_sideband = Sideband::Lsb;
    static constexpr unsigned int voiceBlockSamples = RxAudioFilter::sampleRate / 50;
    std::unique_ptr<SpeexPreprocessState, decltype(&speex_preprocess_state_destroy)>
        m_noiseReduction{nullptr, speex_preprocess_state_destroy};
    int m_nrPercent = 0;
    float m_nrMix = 0.0f;
    std::array<float, voiceBlockSamples> m_voiceBlock{};
    unsigned int m_voiceCount = 0;
    float m_voicePower = 0.0f;
    float m_outputPeak = 0.0f;
    std::vector<std::complex<float>> m_iq;
    std::vector<std::complex<float>> m_at192k;
    std::vector<std::complex<float>> m_at48k;
    std::vector<std::complex<float>> m_at12k;
    std::vector<float> m_audio;
};
