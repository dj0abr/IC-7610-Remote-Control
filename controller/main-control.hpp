#pragma once

#include "icom-lan.hpp"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <memory>
#include <optional>

class MainControl : public QObject
{
    Q_OBJECT

public:
    explicit MainControl(QObject* parent = nullptr);

public slots:
    void start(const QString& address, quint64 session);
    void stop();
    void tune(qint64 hz, quint64 revision);

signals:
    void frequencyChanged(quint64 session, quint64 revision, qint64 hz);
    void statusChanged(quint64 session, quint64 revision, const QString& text);

private:
    struct TuneRequest
    {
        qint64 hz;
        quint64 revision;
    };

    void updateRadio();
    qint64 writeFrequency(const TuneRequest& request);

    QTimer* m_timer;
    QString m_address;
    QElapsedTimer m_clock;
    std::unique_ptr<icom::LanRadio> m_radio;
    std::optional<TuneRequest> m_requested;
    quint64 m_session = 0;
    quint64 m_completedRevision = 0;
    qint64 m_lastFrequency = 0;
    qint64 m_nextRead = 0;
    qint64 m_retryAfter = 0;
};
