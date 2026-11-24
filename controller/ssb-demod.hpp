#pragma once

#include <complex>
#include <liquid.h>

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
    static constexpr int audioHighHz = 2700;

    SsbDemodulator();
    ~SsbDemodulator();
    SsbDemodulator(const SsbDemodulator&) = delete;
    SsbDemodulator& operator=(const SsbDemodulator&) = delete;

    bool ready() const;
    void tune(int offsetHz, Sideband sideband);
    void setFrequencyOffset(int offsetHz);
    std::span<const float> process(const char* data, int bytes);

private:
    float detectAudio(const std::complex<float>& sample);
    float limitAudio(float sample);

    nco_crcf m_oscillator;
    firdecim_crcf m_first;
    firdecim_crcf m_second;
    firdecim_crcf m_third;
    firfilt_cccf m_usb;
    firfilt_cccf m_lsb;
    firinterp_rrrf m_interpolator;
    firfilt_cccf m_filter = nullptr;
    float m_voicePower = 0.0f;
    float m_outputPeak = 0.0f;
    std::vector<std::complex<float>> m_iq;
    std::vector<std::complex<float>> m_at192k;
    std::vector<std::complex<float>> m_at48k;
    std::vector<std::complex<float>> m_at12k;
    std::vector<float> m_detected;
    std::vector<float> m_audio;
};
