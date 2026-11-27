#include <utility>
#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

#include "ft-adif-path.hpp"
#include "icom-lan.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>

namespace
{
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ws = beast::websocket;
using Tcp = net::ip::tcp;
using ErrorCode = boost::system::error_code;
using Request = http::request<http::empty_body>;
using namespace std::chrono_literals;

constexpr unsigned int defaultPort = 8086;
constexpr unsigned int defaultBackendPort = 50020;
constexpr std::size_t maxBackendLine = 32768;
constexpr std::size_t maxCommand = 4094;
constexpr std::size_t maxCommandQueue = 16384;
constexpr std::size_t spectrumQueueLimit = 65536;
constexpr std::size_t audioQueueLimit = 32768;
constexpr std::size_t maxOutputQueue = 262144;
constexpr int spectrumHeaderBytes = 28;
constexpr int maxBandBytes = 16;
constexpr int maxBins = 8192;
constexpr int audioRate = 48000;
constexpr int audioSamples = audioRate / 50;

void writeLe(std::string& buffer, std::size_t offset, std::uint32_t value, unsigned int bytes)
{
    for (unsigned int i = 0; i < bytes; ++i)
    {
        buffer[offset + i] = static_cast<char>(value & 0xff);
        value >>= 8;
    }
}

std::string spectrumPacket(const QJsonObject& message, int kind)
{
    const auto band = message.value(QStringLiteral("band")).toString().toLatin1();
    const auto levels = message.value(QStringLiteral("levels_db10")).toArray();
    if (band.isEmpty() || band.size() >= maxBandBytes || levels.isEmpty() || levels.size() > maxBins)
    {
        return {};
    }
    const auto center = message.value(QStringLiteral("center_hz")).toInt();
    if (kind == 2 && (center <= 0 || center > 60000000))
    {
        return {};
    }

    std::string packet(spectrumHeaderBytes + levels.size() * 2, '\0');
    packet.replace(0, 4, "IC76");
    packet[4] = 1;
    packet[5] = static_cast<char>(kind);
    writeLe(packet, 6, levels.size(), 2);
    writeLe(packet, 8, center, 4);
    std::copy(band.begin(), band.end(), packet.begin() + 12);
    for (int i = 0; i < levels.size(); ++i)
    {
        const auto level = static_cast<std::int16_t>(std::clamp(levels.at(i).toInt(), -1600, 0));
        writeLe(packet, spectrumHeaderBytes + i * 2, static_cast<std::uint16_t>(level), 2);
    }
    return packet;
}

std::string audioPacket(const QJsonObject& message)
{
    const auto pcm = QByteArray::fromBase64(message.value(QStringLiteral("pcm")).toString().toLatin1());
    if (pcm.size() != audioSamples * 2)
    {
        return {};
    }
    std::string packet(spectrumHeaderBytes, '\0');
    packet.replace(0, 4, "IC76");
    packet[4] = 1;
    packet[5] = 3;
    writeLe(packet, 6, audioSamples, 2);
    writeLe(packet, 8, audioRate, 4);
    packet.append(pcm.constData(), pcm.size());
    return packet;
}

bool isCommand(const QJsonObject& object)
{
    const auto op = object.value(QStringLiteral("op")).toString();
    return op == QStringLiteral("band") || op == QStringLiteral("set_main") ||
        op == QStringLiteral("step_main") || op == QStringLiteral("get_setup") ||
        op == QStringLiteral("setup") || op == QStringLiteral("sideband") ||
        op == QStringLiteral("volume") || op == QStringLiteral("audio_output") || op == QStringLiteral("rx_filter") ||
        op == QStringLiteral("ptt") || op == QStringLiteral("rf_power") ||
        op == QStringLiteral("compressor") || op == QStringLiteral("preamp") ||
        op == QStringLiteral("ft_mode") || op == QStringLiteral("ft_config") ||
        op == QStringLiteral("ft_arm") || op == QStringLiteral("ft_auto") || op == QStringLiteral("ft_cq") ||
        op == QStringLiteral("ft_start_qso") || op == QStringLiteral("ft_abort") || op == QStringLiteral("ft_free_text");
}

class BrowserSession : public std::enable_shared_from_this<BrowserSession>
{
public:
    BrowserSession(beast::tcp_stream stream, Request request, unsigned short backendPort)
        : m_socket(std::move(stream)),
          m_backend(m_socket.get_executor()),
          m_request(std::move(request)),
          m_backendAddress(net::ip::address_v4::loopback(), backendPort)
    {
    }

    void start()
    {
        beast::get_lowest_layer(m_socket).expires_never();
        m_socket.set_option(ws::stream_base::timeout{5s, 30s, true});
        m_socket.read_message_max(maxCommand);
        m_socket.async_accept(m_request,
            beast::bind_front_handler(&BrowserSession::accepted, shared_from_this()));
    }

private:
    struct Message
    {
        std::string payload;
        bool binary;
    };

    void accepted(ErrorCode error)
    {
        if (error)
        {
            stop();
            return;
        }
        m_backend.expires_after(3s);
        m_backend.async_connect(m_backendAddress,
            beast::bind_front_handler(&BrowserSession::connected, shared_from_this()));
    }

    void connected(ErrorCode error)
    {
        if (error)
        {
            backendEnded("Backend nicht erreichbar");
            return;
        }
        m_backend.expires_never();
        readBrowser();
        readBackend();
    }

    void readBrowser()
    {
        m_socket.async_read(m_incoming,
            beast::bind_front_handler(&BrowserSession::browserRead, shared_from_this()));
    }

    void browserRead(ErrorCode error, std::size_t)
    {
        if (error)
        {
            stop();
            return;
        }
        if (m_backendEnded || m_stopped)
        {
            return;
        }

        const std::string input = beast::buffers_to_string(m_incoming.data());
        m_incoming.consume(m_incoming.size());
        QJsonDocument document;
        if (m_socket.got_text())
        {
            document = QJsonDocument::fromJson(QByteArray(input.data(), input.size()));
        }
        else if (input.size() == 8 + icom::txAudioBytes && input.starts_with("ICTX"))
        {
            if (m_commandBytes > 4 * 1024)
            {
                readBrowser();
                return;
            }
            const auto id = qFromLittleEndian<quint32>(input.data() + 4);
            const QByteArray pcm(input.data() + 8, icom::txAudioBytes);
            document = QJsonDocument(QJsonObject{
                {QStringLiteral("op"), QStringLiteral("tx_audio")},
                {QStringLiteral("id"), static_cast<double>(id)},
                {QStringLiteral("pcm"), QString::fromLatin1(pcm.toBase64())},
            });
        }
        if (!document.isObject() || (m_socket.got_text() && !isCommand(document.object())))
        {
            sendStatus("Ungültiger Steuerbefehl");
            readBrowser();
            return;
        }
        const QByteArray line = document.toJson(QJsonDocument::Compact) + '\n';
        if (m_commandBytes + line.size() > maxCommandQueue)
        {
            stop();
            return;
        }
        const bool idle = m_commands.empty();
        m_commandBytes += line.size();
        m_commands.emplace_back(line.constData(), line.size());
        if (idle)
        {
            writeBackend();
        }
        readBrowser();
    }

    void writeBackend()
    {
        net::async_write(m_backend, net::buffer(m_commands.front()),
            beast::bind_front_handler(&BrowserSession::backendWritten, shared_from_this()));
    }

    void backendWritten(ErrorCode error, std::size_t)
    {
        if (m_stopped || m_backendEnded)
        {
            return;
        }
        if (error)
        {
            backendEnded("Backend-Verbindung unterbrochen");
            return;
        }
        m_commandBytes -= m_commands.front().size();
        m_commands.pop_front();
        if (!m_commands.empty())
        {
            writeBackend();
        }
    }

    void readBackend()
    {
        net::async_read_until(m_backend, net::dynamic_buffer(m_lines, maxBackendLine), '\n',
            beast::bind_front_handler(&BrowserSession::backendRead, shared_from_this()));
    }

    void backendRead(ErrorCode error, std::size_t size)
    {
        if (m_stopped || m_backendEnded)
        {
            return;
        }
        if (error)
        {
            backendEnded("Backend-Verbindung beendet");
            return;
        }

        const auto line = QByteArray::fromRawData(m_lines.data(), size - 1);
        const auto object = QJsonDocument::fromJson(line).object();
        const auto type = object.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("spectrum") || type == QStringLiteral("zoom-spectrum"))
        {
            if (m_outputBytes < spectrumQueueLimit)
            {
                const int kind = type == QStringLiteral("spectrum") ? 1 : 2;
                auto packet = spectrumPacket(object, kind);
                if (!packet.empty())
                {
                    push(std::move(packet), true);
                }
            }
        }
        else if (type == QStringLiteral("audio"))
        {
            if (m_outputBytes < audioQueueLimit)
            {
                auto packet = audioPacket(object);
                if (!packet.empty())
                {
                    push(std::move(packet), true);
                }
            }
        }
        else if (!type.isEmpty())
        {
            push(std::string(line.constData(), line.size()), false);
        }
        m_lines.erase(0, size);
        if (!m_stopped)
        {
            readBackend();
        }
    }

    void sendStatus(const char* text)
    {
        const QJsonObject status{{QStringLiteral("type"), QStringLiteral("bridge-status")},
                                 {QStringLiteral("text"), QString::fromUtf8(text)}};
        push(QJsonDocument(status).toJson(QJsonDocument::Compact).toStdString(), false);
    }

    void backendEnded(const char* text)
    {
        if (m_stopped || m_backendEnded)
        {
            return;
        }
        m_backendEnded = true;
        ErrorCode ignored;
        m_backend.socket().close(ignored);
        sendStatus(text);
    }

    void push(std::string payload, bool binary)
    {
        if (m_outputBytes + payload.size() > maxOutputQueue)
        {
            stop();
            return;
        }
        const bool idle = m_outgoing.empty();
        m_outputBytes += payload.size();
        m_outgoing.push_back({std::move(payload), binary});
        if (idle)
        {
            writeBrowser();
        }
    }

    void writeBrowser()
    {
        const auto& message = m_outgoing.front();
        m_socket.binary(message.binary);
        m_socket.async_write(net::buffer(message.payload),
            beast::bind_front_handler(&BrowserSession::browserWritten, shared_from_this()));
    }

    void browserWritten(ErrorCode error, std::size_t)
    {
        if (error || m_stopped)
        {
            stop();
            return;
        }
        m_outputBytes -= m_outgoing.front().payload.size();
        m_outgoing.pop_front();
        if (!m_outgoing.empty())
        {
            writeBrowser();
        }
        else if (m_backendEnded)
        {
            stop();
        }
    }

    void stop()
    {
        if (m_stopped)
        {
            return;
        }
        m_stopped = true;
        ErrorCode ignored;
        m_backend.socket().close(ignored);
        beast::get_lowest_layer(m_socket).socket().close(ignored);
    }

    ws::stream<beast::tcp_stream> m_socket;
    beast::tcp_stream m_backend;
    Request m_request;
    Tcp::endpoint m_backendAddress;
    beast::flat_buffer m_incoming;
    std::string m_lines;
    std::deque<std::string> m_commands;
    std::deque<Message> m_outgoing;
    std::size_t m_commandBytes = 0;
    std::size_t m_outputBytes = 0;
    bool m_backendEnded = false;
    bool m_stopped = false;
};

struct Asset
{
    std::string_view path;
    const char* resource;
    const char* mime;
};

constexpr Asset assets[] = {
    {"/", ":/web/index.html", "text/html; charset=utf-8"},
    {"/index.html", ":/web/index.html", "text/html; charset=utf-8"},
    {"/style.css", ":/web/style.css", "text/css; charset=utf-8"},
    {"/app.js", ":/web/app.js", "text/javascript; charset=utf-8"},
    {"/plots.js", ":/web/plots.js", "text/javascript; charset=utf-8"},
    {"/audio.js", ":/web/audio.js", "text/javascript; charset=utf-8"},
    {"/tx-audio.js", ":/web/tx-audio.js", "text/javascript; charset=utf-8"},
    {"/tx-worklet.js", ":/web/tx-worklet.js", "text/javascript; charset=utf-8"},
    {"/ft-panel.js", ":/web/ft-panel.js", "text/javascript; charset=utf-8"},
    {"/dxcc.js", ":/web/dxcc.js", "text/javascript; charset=utf-8"},
    {"/dxcc-data.js", ":/web/dxcc-data.js", "text/javascript; charset=utf-8"},
    {"/dxcc-flags.png", ":/web/dxcc-flags.png", "image/png"},
    {"/dxcc-LICENSE.txt", ":/web/dxcc-LICENSE.txt", "text/plain; charset=utf-8"},
};

class HttpSession : public std::enable_shared_from_this<HttpSession>
{
public:
    HttpSession(Tcp::socket socket, const Tcp::endpoint& endpoint, unsigned short backendPort)
        : m_stream(std::move(socket)),
          m_portSuffix(endpoint.port() == 80 ? "" : ":" + std::to_string(endpoint.port())),
          m_bindHost(endpoint.address().to_string() + m_portSuffix),
          m_allowLocalhost(endpoint.address() == net::ip::address_v4::loopback()),
          m_backendPort(backendPort)
    {
        m_parser.header_limit(8192);
        m_parser.body_limit(0);
    }

    void start()
    {
        m_stream.expires_after(5s);
        http::async_read(m_stream, m_buffer, m_parser,
            beast::bind_front_handler(&HttpSession::read, shared_from_this()));
    }

private:
    void read(ErrorCode error, std::size_t)
    {
        if (error)
        {
            return;
        }
        auto request = m_parser.release();
        m_head = request.method() == http::verb::head;
        m_version = request.version();
        const auto host = std::string(request[http::field::host]);
        if (host != m_bindHost && !(m_allowLocalhost && host == "localhost" + m_portSuffix))
        {
            respond(http::status::forbidden, "text/plain; charset=utf-8", "HTTP-Host ungültig.");
            return;
        }
        if (request.method() != http::verb::get && !m_head)
        {
            respond(http::status::method_not_allowed, "text/plain; charset=utf-8", "GET oder HEAD erforderlich.");
            return;
        }

        const std::string target(request.target());
        const std::string_view path(target.data(), std::min(target.size(), target.find('?')));
        if (path == "/ws")
        {
            const auto origin = std::string(request[http::field::origin]);
            if (!ws::is_upgrade(request))
            {
                respond(http::status::upgrade_required, "text/plain; charset=utf-8", "WebSocket erforderlich.");
                return;
            }
            if (origin != "http://" + host)
            {
                respond(http::status::forbidden, "text/plain; charset=utf-8", "WebSocket-Origin ungültig.");
                return;
            }
            std::make_shared<BrowserSession>(std::move(m_stream), std::move(request), m_backendPort)->start();
            return;
        }
        if (path == "/health")
        {
            respond(http::status::ok, "application/json", "{\"service\":\"kmcontrol-web\"}");
            return;
        }
        if (path == "/ft4-ft8.adi")
        {
            downloadAdif();
            return;
        }
        for (const auto& asset : assets)
        {
            if (asset.path == path)
            {
                QFile file(QString::fromLatin1(asset.resource));
                if (file.open(QIODevice::ReadOnly))
                {
                    respond(http::status::ok, asset.mime, file.readAll().toStdString());
                    return;
                }
                break;
            }
        }
        respond(http::status::not_found, "text/plain; charset=utf-8", "Datei nicht gefunden.");
    }

    void downloadAdif()
    {
        constexpr auto text = "text/plain; charset=utf-8";
        constexpr auto readError = "ADIF-Logbuch kann nicht gelesen werden.";
        const auto path = QFile::encodeName(ftAdifPath());
        const int fd = ::open(path.constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0)
        {
            const bool missing = errno == ENOENT;
            respond(missing ? http::status::not_found : http::status::internal_server_error, text,
                    missing ? "ADIF-Logbuch ist noch nicht vorhanden." : readError);
            return;
        }
        beast::file_posix file;
        file.native_handle(fd);
        struct stat info{};
        if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode))
        {
            respond(http::status::internal_server_error, text, readError);
            return;
        }

        auto response = std::make_unique<http::response<http::file_body>>(http::status::ok, m_version);
        if (::flock(fd, LOCK_SH | LOCK_NB) != 0)
        {
            const bool busy = errno == EWOULDBLOCK;
            respond(busy ? http::status::service_unavailable : http::status::internal_server_error, text,
                    busy ? "ADIF-Logbuch wird gerade geschrieben. Bitte erneut versuchen." : readError);
            return;
        }
        // Capture the committed length, then let the logbook append while we send that snapshot.
        ErrorCode error;
        response->body().reset(std::move(file), error);
        if (::flock(fd, LOCK_UN) != 0 || error)
        {
            respond(http::status::internal_server_error, text, readError);
            return;
        }
        setResponseHeaders(response->base(), "application/octet-stream");
        response->set(http::field::content_disposition, "attachment; filename=\"ft4-ft8.adi\"");
        response->keep_alive(false);
        response->content_length(response->body().size());
        m_fileResponse = std::move(response);
        if (m_head)
        {
            m_fileSerializer = std::make_unique<http::response_serializer<http::file_body>>(*m_fileResponse);
            http::async_write_header(m_stream, *m_fileSerializer,
                beast::bind_front_handler(&HttpSession::written, shared_from_this()));
            return;
        }
        http::async_write(m_stream, *m_fileResponse,
            beast::bind_front_handler(&HttpSession::written, shared_from_this()));
    }

    void setResponseHeaders(http::response_header<>& response, const char* mime)
    {
        response.set(http::field::server, "kmcontrol-web");
        response.set(http::field::content_type, mime);
        response.set(http::field::cache_control, "no-store");
        response.set("X-Content-Type-Options", "nosniff");
        auto policy = "default-src 'self'; connect-src 'self' ws://" + m_bindHost;
        if (m_allowLocalhost)
        {
            policy += " ws://localhost" + m_portSuffix;
        }
        policy += "; base-uri 'none'; frame-ancestors 'none'; form-action 'none'";
        response.set("Content-Security-Policy", policy);
    }

    void respond(http::status status, const char* mime, std::string body)
    {
        m_response.emplace(status, m_version);
        setResponseHeaders(m_response->base(), mime);
        m_response->keep_alive(false);
        m_response->content_length(body.size());
        if (m_head)
        {
            m_serializer.emplace(*m_response);
            http::async_write_header(m_stream, *m_serializer,
                beast::bind_front_handler(&HttpSession::written, shared_from_this()));
            return;
        }
        m_response->body() = std::move(body);
        http::async_write(m_stream, *m_response,
            beast::bind_front_handler(&HttpSession::written, shared_from_this()));
    }

    void written(ErrorCode, std::size_t)
    {
        ErrorCode ignored;
        m_stream.socket().shutdown(Tcp::socket::shutdown_send, ignored);
    }

    beast::tcp_stream m_stream;
    beast::flat_buffer m_buffer;
    http::request_parser<http::empty_body> m_parser;
    std::optional<http::response<http::string_body>> m_response;
    std::optional<http::response_serializer<http::string_body>> m_serializer;
    std::unique_ptr<http::response<http::file_body>> m_fileResponse;
    std::unique_ptr<http::response_serializer<http::file_body>> m_fileSerializer;
    const std::string m_portSuffix;
    const std::string m_bindHost;
    const bool m_allowLocalhost;
    unsigned short m_backendPort;
    unsigned int m_version = 11;
    bool m_head = false;
};

class WebServer
{
public:
    WebServer(net::io_context& context, const Tcp::endpoint& endpoint, unsigned short backendPort)
        : m_acceptor(context, endpoint),
          m_endpoint(endpoint),
          m_backendPort(backendPort)
    {
    }

    void start()
    {
        m_acceptor.async_accept(beast::bind_front_handler(&WebServer::accept, this));
    }

private:
    void accept(ErrorCode error, Tcp::socket socket)
    {
        if (!error)
        {
            std::make_shared<HttpSession>(std::move(socket), m_endpoint, m_backendPort)->start();
        }
        if (m_acceptor.is_open())
        {
            start();
        }
    }

    Tcp::acceptor m_acceptor;
    const Tcp::endpoint m_endpoint;
    unsigned short m_backendPort;
};

void stopOnSignal(net::io_context* context, ErrorCode error, int)
{
    if (!error)
    {
        context->stop();
    }
}

unsigned short portNumber(const QString& text)
{
    bool valid = false;
    const unsigned int number = text.toUInt(&valid);
    if (!valid || number == 0 || number > 65535)
    {
        throw std::runtime_error("Port muss zwischen 1 und 65535 liegen.");
    }
    return static_cast<unsigned short>(number);
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("kmcontrol-web"));
    QCommandLineParser options;
    options.setApplicationDescription(QStringLiteral("Weboberfläche für kmcontrol"));
    options.addHelpOption();
    const QCommandLineOption port(QStringLiteral("port"), QStringLiteral("HTTP-/WebSocket-Port"),
                                  QStringLiteral("Port"), QString::number(defaultPort));
    const QCommandLineOption backendPort(QStringLiteral("backend-port"), QStringLiteral("TCP-Port des Backends"),
                                         QStringLiteral("Port"), QString::number(defaultBackendPort));
    const QCommandLineOption listen(QStringLiteral("listen"), QStringLiteral("Lokale IPv4-Listenadresse"),
                                    QStringLiteral("IPv4"), QStringLiteral("127.0.0.1"));
    options.addOption(port);
    options.addOption(backendPort);
    options.addOption(listen);
    options.process(app);
    try
    {
        const auto webPort = portNumber(options.value(port));
        ErrorCode addressError;
        const auto address = net::ip::make_address_v4(options.value(listen).toStdString(), addressError);
        if (addressError || address.is_unspecified())
        {
            throw std::runtime_error("Eine konkrete lokale IPv4-Adresse zum Lauschen angeben.");
        }
        net::io_context context(1);
        net::signal_set signalSet(context, SIGINT, SIGTERM);
        signalSet.async_wait(beast::bind_front_handler(stopOnSignal, &context));
        WebServer server(context, Tcp::endpoint(address, webPort), portNumber(options.value(backendPort)));
        server.start();
        std::cout << "kmcontrol-WebGUI auf http://" << address.to_string() << ':' << webPort << "/\n" << std::flush;
        context.run();
    }
    catch (const std::exception& error)
    {
        std::cerr << "kmcontrol-web: " << error.what() << '\n';
        return 1;
    }
}
