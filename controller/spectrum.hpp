#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QObject>
#include <QString>
#include <fftw3.h>
#include <vector>

struct Band
{
    QString id;
    QString label;
    int lowHz;
    int highHz;
    int displayLowHz;
    int displayHighHz;
    int centerHz;
};

const std::vector<Band>& bands();

class Spectrum : public QObject
{
public:
    static constexpr int size = 131072;
    static constexpr int bytes = size * 4;
    static constexpr int usableHalfWidthHz = 830000;

    Spectrum();
    ~Spectrum() override;
    Spectrum(const Spectrum&) = delete;
    Spectrum& operator=(const Spectrum&) = delete;

    struct Frames
    {
        QByteArray wide;
        QByteArray zoom;
    };

    bool ready() const;
    Frames render(const QByteArray& samples, const Band& band, int centerHz, int sampleRate,
                  qint64 mainHz, int zoomHalfSpanHz);

private:
    void transform(const QByteArray& samples);
    QJsonArray wideLevels(const Band& band, int centerHz, int sampleRate) const;
    QJsonArray zoomLevels(int offsetHz, int halfSpanHz, int sampleRate) const;
    float power(int bin) const;
    int level(float power) const;

    fftwf_complex* m_input = nullptr;
    fftwf_complex* m_output = nullptr;
    fftwf_plan m_plan = nullptr;
    std::vector<float> m_window;
    float m_normalization = 0;
};
