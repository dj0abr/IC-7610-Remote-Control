#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <liquid.h>
#include <memory>
#include <type_traits>

struct RxFilterSettings
{
    static constexpr int minimumHz = 100;
    static constexpr int maximumHz = 4000;
    static constexpr int minimumBandwidthHz = 500;

    double lowHz = minimumHz;
    double highHz = maximumHz;

    bool valid() const;
    bool operator==(const RxFilterSettings&) const = default;
};

class RxAudioFilter
{
public:
    static constexpr unsigned int sampleRate = 12000;

    RxAudioFilter();
    bool ready() const { return bool(m_filter); }
    bool set(RxFilterSettings settings);
    void reset();
    std::complex<float> execute(std::complex<float> sample);

private:
    static constexpr unsigned int taps = 513;
    static constexpr unsigned int fadeSamples = sampleRate / 50;

    struct FirDeleter
    {
        void operator()(firfilt_cccf filter) const { firfilt_cccf_destroy(filter); }
    };
    using Fir = std::unique_ptr<std::remove_pointer_t<firfilt_cccf>, FirDeleter>;
    static Fir makeFilter(RxFilterSettings settings);

    RxFilterSettings m_settings;
    Fir m_filter;
    Fir m_replacement;
    Fir m_pending;
    std::array<std::complex<float>, taps - 1> m_history{};
    std::size_t m_historyPosition = 0;
    std::size_t m_historyCount = 0;
    unsigned int m_fadePosition = 0;
};
