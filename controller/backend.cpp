#include "spectrum.hpp"
#include "main-control.hpp"
#include "ssb-demod.hpp"
#include "ft-receive.hpp"
#include "ft-transmit.hpp"
#include "ft_logbook.hpp"
#include "qrz-logbook.hpp"

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDebug>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
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
#include <csignal>
#include <limits>
#include <memory>
#include <numeric>
#include <span>
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
volatile std::sig_atomic_t stopRequested = 0;

void requestStop(int)
{
    stopRequested = 1;
}

using Sideband = SsbDemodulator::Sideband;
using NotchMode = RxNotch::Mode;

const QStringList notchModes{QStringLiteral("off"), QStringLiteral("manual"), QStringLiteral("auto")};

std::optional<FtMode> parseFtMode(const QString& mode)
{
    if (mode == QStringLiteral("FT8"))
    {
        return FtMode::ft8;
    }
    if (mode == QStringLiteral("FT4"))
    {
        return FtMode::ft4;
    }
    return std::nullopt;
}

qint64 ftStandardFrequency(const Band& band, FtMode mode)
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
            return mode == FtMode::ft4 ? entry.ft4 : entry.ft8;
        }
    }
    return 0;
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
    Backend(const QString& hostOverride, const QString& qrzApiKey)
        : m_qrzLogbook(qrzApiKey)
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
        connect(&m_ftReceiver, &FtReceiver::decoded, this, &Backend::sendFtDecode);
        connect(&m_ftReceiver, &FtReceiver::error, this, [this](const QString& text) {
            send({{QStringLiteral("type"), QStringLiteral("ft-error")}, {QStringLiteral("text"), text}});
        });
        connect(&m_qrzLogbook, &QrzLogbook::status, this,
                [this](const QString& call, const QString& state, const QString& text) {
            if (state == QStringLiteral("error"))
            {
                qWarning().noquote() << "QRZ-Logbuch" << call << text;
            }
            send({{QStringLiteral("type"), QStringLiteral("ft-qrz-status")},
                  {QStringLiteral("call"), call}, {QStringLiteral("state"), state}, {QStringLiteral("text"), text}});
        });
        const auto dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (!dataDir.isEmpty())
        {
            try
            {
                m_ftLogbook = std::make_unique<bridge::FtLogbook>((dataDir + QStringLiteral("/ft4-ft8.adi")).toStdString());
            }
            catch (const std::exception& error)
            {
                qWarning("FT-Logbuch: %s", error.what());
            }
        }

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
        connect(m_mainControl, &MainControl::digitalTxChanged, this, &Backend::updateFtTx);
        m_mainThread.start();
    }

    ~Backend() override
    {
        m_mainControl->cancelPtt();
        cancelFtTx();
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
        if (m_qrzLogbook.enabled() && m_ftLogbook)
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
            if (operation == QStringLiteral("ft_mode"))
            {
                selectFtMode(request);
                continue;
            }
            if (operation == QStringLiteral("ft8_tx_start"))
            {
                scheduleFtTx(request);
                continue;
            }
            if (operation == QStringLiteral("ft8_tx_stop"))
            {
                cancelFtTx();
                continue;
            }
            if (operation == QStringLiteral("ft_qso_log"))
            {
                logFtQso(request);
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
                cancelFtTx();
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
            cancelFtTx();
            m_ftJob.reset();
            sendFtTx(QStringLiteral("idle"), 0, QStringLiteral("unavailable"));
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
            cancelFtTx();
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

    void sendFtState()
    {
        send({{QStringLiteral("type"), QStringLiteral("ft-state")},
              {QStringLiteral("mode"), QString::fromLatin1(m_ftMode ? ft_mode_name(*m_ftMode) : "off")},
              {QStringLiteral("available"), m_ftReceiver.available()},
              {QStringLiteral("log_available"), bool(m_ftLogbook)},
              {QStringLiteral("utc_ms"), static_cast<double>(QDateTime::currentMSecsSinceEpoch())}});
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

    void ftReply(const QJsonObject& request, bool ok, const QString& code = {}, const QString& text = {},
                 qint64 slot = 0)
    {
        send({{QStringLiteral("type"), QStringLiteral("ft-reply")},
              {QStringLiteral("request_id"), request.value(QStringLiteral("request_id"))},
              {QStringLiteral("ok"), ok},
              {QStringLiteral("code"), code},
              {QStringLiteral("text"), text},
              {QStringLiteral("slot_utc"), slot / 1000.0}});
    }

    void selectFtMode(const QJsonObject& request)
    {
        const auto name = request.value(QStringLiteral("mode")).toString();
        const auto mode = parseFtMode(name);
        if (mode && (!m_ftReceiver.available() || (mode != m_ftMode && (m_rxPaused || m_clientPttId || m_ftJob))))
        {
            ftReply(request, false, QStringLiteral("unavailable"), QStringLiteral("FT ist derzeit nicht verfügbar."));
            return;
        }
        if (!mode && name != QStringLiteral("off"))
        {
            ftReply(request, false, QStringLiteral("ft_mode"), QStringLiteral("Unbekannte FT-Betriebsart."));
            return;
        }
        if (m_ftMode != mode)
        {
            cancelFtTx();
            const bool bypassChanged = m_ftMode.has_value() != mode.has_value();
            m_ftMode = mode;
            if (bypassChanged)
            {
                setVoiceEffectsForFt();
                setRadioSettingsForFt();
                sendRxFilter();
            }
            if (m_ftMode && m_desired)
            {
                const auto frequency = ftStandardFrequency(*m_desired, *m_ftMode);
                if (frequency && (frequency != m_mainHz || !m_mainConfirmed))
                {
                    tuneMain(frequency, frequency);
                }
            }
            m_ftReceiver.select(m_ftMode, m_mainHz);
            updateDemodulator(bypassChanged);
        }
        sendFtState();
        ftReply(request, true);
    }

    void sendFtDecode(QJsonObject message)
    {
        static const QRegularExpression cq(QStringLiteral("^CQ(?: (?:[A-Z]{1,4}|[0-9]{3}))? ([A-Z0-9/]*[0-9][A-Z0-9/]+)(?: |$)"));
        const auto match = cq.match(message.value(QStringLiteral("message")).toString());
        if (match.hasMatch() && message.value(QStringLiteral("message_type")).toInt() != 0)
        {
            const auto call = match.captured(1);
            message.insert(QStringLiteral("cq_call"), call);
            if (m_ftLogbook)
            {
                message.insert(QStringLiteral("worked_recently"),
                               m_ftLogbook->worked_recently(call.toStdString(), std::time(nullptr)));
            }
        }
        send(message);
    }

    void logFtQso(const QJsonObject& request)
    {
        if (!m_ftLogbook)
        {
            ftReply(request, false, QStringLiteral("ft_log_disabled"));
            return;
        }
        const auto text = [&request](const char* key) { return request.value(QLatin1String(key)).toString().toUpper(); };
        const auto number = [&request](const char* key) {
            const double value = request.value(QLatin1String(key)).toDouble();
            return value > 0 && value < 4102444800.0 && std::floor(value) == value ? static_cast<qint64>(value) : 0;
        };
        static const QRegularExpression callPattern(QStringLiteral("^[A-Z0-9]{2,11}(?:/[A-Z0-9]{1,6})?$"));
        static const QRegularExpression gridPattern(QStringLiteral("^[A-R]{2}[0-9]{2}(?:[A-X]{2})?$"));
        static const QRegularExpression reportPattern(QStringLiteral("^[+-][0-9]{2}$"));
        const auto call = text("call");
        const auto own = text("own_call");
        const auto grid = text("grid");
        const auto ownGrid = text("own_grid");
        const auto mode = text("mode");
        const auto sent = text("sent_report");
        const auto received = text("received_report");
        const auto start = number("start");
        const auto end = number("end");
        const auto txHz = number("tx_hz");
        const auto rxHz = number("rx_hz");
        const auto& catalog = bands();
        const auto band = std::find_if(catalog.begin(), catalog.end(), [txHz, rxHz](const Band& item) {
            return txHz >= item.lowHz && txHz <= item.highHz && rxHz >= item.lowHz && rxHz <= item.highHz;
        });
        if (!parseFtMode(mode) || !callPattern.match(call).hasMatch() || !callPattern.match(own).hasMatch() ||
            !gridPattern.match(ownGrid).hasMatch() || (!grid.isEmpty() && !gridPattern.match(grid).hasMatch()) ||
            !reportPattern.match(sent).hasMatch() || !reportPattern.match(received).hasMatch() ||
            !start || end < start || band == catalog.end() || band->id == QStringLiteral("cb"))
        {
            ftReply(request, false, QStringLiteral("ft_log_invalid"), QStringLiteral("Ungültige QSO-Daten."));
            return;
        }
        std::optional<std::string> adif;
        try
        {
            adif = m_ftLogbook->save({call.toStdString(), own.toStdString(), grid.toStdString(), ownGrid.toStdString(),
                mode.toStdString(), sent.toStdString(), received.toStdString(), band->id.section('-', 0, 0).toStdString(),
                static_cast<std::time_t>(start), static_cast<std::time_t>(end),
                static_cast<std::uint64_t>(txHz), static_cast<std::uint64_t>(rxHz)});
        }
        catch (const std::exception& error)
        {
            qWarning("FT-Logbuch: %s", error.what());
            ftReply(request, false, QStringLiteral("ft_log_error"));
            return;
        }
        ftReply(request, true);
        send({{QStringLiteral("type"), QStringLiteral("ft-qso-logged")}, {QStringLiteral("call"), call}});
        if (adif && m_qrzLogbook.enabled())
        {
            m_qrzLogbook.insert(call, own, QByteArray::fromStdString(*adif));
        }
    }

    void scheduleFtTx(const QJsonObject& request)
    {
        if (m_ftJob || m_clientPttId || m_rxPaused || !m_txReady)
        {
            ftReply(request, false, QStringLiteral("tx2_busy"), QStringLiteral("TX ist belegt oder nicht bereit."));
            return;
        }
        const auto mode = parseFtMode(request.value(QStringLiteral("mode")).toString());
        const auto dial = request.value(QStringLiteral("frequency_hz")).toDouble();
        const int audioHz = request.value(QStringLiteral("audio_hz")).toInt();
        const int level = request.value(QStringLiteral("level")).toInt();
        const int parity = request.value(QStringLiteral("parity")).toInt(-1);
        const auto text = request.value(QStringLiteral("message")).toString();
        if (!mode || mode != m_ftMode || !m_mainConfirmed || dial != static_cast<double>(m_mainHz) ||
            !m_active || m_desired != m_active || m_active->id == QStringLiteral("cb") ||
            m_mainHz < m_active->lowHz || m_mainHz + audioHz + 70 > m_active->highHz)
        {
            ftReply(request, false, QStringLiteral("unavailable"), QStringLiteral("Zuerst MAIN auf eine FT-Frequenz im gewählten Amateurband abstimmen."));
            return;
        }
        if (audioHz < 100 || audioHz > (*mode == FtMode::ft4 ? 3930 : 3950) ||
            level < 1 || level > 100 || (parity != 0 && parity != 1) || text.size() > 37 ||
            std::any_of(text.begin(), text.end(), [](QChar c) { return c.unicode() < 32 || c.unicode() > 126; }))
        {
            ftReply(request, false, QStringLiteral("ft_tx_invalid"), QStringLiteral("FT-Nachricht, Audiofrequenz, Pegel und UTC-Slot prüfen."));
            return;
        }
        auto pcm = encodeFtAudio(*mode, text, audioHz, level);
        if (pcm.isEmpty())
        {
            ftReply(request, false, text.simplified().size() > 13 ? QStringLiteral("ft8_free_text_length")
                                                               : QStringLiteral("ft8_invalid_message"));
            return;
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 period = ft_slot_ms(*mode);
        qint64 slot = now / period;
        if (now > slot * period + 100)
        {
            ++slot;
        }
        if (slot % 2 != parity)
        {
            ++slot;
        }
        const qint64 slotMs = slot * period;
        const quint64 id = ++m_nextFtId;
        m_ftJob = FtJob{id, slotMs, m_mainHz, audioHz, text.simplified().toUpper()};
        QMetaObject::invokeMethod(m_mainControl, [control = m_mainControl, session = m_mainSession,
                                  id, hz = m_mainHz, slotMs, pcm = std::move(pcm)]() mutable {
            control->scheduleDigitalTx(session, id, hz, slotMs + 500, std::move(pcm));
        }, Qt::QueuedConnection);
        ftReply(request, true, {}, {}, slotMs);
        sendFtTx(QStringLiteral("scheduled"), slotMs);
    }

    void sendFtTx(const QString& phase, qint64 slotMs, const QString& result = {}, const QString& text = {})
    {
        QJsonObject message{{QStringLiteral("type"), QStringLiteral("ft-tx")},
                            {QStringLiteral("phase"), phase},
                            {QStringLiteral("slot_utc"), slotMs / 1000.0},
                            {QStringLiteral("result"), result},
                            {QStringLiteral("text"), text}};
        if (result == QStringLiteral("complete") && m_ftJob)
        {
            message.insert(QStringLiteral("message"), m_ftJob->message);
            message.insert(QStringLiteral("audio_hz"), m_ftJob->audioHz);
            message.insert(QStringLiteral("dial_hz"), static_cast<double>(m_ftJob->dialHz));
        }
        send(message);
    }

    void updateFtTx(quint64 session, quint64 id, const QString& phase, const QString& result, const QString& text)
    {
        if (session != m_mainSession || !m_ftJob || m_ftJob->id != id)
        {
            return;
        }
        const auto slot = m_ftJob->slotMs;
        sendFtTx(phase, slot, result, text);
        if (phase == QStringLiteral("idle"))
        {
            m_ftJob.reset();
        }
    }

    void cancelFtTx()
    {
        if (m_ftJob)
        {
            m_mainControl->cancelDigitalTx(m_ftJob->id);
        }
    }

    void startMainControl()
    {
        m_mainControl->cancelPtt();
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
                if (!m_txReady || m_rxPaused || m_clientPttId || m_ftJob)
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
        m_mainConfirmed = confirmed;
        if (hz != m_mainHz)
        {
            cancelFtTx();
            m_mainHz = hz;
            m_ftReceiver.select(m_ftMode, hz);
            updateDemodulator(false);
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
        if (reset || ftReceiving != m_ftDemodulating)
        {
            m_ftReceiver.breakStream();
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
        cancelFtTx();
        m_ftJob.reset();
        m_ftMode.reset();
        setVoiceEffectsForFt();
        m_ftReceiver.select(std::nullopt, 0);
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
        if (!m_rxPaused)
        {
            cancelFtTx();
        }
        m_data.clear();
        resetSpectrum();
        m_index = 0;
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
        m_center = static_cast<int>(center);
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

        int offset = 0;
        while (m_data.size() - offset >= 12)
        {
            const auto* data = reinterpret_cast<const uchar*>(m_data.constData() + offset);
            const auto index = qFromBigEndian<quint64>(data);
            const auto length = qFromBigEndian<quint32>(data + 8);
            if (index != m_index || length > maxPiBlock || length % 4 != 0)
            {
                fault(QStringLiteral("I/Q-Rahmen des Pi ist ungültig"));
                return;
            }
            const int payload = length == 0 ? 12 : static_cast<int>(length);
            if (m_data.size() - offset < 12 + payload)
            {
                break;
            }
            if (length == 0)
            {
                if (!control(data + 12))
                {
                    return;
                }
            }
            else
            {
                m_index += length / 4;
                samples(reinterpret_cast<const char*>(data + 12), payload);
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

    void samples(const char* data, int length)
    {
        if (m_rxPaused || !m_active || !m_gui)
        {
            return;
        }
        // The IC-7610 supplies zeroed I/Q during TX, including before the CI-V status arrives.
        if (std::all_of(data, data + length, [](char byte) { return byte == 0; }))
        {
            resetSpectrum();
            m_ftReceiver.breakStream();
            m_demodulator.clearNotchDetection();
            publishNotchFrequency();
            return;
        }
        if (m_demodulating || m_ftDemodulating)
        {
            const auto timestamp = QDateTime::currentMSecsSinceEpoch();
            sendBrowserAudio(m_demodulator.process(data, length, m_demodulating, m_ftDemodulating));
            if (m_ftDemodulating)
            {
                m_ftReceiver.push(m_demodulator.digitalAudio(), timestamp);
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

    QSettings m_settings;
    struct FtJob
    {
        quint64 id;
        qint64 slotMs;
        qint64 dialHz;
        int audioHz;
        QString message;
    };
    std::optional<FtMode> m_ftMode;
    std::optional<FtJob> m_ftJob;
    std::unique_ptr<bridge::FtLogbook> m_ftLogbook;
    QrzLogbook m_qrzLogbook;
    FtReceiver m_ftReceiver;
    quint64 m_nextFtId = 0;
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
    parser.addOption(qrzApiKey);
    parser.process(app);

    Backend backend(parser.value(pi), parser.value(qrzApiKey));
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
