#include "spectrum.hpp"
#include "iq-utc-clock.hpp"
#include "main-control.hpp"
#include "ssb-demod.hpp"

#include <libft8/libft8.hpp>

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QDir>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <deque>
#include <csignal>
#include <limits>
#include <memory>
#include <numeric>
#include <span>
#include <vector>
#include <utility>

namespace
{
constexpr quint16 bridgePort = 50020;
constexpr quint16 piPort = 50010;
constexpr int maxPiBlock = 262144;
constexpr int zoomHalfSpanHz = 30000;
constexpr int spectrumFps = 20;
constexpr int audioFrameSamples = SsbDemodulator::audioRate / 50;
constexpr qint64 streamQueueLimit = 128 * 1024;
constexpr qint64 mainStepHz = 50;
constexpr qint64 iqPerFtSample = SsbDemodulator::inputRate / ft8::sample_rate;
static_assert(SsbDemodulator::inputRate % ft8::sample_rate == 0);
static_assert(RxAudioFilter::sampleRate == ft8::sample_rate);
volatile std::sig_atomic_t stopRequested = 0;

void requestStop(int)
{
    stopRequested = 1;
}

using Sideband = SsbDemodulator::Sideband;
using NotchMode = RxNotch::Mode;

const QStringList notchModes{QStringLiteral("off"), QStringLiteral("manual"), QStringLiteral("auto")};

std::optional<ft8::Mode> parseFtMode(const QString& mode)
{
    if (mode == QStringLiteral("FT8"))
    {
        return ft8::Mode::FT8;
    }
    if (mode == QStringLiteral("FT4"))
    {
        return ft8::Mode::FT4;
    }
    return std::nullopt;
}

qint64 ftStandardFrequency(const Band& band, ft8::Mode mode)
{
    struct Frequencies
    {
        const char* bandId;
        int ft8;
        int ft4;
    };
    static constexpr Frequencies frequencies[]{
        {"160m", 1840000, 0},
        {"80m", 3573000, 3575000},
        {"60m", 5357000, 5357000},
        {"40m", 7074000, 7047500},
        {"30m", 10136000, 10140000},
        {"20m", 14074000, 14080000},
        {"17m", 18100000, 18104000},
        {"15m", 21074000, 21140000},
        {"12m", 24915000, 24919000},
        {"10m-low", 28074000, 28180000},
    };
    for (const auto& entry : frequencies)
    {
        if (band.id == QLatin1String(entry.bandId))
        {
            return mode == ft8::Mode::FT4 ? entry.ft4 : entry.ft8;
        }
    }
    return 0;
}

QString ftModeName(ft8::Mode mode)
{
    return mode == ft8::Mode::FT4 ? QStringLiteral("FT4") : QStringLiteral("FT8");
}

QString ftAutoName(ft8::AutoMode automatic)
{
    switch (automatic)
    {
    case ft8::AutoMode::CallCQ: return QStringLiteral("call-cq");
    case ft8::AutoMode::AnswerCQ: return QStringLiteral("answer-cq");
    case ft8::AutoMode::Off: return QStringLiteral("off");
    }
    return {};
}

QString ftStateName(ft8::State state)
{
    switch (state)
    {
    case ft8::State::Receiving: return QStringLiteral("receiving");
    case ft8::State::CallingCQ: return QStringLiteral("calling-cq");
    case ft8::State::WaitingForReport: return QStringLiteral("report");
    case ft8::State::WaitingForRogerReport: return QStringLiteral("roger-report");
    case ft8::State::WaitingForAcknowledgment: return QStringLiteral("acknowledgment");
    case ft8::State::Closing: return QStringLiteral("closing");
    case ft8::State::SendingFreeText: return QStringLiteral("free-text");
    }
    return {};
}

QString ftOutcomeName(ft8::Outcome outcome)
{
    switch (outcome)
    {
    case ft8::Outcome::Completed: return QStringLiteral("completed");
    case ft8::Outcome::Aborted: return QStringLiteral("aborted");
    case ft8::Outcome::Timeout: return QStringLiteral("timeout");
    case ft8::Outcome::RadioError: return QStringLiteral("radio-error");
    case ft8::Outcome::ClockChanged: return QStringLiteral("clock-changed");
    }
    return {};
}

qint64 utcMilliseconds(ft8::Time utc)
{
    return std::chrono::floor<std::chrono::milliseconds>(utc.time_since_epoch()).count();
}

int ftMaxTxOffset(ft8::Mode mode)
{
    return mode == ft8::Mode::FT4 ? 3930 : 3950;
}

QString sidebandKey(const Band* band)
{
    return band ? QStringLiteral("audio/bandSidebands/") + band->id : QStringLiteral("audio/sideband");
}

Sideband parseSideband(const QString& mode, Sideband fallback = Sideband::Lsb)
{
    if (mode == QStringLiteral("USB"))
    {
        return Sideband::Usb;
    }
    if (mode == QStringLiteral("LSB"))
    {
        return Sideband::Lsb;
    }
    return fallback;
}

QString sidebandMode(Sideband sideband)
{
    return sideband == Sideband::Usb ? QStringLiteral("USB") : QStringLiteral("LSB");
}

QString ipv4(const QString& text)
{
    const QString input = text.trimmed();
    const QHostAddress address(input);
    if (address.protocol() != QAbstractSocket::IPv4Protocol || address.toString() != input)
    {
        return {};
    }
    return input;
}

QJsonObject describeBand(const Band& band)
{
    return {
        {QStringLiteral("id"), band.id},
        {QStringLiteral("label"), band.label},
        {QStringLiteral("low_hz"), band.lowHz},
        {QStringLiteral("high_hz"), band.highHz},
        {QStringLiteral("display_low_hz"), band.displayLowHz},
        {QStringLiteral("display_high_hz"), band.displayHighHz},
        {QStringLiteral("center_hz"), band.centerHz},
    };
}

class Backend : public QObject
{
public:
    Backend(const QString& hostOverride, const QString& qrzApiKey, int pttLeadMs)
    {
        m_host = m_settings.value(QStringLiteral("network/pi"), QStringLiteral("192.168.10.140")).toString();
        if (!hostOverride.isEmpty())
        {
            m_host = hostOverride;
        }
        m_radioAddress = m_settings.value(QStringLiteral("network/radio"),
                                          QString::fromLatin1(icom::radioAddress)).toString();
        m_volume = std::clamp(m_settings.value(QStringLiteral("audio/volume"), 35).toInt(), 0, 100);
        m_defaultSideband = parseSideband(m_settings.value(sidebandKey(nullptr)).toString());
        m_sideband = m_defaultSideband;
        const RxFilterSettings storedFilter{
            m_settings.value(QStringLiteral("audio/filter_low_hz"), RxFilterSettings::minimumHz).toDouble(),
            m_settings.value(QStringLiteral("audio/filter_high_hz"), RxFilterSettings::maximumHz).toDouble(),
        };
        if (storedFilter.valid() && m_demodulator.setRxFilter(storedFilter))
        {
            m_rxFilter = storedFilter;
        }
        m_nrPercent = std::clamp(m_settings.value(QStringLiteral("audio/nr_percent"), 0).toInt(), 0, 100);
        m_demodulator.setNoiseReduction(m_nrPercent);
        const auto mode = notchModes.indexOf(m_settings.value(QStringLiteral("audio/notch_mode")).toString());
        if (mode >= 0 && (mode != static_cast<int>(NotchMode::Auto) || m_demodulator.autoNotchAvailable()))
        {
            m_notchMode = static_cast<NotchMode>(mode);
        }
        m_notchHz = std::clamp(m_settings.value(QStringLiteral("audio/notch_hz"), RxNotch::defaultHz).toInt(),
                               RxNotch::minimumHz, RxNotch::maximumHz);
        m_demodulator.setNotch(m_notchMode, m_notchHz);
        m_pi.setReadBufferSize(2 * 1024 * 1024);
        m_retry.setSingleShot(true);
        m_retry.setInterval(1000);
        m_deadline.setSingleShot(true);
        m_deadline.setInterval(6000);
        connect(&m_server, &QTcpServer::newConnection, this, &Backend::acceptGui);
        connect(&m_pi, &QTcpSocket::readyRead, this, &Backend::readPi);
        connect(&m_pi, &QTcpSocket::disconnected, this, &Backend::piDisconnected);
        connect(&m_pi, &QTcpSocket::errorOccurred, this, &Backend::piError);
        connect(&m_retry, &QTimer::timeout, this, &Backend::connectPi);
        connect(&m_deadline, &QTimer::timeout, this, &Backend::piTimeout);
        const auto dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        m_ftConfig.adif_log_path = QDir(dataDir).absoluteFilePath(QStringLiteral("ft4-ft8.adi")).toStdString();
        if (!QDir().mkpath(dataDir))
        {
            qWarning("FT-Logbuch: Datenverzeichnis konnte nicht angelegt werden.");
        }
        m_ftConfig.callsign = m_settings.value(QStringLiteral("ft/callsign")).toString().toStdString();
        m_ftConfig.locator = m_settings.value(QStringLiteral("ft/locator")).toString().toStdString();
        m_ftConfig.tx_hz = std::clamp(m_settings.value(QStringLiteral("ft/tx_hz"), 1500).toInt(), 200, 3950);
        m_ftLevel = std::clamp(m_settings.value(QStringLiteral("ft/tx_level"), 25).toInt(), 1, 100);
        m_ftConfig.ptt_lead_ms = pttLeadMs;
        m_ftConfig.qrz_api_key = qrzApiKey.trimmed().toStdString();
        m_ftPoll.setInterval(5);
        m_ftPoll.setTimerType(Qt::PreciseTimer);
        connect(&m_ftPoll, &QTimer::timeout, this, &Backend::pollFt);
        qRegisterMetaType<ft8::RadioReport>();

        m_mainControl->moveToThread(&m_mainThread);
        connect(&m_mainThread, &QThread::finished, m_mainControl, &QObject::deleteLater);
        connect(m_mainControl, &MainControl::frequencyChanged, this, &Backend::updateMainFrequency);
        connect(m_mainControl, &MainControl::statusChanged, this, &Backend::updateMainStatus);
        connect(m_mainControl, &MainControl::sidebandChanged, this,
                [this](quint64 session, bool usb, const QString& error) {
            if (m_gui && session == m_mainSession)
            {
                send({{QStringLiteral("type"), QStringLiteral("main-sideband")},
                      {QStringLiteral("mode"), usb ? QStringLiteral("USB") : QStringLiteral("LSB")},
                      {QStringLiteral("text"), error}});
            }
        });
        connect(m_mainControl, &MainControl::radioSettingChanged, this, &Backend::updateRadioSetting);
        connect(m_mainControl, &MainControl::rxMeterChanged, this, &Backend::updateRxMeter);
        connect(m_mainControl, &MainControl::txMetersChanged, this, &Backend::updateTxMeters);
        connect(m_mainControl, &MainControl::txChanged, this, &Backend::updateTx);
        connect(m_mainControl, &MainControl::digitalTxReported, this, &Backend::reportFtRadio);
        m_mainThread.start();
    }

    ~Backend() override
    {
        m_mainControl->cancelPtt();
        stopFt();
        if (m_gui)
        {
            disconnect(m_gui, nullptr, this, nullptr);
            m_gui->abort();
        }
        disconnect(&m_pi, nullptr, this, nullptr);
        m_pi.abort();
        m_spectrumThread.quit();
        m_spectrumThread.wait();
        QMetaObject::invokeMethod(m_mainControl, "stop", Qt::BlockingQueuedConnection);
        m_mainThread.quit();
        m_mainThread.wait();
    }

    bool start()
    {
        auto spectrum = std::make_unique<Spectrum>();
        if (!spectrum->ready())
        {
            qCritical("FFT konnte nicht initialisiert werden.");
            return false;
        }
        if (!m_demodulator.ready())
        {
            qCritical("LiquidDSP konnte nicht initialisiert werden.");
            return false;
        }
        if (!m_server.listen(QHostAddress::LocalHost, bridgePort))
        {
            qCritical().noquote() << m_server.errorString();
            return false;
        }
        m_spectrum = spectrum.release();
        m_spectrum->moveToThread(&m_spectrumThread);
        connect(&m_spectrumThread, &QThread::finished, m_spectrum, &QObject::deleteLater);
        m_spectrumThread.setObjectName(QStringLiteral("kmcontrol-fft"));
        m_spectrumThread.start();
        qInfo("Web-Bridge-Schnittstelle auf 127.0.0.1:50020");
        if (!m_ftConfig.qrz_api_key.empty())
        {
            qInfo("Automatischer QRZ-Logbuch-Upload aktiv.");
        }
        return true;
    }

private:
    void send(const QJsonObject& object)
    {
        if (!m_gui)
        {
            return;
        }
        QByteArray message = QJsonDocument(object).toJson(QJsonDocument::Compact);
        message.append('\n');
        m_gui->write(message);
    }

    void state(const QString& phase, const QString& text, const Band* band = nullptr)
    {
        QJsonObject message{
            {QStringLiteral("type"), QStringLiteral("state")},
            {QStringLiteral("phase"), phase},
            {QStringLiteral("text"), text},
        };
        if (band)
        {
            message.insert(QStringLiteral("band"), band->id);
        }
        send(message);
    }

    void acceptGui()
    {
        while (m_server.hasPendingConnections())
        {
            QTcpSocket* client = m_server.nextPendingConnection();
            if (m_gui)
            {
                client->write("{\"type\":\"state\",\"phase\":\"error\",\"text\":\"Backend bereits belegt.\"}\n");
                connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
                client->disconnectFromHost();
                continue;
            }
            m_gui = client;
            m_gui->setReadBufferSize(8192);
            connect(m_gui, &QTcpSocket::readyRead, this, &Backend::readGui);
            connect(m_gui, &QTcpSocket::disconnected, this, &Backend::guiDisconnected);
            QJsonArray catalog;
            for (const Band& band : bands())
            {
                catalog.append(describeBand(band));
            }
            send({{QStringLiteral("type"), QStringLiteral("bands")},
                  {QStringLiteral("version"), 1},
                  {QStringLiteral("zoom_half_span_hz"), zoomHalfSpanHz},
                  {QStringLiteral("bands"), catalog}});
            state(QStringLiteral("idle"), QStringLiteral("Band auswählen"));
            sendSetup();
            sendRxFilter();
            sendFtState();
            startMainControl();
        }
    }

    void readGui()
    {
        while (m_gui && m_gui->canReadLine())
        {
            const auto request = QJsonDocument::fromJson(m_gui->readLine(4096)).object();
            const QString operation = request.value(QStringLiteral("op")).toString();
            if (operation == QStringLiteral("ptt") || operation == QStringLiteral("tx_audio"))
            {
                handleTxInput(request, operation == QStringLiteral("ptt"));
                continue;
            }
            if (operation.startsWith(QStringLiteral("ft_")))
            {
                handleFtCommand(operation, request);
                continue;
            }
            if ((m_rxPaused || m_clientPttId) &&
                (operation == QStringLiteral("band") || operation == QStringLiteral("set_main") ||
                 operation == QStringLiteral("step_main") || operation == QStringLiteral("setup")))
            {
                if (operation == QStringLiteral("setup"))
                {
                    send({{QStringLiteral("type"), QStringLiteral("setup-result")},
                          {QStringLiteral("request_id"), request.value(QStringLiteral("request_id"))},
                          {QStringLiteral("ok"), false},
                          {QStringLiteral("text"), QStringLiteral("Adresse kann während TX nicht geändert werden.")}});
                }
                continue;
            }
            if (operation == QStringLiteral("set_main"))
            {
                requestMain(request);
                continue;
            }
            if (operation == QStringLiteral("step_main"))
            {
                stepMain(request.value(QStringLiteral("steps")).toInt());
                continue;
            }
            if (operation == QStringLiteral("get_setup"))
            {
                sendSetup();
                continue;
            }
            if (operation == QStringLiteral("setup"))
            {
                applySetup(request);
                continue;
            }
            if (operation == QStringLiteral("sideband"))
            {
                const auto band = request.value(QStringLiteral("band"));
                if (band.isUndefined() || (m_desired && band.toString() == m_desired->id))
                {
                    selectSideband(request.value(QStringLiteral("mode")).toString());
                }
                continue;
            }
            if (operation == QStringLiteral("rx_filter"))
            {
                setRxFilter(request);
                continue;
            }
            if (operation == QStringLiteral("volume"))
            {
                setVolume(request.value(QStringLiteral("percent")).toInt(-1));
                continue;
            }
            if (operation == QStringLiteral("rf_power") || operation == QStringLiteral("compressor") ||
                operation == QStringLiteral("preamp"))
            {
                requestRadioSetting(operation, request);
                continue;
            }
            if (operation == QStringLiteral("audio_output"))
            {
                setAudioOutput(request.value(QStringLiteral("output")).toString());
                continue;
            }
            const QString id = request.value(QStringLiteral("id")).toString();
            const Band* band = nullptr;
            for (const Band& item : bands())
            {
                if (item.id == id)
                {
                    band = &item;
                    break;
                }
            }
            if (operation != QStringLiteral("band") || !band)
            {
                state(QStringLiteral("error"), QStringLiteral("Unbekannter Bandbefehl"));
                continue;
            }
            const bool newBand = m_desired != band;
            if (m_desired && newBand)
            {
                QMetaObject::invokeMethod(m_mainControl, "bandChanged", Qt::QueuedConnection, Q_ARG(quint64, m_mainSession));
            }
            m_desired = band;
            m_sideband = parseSideband(m_settings.value(sidebandKey(band)).toString(), m_defaultSideband);
            setRadioSideband();
            send({{QStringLiteral("type"), QStringLiteral("sideband")},
                  {QStringLiteral("band"), band->id},
                  {QStringLiteral("mode"), sidebandMode(m_sideband)}});
            if (newBand)
            {
                stopFt();
                tuneMainToBand(*band);
            }
            if (m_pi.state() == QAbstractSocket::UnconnectedState)
            {
                connectPi();
            }
            else if (m_header && !m_pending)
            {
                tune();
            }
        }
        if (m_gui && m_gui->bytesAvailable() >= 4096)
        {
            m_gui->abort();
        }
    }

    void sendSetup()
    {
        send({{QStringLiteral("type"), QStringLiteral("setup")},
              {QStringLiteral("radio_ip"), m_radioAddress},
              {QStringLiteral("pi_ip"), m_host},
              {QStringLiteral("mode"), sidebandMode(m_sideband)},
              {QStringLiteral("volume"), m_volume}});
    }

    void applySetup(const QJsonObject& request)
    {
        const auto radioValue = request.value(QStringLiteral("radio_ip"));
        const auto piValue = request.value(QStringLiteral("pi_ip"));
        const bool hasRadio = !radioValue.isUndefined();
        const bool hasPi = !piValue.isUndefined();
        const QString radio = hasRadio ? ipv4(radioValue.toString()) : m_radioAddress;
        const QString pi = hasPi ? ipv4(piValue.toString()) : m_host;
        const auto requestId = request.value(QStringLiteral("request_id"));
        if ((!hasRadio && !hasPi) || (hasRadio && radio.isEmpty()) || (hasPi && pi.isEmpty()))
        {
            send({{QStringLiteral("type"), QStringLiteral("setup-result")},
                  {QStringLiteral("request_id"), requestId},
                  {QStringLiteral("ok"), false},
                  {QStringLiteral("text"), QStringLiteral("Eine gültige IPv4-Adresse eingeben.")}});
            return;
        }

        if (hasRadio)
        {
            m_settings.setValue(QStringLiteral("network/radio"), radio);
        }
        if (hasPi)
        {
            m_settings.setValue(QStringLiteral("network/pi"), pi);
        }

        if (m_radioAddress != radio)
        {
            stopFt();
            m_radioAddress = radio;
            setMainFrequency(0, false);
            sendMainStatus(QStringLiteral("MAIN: LAN-CI-V verbinden …"));
            startMainControl();
            if (m_desired)
            {
                tuneMainToBand(*m_desired);
            }
        }
        if (m_host != pi)
        {
            stopFt();
            m_host = pi;
            m_pi.abort();
            m_deadline.stop();
            resetPi();
            connectPi();
        }
        m_settings.sync();
        sendSetup();
        const bool saved = m_settings.status() == QSettings::NoError;
        const QString result = saved ? QString() : QStringLiteral("Adresse aktiv. Dauerhaftes Speichern fehlgeschlagen.");
        send({{QStringLiteral("type"), QStringLiteral("setup-result")},
              {QStringLiteral("request_id"), requestId},
              {QStringLiteral("ok"), saved},
              {QStringLiteral("text"), result}});
    }

    void selectSideband(const QString& mode)
    {
        if (mode != QStringLiteral("USB") && mode != QStringLiteral("LSB"))
        {
            return;
        }
        const auto sideband = parseSideband(mode);
        if (!m_desired)
        {
            m_defaultSideband = sideband;
        }
        m_settings.setValue(sidebandKey(m_desired), mode);
        m_settings.sync();
        if (m_settings.status() != QSettings::NoError)
        {
            qWarning("Seitenband konnte nicht gespeichert werden.");
        }
        if (m_sideband != sideband)
        {
            m_sideband = sideband;
            if (!m_ftMode)
            {
                updateDemodulator();
            }
        }
        setRadioSideband();
    }

    void setRadioSideband()
    {
        QMetaObject::invokeMethod(m_mainControl, "setVoiceSideband", Qt::QueuedConnection,
                                  Q_ARG(quint64, m_mainSession), Q_ARG(bool, m_sideband == Sideband::Usb));
    }

    int detectedNotchHz() const
    {
        return m_demodulating ? m_demodulator.detectedNotchHz() : 0;
    }

    static QJsonValue notchFrequencyValue(int frequency)
    {
        return frequency ? QJsonValue(frequency) : QJsonValue();
    }

    void publishNotchFrequency()
    {
        const int frequency = detectedNotchHz();
        if (frequency != m_reportedNotchHz)
        {
            m_reportedNotchHz = frequency;
            send({{QStringLiteral("type"), QStringLiteral("notch-frequency")},
                  {QStringLiteral("hz"), notchFrequencyValue(frequency)}});
        }
    }

    void sendRxFilter(const QJsonValue& requestId = {}, const QString& text = {})
    {
        send({{QStringLiteral("type"), QStringLiteral("rx-filter")},
              {QStringLiteral("low_hz"), m_rxFilter.lowHz},
              {QStringLiteral("high_hz"), m_rxFilter.highHz},
              {QStringLiteral("nr_percent"), m_ftMode ? 0 : m_nrPercent},
              {QStringLiteral("notch_mode"), notchModes.at(static_cast<int>(m_ftMode ? NotchMode::Off : m_notchMode))},
              {QStringLiteral("notch_hz"), m_notchHz},
              {QStringLiteral("notch_detected_hz"), notchFrequencyValue(detectedNotchHz())},
              {QStringLiteral("ft_bypass"), m_ftMode.has_value()},
              {QStringLiteral("request_id"), requestId},
              {QStringLiteral("text"), text}});
    }

    void setRxFilter(const QJsonObject& request)
    {
        const RxFilterSettings settings{
            request.value(QStringLiteral("low_hz")).toDouble(-1),
            request.value(QStringLiteral("high_hz")).toDouble(-1),
        };
        // A delayed slider update from the FT view must not store the bypass values.
        const bool effectsEditable = !m_ftMode && !request.value(QStringLiteral("ft_bypass")).toBool();
        const int percent = effectsEditable ? request.value(QStringLiteral("nr_percent")).toInt(-1) : m_nrPercent;
        const auto requestId = request.value(QStringLiteral("request_id"));
        if (!settings.valid() || percent < 0 || percent > 100)
        {
            sendRxFilter(requestId, QStringLiteral("Filtergrenzen 100–4000 Hz, mindestens 500 Hz Abstand. NR 0–100 %."));
            return;
        }
        const auto mode = effectsEditable && request.contains(QStringLiteral("notch_mode"))
            ? notchModes.indexOf(request.value(QStringLiteral("notch_mode")).toString())
            : static_cast<int>(m_notchMode);
        const int notchHz = effectsEditable && request.contains(QStringLiteral("notch_hz"))
            ? request.value(QStringLiteral("notch_hz")).toInt(-1) : m_notchHz;
        if (mode < 0 || notchHz < RxNotch::minimumHz || notchHz > RxNotch::maximumHz)
        {
            sendRxFilter(requestId, QStringLiteral("Ungültiger Notch-Modus oder Frequenz außerhalb 100–4000 Hz."));
            return;
        }
        if (mode == static_cast<int>(NotchMode::Auto) && !m_demodulator.autoNotchAvailable())
        {
            sendRxFilter(requestId, QStringLiteral("Auto-Notch ist derzeit nicht verfügbar."));
            return;
        }
        if (!m_demodulator.setRxFilter(settings))
        {
            sendRxFilter(requestId, QStringLiteral("Der RX-Filter konnte nicht eingestellt werden."));
            return;
        }
        if (mode != static_cast<int>(m_notchMode) || notchHz != m_notchHz)
        {
            m_notchMode = static_cast<NotchMode>(mode);
            m_notchHz = notchHz;
            m_demodulator.setNotch(m_notchMode, notchHz);
            m_settings.setValue(QStringLiteral("audio/notch_mode"), notchModes.at(mode));
            m_settings.setValue(QStringLiteral("audio/notch_hz"), notchHz);
        }
        if (settings != m_rxFilter || percent != m_nrPercent)
        {
            m_rxFilter = settings;
            m_nrPercent = percent;
            m_demodulator.setNoiseReduction(m_ftMode ? 0 : percent);
            m_settings.setValue(QStringLiteral("audio/filter_low_hz"), settings.lowHz);
            m_settings.setValue(QStringLiteral("audio/filter_high_hz"), settings.highHz);
            m_settings.setValue(QStringLiteral("audio/nr_percent"), percent);
            updateDemodulator(false);
        }
        sendRxFilter(requestId);
        publishNotchFrequency();
    }

    void setVolume(int percent)
    {
        if (percent < 0 || percent > 100)
        {
            return;
        }
        m_volume = percent;
        m_settings.setValue(QStringLiteral("audio/volume"), m_volume);
    }

    void setAudioOutput(const QString& output)
    {
        if (output != QStringLiteral("browser") && output != QStringLiteral("off"))
        {
            return;
        }
        const bool enabled = output == QStringLiteral("browser");
        if (enabled != m_streamAudio)
        {
            m_streamAudio = enabled;
            updateDemodulator(false);
        }
    }

    bool ftCanTransmit() const
    {
        return m_ft && m_ftDemodulating && m_mainConfirmed && m_txReady && !m_rxPaused && !m_clientPttId &&
            !m_ftRadio && m_active && m_active == m_desired && m_active->id != QStringLiteral("cb") &&
            m_mainHz >= m_active->lowHz && m_mainHz + ftTxOffset() + 70 <= m_active->highHz;
    }

    int ftTxOffset() const
    {
        return m_ftMode ? std::min(m_ftConfig.tx_hz, ftMaxTxOffset(*m_ftMode)) : m_ftConfig.tx_hz;
    }

    void sendFtState()
    {
        const auto now = ft8::Time::clock::now();
        m_ftStateSent = std::chrono::steady_clock::now();
        send({{QStringLiteral("type"), QStringLiteral("ft-state")},
              {QStringLiteral("mode"), m_ftMode ? ftModeName(*m_ftMode) : QStringLiteral("off")},
              {QStringLiteral("available"), true},
              {QStringLiteral("ready"), bool(m_ft)},
              {QStringLiteral("tx_ready"), ftCanTransmit()},
              {QStringLiteral("armed"), m_ftArmed},
              {QStringLiteral("sending"), m_ftRadio && m_ftRadio->started},
              {QStringLiteral("busy"), m_ftState != ft8::State::Receiving || m_ftRadio.has_value()},
              {QStringLiteral("state"), ftStateName(m_ftState)},
              {QStringLiteral("auto"), ftAutoName(m_ftAuto)},
              {QStringLiteral("peer"), m_ftPeer},
              {QStringLiteral("retries"), static_cast<int>(m_ftRetries)},
              {QStringLiteral("callsign"), QString::fromStdString(m_ftConfig.callsign)},
              {QStringLiteral("locator"), QString::fromStdString(m_ftConfig.locator)},
              {QStringLiteral("tx_hz"), ftTxOffset()},
              {QStringLiteral("tx_level"), m_ftLevel},
              {QStringLiteral("utc_ms"), utcMilliseconds(now)},
              {QStringLiteral("slot_ms"), m_ftMode ? static_cast<int>(ft8::slot_duration(*m_ftMode).count()) : 0}});
    }

    void setVoiceEffectsForFt()
    {
        m_demodulator.setNoiseReduction(m_ftMode ? 0 : m_nrPercent);
        m_demodulator.setNotch(m_ftMode ? NotchMode::Off : m_notchMode, m_notchHz);
    }

    void setRadioSettingsForFt()
    {
        QMetaObject::invokeMethod(m_mainControl, "setFtRadioSettings", Qt::QueuedConnection,
                                  Q_ARG(quint64, m_mainSession), Q_ARG(bool, m_ftMode.has_value()));
    }

    void ftReply(const QJsonObject& request, bool ok, const QString& text = {})
    {
        send({{QStringLiteral("type"), QStringLiteral("ft-reply")},
              {QStringLiteral("request_id"), request.value(QStringLiteral("request_id"))},
              {QStringLiteral("ok"), ok},
              {QStringLiteral("text"), text}});
    }

    void selectFtMode(const QJsonObject& request)
    {
        const auto name = request.value(QStringLiteral("mode")).toString();
        const auto mode = parseFtMode(name);
        if (!mode && name != QStringLiteral("off"))
        {
            ftReply(request, false, QStringLiteral("Unbekannte FT-Betriebsart."));
            return;
        }
        if (mode != m_ftMode)
        {
            if (mode && (m_rxPaused || m_clientPttId || m_ftRadio))
            {
                ftReply(request, false, QStringLiteral("Betriebsart kann während TX nicht geändert werden."));
                return;
            }
            stopFt();
            const bool bypassChanged = m_ftMode.has_value() != mode.has_value();
            m_ftMode = mode;
            if (bypassChanged)
            {
                setVoiceEffectsForFt();
                setRadioSettingsForFt();
                sendRxFilter();
            }
            if (mode)
            {
                m_ftPoll.start();
                if (m_desired)
                {
                    const auto frequency = ftStandardFrequency(*m_desired, *mode);
                    if (frequency && (frequency != m_mainHz || !m_mainConfirmed))
                    {
                        tuneMain(frequency, frequency);
                    }
                }
            }
            else
            {
                m_ftPoll.stop();
            }
            updateDemodulator(bypassChanged);
            startFt();
        }
        sendFtState();
        ftReply(request, true);
    }

    void configureFt(const QJsonObject& request)
    {
        if (m_ftTxAuthorized || m_ftState != ft8::State::Receiving || m_ftRadio)
        {
            ftReply(request, false, QStringLiteral("Senden vor dem Ändern der FT-Einstellungen sperren."));
            return;
        }
        auto config = m_ftConfig;
        config.callsign = request.value(QStringLiteral("callsign")).toString().trimmed().toUpper().toStdString();
        config.locator = request.value(QStringLiteral("locator")).toString().trimmed().toUpper().toStdString();
        config.mode = m_ftMode.value_or(ft8::Mode::FT8);
        config.tx_hz = request.value(QStringLiteral("tx_hz")).toInt(-1);
        const int level = request.value(QStringLiteral("tx_level")).toInt(-1);
        try
        {
            config.validate();
            if (config.tx_hz > ftMaxTxOffset(config.mode) || level < 1 || level > 100)
            {
                throw std::invalid_argument("Ungültiger TX-Offset oder TX-Pegel.");
            }
            if (config.callsign != m_ftConfig.callsign)
            {
                const auto cq = ft8::encode(config.mode, "CQ " + config.callsign + " " + config.locator.substr(0, 4),
                                            config.tx_hz, config.callsign);
                if (cq.message.kind != ft8::Kind::CQ || cq.message.source != config.callsign)
                {
                    throw std::invalid_argument("Rufzeichen kann nicht in einem CQ übertragen werden.");
                }
            }
        }
        catch (const std::exception& error)
        {
            ftReply(request, false, QString::fromUtf8(error.what()));
            return;
        }
        if (m_ft && config.callsign == m_ftConfig.callsign && config.locator == m_ftConfig.locator)
        {
            if (config.tx_hz != ftTxOffset())
            {
                ft8::Command command(ft8::Command::Type::SetTxFrequency);
                command.tx_hz = config.tx_hz;
                if (!m_ft->command(command))
                {
                    ftReply(request, false, QStringLiteral("libft8 hat die Einstellung nicht angenommen."));
                    return;
                }
            }
        }
        else
        {
            stopFt();
        }
        m_ftConfig = std::move(config);
        m_ftLevel = level;
        m_settings.setValue(QStringLiteral("ft/callsign"), QString::fromStdString(m_ftConfig.callsign));
        m_settings.setValue(QStringLiteral("ft/locator"), QString::fromStdString(m_ftConfig.locator));
        m_settings.setValue(QStringLiteral("ft/tx_hz"), m_ftConfig.tx_hz);
        m_settings.setValue(QStringLiteral("ft/tx_level"), m_ftLevel);
        m_settings.sync();
        startFt();
        sendFtState();
        const bool saved = m_settings.status() == QSettings::NoError;
        ftReply(request, saved, saved ? QString() : QStringLiteral("FT-Einstellungen konnten nicht gespeichert werden."));
    }

    void handleFtCommand(const QString& operation, const QJsonObject& request)
    {
        if (operation == QStringLiteral("ft_mode"))
        {
            selectFtMode(request);
            return;
        }
        if (operation == QStringLiteral("ft_config"))
        {
            configureFt(request);
            return;
        }
        if (!m_ft)
        {
            ftReply(request, false, QStringLiteral("FT-Empfang ist nicht bereit."));
            return;
        }

        using Type = ft8::Command::Type;
        const auto submit = [&](bool queued) {
            ftReply(request, queued, queued ? QString() : QStringLiteral("libft8 hat den Befehl nicht angenommen."));
        };
        if (operation == QStringLiteral("ft_abort"))
        {
            cancelFtRadio();
            submit(m_ft->command(ft8::Command(Type::Abort)));
            return;
        }
        const auto requestedArm = operation == QStringLiteral("ft_arm")
            ? request.value(QStringLiteral("armed")) : QJsonValue();
        if (requestedArm.isBool() && !requestedArm.toBool())
        {
            submit(disarmFt());
            sendFtState();
            return;
        }
        const auto automatic = operation == QStringLiteral("ft_auto")
            ? request.value(QStringLiteral("auto")).toString() : QString();
        if (automatic == QStringLiteral("off"))
        {
            submit(m_ft->command(ft8::Command(Type::SetAuto)));
            return;
        }
        if (!ftCanTransmit())
        {
            ftReply(request, false, QStringLiteral("TX ist belegt oder MAIN ist nicht sendebereit."));
            return;
        }
        if (requestedArm.toBool())
        {
            const bool queued = m_ft->command(ft8::Command(Type::ArmTx));
            m_ftTxAuthorized = m_ftTxAuthorized || queued;
            submit(queued);
            return;
        }
        if (!m_ftTxAuthorized)
        {
            ftReply(request, false, QStringLiteral("FT-TX zuerst freigeben."));
            return;
        }
        if (automatic == QStringLiteral("call-cq") || automatic == QStringLiteral("answer-cq"))
        {
            ft8::Command command(Type::SetAuto);
            command.automatic = automatic == QStringLiteral("call-cq") ? ft8::AutoMode::CallCQ : ft8::AutoMode::AnswerCQ;
            submit(m_ft->command(command));
            return;
        }
        if (operation == QStringLiteral("ft_free_text"))
        {
            submit(m_ft->send_free_text(request.value(QStringLiteral("text")).toString().toStdString()));
            return;
        }

        const bool startQso = operation == QStringLiteral("ft_start_qso");
        if (!startQso && operation != QStringLiteral("ft_cq"))
        {
            ftReply(request, false, QStringLiteral("Ungültiger FT-Befehl."));
            return;
        }
        ft8::Command command(startQso ? Type::StartQso : Type::CallCQ);
        const auto requestedParity = request.value(QStringLiteral("parity"));
        if (!requestedParity.isUndefined())
        {
            const int parity = requestedParity.toInt(-1);
            if (parity != 0 && parity != 1)
            {
                ftReply(request, false, QStringLiteral("Ungültige Slotparität."));
                return;
            }
            command.tx_parity = parity;
        }
        if (startQso)
        {
            command.peer = request.value(QStringLiteral("peer")).toString().trimmed().toUpper().toStdString();
            const auto id = request.value(QStringLiteral("decode_id")).toString();
            if (!id.isEmpty())
            {
                const auto decodeId = id.toULongLong();
                // Match the exact decimal form emitted in ft-decode.
                const auto selected = QString::number(decodeId) == id
                    ? std::ranges::find_if(m_ftDecodes, [&](const auto& row) { return row.first == decodeId; })
                    : m_ftDecodes.end();
                if (selected == m_ftDecodes.end() || selected->second.message.source != command.peer)
                {
                    ftReply(request, false, QStringLiteral("Decode ist veraltet oder passt nicht zum Rufzeichen."));
                    return;
                }
                command.selected = selected->second;
            }
        }
        submit(m_ft->command(std::move(command)));
    }

    void startFt()
    {
        if (m_ft || !m_ftMode || m_mainHz <= 0 || !m_active || m_active != m_desired ||
            m_ftConfig.callsign.empty() || m_ftConfig.locator.empty())
        {
            return;
        }
        try
        {
            auto config = m_ftConfig;
            config.mode = *m_ftMode;
            config.tx_hz = ftTxOffset();
            config.dial_frequency_hz = m_mainHz;
            m_ft = std::make_unique<ft8::Transceiver>(std::move(config));
        }
        catch (const std::exception& error)
        {
            sendFtNotice(QStringLiteral("error"), QString::fromUtf8(error.what()));
        }
    }

    void stopFt()
    {
        m_ftTxAuthorized = false;
        cancelFtRadio();
        if (m_ft)
        {
            m_ft->stop();
            for (const auto& action : m_ft->take_radio_actions())
            {
                if (action.type == ft8::RadioAction::Type::Cancel)
                {
                    cancelFtRadio();
                }
            }
            for (const auto& event : m_ft->take_events())
            {
                if (event.type >= ft8::Event::Type::LogSaved && event.type <= ft8::Event::Type::QrzError)
                {
                    handleFtEvent(event);
                }
            }
            m_ft.reset();
        }
        m_ftDecodes.clear();
        m_ftAudioRejectionLogged = false;
        m_ftArmed = false;
        m_ftAuto = ft8::AutoMode::Off;
        m_ftState = ft8::State::Receiving;
        m_ftPeer.clear();
        m_ftRetries = 0;
    }

    bool disarmFt()
    {
        m_ftTxAuthorized = m_ftArmed = false;
        cancelFtRadio();
        return m_ft->command(ft8::Command(ft8::Command::Type::DisarmTx));
    }

    void cancelFtRadio()
    {
        if (m_ftRadio && !m_ftRadio->cancelled)
        {
            m_ftRadio->cancelled = true;
            m_mainControl->cancelDigitalTx(m_ftRadio->token);
        }
    }

    void handleFtRadio(const ft8::RadioAction& action)
    {
        using Type = ft8::RadioAction::Type;
        const auto& playback = action.tx;
        if (action.type == Type::Cancel)
        {
            if (m_ftRadio && m_ftRadio->id == playback.id)
            {
                cancelFtRadio();
            }
            return;
        }
        if (!m_ftTxAuthorized)
        {
            return;
        }
        if (!ftCanTransmit() || playback.mode != *m_ftMode ||
            !std::isfinite(playback.frequency_hz) || playback.frequency_hz < 200 ||
            playback.frequency_hz > ftMaxTxOffset(playback.mode) ||
            m_mainHz + playback.frequency_hz + 70 > m_active->highHz)
        {
            m_ft->report_radio({playback.id, ft8::RadioReport::Status::Failed, {}, "MAIN ist nicht sendebereit."});
            return;
        }
        const auto token = ++m_ftNextTxToken;
        m_ftRadio = FtRadio{token, playback.id};
        QMetaObject::invokeMethod(m_mainControl, [control = m_mainControl, session = m_mainSession,
                                  token, hz = m_mainHz, playback, level = m_ftLevel] {
            control->scheduleDigitalTx(session, token, hz, playback, level);
        }, Qt::QueuedConnection);
        sendFtState();
    }

    void reportFtRadio(quint64 session, quint64 token, const ft8::RadioReport& report)
    {
        if (session != m_mainSession || !m_ftRadio || m_ftRadio->token != token || m_ftRadio->id != report.id)
        {
            return;
        }
        if (m_ft && !m_ftRadio->cancelled)
        {
            if (!m_ft->report_radio(report))
            {
                disarmFt();
            }
            else if (report.status == ft8::RadioReport::Status::Failed)
            {
                m_ftTxAuthorized = m_ftArmed = false;
            }
        }
        if (report.status == ft8::RadioReport::Status::Started)
        {
            m_ftRadio->started = !m_ftRadio->cancelled;
        }
        else
        {
            m_ftRadio.reset();
        }
        sendFtState();
    }

    void sendFtNotice(const QString& kind, const QString& text, const QString& peer = {},
                      const QString& outcome = {})
    {
        send({{QStringLiteral("type"), QStringLiteral("ft-notice")},
              {QStringLiteral("kind"), kind}, {QStringLiteral("text"), text},
              {QStringLiteral("peer"), peer}, {QStringLiteral("outcome"), outcome}});
    }

    void handleFtEvent(const ft8::Event& event)
    {
        using Type = ft8::Event::Type;
        switch (event.type)
        {
        case Type::Rx:
        case Type::Tx:
        {
            const auto& message = event.rx ? event.rx->message : *event.tx;
            const auto utc = utcMilliseconds(event.utc);
            QJsonObject row{{QStringLiteral("type"), event.type == Type::Rx ? QStringLiteral("ft-decode") : QStringLiteral("ft-tx")},
                            {QStringLiteral("mode"), ftModeName(event.mode)},
                            {QStringLiteral("slot_utc_ms"), utc},
                            {QStringLiteral("audio_hz"), event.frequency_hz},
                            {QStringLiteral("dial_hz"), m_mainHz},
                            {QStringLiteral("text"), QString::fromStdString(message.text)},
                            {QStringLiteral("source"), QString::fromStdString(message.source)},
                            {QStringLiteral("destination"), QString::fromStdString(message.destination)},
                            {QStringLiteral("value"), QString::fromStdString(message.value)},
                            {QStringLiteral("cq"), message.kind == ft8::Kind::CQ}};
            if (event.rx)
            {
                const auto id = ++m_ftNextDecodeId;
                m_ftDecodes.emplace_back(id, *event.rx);
                if (m_ftDecodes.size() > 512)
                {
                    m_ftDecodes.pop_front();
                }
                row.insert(QStringLiteral("decode_id"), QString::number(id));
                row.insert(QStringLiteral("snr_db"), event.rx->snr_db);
                row.insert(QStringLiteral("dt_s"), event.rx->dt_seconds);
            }
            else if (event.audio_start)
            {
                row.insert(QStringLiteral("audio_start_ms"), utcMilliseconds(*event.audio_start));
            }
            send(row);
            break;
        }
        case Type::StateChanged:
            m_ftState = event.state;
            if (m_ftArmed && !event.armed)
            {
                m_ftTxAuthorized = false;
            }
            m_ftArmed = event.armed && m_ftTxAuthorized;
            m_ftAuto = event.automatic;
            m_ftPeer = QString::fromStdString(event.peer);
            m_ftRetries = event.retries;
            sendFtState();
            break;
        case Type::Finished:
        case Type::FreeTextFinished:
            sendFtNotice(event.type == Type::Finished ? QStringLiteral("qso") : QStringLiteral("free-text"),
                         QString::fromStdString(event.detail), QString::fromStdString(event.peer),
                         event.outcome ? ftOutcomeName(*event.outcome) : QString());
            break;
        case Type::LogSaved:
        case Type::LogError:
        case Type::QrzUploaded:
        case Type::QrzError:
        {
            const bool qrz = event.type == Type::QrzUploaded || event.type == Type::QrzError;
            const bool ok = event.type == Type::LogSaved || event.type == Type::QrzUploaded;
            sendFtNotice(qrz ? QStringLiteral("qrz") : QStringLiteral("adif"), QString::fromStdString(event.detail),
                         QString::fromStdString(event.peer), ok ? QStringLiteral("completed") : QStringLiteral("error"));
            if (!ok)
            {
                qWarning("FT-Logbuch: %s", event.detail.c_str());
            }
            break;
        }
        case Type::CommandRejected:
        case Type::SlotMissed:
        case Type::Radio:
            if (event.type == Type::Radio)
            {
                qWarning("FT-Funkgerät: %s", event.detail.c_str());
            }
            sendFtNotice(event.type == Type::SlotMissed ? QStringLiteral("slot") : QStringLiteral("error"),
                         QString::fromStdString(event.detail));
            break;
        }
    }

    void pollFt()
    {
        if (m_ft)
        {
            for (const auto& event : m_ft->take_events())
            {
                handleFtEvent(event);
            }
            for (const auto& action : m_ft->take_radio_actions())
            {
                handleFtRadio(action);
            }
        }
        if (std::chrono::steady_clock::now() - m_ftStateSent >= std::chrono::seconds(1))
        {
            sendFtState();
        }
    }

    void startMainControl()
    {
        m_mainControl->cancelPtt();
        cancelFtRadio();
        m_ftRadio.reset();
        m_clientPttId = 0;
        m_txReady = false;
        ++m_mainSession;
        m_mainRevision = 0;
        for (int index = 0; index < MainControl::RadioSettingCount; ++index)
        {
            updateRadioSetting(m_mainSession, index, -1, {});
        }
        QMetaObject::invokeMethod(m_mainControl, "start", Qt::QueuedConnection,
                                  Q_ARG(QString, m_radioAddress),
                                  Q_ARG(quint64, m_mainSession));
        if (m_desired)
        {
            setRadioSideband();
        }
        if (m_ftMode)
        {
            setRadioSettingsForFt();
        }
    }

    void handleTxInput(const QJsonObject& request, bool control)
    {
        const auto value = request.value(QStringLiteral("id")).toDouble(0);
        if (!std::isfinite(value) || value < 1 || value > std::numeric_limits<quint32>::max() ||
            std::floor(value) != value)
        {
            return;
        }
        const auto id = static_cast<quint32>(value);
        if (control)
        {
            const auto action = request.value(QStringLiteral("action")).toString();
            bool latched = false;
            if (action == QStringLiteral("start"))
            {
                const auto mode = request.value(QStringLiteral("mode")).toString(QStringLiteral("hold"));
                if (!m_txReady || m_rxPaused || m_clientPttId || m_ftTxAuthorized || m_ftRadio)
                {
                    return;
                }
                if (mode != QStringLiteral("hold") && mode != QStringLiteral("toggle"))
                {
                    return;
                }
                latched = mode == QStringLiteral("toggle");
                m_clientPttId = id;
            }
            else if (id != m_clientPttId ||
                     (action != QStringLiteral("stop") && action != QStringLiteral("keepalive")))
            {
                return;
            }
            else if (action == QStringLiteral("stop"))
            {
                m_clientPttId = 0;
            }
            m_mainControl->pttInput(m_mainSession, id, action, latched);
        }
        else if (id == m_clientPttId)
        {
            auto pcm = QByteArray::fromBase64(request.value(QStringLiteral("pcm")).toString().toLatin1(),
                                              QByteArray::AbortOnBase64DecodingErrors);
            if (pcm.size() == icom::txAudioBytes)
            {
                m_mainControl->txAudioInput(m_mainSession, id, std::move(pcm));
            }
        }
    }

    void requestRadioSetting(const QString& operation, const QJsonObject& request)
    {
        if (m_rxPaused || m_clientPttId)
        {
            return;
        }

        int index;
        int value;
        if (operation == QStringLiteral("rf_power"))
        {
            const int percent = request.value(QStringLiteral("percent")).toInt(-1);
            if (percent < 0 || percent > 100)
            {
                return;
            }
            index = MainControl::RfPower;
            value = (percent * icom::maxRfPowerLevel + 50) / 100;
        }
        else
        {
            index = operation == QStringLiteral("compressor") ? MainControl::Compressor : MainControl::Preamp;
            value = request.value(QStringLiteral("value")).toInt(-1);
        }
        QMetaObject::invokeMethod(m_mainControl, "setRadioSetting", Qt::QueuedConnection,
                                  Q_ARG(quint64, m_mainSession), Q_ARG(int, index), Q_ARG(int, value));
    }

    void updateRadioSetting(quint64 session, int index, int value, const QString& text)
    {
        if (session != m_mainSession)
        {
            return;
        }

        if (index == MainControl::RfPower)
        {
            const QJsonValue percent = value < 0 ? QJsonValue(QJsonValue::Null)
                : QJsonValue((value * 100 + icom::maxRfPowerLevel / 2) / icom::maxRfPowerLevel);
            send({{QStringLiteral("type"), QStringLiteral("rf-power")},
                  {QStringLiteral("percent"), percent},
                  {QStringLiteral("text"), text}});
            return;
        }
        const QString type = index == MainControl::Compressor ? QStringLiteral("compressor") : QStringLiteral("preamp");
        send({{QStringLiteral("type"), type},
              {QStringLiteral("value"), value < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(value)},
              {QStringLiteral("text"), text}});
    }

    void updateRxMeter(quint64 session, int level)
    {
        if (m_gui && session == m_mainSession)
        {
            send({{QStringLiteral("type"), QStringLiteral("rx-meter")},
                  {QStringLiteral("level"), level < 0 ? QJsonValue() : QJsonValue(level)}});
        }
    }

    void updateTxMeters(quint64 session, int power, int swr)
    {
        if (m_gui && session == m_mainSession)
        {
            send({{QStringLiteral("type"), QStringLiteral("tx-meters")},
                  {QStringLiteral("po"), power < 0 ? QJsonValue() : QJsonValue(power)},
                  {QStringLiteral("swr"), swr < 0 ? QJsonValue() : QJsonValue(swr)}});
        }
    }

    void updateTx(quint64 session, const QString& phase, bool ready, const QString& text)
    {
        if (!m_gui || session != m_mainSession)
        {
            return;
        }
        m_txReady = ready && phase == QStringLiteral("rx");
        if (m_ft && m_ftTxAuthorized && !m_txReady && !m_ftRadio)
        {
            disarmFt();
        }
        if (phase == QStringLiteral("rx") || phase == QStringLiteral("offline") ||
            phase == QStringLiteral("unknown") || !ready)
        {
            m_mainControl->cancelPtt();
            m_clientPttId = 0;
        }
        const bool paused = phase != QStringLiteral("rx") && phase != QStringLiteral("offline");
        const bool changed = paused != m_rxPaused;
        m_rxPaused = paused;
        send({{QStringLiteral("type"), QStringLiteral("tx")},
              {QStringLiteral("phase"), phase},
              {QStringLiteral("ready"), ready},
              {QStringLiteral("text"), text}});
        if (changed)
        {
            resetSpectrum();
            updateDemodulator();
            if (paused)
            {
                m_iqClock.resetOrigin();
                m_deadline.stop();
            }
            else if (m_pi.state() == QAbstractSocket::UnconnectedState)
            {
                connectPi();
            }
            else if (m_header && !m_pending && m_desired != m_active)
            {
                tune();
            }
            else
            {
                m_deadline.start();
            }
        }
    }

    void updateMainFrequency(quint64 session, quint64 revision, qint64 hz)
    {
        if (m_gui && session == m_mainSession && revision == m_mainRevision)
        {
            setMainFrequency(hz, true);
        }
    }

    void setMainFrequency(qint64 hz, bool confirmed)
    {
        const bool frequencyChanged = hz != m_mainHz;
        const bool confirmationChanged = confirmed != m_mainConfirmed;
        if (frequencyChanged)
        {
            stopFt();
        }
        else if (!confirmed && m_ft && m_ftTxAuthorized)
        {
            disarmFt();
        }
        m_mainConfirmed = confirmed;
        m_mainHz = hz;
        if (frequencyChanged || confirmationChanged)
        {
            updateDemodulator(frequencyChanged);
            startFt();
            sendFtState();
        }
        send({{QStringLiteral("type"), QStringLiteral("main")},
              {QStringLiteral("hz"), static_cast<double>(hz)},
              {QStringLiteral("confirmed"), confirmed}});
    }

    void updateMainStatus(quint64 session, quint64 revision, const QString& text)
    {
        if (m_gui && session == m_mainSession && revision == m_mainRevision)
        {
            sendMainStatus(text);
        }
    }

    void updateDemodulator(bool reset = true)
    {
        const qint64 offset = m_mainHz - m_center;
        const bool usable = !m_rxPaused && m_active && m_active == m_desired && m_mainHz > 0;
        const bool receiving = usable && m_streamAudio && std::abs(offset) + m_rxFilter.highHz <= Spectrum::usableHalfWidthHz;
        const bool ftReceiving = usable && m_ftMode && std::abs(offset) + RxFilterSettings::maximumHz <= Spectrum::usableHalfWidthHz;
        if (reset || receiving != m_demodulating)
        {
            m_audioCount = 0;
            if (m_streamAudio)
            {
                send({{QStringLiteral("type"), QStringLiteral("audio-reset")}});
            }
        }
        if (receiving || ftReceiving)
        {
            if (reset || (!m_demodulating && !m_ftDemodulating))
            {
                const auto sideband = m_ftMode ? Sideband::Usb : m_sideband;
                m_demodulator.tune(static_cast<int>(offset), sideband);
            }
            else
            {
                m_demodulator.setFrequencyOffset(static_cast<int>(offset));
                if (receiving && !m_demodulating)
                {
                    m_demodulator.resetVoice();
                }
            }
        }
        m_demodulating = receiving;
        m_ftDemodulating = ftReceiving;
        publishNotchFrequency();
    }

    void tuneMain(qint64 lowHz, qint64 highHz)
    {
        ++m_mainRevision;
        const qint64 frequency = m_mainHz >= lowHz && m_mainHz <= highHz ? m_mainHz : std::midpoint(lowHz, highHz);
        setMainFrequency(frequency, false);
        sendMainStatus(QStringLiteral("MAIN wird mit dem IC-7610 synchronisiert …"));
        QMetaObject::invokeMethod(m_mainControl, "tune", Qt::QueuedConnection,
                                  Q_ARG(qint64, lowHz), Q_ARG(qint64, highHz), Q_ARG(quint64, m_mainRevision));
    }

    void tuneMainToBand(const Band& band)
    {
        const auto frequency = m_ftMode ? ftStandardFrequency(band, *m_ftMode) : 0;
        if (frequency)
        {
            tuneMain(frequency, frequency);
        }
        else
        {
            tuneMain(band.lowHz, band.highHz);
        }
    }

    void requestMain(const QJsonObject& request)
    {
        const double hz = request.value(QStringLiteral("hz")).toDouble(-1);
        if (!m_active || !std::isfinite(hz) || std::floor(hz) != hz ||
            hz < m_active->displayLowHz || hz > m_active->displayHighHz)
        {
            sendMainStatus(QStringLiteral("MAIN-Frequenz liegt außerhalb des aktiven Spektrums."));
            return;
        }
        const auto frequency = static_cast<qint64>(hz);
        tuneMain(frequency, frequency);
    }

    void stepMain(int steps)
    {
        if (!m_active || m_mainHz < m_active->displayLowHz || m_mainHz > m_active->displayHighHz)
        {
            return;
        }
        const qint64 minSteps = (m_active->displayLowHz - m_mainHz) / mainStepHz;
        const qint64 maxSteps = (m_active->displayHighHz - m_mainHz) / mainStepHz;
        const qint64 allowedSteps = std::clamp<qint64>(steps, minSteps, maxSteps);
        if (allowedSteps != 0)
        {
            const qint64 frequency = m_mainHz + allowedSteps * mainStepHz;
            tuneMain(frequency, frequency);
        }
    }

    void sendMainStatus(const QString& text)
    {
        send({{QStringLiteral("type"), QStringLiteral("main-status")},
              {QStringLiteral("text"), text}});
    }

    void guiDisconnected()
    {
        QTcpSocket* client = m_gui;
        m_gui = nullptr;
        m_mainHz = 0;
        m_mainConfirmed = false;
        m_streamAudio = false;
        m_mainControl->cancelPtt();
        stopFt();
        m_ftMode.reset();
        m_ftPoll.stop();
        setVoiceEffectsForFt();
        m_clientPttId = 0;
        m_rxPaused = m_txReady = false;
        QMetaObject::invokeMethod(m_mainControl, "stop", Qt::QueuedConnection);
        m_desired = nullptr;
        m_sideband = m_defaultSideband;
        m_retry.stop();
        m_deadline.stop();
        m_pi.abort();
        resetPi();
        client->deleteLater();
    }

    void resetPi()
    {
        stopFt();
        m_data.clear();
        resetSpectrum();
        m_index = 0;
        m_iqClock = {};
        m_iqGap = false;
        m_header = false;
        m_active = nullptr;
        m_pending = nullptr;
        updateDemodulator();
    }

    void connectPi()
    {
        if (m_rxPaused || !m_gui || !m_desired || m_pi.state() != QAbstractSocket::UnconnectedState)
        {
            return;
        }
        m_retry.stop();
        resetPi();
        state(QStringLiteral("connecting"), QStringLiteral("Pi verbinden …"), m_desired);
        m_deadline.start();
        m_pi.connectToHost(m_host, piPort);
    }

    void piDisconnected()
    {
        resetPi();
        m_deadline.stop();
        if (m_gui && m_desired)
        {
            state(QStringLiteral("connecting"), QStringLiteral("Pi-Verbindung unterbrochen"), m_desired);
            m_retry.start();
        }
    }

    void piError(QAbstractSocket::SocketError)
    {
        if (m_gui && m_desired)
        {
            state(QStringLiteral("connecting"), QStringLiteral("Pi: ") + m_pi.errorString(), m_desired);
            m_deadline.stop();
            m_retry.start();
        }
    }

    void piTimeout()
    {
        fault(QStringLiteral("Keine Antwort vom Pi"));
    }

    void fault(const QString& text)
    {
        m_pi.abort();
        resetPi();
        m_deadline.stop();
        if (m_gui && m_desired)
        {
            state(QStringLiteral("error"), text, m_desired);
            m_retry.start();
        }
    }

    void tune()
    {
        if (m_rxPaused || !m_desired)
        {
            return;
        }
        if (m_center == m_desired->centerHz)
        {
            activate();
            return;
        }
        m_pending = m_desired;
        m_active = nullptr;
        resetSpectrum();
        updateDemodulator();
        state(QStringLiteral("tuning"), QStringLiteral("SUB wird eingestellt …"), m_pending);
        m_deadline.start();
        m_pi.write("SET SUB " + QByteArray::number(m_pending->centerHz) + '\n');
    }

    void activate()
    {
        resetSpectrum();
        m_samples.reserve(Spectrum::bytes);
        m_active = m_desired;
        updateDemodulator();
        startFt();
        state(QStringLiteral("live"), QStringLiteral("Live"), m_active);
    }

    bool header()
    {
        if (m_data.size() < 24)
        {
            return false;
        }
        const auto* data = reinterpret_cast<const uchar*>(m_data.constData());
        const auto rate = qFromBigEndian<quint32>(data + 8);
        const auto format = qFromBigEndian<quint32>(data + 12);
        const auto center = qFromBigEndian<quint64>(data + 16);
        if (!m_data.startsWith("IC76IQ01") || format != 1 || rate != SsbDemodulator::inputRate ||
            center < 30000 || center > 60000000)
        {
            fault(QStringLiteral("I/Q-Format des Pi wird nicht unterstützt"));
            return false;
        }
        m_data.remove(0, 24);
        m_rate = static_cast<int>(rate);
        m_center = static_cast<int>(center);
        m_header = true;
        m_deadline.stop();
        tune();
        return true;
    }

    bool control(const uchar* payload)
    {
        const auto status = qFromBigEndian<quint32>(payload);
        const auto center = qFromBigEndian<quint64>(payload + 4);
        if (!m_pending || center < 30000 || center > 60000000)
        {
            fault(QStringLiteral("Ungültige SUB-Antwort vom Pi"));
            return false;
        }
        m_deadline.stop();
        const Band* requested = m_pending;
        m_pending = nullptr;
        if (static_cast<int>(center) != m_center)
        {
            // Changing SUB restarts capture on the Pi and discards a settling block.
            m_iqClock.resetOrigin();
            m_center = static_cast<int>(center);
        }
        if (requested != m_desired)
        {
            tune();
        }
        else if (status == 0 && m_center == requested->centerHz)
        {
            activate();
        }
        else
        {
            QString error = QStringLiteral("Pi hat die SUB-Frequenz abgewiesen");
            if (status == 2)
            {
                error = QStringLiteral("SUB kann während des Sendens nicht umgestellt werden");
            }
            state(QStringLiteral("error"), error, requested);
        }
        return true;
    }

    void readPi()
    {
        m_data.append(m_pi.readAll());
        if (!m_header && !header())
        {
            return;
        }

        // Anchor the sample count at the end of the newest complete block in this
        // TCP read. Older blocks in the same read already spent time in the queue.
        const auto steadyBefore = std::chrono::steady_clock::now();
        const auto received = ft8::Time::clock::now();
        const auto steadyAfter = std::chrono::steady_clock::now();
        const auto clockReadTime = steadyAfter - steadyBefore;
        int completeBytes = 0;
        auto endIndex = m_index;
        while (m_data.size() - completeBytes >= 12)
        {
            const auto* frame = reinterpret_cast<const uchar*>(m_data.constData() + completeBytes);
            const auto index = qFromBigEndian<quint64>(frame);
            const auto length = qFromBigEndian<quint32>(frame + 8);
            if (index != endIndex || length > maxPiBlock || length % 4 != 0)
            {
                fault(QStringLiteral("I/Q-Rahmen des Pi ist ungültig"));
                return;
            }
            const int payload = length == 0 ? 12 : static_cast<int>(length);
            if (m_data.size() - completeBytes < 12 + payload)
            {
                break;
            }
            endIndex += length / 4;
            completeBytes += 12 + payload;
        }
        // Ignore clock comparisons if this thread was descheduled while reading the clocks.
        if (endIndex != m_index && clockReadTime <= std::chrono::milliseconds(5) &&
            m_iqClock.observeSystemClock(received, steadyBefore + clockReadTime / 2))
        {
            m_iqGap = true;
            m_ftDecodes.clear();
            if (m_ft && m_ftTxAuthorized)
            {
                disarmFt();
                sendFtNotice(QStringLiteral("error"), QStringLiteral("Die Systemuhr wurde verstellt. FT-TX wurde gesperrt."));
            }
        }

        const auto endAudioSample = static_cast<qint64>(endIndex / iqPerFtSample);
        int offset = 0;
        while (offset < completeBytes)
        {
            const auto* frame = reinterpret_cast<const uchar*>(m_data.constData() + offset);
            const auto length = qFromBigEndian<quint32>(frame + 8);
            const int payload = length == 0 ? 12 : static_cast<int>(length);
            if (length == 0)
            {
                if (!control(frame + 12))
                {
                    return;
                }
            }
            else
            {
                samples(reinterpret_cast<const char*>(frame + 12), payload, m_index, received, endAudioSample);
                m_index += length / 4;
            }
            offset += 12 + payload;
        }
        m_data.remove(0, offset);
        if (!m_rxPaused && m_active && !m_pending)
        {
            m_deadline.start();
        }
    }

    void sendBrowserAudio(std::span<const float> audio)
    {
        for (const float sample : audio)
        {
            const auto pcm = static_cast<qint16>(std::clamp(std::lround(sample * 32768), -32768L, 32767L));
            qToLittleEndian(pcm, m_audioFrame.data() + m_audioCount * 2);
            if (++m_audioCount == audioFrameSamples)
            {
                if (m_gui->bytesToWrite() <= streamQueueLimit)
                {
                    send({{QStringLiteral("type"), QStringLiteral("audio")},
                          {QStringLiteral("pcm"), QString::fromLatin1(m_audioFrame.toBase64())}});
                }
                m_audioCount = 0;
            }
        }
    }

    void resetSpectrum()
    {
        m_samples.clear();
        ++m_spectrumGeneration;
    }

    void queueSpectrum()
    {
        // Keep at most one frame in flight so slow consumers cannot delay live spectra.
        if (m_spectrumBusy || !m_gui || m_gui->bytesToWrite() > streamQueueLimit)
        {
            return;
        }
        m_spectrumBusy = true;
        QMetaObject::invokeMethod(m_spectrum, [this, spectrum = m_spectrum, samples = m_samples,
                                             band = m_active, center = m_center, rate = m_rate,
                                             mainHz = m_mainHz, generation = m_spectrumGeneration] {
            auto frames = spectrum->render(samples, *band, center, rate, mainHz, zoomHalfSpanHz);
            QMetaObject::invokeMethod(this, [this, frames = std::move(frames), generation, mainHz] {
                m_spectrumBusy = false;
                if (generation != m_spectrumGeneration || !m_gui || m_gui->bytesToWrite() > streamQueueLimit)
                {
                    return;
                }
                m_gui->write(frames.wide);
                if (mainHz == m_mainHz && !frames.zoom.isEmpty())
                {
                    m_gui->write(frames.zoom);
                }
            }, Qt::QueuedConnection);
        }, Qt::QueuedConnection);
    }

    void samples(const char* data, int length, quint64 firstIq, ft8::Time received, qint64 endAudioSample)
    {
        if (m_rxPaused || !m_active || !m_gui)
        {
            return;
        }
        // The IC-7610 supplies zeroed I/Q during TX, including before the CI-V status arrives.
        if (std::all_of(data, data + length, [](char byte) { return byte == 0; }))
        {
            resetSpectrum();
            m_iqGap = true;
            m_iqClock.resetOrigin();
            m_demodulator.clearNotchDetection();
            publishNotchFrequency();
            return;
        }
        m_iqClock.establishOrigin(received, endAudioSample);
        if (m_demodulating || m_ftDemodulating)
        {
            if (std::exchange(m_iqGap, false))
            {
                m_demodulator.tune(static_cast<int>(m_mainHz - m_center), m_ftMode ? Sideband::Usb : m_sideband);
            }
            const auto firstInput = firstIq - m_demodulator.pendingIqSamples();
            sendBrowserAudio(m_demodulator.process(data, length, m_demodulating, m_ftDemodulating));
            if (m_ftDemodulating)
            {
                submitFtAudio(firstInput, m_demodulator.digitalAudio());
            }
            publishNotchFrequency();
        }

        // The hop is set by the I/Q sample clock, independently of TCP block boundaries.
        const int hopBytes = m_rate / spectrumFps * 4;
        while (length > 0)
        {
            const int count = std::min(length, Spectrum::bytes - m_samples.size());
            m_samples.append(data, count);
            data += count;
            length -= count;
            if (m_samples.size() == Spectrum::bytes)
            {
                queueSpectrum();
                m_samples.remove(0, hopBytes);
            }
        }
    }

    void submitFtAudio(quint64 firstIq, std::span<const float> audio)
    {
        const auto sampleZero = m_iqClock.sampleZero();
        if (!m_ft || !sampleZero || audio.empty())
        {
            return;
        }
        const auto firstAudio = *sampleZero + static_cast<qint64>(firstIq / iqPerFtSample);
        m_ftPcm.resize(audio.size());
        std::transform(audio.begin(), audio.end(), m_ftPcm.begin(), [](float sample) {
            return static_cast<ft8::Sample>(std::clamp(std::lround(sample * 32768), -32768L, 32767L));
        });
        const bool submitted = m_ft->submit_audio(firstAudio, m_ftPcm);
        if (!submitted && !m_ftAudioRejectionLogged)
        {
            m_ftAudioRejectionLogged = true;
            qWarning("FT-RX: Audioblock abgewiesen");
        }
    }

    QSettings m_settings;
    std::unique_ptr<ft8::Transceiver> m_ft;
    std::vector<ft8::Sample> m_ftPcm;
    bool m_ftAudioRejectionLogged = false;
    struct FtRadio
    {
        quint64 token;
        quint64 id;
        bool started = false;
        bool cancelled = false;
    };
    ft8::Config m_ftConfig;
    std::optional<ft8::Mode> m_ftMode;
    std::optional<FtRadio> m_ftRadio;
    QTimer m_ftPoll;
    std::chrono::steady_clock::time_point m_ftStateSent{};
    std::deque<std::pair<quint64, ft8::Decode>> m_ftDecodes;
    quint64 m_ftNextDecodeId = 0;
    quint64 m_ftNextTxToken = 0;
    ft8::State m_ftState = ft8::State::Receiving;
    ft8::AutoMode m_ftAuto = ft8::AutoMode::Off;
    QString m_ftPeer;
    unsigned m_ftRetries = 0;
    bool m_ftArmed = false;
    bool m_ftTxAuthorized = false;
    int m_ftLevel = 25;
    QString m_host;
    QString m_radioAddress;
    int m_volume = 35;
    bool m_streamAudio = false;
    QByteArray m_audioFrame = QByteArray(audioFrameSamples * 2, Qt::Uninitialized);
    int m_audioCount = 0;
    Sideband m_defaultSideband = Sideband::Lsb;
    Sideband m_sideband = Sideband::Lsb;
    RxFilterSettings m_rxFilter;
    int m_nrPercent = 0;
    NotchMode m_notchMode = NotchMode::Off;
    int m_notchHz = RxNotch::defaultHz;
    int m_reportedNotchHz = 0;
    SsbDemodulator m_demodulator;
    QThread m_mainThread;
    MainControl* m_mainControl = new MainControl;
    QTcpServer m_server;
    QTcpSocket* m_gui = nullptr;
    QTcpSocket m_pi;
    QTimer m_retry;
    QTimer m_deadline;
    QThread m_spectrumThread;
    Spectrum* m_spectrum = nullptr;
    quint64 m_spectrumGeneration = 0;
    bool m_spectrumBusy = false;
    const Band* m_desired = nullptr;
    const Band* m_pending = nullptr;
    const Band* m_active = nullptr;
    QByteArray m_data;
    QByteArray m_samples;
    quint64 m_index = 0;
    IqUtcClock m_iqClock;
    bool m_iqGap = false;
    int m_center = 0;
    int m_rate = 0;
    qint64 m_mainHz = 0;
    quint64 m_mainSession = 0;
    quint64 m_mainRevision = 0;
    bool m_header = false;
    bool m_demodulating = false;
    bool m_ftDemodulating = false;
    bool m_mainConfirmed = false;
    bool m_rxPaused = false;
    bool m_txReady = false;
    quint32 m_clientPttId = 0;
};
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("kmcontrol"));
    QCoreApplication::setApplicationName(QStringLiteral("kmcontrol-backend"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("I/Q-Backend für den IC-7610"));
    parser.addHelpOption();
    const QCommandLineOption pi(QStringLiteral("pi"), QStringLiteral("Adresse des Pi"),
                                QStringLiteral("Host"));
    const QCommandLineOption qrzApiKey(QStringLiteral("qrz-api-key"),
                                       QStringLiteral("API-Key für den automatischen QRZ-Logbuch-Upload"),
                                       QStringLiteral("KEY"));
    parser.addOption(pi);
    const QCommandLineOption ftPttLead(QStringLiteral("ft-ptt-lead-ms"),
                                       QStringLiteral("FT-PTT-Vorlauf in Millisekunden"), QStringLiteral("MS"),
                                       QStringLiteral("280"));
    parser.addOption(qrzApiKey);
    parser.addOption(ftPttLead);
    parser.process(app);

    bool validLead = false;
    const int pttLeadMs = parser.value(ftPttLead).toInt(&validLead);
    if (!validLead || pttLeadMs < 0 || pttLeadMs > 1000)
    {
        qCritical("FT-PTT-Vorlauf muss zwischen 0 und 1000 ms liegen.");
        return 1;
    }
    const auto qrzKey = parser.isSet(qrzApiKey) ? parser.value(qrzApiKey) : qEnvironmentVariable("KMCONTROL_QRZ_API_KEY");
    Backend backend(parser.value(pi), qrzKey, pttLeadMs);
    if (!backend.start())
    {
        return 1;
    }
    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
    QTimer signalTimer;
    QObject::connect(&signalTimer, &QTimer::timeout, &app, [&app] {
        if (stopRequested)
        {
            app.quit();
        }
    });
    signalTimer.start(50);
    return app.exec();
}
