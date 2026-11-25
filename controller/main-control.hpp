#pragma once

#include "icom-lan.hpp"

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <array>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>

class MainControl : public QObject
{
    Q_OBJECT

public:
    enum RadioSetting { RfPower, Compressor, Preamp, RadioSettingCount };

    explicit MainControl(QObject* parent = nullptr);
    // These methods only update input under the mutex and can run in the backend thread.
    void pttInput(quint64 session, quint32 id, const QString& action, bool latched);
    void txAudioInput(quint64 session, quint32 id, QByteArray pcm);
    void cancelPtt();

public slots:
    void start(const QString& address, quint64 session);
    void stop();
    void tune(qint64 hz, quint64 revision);
    void setRadioSetting(quint64 session, int index, int value);
    void bandChanged(quint64 session);

signals:
    void frequencyChanged(quint64 session, quint64 revision, qint64 hz);
    void statusChanged(quint64 session, quint64 revision, const QString& text);
    void radioSettingChanged(quint64 session, int index, int value, const QString& text);
    void rxMeterChanged(quint64 session, int level);
    void txMetersChanged(quint64 session, int power, int swr);
    void txChanged(quint64 session, const QString& phase, bool ready, const QString& text);

private:
    struct TuneRequest
    {
        qint64 hz;
        quint64 revision;
    };

    void updateRadio();
    struct SettingState
    {
        std::optional<int> requested;
        int value = -1;
        qint64 nextRead = 0;
        QString text;
    };

    void updateRadioSettings(qint64 now);
    void applyMicGain();
    qint64 writeFrequency(const TuneRequest& request);
    using Clock = std::chrono::steady_clock;

    struct PttRequest
    {
        quint32 id = 0;
        bool latched = false;
    };

    struct TxInput
    {
        quint64 session = 0;
        PttRequest ptt;
        Clock::time_point lease;
        Clock::time_point lastAudio;
        std::deque<QByteArray> frames;
    };

    PttRequest currentPtt();
    void cancelPtt(quint32 id);
    bool processTx(Clock::time_point now, bool settingWritePending);
    void finishTx(const QString& text = {});
    void updateTxMeters(Clock::time_point now, std::chrono::milliseconds timeout = icom::txMeterTimeout);
    void reportTx(const QString& phase, bool ready, const QString& text = {});

    QTimer* m_timer;
    QString m_address;
    QElapsedTimer m_clock;
    std::unique_ptr<icom::LanRadio> m_radio;
    std::optional<TuneRequest> m_requested;
    std::array<SettingState, RadioSettingCount> m_radioSettings{};
    std::optional<qint64> m_micGainDue;
    quint64 m_session = 0;
    quint64 m_completedRevision = 0;
    qint64 m_lastFrequency = 0;
    qint64 m_nextRead = 0;
    qint64 m_retryAfter = 0;
    qint64 m_nextPttRead = 0;
    qint64 m_nextRxMeter = 0;
    bool m_externalTx = false;
    quint32 m_ownedTx = 0;
    Clock::time_point m_txDeadline;
    Clock::time_point m_nextAudio;
    Clock::time_point m_nextTxStatus;
    Clock::time_point m_nextTxMeter;
    std::optional<int> m_pendingPo;
    std::mutex m_txMutex;
    TxInput m_input;
    QString m_txPhase;
    QString m_txText;
    bool m_txReady = false;
};
