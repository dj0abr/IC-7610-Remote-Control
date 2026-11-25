#pragma once

#include "ft_mode.hpp"

#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>

#include <optional>
#include <span>
#include <vector>

class FtReceiver : public QObject
{
    Q_OBJECT

public:
    explicit FtReceiver(QObject* parent = nullptr);
    ~FtReceiver() override;
    bool available() const;
    void select(std::optional<FtMode> mode, qint64 dialHz);
    void breakStream();
    void push(std::span<const float> audio, qint64 endUtcMs);

signals:
    void decoded(const QJsonObject& message);
    void error(const QString& text);

private:
    struct Slot
    {
        quint64 generation;
        FtMode mode;
        qint64 dialHz;
        qint64 utcMs;
        bool early;
        std::vector<float> audio;
    };

    void enqueue(Slot slot);
    void launchPending();
    void finished(int code, QProcess::ExitStatus status);

    QProcess m_decoder;
    QTimer m_deadline;
    QString m_program;
    std::optional<FtMode> m_mode;
    qint64 m_dialHz = 0;
    quint64 m_generation = 0;
    qint64 m_nextTick = 0;
    std::optional<Slot> m_slot;
    std::optional<Slot> m_pending;
    std::optional<Slot> m_running;
    bool m_timedOut = false;
};
