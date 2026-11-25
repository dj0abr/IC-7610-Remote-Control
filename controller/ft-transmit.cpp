#include "ft-transmit.hpp"

#include "ft8_bridge.h"
#include "icom-lan.hpp"

#include <QtEndian>
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <liquid.h>
#include <memory>
#include <type_traits>
#include <vector>

QByteArray encodeFtAudio(FtMode mode, const QString& message, int audioHz, int level)
{
    const auto normalized = message.simplified().toUpper().toLatin1();
    if (normalized.isEmpty())
    {
        return {};
    }
    std::vector<std::int16_t> wave(ft_wave_samples(mode));
    std::array<char, 37> encoded{};
    const int length = ft_encode_wave(normalized.constData(), normalized.size(),
                                      static_cast<float>(audioHz), static_cast<int>(mode),
                                      encoded.data(), wave.data());
    if (length <= 0 || normalized != QByteArray(encoded.data(), length))
    {
        return {};
    }

    constexpr int inputCount = 3;
    constexpr int outputCount = 4;
    using Resampler = std::unique_ptr<std::remove_pointer_t<rresamp_rrrf>, decltype(&rresamp_rrrf_destroy)>;
    const Resampler resampler(rresamp_rrrf_create_kaiser(outputCount, inputCount, 16, 0.48f, 80.0f),
                              rresamp_rrrf_destroy);
    if (!resampler)
    {
        return {};
    }
    rresamp_rrrf_set_scale(resampler.get(), 1.0f);
    const int bytes = static_cast<int>(wave.size() / inputCount * outputCount * sizeof(qint16));
    QByteArray pcm((bytes + icom::txAudioBytes - 1) / icom::txAudioBytes * icom::txAudioBytes, '\0');
    std::array<float, inputCount> input{};
    std::array<float, outputCount> output{};
    const float gain = level / 100.0f;
    int position = 0;
    for (std::size_t i = 0; i < wave.size(); i += inputCount)
    {
        for (int j = 0; j < inputCount; ++j)
        {
            input[j] = static_cast<float>(wave[i + j]);
        }
        rresamp_rrrf_execute(resampler.get(), input.data(), output.data());
        for (const float sample : output)
        {
            const auto value = static_cast<qint16>(std::clamp(std::lround(sample * gain), -32768L, 32767L));
            qToLittleEndian(value, pcm.data() + position);
            position += sizeof(value);
        }
    }
    return pcm;
}
