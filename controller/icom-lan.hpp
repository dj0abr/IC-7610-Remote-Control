#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace icom
{
inline constexpr std::uint16_t defaultControlPort = 50001;
inline constexpr char radioAddress[] = "192.168.10.133";
inline constexpr int maxRfPowerLevel = 255;
inline constexpr int txAudioRate = 16000;
inline constexpr int txAudioBytes = txAudioRate / 50 * 2;

class CivRejected : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

class LanRadio
{
public:
    explicit LanRadio(bool verbose = false, std::string_view address = radioAddress);
    ~LanRadio();

    void connectDefault(std::uint16_t port = defaultControlPort, bool withAudio = false);
    void connect(std::string_view user, std::string_view password,
                 std::uint16_t port = defaultControlPort, bool withAudio = false);
    std::uint64_t frequency();
    std::string mode();
    std::uint64_t mainFrequency();
    std::uint64_t setMainFrequency(std::uint64_t hz);
    int rfPower();
    void setRfPower(int level);
    void setMicGain50Percent();
    int compressor();
    void setCompressor(int enabled);
    int subPreamp();
    void setSubPreamp(int level);
    void service();
    bool txAudioReady() const;
    bool transmitting();
    bool voiceTxMode();
    void setPtt(bool pressed);
    void releasePtt() noexcept;
    void sendTxAudio(std::span<const char> pcm);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
}
