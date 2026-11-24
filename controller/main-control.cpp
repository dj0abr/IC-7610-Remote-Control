#include "main-control.hpp"

#include <exception>

MainControl::MainControl(QObject* parent)
    : QObject(parent),
      m_timer(new QTimer(this))
{
    m_timer->setInterval(80);
    connect(m_timer, &QTimer::timeout, this, &MainControl::updateRadio);
}

void MainControl::start(const QString& address, quint64 session)
{
    stop();
    m_address = address;
    m_session = session;
    m_completedRevision = 0;
    m_clock.start();
    m_nextRead = 0;
    m_retryAfter = 0;
    m_timer->start();
    emit statusChanged(m_session, 0, QStringLiteral("MAIN: LAN-CI-V verbinden …"));
}

void MainControl::stop()
{
    m_timer->stop();
    m_radio.reset();
    m_requested.reset();
    m_lastFrequency = 0;
}

void MainControl::tune(qint64 hz, quint64 revision)
{
    if (m_timer->isActive())
    {
        m_requested = TuneRequest{hz, revision};
    }
}

qint64 MainControl::writeFrequency(const TuneRequest& request)
{
    try
    {
        return static_cast<qint64>(m_radio->setMainFrequency(static_cast<std::uint64_t>(request.hz)));
    }
    catch (const icom::CivRejected& error)
    {
        emit statusChanged(m_session, request.revision, QString::fromUtf8(error.what()));
        return 0;
    }
}

void MainControl::updateRadio()
{
    const qint64 now = m_clock.elapsed();
    if (!m_radio && now < m_retryAfter)
    {
        return;
    }

    try
    {
        const bool newSession = !m_radio;
        if (newSession)
        {
            auto radio = std::make_unique<icom::LanRadio>(false, m_address.toStdString());
            radio->connectDefault();
            m_radio = std::move(radio);
        }

        m_radio->service();
        if (!m_requested && now < m_nextRead)
        {
            return;
        }

        const auto requested = m_requested;
        qint64 frequency = 0;
        if (requested)
        {
            frequency = writeFrequency(*requested);
            m_completedRevision = requested->revision;
            m_requested.reset();
            if (frequency == 0)
            {
                // Read the actual setting again after an explicit rejection.
                m_lastFrequency = 0;
                m_nextRead = 0;
                return;
            }
        }
        else
        {
            frequency = static_cast<qint64>(m_radio->mainFrequency());
        }

        m_nextRead = m_clock.elapsed() + 300;
        if (frequency != m_lastFrequency || requested)
        {
            m_lastFrequency = frequency;
            emit frequencyChanged(m_session, m_completedRevision, frequency);
        }
        if (requested)
        {
            const QString status = frequency == requested->hz
                ? QStringLiteral("MAIN-Frequenz am IC-7610 bestätigt")
                : QStringLiteral("IC-7610 meldet abweichende MAIN-Frequenz");
            emit statusChanged(m_session, m_completedRevision, status);
        }
        else if (newSession)
        {
            emit statusChanged(m_session, m_completedRevision, QStringLiteral("MAIN über LAN-CI-V verbunden"));
        }
    }
    catch (const std::exception& error)
    {
        m_radio.reset();
        m_lastFrequency = 0;
        m_nextRead = 0;
        m_retryAfter = m_clock.elapsed() + 1500;
        const quint64 revision = m_requested ? m_requested->revision : m_completedRevision;
        emit statusChanged(m_session, revision, QStringLiteral("MAIN: ") + QString::fromUtf8(error.what()));
    }
}
