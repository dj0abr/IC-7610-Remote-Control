#pragma once

#include "icom-lan.hpp"

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <array>
#include <atomic>
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
    void scheduleDigitalTx(quint64 session, quint64 id, qint64 dialHz, qint64 audioUtcMs, QByteArray pcm);
    void cancelDigitalTx(quint64 id) { m_cancelDigitalId.store(id); }

public slots:
    void start(const QString& address, quint64 session);
    void stop();
    void tune(qint64 lowHz, qint64 highHz, quint64 revision);
    void setRadioSetting(quint64 session, int index, int value);
    void setFtRadioSettings(quint64 session, bool active);
    void bandChanged(quint64 session);

signals:
    void frequencyChanged(quint64 session, quint64 revision, qint64 hz);
    void statusChanged(quint64 session, quint64 revision, const QString& text);
    void radioSettingChanged(quint64 session, int index, int value, const QString& text);
    void rxMeterChanged(quint64 session, int level);
    void txMetersChanged(quint64 session, int power, int swr);
    void txChanged(quint64 session, const QString& phase, bool ready, const QString& text);
    void digitalTxChanged(quint64 session, quint64 id, const QString& phase,
                          const QString& result, const QString& text);

private:
    struct TuneRequest
    {
        qint64 lowHz;
        qint64 highHz;
        quint64 revision;

        bool matches(qint64 frequency) const { return frequency >= lowHz && frequency <= highHz; }
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
    int applyFtCompressor();
    void applyFtUsbFilter();
    void applyMicGain();
    qint64 writeFrequency(const TuneRequest& request);
    using Clock = std::chrono::steady_clock;

    struct DigitalTx
    {
        quint64 id;
        qint64 dialHz;
        qint64 audioUtcMs;
        Clock::time_point audioAt;
        QByteArray pcm;
        int position = 0;
        bool keyed = false;
    };
    bool processDigitalTx(Clock::time_point now, bool settingWritePending);
    void finishDigitalTx(const QString& result, const QString& text = {});

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
    bool m_ftRadioSettingsActive = false;
    std::optional<int> m_compressorBeforeFt;
    struct UsbFilter
    {
        int preset;
        int widthCode;
        std::array<int, 2> twinPbt;
    };
    std::optional<UsbFilter> m_usbFilterBeforeFt;
    qint64 m_nextFtFilterRead = 0;
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
    std::optional<DigitalTx> m_digitalTx;
    std::atomic<quint64> m_cancelDigitalId{0};
    QString m_txPhase;
    QString m_txText;
    bool m_txReady = false;
};
