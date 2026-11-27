#include "main-control.hpp"

#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <exception>
#include <numeric>
#include <liquid.h>
#include <stdexcept>
#include <type_traits>
#include <utility>

using namespace std::chrono_literals;

namespace
{
constexpr auto latchTimeout = 5min;
using RadioStatus = ft8::RadioReport::Status;

QByteArray digitalTxPcm(std::span<const ft8::Sample> wave, int level)
{
    constexpr int inputCount = 3;
    constexpr int outputCount = 4;
    static_assert(ft8::sample_rate * outputCount == icom::txAudioRate * inputCount);
    using Resampler = std::unique_ptr<std::remove_pointer_t<rresamp_rrrf>, decltype(&rresamp_rrrf_destroy)>;
    const Resampler resampler(rresamp_rrrf_create_kaiser(outputCount, inputCount, 16, 0.48f, 80.0f),
                              rresamp_rrrf_destroy);
    if (!resampler)
    {
        throw std::runtime_error("FT-TX-Resampler nicht verfügbar.");
    }
    rresamp_rrrf_set_scale(resampler.get(), 1.0f);
    const auto groups = (wave.size() + inputCount - 1) / inputCount;
    QByteArray pcm(static_cast<int>(groups * outputCount * sizeof(qint16)), Qt::Uninitialized);
    std::array<float, inputCount> input{};
    std::array<float, outputCount> output{};
    const float gain = level / 100.0f;
    int position = 0;
    for (std::size_t i = 0; i < wave.size(); i += inputCount)
    {
        for (std::size_t j = 0; j < input.size(); ++j)
        {
            input[j] = i + j < wave.size() ? wave[i + j] : 0;
        }
        rresamp_rrrf_execute(resampler.get(), input.data(), output.data());
        for (const auto sample : output)
        {
            const auto value = static_cast<qint16>(std::clamp(std::lround(sample * gain), -32768L, 32767L));
            qToLittleEndian(value, pcm.data() + position);
            position += sizeof(value);
        }
    }
    return pcm;
}

struct RadioSettingCommands
{
    const char* label;
    int maximum;
    int (icom::LanRadio::*read)();
    void (icom::LanRadio::*write)(int);
};

constexpr std::array<RadioSettingCommands, MainControl::RadioSettingCount> radioSettings{{
    {"Sendeleistung", icom::maxRfPowerLevel, &icom::LanRadio::rfPower, &icom::LanRadio::setRfPower},
    {"TX-Kompressor", 1, &icom::LanRadio::compressor, &icom::LanRadio::setCompressor},
    {"RX-Vorverstärker", 2, &icom::LanRadio::subPreamp, &icom::LanRadio::setSubPreamp},
}};
}

MainControl::MainControl(QObject* parent)
    : QObject(parent),
      m_timer(new QTimer(this))
{
    m_timer->setTimerType(Qt::PreciseTimer);
    m_timer->setInterval(10);
    connect(m_timer, &QTimer::timeout, this, &MainControl::updateRadio);
}

void MainControl::start(const QString& address, quint64 session)
{
    stop();
    if (address != m_address)
    {
        m_compressorBeforeFt.reset();
        m_usbFilterBeforeFt.reset();
    }
    m_address = address;
    m_session = session;
    m_completedRevision = 0;
    m_clock.start();
    m_nextRead = 0;
    m_nextPttRead = 0;
    m_nextMainModeRead = 0;
    m_retryAfter = 0;
    m_timer->start();
    emit statusChanged(m_session, 0, QStringLiteral("MAIN: LAN-CI-V verbinden …"));
    reportTx(QStringLiteral("offline"), false, QStringLiteral("LAN verbinden …"));
}

void MainControl::stop()
{
    m_timer->stop();
    const bool restoreVoiceMode = m_ftRadioSettingsActive || m_usbFilterBeforeFt.has_value();
    m_ftRadioSettingsActive = false;
    if (m_radio)
    {
        m_radio->releasePtt();
        if (m_compressorBeforeFt)
        {
            try
            {
                if (!m_radio->transmitting())
                {
                    applyFtCompressor();
                }
            }
            catch (const std::exception& error)
            {
                qWarning("TX-Kompressor nach FT: %s", error.what());
            }
        }
        if (restoreVoiceMode)
        {
            try
            {
                if (!m_radio->transmitting())
                {
                    applyMainMode();
                }
            }
            catch (const std::exception& error)
            {
                qWarning("MAIN-Betriebsart nach FT: %s", error.what());
            }
        }
    }
    m_radio.reset();
    m_requested.reset();
    if (m_digitalTx)
    {
        emit digitalTxReported(m_session, m_digitalTx->token,
                               {m_digitalTx->playback.id, RadioStatus::Failed, {}, "Funkgerät getrennt."});
        m_digitalTx.reset();
    }
    m_radioSettings = {};
    m_voiceSideband.reset();
    m_mainModePending = false;
    m_micGainDue.reset();
    m_lastFrequency = 0;
    m_ownedTx = 0;
    m_externalTx = false;
    m_txPhase.clear();
}

void MainControl::pttInput(quint64 session, quint32 id, const QString& action, bool latched)
{
    const std::lock_guard lock(m_txMutex);
    if (action == QStringLiteral("start"))
    {
        m_input = {session, {id, latched}, Clock::now() + 750ms, {}, {}};
    }
    else if (session == m_input.session && id == m_input.ptt.id)
    {
        if (action == QStringLiteral("stop"))
        {
            m_input = {};
        }
        else if (Clock::now() < m_input.lease)
        {
            m_input.lease = Clock::now() + 750ms;
        }
    }
}

void MainControl::txAudioInput(quint64 session, quint32 id, QByteArray pcm)
{
    const std::lock_guard lock(m_txMutex);
    const auto now = Clock::now();
    if (session != m_input.session || id != m_input.ptt.id || now >= m_input.lease)
    {
        return;
    }
    m_input.lastAudio = now;
    if (m_input.frames.size() == 4)
    {
        m_input.frames.pop_front();
    }
    m_input.frames.push_back(std::move(pcm));
}

void MainControl::cancelPtt()
{
    const std::lock_guard lock(m_txMutex);
    m_input = {};
}

void MainControl::cancelPtt(quint32 id)
{
    const std::lock_guard lock(m_txMutex);
    if (m_input.session == m_session && m_input.ptt.id == id)
    {
        m_input = {};
    }
}

MainControl::PttRequest MainControl::currentPtt()
{
    const std::lock_guard lock(m_txMutex);
    const auto now = Clock::now();
    if (m_input.session != m_session || now >= m_input.lease || now - m_input.lastAudio > 320ms)
    {
        return {};
    }
    return m_input.ptt;
}

void MainControl::reportTx(const QString& phase, bool ready, const QString& text)
{
    if (phase != m_txPhase || ready != m_txReady || text != m_txText)
    {
        for (auto& setting : m_radioSettings)
        {
            if (phase != QStringLiteral("rx"))
            {
                setting.requested.reset();
            }
            else if (phase != m_txPhase)
            {
                setting.nextRead = 0;
            }
        }
        if (phase != m_txPhase)
        {
            m_nextRxMeter = 0;
            m_pendingPo.reset();
            m_nextTxMeter = Clock::time_point{};
        }
        m_txPhase = phase;
        m_txReady = ready;
        m_txText = text;
        emit txChanged(m_session, phase, ready, text);
    }
}

void MainControl::finishTx(const QString& text)
{
    cancelPtt(m_ownedTx);
    reportTx(QStringLiteral("ending"), false);
    m_radio->setPtt(false);
    if (m_radio->transmitting())
    {
        m_radio->setPtt(false);
        if (m_radio->transmitting())
        {
            throw std::runtime_error("RX wurde vom IC-7610 nicht bestätigt.");
        }
    }
    m_ownedTx = 0;
    m_externalTx = false;
    reportTx(QStringLiteral("rx"), m_radio->txAudioReady(), text);
}

void MainControl::updateTxMeters(Clock::time_point now, std::chrono::milliseconds timeout)
{
    if (now < m_nextTxMeter)
    {
        return;
    }
    if (m_pendingPo)
    {
        const auto swr = m_radio->readTxMeter(icom::TxMeter::Swr, timeout);
        emit txMetersChanged(m_session, *m_pendingPo, swr.value_or(-1));
        m_pendingPo.reset();
        m_nextTxMeter = now + 180ms;
        return;
    }

    const auto power = m_radio->readTxMeter(icom::TxMeter::OutputPower, timeout);
    if (power && *power > 0)
    {
        // Read SWR on the following tick, while this transmission has RF output.
        m_pendingPo = *power;
        return;
    }
    emit txMetersChanged(m_session, power.value_or(-1), -1);
    m_nextTxMeter = now + 180ms;
}

void MainControl::scheduleDigitalTx(quint64 session, quint64 token, qint64 dialHz,
                                    ft8::Playback playback, int level)
{
    if (session != m_session || !m_timer->isActive() || m_digitalTx || m_ownedTx || !m_radio ||
        !playback.frame || playback.frame->samples.empty() || m_cancelDigitalId.load() == token)
    {
        emit digitalTxReported(session, token, {playback.id, RadioStatus::Failed, {}, "FT-TX nicht verfügbar."});
        return;
    }
    try
    {
        auto pcm = digitalTxPcm(playback.frame->samples, level);
        const auto utc = ft8::Time::clock::now();
        const auto steady = Clock::now();
        const auto packetAt = steady + (playback.audio_start - utc) - icom::txAudioBuffer;
        const auto deadline = steady + (playback.ptt_deadline - utc);
        m_digitalTx = DigitalTx{token, dialHz, std::move(playback), packetAt, deadline, std::move(pcm)};
    }
    catch (const std::exception& error)
    {
        emit digitalTxReported(session, token, {playback.id, RadioStatus::Failed, {}, error.what()});
    }
}

void MainControl::finishDigitalTx(RadioStatus status, std::string detail)
{
    const auto token = m_digitalTx->token;
    const auto id = m_digitalTx->playback.id;
    if (m_digitalTx->keyed)
    {
        finishTx();
    }
    m_digitalTx.reset();
    emit digitalTxReported(m_session, token, {id, status, {}, std::move(detail)});
}

bool MainControl::processDigitalTx(Clock::time_point now, bool settingWritePending)
{
    if (!m_digitalTx)
    {
        return false;
    }
    auto& job = *m_digitalTx;
    const auto cancelled = [this, &job] { return m_cancelDigitalId.load() == job.token; };
    const auto latestPacket = job.packetAt + (job.playback.latest_audio_start - job.playback.audio_start);
    if (cancelled() || now >= job.deadline)
    {
        finishDigitalTx(RadioStatus::Failed, cancelled() ? "FT-TX abgebrochen." : "PTT-Zeitlimit erreicht.");
        return true;
    }
    const auto utc = ft8::Time::clock::now();
    const auto offset = (job.playback.ptt_deadline - utc) - (job.deadline - now);
    if (offset < -150ms || offset > 150ms)
    {
        finishDigitalTx(RadioStatus::Failed, "UTC-Uhr hat sich geändert.");
        return true;
    }
    if (!job.keyed)
    {
        std::string detail;
        if (now > latestPacket)
        {
            detail = "FT-Sendefenster verpasst.";
        }
        else if (currentPtt().id || m_ownedTx || m_externalTx || m_requested || settingWritePending ||
                 !m_radio->txAudioReady())
        {
            detail = "TX ist belegt oder nicht bereit.";
        }
        else if (static_cast<qint64>(m_radio->mainFrequency()) != job.dialHz)
        {
            detail = "MAIN wurde umgestimmt.";
        }
        else if (!m_radio->digitalTxMode())
        {
            detail = "FT-TX erfordert MAIN in USB ohne DATA und ausgeschaltetes Split.";
        }
        else if (!m_ftRadioSettingsActive || !m_compressorBeforeFt || m_radio->compressor() != 0)
        {
            detail = "TX-Kompressor ist für FT noch nicht ausgeschaltet.";
        }
        else if (!m_usbFilterBeforeFt || m_radio->mainIfFilterWidthCode() != icom::maxSsbIfFilterWidthCode ||
                 m_radio->mainTwinPbt() != icom::centeredTwinPbt)
        {
            detail = "MAIN-USB-Filter ist für FT noch nicht auf 3,6 kHz.";
        }
        else if (m_radio->transmitting())
        {
            detail = "Der IC-7610 sendet bereits.";
        }
        else if (cancelled() || Clock::now() > latestPacket)
        {
            detail = "FT-TX abgebrochen oder Sendefenster verpasst.";
        }
        if (!detail.empty())
        {
            finishDigitalTx(RadioStatus::Failed, std::move(detail));
            return true;
        }

        job.keyed = true;
        reportTx(QStringLiteral("starting"), true);
        m_radio->setPtt(true);
        if (!m_radio->transmitting())
        {
            finishDigitalTx(RadioStatus::Failed, "Der IC-7610 hat TX nicht bestätigt.");
            return true;
        }
        m_nextAudio = job.packetAt;
        m_nextTxStatus = job.packetAt + 1s;
        reportTx(QStringLiteral("tx"), true);
        now = Clock::now();
        if (cancelled() || now >= job.deadline)
        {
            finishDigitalTx(RadioStatus::Failed, "PTT wurde außerhalb des Sendefensters bestätigt.");
            return true;
        }
    }

    if (job.position == job.pcm.size())
    {
        if (now >= m_nextAudio + icom::txAudioBuffer)
        {
            finishDigitalTx(RadioStatus::Completed);
        }
        return true;
    }
    if (now >= m_nextAudio)
    {
        ft8::Time firstAudio{};
        if (!job.started)
        {
            firstAudio = ft8::Time::clock::now() + icom::txAudioBuffer;
            if (firstAudio < job.playback.audio_start)
            {
                m_nextAudio = now + (job.playback.audio_start - firstAudio);
                return true;
            }
            const auto prefix = ft8::playback_offset(job.playback, firstAudio);
            if (!prefix)
            {
                finishDigitalTx(RadioStatus::Failed, "FT-Audio konnte nicht rechtzeitig beginnen.");
                return true;
            }
            const auto lanSamples = (*prefix * icom::txAudioRate + ft8::sample_rate - 1) / ft8::sample_rate;
            job.position = static_cast<int>(lanSamples * sizeof(qint16));
            m_nextAudio = now;
        }
        else if (now - m_nextAudio > 75ms)
        {
            finishDigitalTx(RadioStatus::Failed, "FT-Audiostrom unterbrochen.");
            return true;
        }
        if (cancelled())
        {
            finishDigitalTx(RadioStatus::Failed, "FT-TX abgebrochen.");
            return true;
        }

        const auto count = std::min(icom::txAudioBytes, job.pcm.size() - job.position);
        std::array<char, icom::txAudioBytes> packet{};
        std::copy_n(job.pcm.constData() + job.position, count, packet.begin());
        m_radio->sendTxAudio(packet);
        job.position += count;
        m_nextAudio += 20ms;
        if (!job.started)
        {
            job.started = true;
            emit digitalTxReported(m_session, job.token, {job.playback.id, RadioStatus::Started, firstAudio, {}});
        }
    }
    if (now >= m_nextTxStatus)
    {
        if (!m_radio->transmitting())
        {
            finishDigitalTx(RadioStatus::Failed, "Der IC-7610 hat TX beendet.");
        }
        m_nextTxStatus = now + 1s;
    }
    else if (now >= m_nextTxMeter)
    {
        const auto remaining = std::min(m_nextAudio, job.deadline) - Clock::now() - 2ms;
        const auto budget = std::min(icom::txMeterTimeout, std::chrono::floor<std::chrono::milliseconds>(remaining));
        if (budget >= 7ms)
        {
            updateTxMeters(now, budget);
        }
    }
    return true;
}

bool MainControl::processTx(Clock::time_point now, bool settingWritePending)
{
    const auto request = currentPtt();
    const auto id = request.id;
    if (m_ownedTx)
    {
        if (now >= m_txDeadline)
        {
            finishTx(QStringLiteral("Rast-PTT nach fünf Minuten beendet."));
            return true;
        }
        if (id != m_ownedTx)
        {
            finishTx();
            return true;
        }
        if (now >= m_nextAudio)
        {
            QByteArray pcm(icom::txAudioBytes, '\0');
            {
                const std::lock_guard lock(m_txMutex);
                if (m_input.session == m_session && m_input.ptt.id == id && !m_input.frames.empty())
                {
                    pcm = std::move(m_input.frames.front());
                    m_input.frames.pop_front();
                }
            }
            if (currentPtt().id == id)
            {
                m_radio->sendTxAudio({pcm.constData(), static_cast<std::size_t>(pcm.size())});
            }
            m_nextAudio += 20ms;
            if (m_nextAudio <= now)
            {
                m_nextAudio = now + 20ms;
            }
        }
        if (now >= m_nextTxStatus)
        {
            if (!m_radio->transmitting())
            {
                finishTx(QStringLiteral("Der IC-7610 hat TX beendet."));
            }
            m_nextTxStatus = now + 1s;
        }
        else if (now >= m_nextTxMeter)
        {
            // Use only the time left before the next audio packet, with a scheduling margin.
            const auto meterStart = Clock::now();
            const auto budget = std::min(icom::txMeterTimeout,
                std::chrono::floor<std::chrono::milliseconds>(m_nextAudio - meterStart - 2ms));
            if (budget >= 7ms && currentPtt().id == id)
            {
                updateTxMeters(meterStart, budget);
            }
        }
        return true;
    }
    if (!id || m_requested || settingWritePending || !m_radio->txAudioReady())
    {
        return false;
    }
    if (m_radio->transmitting())
    {
        cancelPtt(id);
        m_externalTx = true;
        reportTx(QStringLiteral("tx"), false, QStringLiteral("TX am Funkgerät"));
        return true;
    }
    if (!m_radio->voiceTxMode())
    {
        cancelPtt(id);
        reportTx(QStringLiteral("rx"), true, QStringLiteral("Für den Test SSB ohne DATA und ohne Split wählen."));
        return true;
    }
    if (currentPtt().id != id)
    {
        return true;
    }

    m_ownedTx = id;
    m_txDeadline = request.latched ? Clock::now() + latchTimeout : Clock::time_point::max();
    reportTx(QStringLiteral("starting"), true);
    m_radio->setPtt(true);
    if (currentPtt().id != id)
    {
        finishTx();
    }
    else if (m_radio->transmitting())
    {
        m_nextAudio = Clock::now();
        m_nextTxStatus = m_nextAudio + 1s;
        reportTx(QStringLiteral("tx"), true);
    }
    else
    {
        finishTx();
    }
    return true;
}

void MainControl::tune(qint64 lowHz, qint64 highHz, quint64 revision)
{
    if (m_timer->isActive())
    {
        m_requested = TuneRequest{lowHz, highHz, revision};
    }
}

void MainControl::setRadioSetting(quint64 session, int index, int value)
{
    if (session == m_session && m_radio && m_txPhase == QStringLiteral("rx") &&
        index >= 0 && index < RadioSettingCount && value >= 0 && value <= radioSettings[index].maximum &&
        (index != Compressor || (!m_ftRadioSettingsActive && !m_compressorBeforeFt)))
    {
        auto& setting = m_radioSettings[index];
        if (setting.value >= 0)
        {
            setting.requested = value;
        }
    }
}

void MainControl::setVoiceSideband(quint64 session, bool usb)
{
    if (session == m_session)
    {
        m_voiceSideband = usb ? icom::Sideband::Usb : icom::Sideband::Lsb;
        if (!m_ftRadioSettingsActive)
        {
            m_mainModePending = true;
        }
        m_nextMainModeRead = 0;
    }
}

void MainControl::setFtRadioSettings(quint64 session, bool active)
{
    if (session != m_session || active == m_ftRadioSettingsActive)
    {
        return;
    }
    m_ftRadioSettingsActive = active;
    m_mainModePending = true;
    m_nextMainModeRead = 0;
    auto& setting = m_radioSettings[Compressor];
    setting.requested.reset();
    setting.nextRead = 0;
}

void MainControl::applyFtUsbFilter()
{
    const int preset = m_radio->setMainSideband(icom::Sideband::Usb);
    if (!m_usbFilterBeforeFt)
    {
        m_usbFilterBeforeFt = UsbFilter{preset, m_radio->mainIfFilterWidthCode(), m_radio->mainTwinPbt()};
    }
    const auto [previousPreset, previousWidth, previousPbt] = *m_usbFilterBeforeFt;
    if (preset != previousPreset)
    {
        if (m_radio->transmitting())
        {
            return;
        }
        m_radio->setMainUsbFilterPreset(previousPreset);
        if (m_radio->mainUsbFilterPreset() != previousPreset)
        {
            throw std::runtime_error("IC-7610 bestätigt die MAIN-USB-Filterwahl nicht.");
        }
    }
    const int width = m_ftRadioSettingsActive ? icom::maxSsbIfFilterWidthCode : previousWidth;
    if (m_radio->mainIfFilterWidthCode() != width)
    {
        if (m_radio->transmitting())
        {
            return;
        }
        m_radio->setMainIfFilterWidthCode(width);
        if (m_radio->mainIfFilterWidthCode() != width)
        {
            throw std::runtime_error("IC-7610 bestätigt die MAIN-USB-Filterbreite nicht.");
        }
    }
    // The radio resets both Twin PBT positions when the IF bandwidth changes.
    const auto& pbt = m_ftRadioSettingsActive ? icom::centeredTwinPbt : previousPbt;
    if (m_radio->mainTwinPbt() != pbt)
    {
        if (m_radio->transmitting())
        {
            return;
        }
        m_radio->setMainTwinPbt(pbt);
        if (m_radio->mainTwinPbt() != pbt)
        {
            throw std::runtime_error("IC-7610 bestätigt MAIN Twin PBT nicht.");
        }
    }
    if (!m_ftRadioSettingsActive)
    {
        m_usbFilterBeforeFt.reset();
    }
}

void MainControl::applyMainMode()
{
    if (m_ftRadioSettingsActive || m_usbFilterBeforeFt)
    {
        applyFtUsbFilter();
    }
    if (!m_ftRadioSettingsActive && !m_usbFilterBeforeFt && m_voiceSideband)
    {
        m_radio->setMainSideband(*m_voiceSideband);
    }
}

void MainControl::updateMainMode(qint64 now)
{
    if (now < m_nextMainModeRead || (!m_mainModePending && !m_ftRadioSettingsActive && !m_usbFilterBeforeFt))
    {
        return;
    }
    const bool usb = m_ftRadioSettingsActive || m_voiceSideband == icom::Sideband::Usb;
    try
    {
        const bool checkRequested = m_mainModePending || m_nextMainModeRead == 0;
        applyMainMode();
        m_mainModePending = false;
        m_nextMainModeRead = m_clock.elapsed() + 500;
        if (checkRequested && (m_ftRadioSettingsActive || (m_voiceSideband && !m_usbFilterBeforeFt)))
        {
            emit sidebandChanged(m_session, usb, {});
        }
    }
    catch (const std::exception& error)
    {
        m_mainModePending = true;
        m_nextMainModeRead = m_clock.elapsed() + 1500;
        emit sidebandChanged(m_session, usb, QString::fromUtf8(error.what()));
    }
}

int MainControl::applyFtCompressor()
{
    int current = m_radio->compressor();
    if (!m_compressorBeforeFt)
    {
        m_compressorBeforeFt = current;
    }
    const int target = m_ftRadioSettingsActive ? 0 : *m_compressorBeforeFt;
    if (current != target)
    {
        if (m_radio->transmitting())
        {
            throw std::runtime_error("Der IC-7610 sendet.");
        }
        m_radio->setCompressor(target);
        current = m_radio->compressor();
        if (current != target)
        {
            throw std::runtime_error("IC-7610 bestätigt den TX-Kompressorwert nicht.");
        }
    }
    if (!m_ftRadioSettingsActive)
    {
        m_compressorBeforeFt.reset();
    }
    return current;
}

void MainControl::bandChanged(quint64 session)
{
    if (session == m_session && m_timer->isActive())
    {
        m_micGainDue = 0;
        for (auto& setting : m_radioSettings)
        {
            setting.nextRead = 0;
        }
    }
}

void MainControl::applyMicGain()
{
    m_micGainDue.reset();
    try
    {
        m_radio->setMicGain50Percent();
    }
    catch (const icom::CivRejected& error)
    {
        qWarning("MIC GAIN 50%%: %s", error.what());
    }
    catch (const std::exception& error)
    {
        qWarning("MIC GAIN 50%%: %s", error.what());
        m_micGainDue = m_clock.elapsed() + 1500;
    }
}

void MainControl::updateRadioSettings(qint64 now)
{
    // Prioritize writes and perform at most one setting per tick, so TX handling stays responsive.
    auto next = std::ranges::find_if(m_radioSettings, [](const auto& setting) { return setting.requested.has_value(); });
    if (next == m_radioSettings.end())
    {
        const auto compressor = m_radioSettings.begin() + Compressor;
        next = (m_ftRadioSettingsActive || m_compressorBeforeFt) && now >= compressor->nextRead
            ? compressor : std::ranges::min_element(m_radioSettings, {}, &SettingState::nextRead);
        if (now < next->nextRead)
        {
            return;
        }
    }

    const auto index = static_cast<int>(next - m_radioSettings.begin());
    const auto& command = radioSettings[index];
    const QString label = QString::fromUtf8(command.label);
    const auto requested = std::exchange(next->requested, std::nullopt);
    QString text;
    if (requested)
    {
        try
        {
            ((*m_radio).*command.write)(*requested);
        }
        catch (const std::exception&)
        {
            text = QStringLiteral("IC-7610 hat die Änderung nicht bestätigt (%1).").arg(label);
        }
    }

    int value = -1;
    try
    {
        value = index == Compressor && (m_ftRadioSettingsActive || m_compressorBeforeFt)
            ? applyFtCompressor() : ((*m_radio).*command.read)();
    }
    catch (const std::exception&)
    {
        text = QStringLiteral("%1 konnte nicht vom IC-7610 gelesen werden.").arg(label);
    }
    next->nextRead = m_clock.elapsed() + (value >= 0 ? 500 : 1500);
    if (value != next->value || requested || text != next->text)
    {
        next->value = value;
        next->text = text;
        emit radioSettingChanged(m_session, index, value, text);
    }
}

qint64 MainControl::writeFrequency(const TuneRequest& request)
{
    try
    {
        if (request.lowHz != request.highHz)
        {
            const auto current = static_cast<qint64>(m_radio->mainFrequency());
            if (request.matches(current))
            {
                return current;
            }
        }
        const auto target = std::midpoint(request.lowHz, request.highHz);
        return static_cast<qint64>(m_radio->setMainFrequency(static_cast<std::uint64_t>(target)));
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
            radio->connectDefault(icom::defaultControlPort, true);
            m_radio = std::move(radio);
            m_micGainDue = 0;
            m_mainModePending = m_voiceSideband || m_ftRadioSettingsActive || m_usbFilterBeforeFt;
        }

        m_radio->service();
        const bool micGainDue = m_micGainDue && now >= *m_micGainDue;
        const bool settingWritePending = micGainDue || m_mainModePending ||
            std::ranges::any_of(m_radioSettings, [](const auto& setting) { return setting.requested.has_value(); });
        const bool restoringVoiceSettings = !m_ftRadioSettingsActive && (m_compressorBeforeFt || m_usbFilterBeforeFt);
        if (processDigitalTx(Clock::now(), settingWritePending) ||
            processTx(Clock::now(), settingWritePending || restoringVoiceSettings))
        {
            return;
        }
        if (now >= m_nextPttRead || (!m_externalTx && (settingWritePending || m_requested)))
        {
            m_externalTx = m_radio->transmitting();
            m_nextPttRead = m_clock.elapsed() + 120;
            if (m_externalTx)
            {
                reportTx(QStringLiteral("tx"), false, QStringLiteral("TX am Funkgerät"));
            }
            else if (m_txPhase != QStringLiteral("rx"))
            {
                const bool ready = m_radio->txAudioReady();
                reportTx(QStringLiteral("rx"), ready,
                         ready ? QString() : QStringLiteral("LAN-TX-Audiokanal nicht verfügbar."));
            }
        }
        if (m_externalTx)
        {
            updateTxMeters(Clock::now());
            return;
        }
        if (micGainDue)
        {
            applyMicGain();
        }
        else
        {
            updateRadioSettings(now);
        }
        const bool frequencyDue = m_requested || now >= m_nextRead;
        if (frequencyDue)
        {
            const auto requested = m_requested;
            qint64 frequency = 0;
            if (requested)
            {
                frequency = writeFrequency(*requested);
                m_completedRevision = requested->revision;
                m_requested.reset();
                // A frequency change across bands can select another mode on the radio.
                if (m_ftRadioSettingsActive || (m_voiceSideband && requested->lowHz != requested->highHz))
                {
                    m_mainModePending = true;
                    m_nextMainModeRead = 0;
                }
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
                const QString status = requested->matches(frequency)
                    ? QStringLiteral("MAIN-Frequenz am IC-7610 bestätigt")
                    : QStringLiteral("IC-7610 meldet abweichende MAIN-Frequenz");
                emit statusChanged(m_session, m_completedRevision, status);
            }
            else if (newSession)
            {
                emit statusChanged(m_session, m_completedRevision, QStringLiteral("MAIN über LAN-CI-V verbunden"));
            }
        }
        if (!micGainDue)
        {
            updateMainMode(now);
        }
        if (!frequencyDue && !settingWritePending && now >= m_nextRxMeter)
        {
            const auto level = m_radio->readMainSMeter();
            m_nextRxMeter = m_clock.elapsed() + 150;
            emit rxMeterChanged(m_session, level.value_or(-1));
        }
    }
    catch (const std::exception& error)
    {
        const bool uncertainTx = m_ownedTx || (m_digitalTx && m_digitalTx->keyed) ||
            m_externalTx || m_txPhase == QStringLiteral("unknown");
        if (m_digitalTx)
        {
            emit digitalTxReported(m_session, m_digitalTx->token,
                                   {m_digitalTx->playback.id, RadioStatus::Failed, {}, error.what()});
            m_digitalTx.reset();
        }
        if (m_ownedTx)
        {
            cancelPtt(m_ownedTx);
        }
        else if (const auto id = currentPtt().id)
        {
            cancelPtt(id);
        }
        if (m_radio)
        {
            m_radio->releasePtt();
        }
        m_ownedTx = 0;
        m_radio.reset();
        for (int index = 0; index < RadioSettingCount; ++index)
        {
            auto& setting = m_radioSettings[index];
            if (setting.value >= 0 || !setting.text.isEmpty())
            {
                emit radioSettingChanged(m_session, index, -1, {});
            }
            setting = {};
        }
        m_micGainDue.reset();
        m_lastFrequency = 0;
        m_nextRead = 0;
        m_nextPttRead = 0;
        m_nextMainModeRead = 0;
        m_retryAfter = m_clock.elapsed() + 1500;
        const quint64 revision = m_requested ? m_requested->revision : m_completedRevision;
        emit statusChanged(m_session, revision, QStringLiteral("MAIN: ") + QString::fromUtf8(error.what()));
        reportTx(uncertainTx ? QStringLiteral("unknown") : QStringLiteral("offline"), false,
                 uncertainTx ? QStringLiteral("RX prüfen · LAN-Verbindung gestört") : QStringLiteral("LAN verbinden …"));
    }
}
