#include "ft-receive.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStringList>
#include <QtEndian>

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

namespace
{
constexpr qint64 samplesPerMs = 12;
constexpr qint64 stallTolerance = 350 * samplesPerMs;
}

FtReceiver::FtReceiver(QObject* parent)
    : QObject(parent), m_program(QCoreApplication::applicationDirPath() + QStringLiteral("/ft8-decoder"))
{
    m_deadline.setSingleShot(true);
    m_deadline.setInterval(45000);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        m_timedOut = true;
        m_decoder.kill();
    });
    connect(&m_decoder, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &FtReceiver::finished);
    connect(&m_decoder, &QProcess::errorOccurred, this, [this](QProcess::ProcessError problem) {
        if (problem != QProcess::FailedToStart)
        {
            return;
        }
        m_deadline.stop();
        if (m_running && m_running->generation == m_generation)
        {
            emit error(QStringLiteral("FT-Decoder konnte nicht gestartet werden."));
        }
        m_running.reset();
        m_pending.reset();
    });
    m_decoder.setStandardErrorFile(QProcess::nullDevice());
}

FtReceiver::~FtReceiver()
{
    disconnect(&m_decoder, nullptr, this, nullptr);
    if (m_decoder.state() != QProcess::NotRunning)
    {
        m_decoder.kill();
        m_decoder.waitForFinished(1000);
    }
}

bool FtReceiver::available() const
{
    const QFileInfo file(m_program);
    return file.isFile() && file.isExecutable();
}

void FtReceiver::select(std::optional<FtMode> mode, qint64 dialHz)
{
    if (m_mode == mode && m_dialHz == dialHz)
    {
        return;
    }
    m_mode = mode;
    m_dialHz = dialHz;
    ++m_generation;
    breakStream();
    m_pending.reset();
    if (m_running && m_decoder.state() != QProcess::NotRunning)
    {
        m_decoder.kill();
    }
}

void FtReceiver::breakStream()
{
    m_nextTick = 0;
    m_slot.reset();
}

void FtReceiver::push(std::span<const float> audio, qint64 endUtcMs)
{
    if (!m_mode || !m_dialHz || audio.empty())
    {
        return;
    }
    // The Pi stream has a sample counter but no UTC timestamp. Estimate the
    // first block's arrival in UTC, then follow samples through network jitter.
    const qint64 estimated = endUtcMs * samplesPerMs - static_cast<qint64>(audio.size());
    if (!m_nextTick || std::abs(estimated - m_nextTick) > stallTolerance)
    {
        breakStream();
        m_nextTick = estimated;
    }

    const auto slotSamples = static_cast<qint64>(ft_slot_samples(*m_mode));
    const auto decodeSamples = ft_decode_samples(*m_mode);
    for (const float sample : audio)
    {
        const auto tick = m_nextTick++;
        if (tick % slotSamples == 0)
        {
            m_slot = Slot{m_generation, *m_mode, m_dialHz, tick / samplesPerMs, false, {}};
            m_slot->audio.reserve(decodeSamples);
        }
        if (!m_slot)
        {
            continue;
        }
        auto& slot = *m_slot;
        slot.audio.push_back(sample);
        if (slot.mode == FtMode::ft8 && slot.audio.size() == ft8_early_samples)
        {
            auto early = slot;
            early.early = true;
            enqueue(std::move(early));
        }
        if (slot.audio.size() == decodeSamples)
        {
            enqueue(std::move(slot));
            m_slot.reset();
        }
    }
}

void FtReceiver::enqueue(Slot slot)
{
    // Retain at most one waiting decode so reception cannot build up a backlog.
    m_pending = std::move(slot);
    launchPending();
}

void FtReceiver::launchPending()
{
    if (m_running || !m_pending)
    {
        return;
    }
    m_running = std::move(m_pending);
    m_pending.reset();
    auto& slot = *m_running;
    const auto utc = QDateTime::fromMSecsSinceEpoch(slot.utcMs, Qt::UTC);
    const auto label = utc.toString(QStringLiteral("HHmmss")) +
        (slot.utcMs % 1000 ? QStringLiteral(".5") : QString());
    QStringList args{QStringLiteral("--format"), QStringLiteral("f32le"),
                     QStringLiteral("--utc"), label, QStringLiteral("--depth"),
                     slot.early ? QStringLiteral("2") : QStringLiteral("1")};
    if (slot.early)
    {
        args.append(QStringLiteral("--early-ft8"));
    }
    if (slot.mode == FtMode::ft4)
    {
        args.append({QStringLiteral("--mode"), QStringLiteral("FT4")});
    }
    args.append(QStringLiteral("-"));
    float peak = 0.0f;
    for (const float sample : slot.audio)
    {
        peak = std::max(peak, std::abs(sample));
    }
    const float gain = peak > 0.0f ? 0.8f / peak : 1.0f;
    QByteArray pcm(static_cast<int>(slot.audio.size() * sizeof(float)), Qt::Uninitialized);
    for (std::size_t i = 0; i < slot.audio.size(); ++i)
    {
        qToLittleEndian(std::bit_cast<quint32>(slot.audio[i] * gain), pcm.data() + i * sizeof(float));
    }
    slot.audio.clear();
    m_timedOut = false;
    m_decoder.start(m_program, args);
    m_decoder.write(pcm);
    m_decoder.closeWriteChannel();
    m_deadline.start();
}

void FtReceiver::finished(int code, QProcess::ExitStatus status)
{
    m_deadline.stop();
    const auto output = m_decoder.readAllStandardOutput();
    const auto slot = std::exchange(m_running, std::nullopt);
    if (slot && slot->generation == m_generation)
    {
        if (status == QProcess::NormalExit && code == 0)
        {
            for (const auto& line : output.split('\n'))
            {
                auto message = QJsonDocument::fromJson(line).object();
                if (message.value(QStringLiteral("type")) != QStringLiteral("decode"))
                {
                    continue;
                }
                message.insert(QStringLiteral("type"), QStringLiteral("ft-decode"));
                message.insert(QStringLiteral("mode"), QString::fromLatin1(ft_mode_name(slot->mode)));
                message.insert(QStringLiteral("dial_hz"), static_cast<double>(slot->dialHz));
                message.insert(QStringLiteral("slot_utc"), slot->utcMs / 1000.0);
                emit decoded(message);
            }
        }
        else
        {
            emit error(m_timedOut ? QStringLiteral("FT-Dekodierung hat zu lange gedauert.")
                                 : QStringLiteral("FT-Dekodierung ist fehlgeschlagen."));
        }
    }
    launchPending();
}
