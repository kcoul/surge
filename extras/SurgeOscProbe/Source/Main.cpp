#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <netinet/in.h>
#include <optional>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace
{
constexpr int defaultPort = 53280;
volatile std::sig_atomic_t keepRunning = 1;

void handleSignal (int)
{
    keepRunning = 0;
}

std::uint32_t readBE32 (const std::uint8_t* data)
{
    return (static_cast<std::uint32_t> (data[0]) << 24)
           | (static_cast<std::uint32_t> (data[1]) << 16)
           | (static_cast<std::uint32_t> (data[2]) << 8)
           | static_cast<std::uint32_t> (data[3]);
}

std::optional<std::string> readOscString (const std::vector<std::uint8_t>& data,
                                          std::size_t& offset)
{
    if (offset >= data.size())
        return std::nullopt;

    const auto start = offset;
    while (offset < data.size() && data[offset] != 0)
        ++offset;

    if (offset >= data.size())
        return std::nullopt;

    std::string result (reinterpret_cast<const char*> (data.data() + start), offset - start);

    ++offset;
    while ((offset % 4) != 0)
    {
        if (offset >= data.size())
            return std::nullopt;
        ++offset;
    }

    return result;
}

std::string hexDump (const std::vector<std::uint8_t>& data, std::size_t maxBytes = 64)
{
    std::ostringstream os;
    const auto count = std::min (data.size(), maxBytes);

    for (std::size_t i = 0; i < count; ++i)
    {
        if (i != 0)
            os << ' ';

        os << std::hex << std::setw (2) << std::setfill ('0')
           << static_cast<int> (data[i]);
    }

    if (data.size() > maxBytes)
        os << " ...";

    return os.str();
}

std::string parseOscMessage (const std::vector<std::uint8_t>& data)
{
    std::size_t offset = 0;
    auto address = readOscString (data, offset);
    if (! address || address->empty() || (*address)[0] != '/')
        return "not an OSC message";

    auto typeTags = readOscString (data, offset);
    if (! typeTags || typeTags->empty() || (*typeTags)[0] != ',')
        return "OSC address=" + *address + " (missing typetags)";

    std::ostringstream os;
    os << "OSC address=" << *address << " args=[";

    bool first = true;
    for (std::size_t i = 1; i < typeTags->size(); ++i)
    {
        if (! first)
            os << ", ";
        first = false;

        const auto tag = (*typeTags)[i];
        if (tag == 'f')
        {
            if (offset + 4 > data.size())
            {
                os << "float:<truncated>";
                break;
            }

            auto bits = readBE32 (data.data() + offset);
            float value = 0.0f;
            std::memcpy (&value, &bits, sizeof (value));
            os << value;
            offset += 4;
        }
        else if (tag == 'i')
        {
            if (offset + 4 > data.size())
            {
                os << "int:<truncated>";
                break;
            }

            os << static_cast<std::int32_t> (readBE32 (data.data() + offset));
            offset += 4;
        }
        else if (tag == 's')
        {
            auto value = readOscString (data, offset);
            os << '"' << (value ? *value : std::string ("<truncated>")) << '"';
        }
        else
        {
            os << tag << ":<unsupported>";
        }
    }

    os << ']';
    return os.str();
}

int parsePort (const char* value)
{
    char* end = nullptr;
    errno = 0;
    const auto parsed = std::strtol (value, &end, 10);

    if (errno != 0 || end == value || *end != '\0' || parsed < 1 || parsed > 65535)
        return -1;

    return static_cast<int> (parsed);
}

void printUsage (const char* argv0)
{
    std::cerr << "usage: " << argv0 << " [port]\n"
              << "default port: " << defaultPort << '\n';
}
}

int main (int argc, char** argv)
{
    int port = defaultPort;

    if (argc > 2)
    {
        printUsage (argv[0]);
        return 2;
    }

    if (argc == 2)
    {
        port = parsePort (argv[1]);
        if (port < 0)
        {
            std::cerr << "invalid port: " << argv[1] << '\n';
            return 2;
        }
    }

    std::signal (SIGINT, handleSignal);
    std::signal (SIGTERM, handleSignal);

    const int fd = ::socket (AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        std::cerr << "socket failed: " << std::strerror (errno) << '\n';
        return 1;
    }

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl (INADDR_ANY);
    address.sin_port = htons (static_cast<std::uint16_t> (port));

    if (::bind (fd, reinterpret_cast<sockaddr*> (&address), sizeof (address)) < 0)
    {
        std::cerr << "bind 0.0.0.0:" << port << " failed: " << std::strerror (errno) << '\n';
        ::close (fd);
        return 1;
    }

    std::cout << "Listening for UDP/OSC on 0.0.0.0:" << port << '\n';

    while (keepRunning)
    {
        std::vector<std::uint8_t> buffer (65536);
        sockaddr_in sender {};
        socklen_t senderLength = sizeof (sender);

        const auto bytes = ::recvfrom (fd, buffer.data(), buffer.size(), 0,
                                       reinterpret_cast<sockaddr*> (&sender), &senderLength);
        if (bytes < 0)
        {
            if (errno == EINTR)
                continue;

            std::cerr << "recvfrom failed: " << std::strerror (errno) << '\n';
            break;
        }

        buffer.resize (static_cast<std::size_t> (bytes));

        char ip[INET_ADDRSTRLEN] {};
        ::inet_ntop (AF_INET, &sender.sin_addr, ip, sizeof (ip));

        std::cout << "from " << ip << ':' << ntohs (sender.sin_port)
                  << " bytes=" << buffer.size()
                  << " " << parseOscMessage (buffer)
                  << " hex=" << hexDump (buffer)
                  << '\n';
    }

    ::close (fd);
    return 0;
}
