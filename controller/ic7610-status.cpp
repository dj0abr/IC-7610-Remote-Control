#include "icom-lan.hpp"

#include <charconv>
#include <cerrno>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <termios.h>
#include <unistd.h>

namespace
{
[[noreturn]] void systemError(const char* operation)
{
    throw std::system_error(errno, std::generic_category(), operation);
}

std::string readPassword()
{
    const bool terminal = isatty(STDIN_FILENO);
    termios previous{};
    if (terminal)
    {
        if (tcgetattr(STDIN_FILENO, &previous) < 0)
        {
            systemError("Terminaleinstellungen lesen");
        }
        auto hidden = previous;
        hidden.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) < 0)
        {
            systemError("Kennworteingabe ausblenden");
        }
        std::cerr << "Kennwort für den IC-7610: ";
    }
    std::string password;
    const bool received = static_cast<bool>(std::getline(std::cin, password));
    if (terminal)
    {
        const int restored = tcsetattr(STDIN_FILENO, TCSANOW, &previous);
        std::cerr << '\n';
        if (restored < 0)
        {
            systemError("Terminaleinstellungen wiederherstellen");
        }
    }
    if (!received)
    {
        throw std::runtime_error("Kein Kennwort auf der Standardeingabe empfangen.");
    }
    return password;
}

}
int main(int argc, char** argv)
{
    constexpr char usage[] = "Aufruf: ic7610-status [BENUTZER] [--port PORT] [--verbose]\n";
    if (argc == 2 && std::string_view(argv[1]) == "--help")
    {
        std::cout << usage
                  << "Liest Frequenz und Betriebsart von 192.168.10.133 über LAN.\n"
                     "Control-Port standardmäßig 50001.\n"
                     "Ohne BENUTZER werden KMCONTROL_ICOM_USER und KMCONTROL_ICOM_PASSWORD verwendet.\n"
                     "Mit BENUTZER wird das Kennwort am Terminal oder über stdin eingelesen.\n";
        return 0;
    }
    const bool customUser = argc > 1 && !std::string_view(argv[1]).starts_with('-');

    bool verbose = false;
    std::uint16_t port = icom::defaultControlPort;
    for (int i = customUser ? 2 : 1; i < argc; ++i)
    {
        const std::string_view option(argv[i]);
        if (option == "--verbose")
        {
            verbose = true;
        }
        else if (option == "--port" && i + 1 < argc)
        {
            const std::string_view value(argv[++i]);
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), port);
            if (error != std::errc{} || end != value.data() + value.size() || port == 0)
            {
                std::cerr << "Der Control-Port muss zwischen 1 und 65535 liegen.\n";
                return 2;
            }
        }
        else
        {
            std::cerr << usage;
            return 2;
        }
    }
    try
    {
        icom::LanRadio radio(verbose);
        if (customUser)
        {
            radio.connect(argv[1], readPassword(), port);
        }
        else
        {
            radio.connectDefault(port);
        }
        std::cout << "Anmeldung am IC-7610 (" << icom::radioAddress << ") erfolgreich.\n";
        const auto hz = radio.frequency();
        const auto mode = radio.mode();
        std::cout << "Frequenz: " << hz << " Hz\n"
                  << "Betriebsart: " << mode << '\n';
    }
    catch (const std::exception& e)
    {
        std::cerr << "Fehler (" << icom::radioAddress << ", UDP-Control-Port " << port
                  << "): " << e.what() << '\n';
        return 1;
    }
}
