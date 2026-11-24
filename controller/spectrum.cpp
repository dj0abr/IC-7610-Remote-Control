#include "spectrum.hpp"

#include <QString>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace
{
Band makeBand(const QString& id, const QString& label, int low, int high, int center = 0)
{
    const int margin = std::clamp((high - low) / 20, 5000, 25000);
    const int displayLow = low - margin;
    if (center == 0)
    {
        center = displayLow - 15000;
    }
    return {id, label, low, high, displayLow, high + margin, center};
}
}

const std::vector<Band>& bands()
{
    static const std::vector<Band> result{
        makeBand(QStringLiteral("160m"), QStringLiteral("160 m"), 1810000, 2000000),
        makeBand(QStringLiteral("80m"), QStringLiteral("80 m"), 3500000, 3800000),
        makeBand(QStringLiteral("60m"), QStringLiteral("60 m"), 5351500, 5366500),
        makeBand(QStringLiteral("40m"), QStringLiteral("40 m"), 7000000, 7200000),
        makeBand(QStringLiteral("30m"), QStringLiteral("30 m"), 10100000, 10150000),
        makeBand(QStringLiteral("20m"), QStringLiteral("20 m"), 14000000, 14350000),
        makeBand(QStringLiteral("17m"), QStringLiteral("17 m"), 18068000, 18168000),
        makeBand(QStringLiteral("15m"), QStringLiteral("15 m"), 21000000, 21450000),
        makeBand(QStringLiteral("12m"), QStringLiteral("12 m"), 24890000, 24990000),
        makeBand(QStringLiteral("10m-low"), QStringLiteral("10 m · 28–29 MHz"),
                 28000000, 29000000, 28500000),
        makeBand(QStringLiteral("10m-high"), QStringLiteral("10 m · 29–30 MHz"),
                 29000000, 30000000, 29500000),
        makeBand(QStringLiteral("cb"), QStringLiteral("CB · 27 MHz"),
                 26565000, 27405000, 26620000),
    };
    return result;
}

Spectrum::Spectrum()
    : m_input(fftwf_alloc_complex(size)),
      m_output(fftwf_alloc_complex(size)),
      m_window(size)
{
    if (!m_input || !m_output)
    {
        return;
    }

    double windowSum = 0;
    for (int i = 0; i < size; ++i)
    {
        const double angle = 2 * std::numbers::pi * i / (size - 1);
        m_window[i] = static_cast<float>(0.5 - 0.5 * std::cos(angle));
        windowSum += m_window[i];
    }

    const double reference = 32768.0 * windowSum;
    m_normalization = static_cast<float>(1.0 / (reference * reference));
    m_plan = fftwf_plan_dft_1d(size, m_input, m_output, FFTW_FORWARD, FFTW_ESTIMATE);
}

Spectrum::~Spectrum()
{
    if (m_plan)
    {
        fftwf_destroy_plan(m_plan);
    }
    fftwf_free(m_input);
    fftwf_free(m_output);
}

bool Spectrum::ready() const
{
    return m_plan != nullptr;
}

float Spectrum::power(int bin) const
{
    const auto& value = m_output[bin < 0 ? bin + size : bin];
    return value[0] * value[0] + value[1] * value[1];
}

int Spectrum::level(float power) const
{
    return static_cast<int>(std::lround(100.0f * std::log10(std::max(power * m_normalization, 1.0e-16f))));
}

void Spectrum::transform(const QByteArray& samples)
{
    const auto* data = reinterpret_cast<const uchar*>(samples.constData());
    for (int i = 0; i < size; ++i)
    {
        const float weight = m_window[i];
        m_input[i][0] = qFromLittleEndian<qint16>(data + 4 * i) * weight;
        m_input[i][1] = qFromLittleEndian<qint16>(data + 4 * i + 2) * weight;
    }
    fftwf_execute(m_plan);
}

QJsonArray Spectrum::wideLevels(const Band& band, int centerHz, int sampleRate) const
{
    constexpr int columns = 960;
    const double binHz = static_cast<double>(sampleRate) / size;
    const double left = (band.displayLowHz - centerHz) / binHz;
    const double step = (band.displayHighHz - band.displayLowHz) / (columns * binHz);
    const int lastUsable = std::min(size / 2 - 1, static_cast<int>(usableHalfWidthHz / binHz));

    QJsonArray levels;
    for (int column = 0; column < columns; ++column)
    {
        const int begin = std::max(-lastUsable, static_cast<int>(std::ceil(left + column * step)));
        const int end = std::min(lastUsable + 1, static_cast<int>(std::ceil(left + (column + 1) * step)));
        float strongest = 0;
        for (int bin = begin; bin < end; ++bin)
        {
            strongest = std::max(strongest, power(bin));
        }
        levels.append(level(strongest));
    }
    return levels;
}

QJsonArray Spectrum::zoomLevels(int offsetHz, int halfSpanHz, int sampleRate) const
{
    const double binHz = static_cast<double>(sampleRate) / size;
    const int middle = static_cast<int>(std::lround(offsetHz / binHz));
    const int halfBins = static_cast<int>(halfSpanHz / binHz);
    const int lastUsable = std::min(size / 2 - 1, static_cast<int>(usableHalfWidthHz / binHz));
    QJsonArray levels;
    for (int bin = middle - halfBins; bin <= middle + halfBins; ++bin)
    {
        levels.append(level(std::abs(bin) <= lastUsable ? power(bin) : 0));
    }

    return levels;
}
