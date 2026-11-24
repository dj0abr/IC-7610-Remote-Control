#include "icom-lan.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace icom
{
namespace
{

using Byte = std::uint8_t;
using Packet = std::vector<Byte>;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr Byte controllerCivAddress = 0xe0;
constexpr std::string_view clientName = "kmcontrol";
constexpr std::size_t headerSize = 0x10;
constexpr std::size_t civHeaderSize = 0x15;

[[noreturn]] void systemError(const char* operation)
{
    throw std::system_error(errno, std::generic_category(), operation);
}

std::uint32_t readLe(std::span<const Byte> p, std::size_t offset, unsigned size)
{
    std::uint32_t value = 0;
    for (unsigned i = 0; i < size; ++i)
    {
        value |= static_cast<std::uint32_t>(p[offset + i]) << (8 * i);
    }
    return value;
}

void writeLe(Packet& p, std::size_t offset, unsigned size, std::uint32_t value)
{
    for (unsigned i = 0; i < size; ++i)
    {
        p[offset + i] = static_cast<Byte>(value >> (8 * i));
    }
}

void writeBe(Packet& p, std::size_t offset, unsigned size, std::uint32_t value)
{
    for (unsigned i = 0; i < size; ++i)
    {
        p[offset + size - 1 - i] = static_cast<Byte>(value >> (8 * i));
    }
}

std::uint16_t readPort(std::span<const Byte> p, std::size_t offset)
{
    return static_cast<std::uint16_t>((p[offset] << 8) | p[offset + 1]);
}

std::array<Byte, 16> passcode(std::string_view text)
{
    // The position-dependent byte mapping is documented by CardSat (see README).
    static constexpr std::array<Byte, 95> table{
        0x47, 0x5d, 0x4c, 0x42, 0x66, 0x20, 0x23, 0x46, 0x4e, 0x57,
        0x45, 0x3d, 0x67, 0x76, 0x60, 0x41, 0x62, 0x39, 0x59, 0x2d,
        0x68, 0x7e, 0x7c, 0x65, 0x7d, 0x49, 0x29, 0x72, 0x73, 0x78,
        0x21, 0x6e, 0x5a, 0x5e, 0x4a, 0x3e, 0x71, 0x2c, 0x2a, 0x54,
        0x3c, 0x3a, 0x63, 0x4f, 0x43, 0x75, 0x27, 0x79, 0x5b, 0x35,
        0x70, 0x48, 0x6b, 0x56, 0x6f, 0x34, 0x32, 0x6c, 0x30, 0x61,
        0x6d, 0x7b, 0x2f, 0x4b, 0x64, 0x38, 0x2b, 0x2e, 0x50, 0x40,
        0x3f, 0x55, 0x33, 0x37, 0x25, 0x77, 0x24, 0x26, 0x74, 0x6a,
        0x28, 0x53, 0x4d, 0x69, 0x22, 0x5c, 0x44, 0x31, 0x36, 0x58,
        0x3b, 0x7a, 0x51, 0x5f, 0x52,
    };
    std::array<Byte, 16> result{};
    if (text.empty() || text.size() > result.size())
    {
        throw std::runtime_error("Benutzername und Kennwort müssen 1 bis 16 Zeichen lang sein.");
    }
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const auto character = static_cast<Byte>(text[i]);
        if (character < 32 || character > 126)
        {
            throw std::runtime_error("Benutzername und Kennwort müssen aus druckbarem ASCII bestehen.");
        }
        const auto index = character + i;
        result[i] = table[index > 126 ? index % 127 : index - 32];
    }
    return result;
}

class UdpSocket
{
public:
    UdpSocket() : fd_(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0))
    {
        if (fd_ < 0)
        {
            systemError("UDP-Socket öffnen");
        }
    }

    ~UdpSocket()
    {
        ::close(fd_);
    }
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    int fd() const
    {
        return fd_;
    }

    sockaddr_in localAddress() const
    {
        sockaddr_in address{};
        socklen_t size = sizeof(address);
        if (getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &size) < 0)
        {
            systemError("Lokalen UDP-Port ermitteln");
        }
        return address;
    }

    void bindEphemeral(sockaddr_in address) const
    {
        address.sin_port = 0;
        if (::bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0)
        {
            systemError("Lokalen UDP-Port reservieren");
        }
    }

    void connectTo(sockaddr_in address, std::uint16_t port) const
    {
        address.sin_port = htons(port);
        if (::connect(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0)
        {
            systemError("UDP-Ziel setzen");
        }
    }

    void send(std::span<const Byte> packet) const
    {
        if (::send(fd_, packet.data(), packet.size(), 0) < 0)
        {
            systemError("UDP-Datagramm senden");
        }
    }

    Packet receive() const
    {
        std::array<Byte, 4096> buffer{};
        const auto size = ::recv(fd_, buffer.data(), buffer.size(), MSG_TRUNC);
        if (size < 0)
        {
            if (errno == EAGAIN || errno == EINTR)
            {
                return {};
            }
            systemError("UDP-Datagramm empfangen");
        }
        if (static_cast<std::size_t>(size) > buffer.size())
        {
            return {};
        }
        return Packet(buffer.begin(), buffer.begin() + size);
    }

private:
    int fd_;
};

class Channel
{
public:
    Channel(const char* name, bool verbose)
        : name_(name), verbose_(verbose)
    {
    }

    void connectTo(sockaddr_in destination, std::uint16_t port)
    {
        socket.connectTo(destination, port);
        const auto local = socket.localAddress();
        localId_ = ((ntohl(local.sin_addr.s_addr) & 0xffff) << 16) | ntohs(local.sin_port);
        connected = true;
    }

    Packet packet(std::size_t size, std::uint16_t type = 0, std::uint16_t seq = 0) const
    {
        Packet result(size, 0);
        writeLe(result, 0x00, 4, size);
        writeLe(result, 0x04, 2, type);
        writeLe(result, 0x06, 2, seq);
        writeLe(result, 0x08, 4, localId_);
        writeLe(result, 0x0c, 4, remoteId);
        return result;
    }

    void send(const Packet& p) const
    {
        trace("TX", p);
        socket.send(p);
    }

    void sendTracked(Packet p)
    {
        writeLe(p, 0x06, 2, sequence_);
        constexpr std::uint16_t retainedPackets = 256;
        sent_.erase(static_cast<std::uint16_t>(sequence_ - retainedPackets));
        sent_[sequence_] = p;
        ++sequence_;
        send(p);
        nextIdle_ = Clock::now() + 100ms;
    }

    Packet receive()
    {
        auto p = socket.receive();
        if (p.size() < headerSize || readLe(p, 0x00, 4) != p.size()
            || readLe(p, 0x0c, 4) != localId_
            || (remoteId && readLe(p, 0x08, 4) != remoteId))
    {
            return {};
        }
        trace("RX", p);
        const auto type = readLe(p, 0x04, 2);
        const auto sequence = static_cast<std::uint16_t>(readLe(p, 0x06, 2));
        if (type == 0x07 && p.size() == 0x15 && p[0x10] == 0)
        {
            auto pong = packet(p.size(), 0x07, sequence);
            pong[0x10] = 1;
            std::copy(p.begin() + 0x11, p.end(), pong.begin() + 0x11);
            send(pong);
        }
        if (type == 0x01)
        {
            if (p.size() == headerSize)
            {
                resend(sequence);
            }
            else
            {
                for (std::size_t i = headerSize; i + 1 < p.size(); i += 2)
                {
                    resend(static_cast<std::uint16_t>(readLe(p, i, 2)));
                }
            }
        }
        if (type == 0x05 && p.size() == headerSize)
        {
            ready = false;
            throw std::runtime_error(std::string(name_) + ": Gegenstelle hat die Sitzung beendet.");
        }
        return p;
    }

    void keepAlive(Clock::time_point now)
    {
        if (!ready)
        {
            return;
        }
        if (now >= nextIdle_)
        {
            sendTracked(packet(headerSize));
        }
        if (now >= nextPing_)
        {
            auto ping = packet(0x15, 0x07, pingSequence_++);
            const auto ticks = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();
            writeLe(ping, 0x11, 4, static_cast<std::uint32_t>(ticks));
            send(ping);
            nextPing_ = now + 500ms;
        }
    }

    UdpSocket socket;
    bool connected = false;
    bool ready = false;
    std::uint32_t remoteId = 0;

private:
    void resend(std::uint16_t seq) const
    {
        if (const auto found = sent_.find(seq); found != sent_.end())
        {
            send(found->second);
        }
    }

    void trace(const char* direction, const Packet& p) const
    {
        const auto type = readLe(p, 0x04, 2);
        if (verbose_ && type != 0x07 && !(type == 0 && p.size() == headerSize))
        {
            std::cerr << name_ << ' ' << direction << ' ' << p.size()
                      << " Byte, Typ 0x" << std::hex << type
                      << ", Sequenz 0x" << readLe(p, 0x06, 2) << std::dec << '\n';
        }
    }

    const char* name_;
    bool verbose_;
    std::uint32_t localId_ = 0;
    std::uint16_t sequence_ = 1;
    std::uint16_t pingSequence_ = 0;
    std::map<std::uint16_t, Packet> sent_;
    Clock::time_point nextIdle_{};
    Clock::time_point nextPing_{};
};

struct PacketShape
{
    std::size_t size;
    std::uint16_t type;

    bool operator()(const Packet& packet) const
    {
        return packet.size() == size && readLe(packet, 0x04, 2) == type;
    }
};

struct LoginReply
{
    std::uint16_t request;

    bool operator()(const Packet& packet) const
    {
        return PacketShape{0x60, 0}(packet) && readLe(packet, 0x1a, 2) == request;
    }
};

struct RadioInfo
{
    Packet& capabilities;
    Packet& connection;

    bool operator()(const Packet& packet) const
    {
        if (PacketShape{0xa8, 0}(packet))
        {
            capabilities = packet;
        }
        if (PacketShape{0x90, 0}(packet))
        {
            connection = packet;
        }
        return !capabilities.empty() && !connection.empty();
    }
};

struct IgnorePacket
{
    bool operator()(const Packet&) const
    {
        return false;
    }
};

struct CivReply
{
    std::span<const Byte> command;
    std::array<Byte, 4> address;
    bool writing;

    bool operator()(const Packet& incoming) const
    {
        const auto offset = civHeaderSize + address.size();
        if (incoming.size() < offset + 2 || readLe(incoming, 0x04, 2) != 0 ||
            incoming[0x10] != 0xc1 ||
            readLe(incoming, 0x11, 2) != incoming.size() - civHeaderSize ||
            incoming.back() != 0xfd ||
            !std::equal(address.begin(), address.end(), incoming.begin() + civHeaderSize))
        {
            return false;
        }
        const bool ack = incoming.size() == offset + 2;
        if (ack && incoming[offset] == 0xfa)
        {
            throw CivRejected("Der IC-7610 hat den CI-V-Befehl abgewiesen.");
        }
        if (writing)
        {
            return ack && incoming[offset] == 0xfb;
        }
        return incoming.size() > offset + command.size() + 1 &&
               std::equal(command.begin(), command.end(), incoming.begin() + offset);
    }
};

std::uint64_t decodeFrequency(std::span<const Byte> bytes)
{
    if (bytes.size() != 5)
    {
        throw std::runtime_error("Unerwartete Länge der CI-V-Frequenzantwort.");
    }
    std::uint64_t hz = 0;
    for (auto it = bytes.rbegin(); it != bytes.rend(); ++it)
    {
        const auto low = *it & 0x0f;
        const auto high = *it >> 4;
        if (low > 9 || high > 9)
        {
            throw std::runtime_error("Ungültige BCD-Frequenz in der CI-V-Antwort.");
        }
        hz = hz * 100 + high * 10 + low;
    }
    return hz;
}

class Radio
{
public:
    Radio(bool verbose, std::string_view address)
        : control_("Control", verbose), civ_("CI-V", verbose)
    {
        destination_.sin_family = AF_INET;
        if (inet_pton(AF_INET, std::string(address).c_str(), &destination_.sin_addr) != 1)
        {
            throw std::runtime_error("Ungültige IC-7610-IP.");
        }
    }

    ~Radio()
    {
        // Best-effort cleanup also applies if a read or stream setup fails.
        try
        {
            if (civOpen_)
            {
                openCiv(false);
            }
            if (civ_.remoteId)
            {
                civ_.send(civ_.packet(headerSize, 0x05));
            }
        }
        catch (const std::exception&)
        {
        }
        try
        {
            if (authenticated_)
            {
                sendToken(0x01);
            }
            if (control_.remoteId)
            {
                control_.send(control_.packet(headerSize, 0x05));
            }
        }
        catch (const std::exception&)
        {
        }
    }

    void login(const std::array<Byte, 16>& user, const std::array<Byte, 16>& password,
               std::uint16_t port)
    {
        control_.connectTo(destination_, port);
        handshake(control_, "Keine LAN-Antwort auf dem Control-Port (Are You There).");

        std::random_device random;
        tokenRequest_ = std::uniform_int_distribution<std::uint16_t>(1, 65535)(random);
        auto login = authPacket(0x80, 0x00);
        std::copy(user.begin(), user.end(), login.begin() + 0x40);
        std::copy(password.begin(), password.end(), login.begin() + 0x50);
        std::copy(clientName.begin(), clientName.end(), login.begin() + 0x60);
        control_.sendTracked(std::move(login));
        const auto response = waitFor(control_, LoginReply{tokenRequest_});
        if (response.empty())
        {
            throw std::runtime_error("Das Funkgerät hat die Anmeldung nicht beantwortet.");
        }
        if (readLe(response, 0x30, 4) != 0)
        {
            throw std::runtime_error("Das Funkgerät hat die Anmeldung abgewiesen.");
        }
        token_ = readLe(response, 0x1c, 4);
        authenticated_ = true;
        sendToken(0x02);
        tokenRenewal_ = Clock::now() + 50s;
    }

    void openSerial(const std::array<Byte, 16>& user)
    {
        Packet capabilities;
        Packet connection;
        const auto available = waitFor(control_, RadioInfo{capabilities, connection});
        if (available.empty())
        {
            throw std::runtime_error("Geräteinformationen nach der Anmeldung fehlen.");
        }
        if (readLe(connection, 0x60, 4) != 0 && readLe(connection, 0x84, 4) != 0)
        {
            throw std::runtime_error("Das Funkgerät meldet eine bereits belegte Sitzung.");
        }
        radioCivAddress_ = capabilities[0x94];
        if (radioCivAddress_ == 0 || radioCivAddress_ >= 0xe0)
        {
            throw std::runtime_error("Das Funkgerät hat keine gültige CI-V-Adresse gemeldet.");
        }

        const auto local = control_.socket.localAddress();
        civ_.socket.bindEphemeral(local);
        // ConnInfo has an audio port field even when both audio directions are disabled.
        UdpSocket audioPortReservation;
        audioPortReservation.bindEphemeral(local);

        auto request = authPacket(0x90, 0x03);
        if (readLe(capabilities, 0x49, 2) == 0x8010)
        {
            writeLe(request, 0x27, 2, 0x8010);
            std::copy_n(capabilities.begin() + 0x4c, 6, request.begin() + 0x2a);
        }
        else
        {
            std::copy_n(capabilities.begin() + 0x42, 16, request.begin() + 0x20);
        }
        std::copy_n(capabilities.begin() + 0x52, 32, request.begin() + 0x40);
        std::copy(user.begin(), user.end(), request.begin() + 0x60);
        writeBe(request, 0x7c, 4, ntohs(civ_.socket.localAddress().sin_port));
        writeBe(request, 0x80, 4, ntohs(audioPortReservation.localAddress().sin_port));
        request[0x88] = 1;
        control_.sendTracked(std::move(request));

        const auto status = waitFor(control_, PacketShape{0x50, 0});
        if (status.empty())
        {
            throw std::runtime_error("Keine Antwort auf die Anforderung des CI-V-Kanals.");
        }
        const auto port = readPort(status, 0x42);
        if (readLe(status, 0x30, 4) != 0 || status[0x40] != 0 || port == 0)
        {
            throw std::runtime_error("Das Funkgerät hat den CI-V-Kanal nicht freigegeben.");
        }
        civ_.connectTo(destination_, port);
        handshake(civ_, "Keine Antwort auf dem ausgehandelten CI-V-Port.");
        openCiv(true);
        civOpen_ = true;
    }

    std::uint64_t frequency()
    {
        return decodeFrequency(query({0x03}));
    }

    std::uint64_t mainFrequency()
    {
        return decodeFrequency(query({0x25, 0x00}));
    }

    std::uint64_t setMainFrequency(std::uint64_t hz)
    {
        if (hz < 30000 || hz > 60000000)
        {
            throw std::runtime_error("MAIN-Frequenz außerhalb des Empfangsbereichs.");
        }

        std::array<Byte, 7> command{0x25, 0x00};
        for (std::size_t i = 2; i < command.size(); ++i)
        {
            command[i] = static_cast<Byte>(((hz / 10 % 10) << 4) | (hz % 10));
            hz /= 100;
        }
        exchangeCiv(command, true);
        return mainFrequency();
    }

    void service()
    {
        waitFor(control_, IgnorePacket{}, 2ms);
    }

    std::string mode()
    {
        const auto bytes = query({0x04});
        const auto dataMode = query({0x1a, 0x06});
        if (bytes.empty() || bytes.size() > 2 || dataMode.size() != 2 || dataMode[0] > 3)
        {
            throw std::runtime_error("Ungültige CI-V-Antwort zur Betriebsart.");
        }
        static constexpr std::array<std::string_view, 20> modes{
            "LSB", "USB", "AM", "CW", "RTTY", "FM", "", "CW-R", "RTTY-R", "",
            "", "", "", "", "", "", "", "", "PSK", "PSK-R",
        };
        const auto code = bytes.front();
        std::string result;
        if (code < modes.size() && !modes[code].empty())
        {
            result = modes[code];
        }
        else
        {
            constexpr std::string_view hex = "0123456789ABCDEF";
            result = std::string("unbekannt (CI-V 0x") + hex[code >> 4] + hex[code & 0x0f];
            result.push_back(')');
        }
        if (dataMode[0] != 0)
        {
            result += " (DATA" + std::to_string(dataMode[0]) + ')';
        }
        return result;
    }

private:
    template <typename Predicate>
    Packet waitFor(Channel& target, Predicate matches,
                   std::chrono::milliseconds duration = 3000ms)
    {
        const auto deadline = Clock::now() + duration;
        for (auto now = Clock::now(); now < deadline; now = Clock::now())
        {
            control_.keepAlive(now);
            civ_.keepAlive(now);
            if (authenticated_ && now >= tokenRenewal_)
            {
                sendToken(0x05);
                tokenRenewal_ = now + 50s;
            }
            std::array<pollfd, 2> fds{{
                {control_.socket.fd(), POLLIN, 0},
                {civ_.connected ? civ_.socket.fd() : -1, POLLIN, 0},
            }};
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
            const auto timeout = static_cast<int>(std::min(remaining, 50ms).count());
            if (::poll(fds.data(), fds.size(), timeout) < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                systemError("Auf UDP warten");
            }
            for (std::size_t i = 0; i < fds.size(); ++i)
            {
                if (fds[i].revents & (POLLIN | POLLERR))
                {
                    auto& channel = i == 0 ? control_ : civ_;
                    auto packet = channel.receive();
                    if (&channel == &target && !packet.empty() && matches(packet))
                    {
                        return packet;
                    }
                }
            }
        }
        return {};
    }

    Packet exchangeHandshake(Channel& channel, std::uint16_t outgoing,
                             std::uint16_t expected, std::uint16_t sequence)
    {
        for (int attempt = 0; attempt < 6; ++attempt)
        {
            channel.send(channel.packet(headerSize, outgoing, sequence));
            auto reply = waitFor(channel, PacketShape{headerSize, expected}, 500ms);
            if (!reply.empty())
            {
                return reply;
            }
        }
        return {};
    }

    void handshake(Channel& channel, const char* timeout)
    {
        const auto here = exchangeHandshake(channel, 0x03, 0x04, 0);
        if (here.empty())
        {
            throw std::runtime_error(timeout);
        }
        channel.remoteId = readLe(here, 0x08, 4);
        if (channel.remoteId == 0)
        {
            throw std::runtime_error("Die LAN-Antwort enthält keine Sitzungskennung.");
        }
        if (exchangeHandshake(channel, 0x06, 0x06, 1).empty())
        {
            throw std::runtime_error("Der LAN-Handshake blieb bei Are You Ready stehen.");
        }
        channel.ready = true;
    }

    Packet authPacket(std::size_t size, Byte subtype)
    {
        auto p = control_.packet(size);
        writeBe(p, 0x10, 4, size - headerSize);
        p[0x14] = 1;
        p[0x15] = subtype;
        writeBe(p, 0x16, 2, authSequence_++);
        writeLe(p, 0x1a, 2, tokenRequest_);
        writeLe(p, 0x1c, 4, token_);
        return p;
    }

    void sendToken(Byte subtype)
    {
        auto p = authPacket(0x40, subtype);
        writeBe(p, 0x24, 2, 0x0798);
        control_.sendTracked(std::move(p));
    }

    void openCiv(bool open)
    {
        auto p = civ_.packet(0x16);
        writeLe(p, 0x10, 2, 0x01c0);
        writeBe(p, 0x13, 2, civSequence_++);
        p[0x15] = open ? 0x04 : 0x00;
        civ_.sendTracked(std::move(p));
    }

    Packet query(std::initializer_list<Byte> command)
    {
        return exchangeCiv(command, false);
    }

    Packet exchangeCiv(std::span<const Byte> command, bool writing)
    {
        const std::array<Byte, 4> requestStart{0xfe, 0xfe, radioCivAddress_, controllerCivAddress};
        const std::array<Byte, 4> replyStart{0xfe, 0xfe, controllerCivAddress, radioCivAddress_};
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            const auto length = requestStart.size() + command.size() + 1;
            auto packet = civ_.packet(civHeaderSize + length);
            packet[0x10] = 0xc1;
            writeLe(packet, 0x11, 2, length);
            writeBe(packet, 0x13, 2, civSequence_++);
            std::copy(requestStart.begin(), requestStart.end(), packet.begin() + civHeaderSize);
            std::copy(command.begin(), command.end(),
                      packet.begin() + civHeaderSize + requestStart.size());
            packet.back() = 0xfd;
            civ_.sendTracked(std::move(packet));

            const auto reply = waitFor(civ_, CivReply{command, replyStart, writing}, 900ms);
            if (!reply.empty())
            {
                if (writing)
                {
                    return {};
                }
                return Packet(reply.begin() + civHeaderSize + replyStart.size() + command.size(),
                              reply.end() - 1);
            }
        }
        throw std::runtime_error("Keine passende Antwort auf CI-V-Befehl " +
                                 std::to_string(command.front()) + " (dezimal).");
    }

    sockaddr_in destination_{};
    Channel control_;
    Channel civ_;
    Clock::time_point tokenRenewal_{};
    bool authenticated_ = false;
    bool civOpen_ = false;
    Byte radioCivAddress_ = 0;
    std::uint32_t token_ = 0;
    std::uint16_t tokenRequest_ = 0;
    std::uint16_t authSequence_ = 0;
    std::uint16_t civSequence_ = 0;
};

} // namespace

class LanRadio::Impl
{
public:
    Impl(bool verbose, std::string_view address)
        : radio(verbose, address)
    {
    }

    Radio radio;
};

LanRadio::LanRadio(bool verbose, std::string_view address)
    : m_impl(std::make_unique<Impl>(verbose, address))
{
}

LanRadio::~LanRadio() = default;

void LanRadio::connectDefault(std::uint16_t port)
{
    const char* user = std::getenv("KMCONTROL_ICOM_USER");
    const char* password = std::getenv("KMCONTROL_ICOM_PASSWORD");
    if (!user || !*user || !password)
    {
        throw std::runtime_error("KMCONTROL_ICOM_USER und KMCONTROL_ICOM_PASSWORD für die IC-7610-Anmeldung setzen.");
    }
    connect(user, password, port);
}

void LanRadio::connect(std::string_view user, std::string_view password, std::uint16_t port)
{
    const auto encodedUser = passcode(user);
    m_impl->radio.login(encodedUser, passcode(password), port);
    m_impl->radio.openSerial(encodedUser);
}

std::uint64_t LanRadio::frequency()
{
    return m_impl->radio.frequency();
}

std::string LanRadio::mode()
{
    return m_impl->radio.mode();
}

std::uint64_t LanRadio::mainFrequency()
{
    return m_impl->radio.mainFrequency();
}

std::uint64_t LanRadio::setMainFrequency(std::uint64_t hz)
{
    return m_impl->radio.setMainFrequency(hz);
}

void LanRadio::service()
{
    m_impl->radio.service();
}
}
