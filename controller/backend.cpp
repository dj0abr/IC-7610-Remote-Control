#include "spectrum.hpp"
#include "main-control.hpp"
#include "ssb-demod.hpp"

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <span>

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

using Sideband = SsbDemodulator::Sideband;

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
    explicit Backend(const QString& hostOverride)
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

        m_mainControl->moveToThread(&m_mainThread);
        connect(&m_mainThread, &QThread::finished, m_mainControl, &QObject::deleteLater);
        connect(m_mainControl, &MainControl::frequencyChanged, this, &Backend::updateMainFrequency);
        connect(m_mainControl, &MainControl::statusChanged, this, &Backend::updateMainStatus);
        m_mainThread.start();
    }

    ~Backend() override
    {
        if (m_gui)
        {
            disconnect(m_gui, nullptr, this, nullptr);
            m_gui->abort();
        }
        disconnect(&m_pi, nullptr, this, nullptr);
        m_pi.abort();
        QMetaObject::invokeMethod(m_mainControl, "stop", Qt::BlockingQueuedConnection);
        m_mainThread.quit();
        m_mainThread.wait();
    }

    bool start()
    {
        if (!m_fft.ready())
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
        qInfo("Web-Bridge-Schnittstelle auf 127.0.0.1:50020");
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
            startMainControl();
        }
    }

    void readGui()
    {
        while (m_gui && m_gui->canReadLine())
        {
            const auto request = QJsonDocument::fromJson(m_gui->readLine(4096)).object();
            const QString operation = request.value(QStringLiteral("op")).toString();
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
            if (operation == QStringLiteral("volume"))
            {
                setVolume(request.value(QStringLiteral("percent")).toInt(-1));
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
            m_desired = band;
            m_sideband = parseSideband(m_settings.value(sidebandKey(band)).toString(), m_defaultSideband);
            send({{QStringLiteral("type"), QStringLiteral("sideband")},
                  {QStringLiteral("band"), band->id},
                  {QStringLiteral("mode"), sidebandMode(m_sideband)}});
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
            m_radioAddress = radio;
            setMainFrequency(0, false);
            sendMainStatus(QStringLiteral("MAIN: LAN-CI-V verbinden …"));
            startMainControl();
        }
        if (m_host != pi)
        {
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
            updateDemodulator();
        }
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
            updateDemodulator();
        }
    }

    void startMainControl()
    {
        ++m_mainSession;
        m_mainRevision = 0;
        QMetaObject::invokeMethod(m_mainControl, "start", Qt::QueuedConnection,
                                  Q_ARG(QString, m_radioAddress),
                                  Q_ARG(quint64, m_mainSession));
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
        if (hz != m_mainHz)
        {
            m_mainHz = hz;
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
        const bool receiving = m_streamAudio && m_active && m_mainHz > 0 &&
            std::abs(offset) + SsbDemodulator::audioHighHz <= Spectrum::usableHalfWidthHz;
        if (reset || receiving != m_demodulating)
        {
            m_audioCount = 0;
            if (m_streamAudio)
            {
                send({{QStringLiteral("type"), QStringLiteral("audio-reset")}});
            }
        }
        if (receiving)
        {
            if (reset || !m_demodulating)
            {
                m_demodulator.tune(static_cast<int>(offset), m_sideband);
            }
            else
            {
                m_demodulator.setFrequencyOffset(static_cast<int>(offset));
            }
        }
        m_demodulating = receiving;
    }

    void tuneMain(qint64 hz)
    {
        ++m_mainRevision;
        setMainFrequency(hz, false);
        sendMainStatus(QStringLiteral("MAIN wird mit dem IC-7610 synchronisiert …"));
        QMetaObject::invokeMethod(m_mainControl, "tune", Qt::QueuedConnection,
                                  Q_ARG(qint64, hz), Q_ARG(quint64, m_mainRevision));
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
        tuneMain(static_cast<qint64>(hz));
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
            tuneMain(m_mainHz + allowedSteps * mainStepHz);
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
        m_streamAudio = false;
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
        m_data.clear();
        m_samples.clear();
        m_index = 0;
        m_header = false;
        m_active = nullptr;
        m_pending = nullptr;
        updateDemodulator();
    }

    void connectPi()
    {
        if (!m_gui || !m_desired || m_pi.state() != QAbstractSocket::UnconnectedState)
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
        if (!m_desired)
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
        m_samples.clear();
        updateDemodulator();
        state(QStringLiteral("tuning"), QStringLiteral("SUB wird eingestellt …"), m_pending);
        m_deadline.start();
        m_pi.write("SET SUB " + QByteArray::number(m_pending->centerHz) + '\n');
    }

    void activate()
    {
        m_samples.clear();
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
        if (m_active && !m_pending)
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

    void sendSpectrum()
    {
        m_fft.transform(m_samples);
        send({{QStringLiteral("type"), QStringLiteral("spectrum")},
              {QStringLiteral("band"), m_active->id},
              {QStringLiteral("levels_db10"), m_fft.wideLevels(*m_active, m_center, m_rate)}});

        const qint64 offset = m_mainHz - m_center;
        if (m_mainHz > 0 && std::abs(offset) <= Spectrum::usableHalfWidthHz + zoomHalfSpanHz)
        {
            send({{QStringLiteral("type"), QStringLiteral("zoom-spectrum")},
                  {QStringLiteral("band"), m_active->id},
                  {QStringLiteral("center_hz"), static_cast<double>(m_mainHz)},
                  {QStringLiteral("levels_db10"),
                   m_fft.zoomLevels(static_cast<int>(offset), zoomHalfSpanHz, m_rate)}});
        }
    }

    void samples(const char* data, int length)
    {
        if (!m_active || !m_gui)
        {
            return;
        }
        if (m_demodulating)
        {
            sendBrowserAudio(m_demodulator.process(data, length));
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
                if (m_gui->bytesToWrite() <= streamQueueLimit)
                {
                    sendSpectrum();
                }
                m_samples.remove(0, hopBytes);
            }
        }
    }

    QSettings m_settings;
    QString m_host;
    QString m_radioAddress;
    int m_volume = 35;
    bool m_streamAudio = false;
    QByteArray m_audioFrame = QByteArray(audioFrameSamples * 2, Qt::Uninitialized);
    int m_audioCount = 0;
    Sideband m_defaultSideband = Sideband::Lsb;
    Sideband m_sideband = Sideband::Lsb;
    SsbDemodulator m_demodulator;
    QThread m_mainThread;
    MainControl* m_mainControl = new MainControl;
    QTcpServer m_server;
    QTcpSocket* m_gui = nullptr;
    QTcpSocket m_pi;
    QTimer m_retry;
    QTimer m_deadline;
    Spectrum m_fft;
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
    parser.addOption(pi);
    parser.process(app);

    Backend backend(parser.value(pi));
    if (!backend.start())
    {
        return 1;
    }
    return app.exec();
}
