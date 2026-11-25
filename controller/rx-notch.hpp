#pragma once

#include "rx-audio-filter.hpp"

#include <fftw3.h>

class RxNotch
{
public:
    enum class Mode { Off, Manual, Auto };

    static constexpr int minimumHz = RxFilterSettings::minimumHz;
    static constexpr int maximumHz = RxFilterSettings::maximumHz;
    static constexpr int defaultHz = 1000;

    RxNotch();
    ~RxNotch();
    RxNotch(const RxNotch&) = delete;
    RxNotch& operator=(const RxNotch&) = delete;

    bool autoAvailable() const { return m_plan != nullptr; }
    void set(Mode mode, int manualHz);
    void setPassband(RxFilterSettings passband);
    void clearDetection();
    void reset();
    int detectedHz() const { return m_detectedHz; }
    std::complex<float> execute(std::complex<float> sample);

private:
    static constexpr int sampleRate = RxAudioFilter::sampleRate;
    static constexpr int fftSize = 2048;
    static constexpr int hop = fftSize / 4;
    static constexpr int fadeSamples = sampleRate / 50;

    struct IirDeleter
    {
        void operator()(iirfilt_crcf filter) const { iirfilt_crcf_destroy(filter); }
    };
    using Iir = std::unique_ptr<std::remove_pointer_t<iirfilt_crcf>, IirDeleter>;
    static Iir makeFilter(int frequency);
    static std::complex<float> filterSample(const Iir& filter, std::complex<float> sample);

    void analyze();
    int findTone() const;
    void followTone(int candidate);
    void startTransition(int frequency);

    Mode m_mode = Mode::Off;
    RxFilterSettings m_passband;
    int m_manualHz = defaultHz;
    int m_detectedHz = 0;
    int m_candidateHz = 0;
    int m_candidateFrames = 0;
    int m_missingFrames = 0;
    std::array<float, fftSize> m_window{};
    std::array<float, fftSize> m_history{};
    std::array<float, fftSize> m_fftInput{};
    fftwf_complex m_fftOutput[fftSize / 2 + 1]{};
    fftwf_plan m_plan = nullptr;
    int m_writePosition = 0;
    int m_untilAnalysis = fftSize;

    Iir m_filter;
    Iir m_nextFilter;
    int m_filterHz = 0;
    int m_nextFilterHz = 0;
    int m_warmupRemaining = 0;
    int m_fadeRemaining = 0;
};
