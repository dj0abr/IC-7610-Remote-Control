#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace icom
{
inline constexpr std::uint16_t defaultControlPort = 50001;
inline constexpr char radioAddress[] = "192.168.10.133";

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

    void connectDefault(std::uint16_t port = defaultControlPort);
    void connect(std::string_view user, std::string_view password,
                 std::uint16_t port = defaultControlPort);
    std::uint64_t frequency();
    std::string mode();
    std::uint64_t mainFrequency();
    std::uint64_t setMainFrequency(std::uint64_t hz);
    void service();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
}
