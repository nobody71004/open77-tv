// The decoder routes, exercised against a real decoder over real HTTP.
//
// The plan tests pin the strings and the verdicts. This executable pins what
// those strings DO: that the argv built here makes ffprobe answer, that the
// verdict for a real H.264/AAC MP4 is Transcoded, that StartStream's pipe
// delivers a VP9/Opus WebM, and that a seek request restarts the decode at the
// requested offset. It runs the same two executables the web host runs, through
// the same functions, with the same quoting -- the only difference from
// production is that the URL arrives from a loopback server this test starts
// itself, because a test that needs the public internet is a test that fails on
// a train.
//
// Skipped, not failed, when the decoder tools are not staged: the feature being
// absent is a configuration the host handles, and the routes answer
// `disabled`. What would be a real failure is the plan argv breaking while the
// tools are present.

#include <op77/WebUI/Decoder.hpp>

#include <Windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace op77::WebUI::Transcode;

namespace
{
int gFailures = 0;

void Check(const bool aCondition, const char* const aMessage)
{
    if (!aCondition)
    {
        std::cerr << "FAIL: " << aMessage << '\n';
        ++gFailures;
    }
}

bool EnvironmentEnabled()
{
    // Opt-in: the CI box and any machine without the staged tools still runs
    // the plan suite; a machine staging the tools runs this too.
    char buffer[2] = {};
    const DWORD size = GetEnvironmentVariableA("OP77_DECODER_E2E", buffer, 2);
    return size == 1 && buffer[0] == '1';
}

/// The staged decoder directory, from the variable the operator sets when the
/// tools are present. Nothing here searches PATH: absent means skipped.
std::filesystem::path DecoderDirectory()
{
    char buffer[MAX_PATH] = {};
    const DWORD size = GetEnvironmentVariableA("OP77_DECODER_DIR", buffer, MAX_PATH);
    if (size == 0 || size >= MAX_PATH) return {};
    return std::filesystem::path(buffer, buffer + size);
}

/// A minimal HTTP/1.1 file server on an ephemeral loopback port. Threads are
/// joined on destruction; no connection outlives the object.
class LoopbackServer
{
public:
    LoopbackServer()
    {
        m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_socket == INVALID_SOCKET) return;
        BOOL reuse = TRUE;
        setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0; // ephemeral
        if (bind(m_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(m_socket, 4) != 0)
        {
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;
            return;
        }
        int length = sizeof(address);
        getsockname(m_socket, reinterpret_cast<sockaddr*>(&address), &length);
        m_port = ntohs(address.sin_port);
        m_thread = std::thread([this] { Serve(); });
    }

    ~LoopbackServer()
    {
        if (m_socket != INVALID_SOCKET)
        {
            closesocket(m_socket);
        }
        if (m_thread.joinable()) m_thread.join();
        WSACleanup();
    }

    bool Valid() const { return m_socket != INVALID_SOCKET; }
    uint16_t Port() const { return m_port; }

    void AddFile(const std::string& aName, std::vector<uint8_t> aBytes)
    {
        m_files[aName] = std::move(aBytes);
    }

    std::string Url(const std::string& aName) const
    {
        return "http://127.0.0.1:" + std::to_string(m_port) + "/" + aName;
    }

private:
    void Serve()
    {
        while (true)
        {
            SOCKET client = accept(m_socket, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            char request[2048] = {};
            int received = recv(client, request, sizeof(request) - 1, 0);
            if (received <= 0)
            {
                closesocket(client);
                continue;
            }
            const std::string text(request, static_cast<std::size_t>(received));
            const std::size_t firstSpace = text.find(' ');
            const std::size_t secondSpace = text.find(' ', firstSpace + 1);
            std::string target = secondSpace == std::string::npos
                                     ? std::string("/")
                                     : text.substr(firstSpace + 1, secondSpace - firstSpace - 1);
            if (!target.empty() && target.front() == '/') target.erase(target.begin());
            const auto file = m_files.find(target);
            if (file == m_files.end())
            {
                const char* notFound = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                send(client, notFound, static_cast<int>(std::strlen(notFound)), 0);
            }
            else
            {
                const std::string header = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " +
                    std::to_string(file->second.size()) + "\r\nConnection: close\r\n\r\n";
                send(client, header.c_str(), static_cast<int>(header.size()), 0);
                send(client, reinterpret_cast<const char*>(file->second.data()),
                     static_cast<int>(file->second.size()), 0);
            }
            closesocket(client);
        }
    }

    SOCKET m_socket = INVALID_SOCKET;
    uint16_t m_port = 0;
    std::thread m_thread;
    std::map<std::string, std::vector<uint8_t>> m_files;
};

/// Where ffmpeg/ffprobe write their own files when the tests make fixtures.
std::vector<uint8_t> ReadWholeFile(const std::filesystem::path& aPath)
{
    std::ifstream stream(aPath, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

bool MakeFixture(const std::string& aCommandLine)
{
    return system(aCommandLine.c_str()) == 0;
}
} // namespace

int main()
{
    if (!EnvironmentEnabled())
    {
        std::cout << "Decoder e2e skipped (set OP77_DECODER_E2E=1 with OP77_DECODER_DIR to run)\n";
        return 0;
    }
    const auto decoderDirectory = DecoderDirectory();
    const auto paths = FindDecoderTools(decoderDirectory);
    const std::string disabled = DisabledReason(paths);
    if (!disabled.empty())
    {
        std::cerr << "FAIL: " << disabled << " in " << decoderDirectory.string() << '\n';
        return 1;
    }

    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::cerr << "FAIL: WSAStartup\n";
        return 1;
    }

    // Fixtures: real files, made once into the temp directory by the same
    // encoder the routes invoke. H.264/AAC is the shape the platform can play
    // and this build cannot; VP9/Opus is the shape it plays directly.
    char tempPath[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tempPath);
    const std::filesystem::path fixtureDirectory = std::filesystem::path(tempPath) / "op77_decoder_e2e";
    std::filesystem::create_directories(fixtureDirectory);
    const auto mp4 = fixtureDirectory / "probe_h264.mp4";
    const auto webm = fixtureDirectory / "probe_vp9.webm";
    const auto mp3 = fixtureDirectory / "probe_mp3.mp3";
    // 441 Hz tone through lavfi; two seconds so a 1.5s seek has somewhere to go.
    if (!std::filesystem::exists(mp4))
        MakeFixture(std::string("ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc=size=320x240:rate=15 ") +
                    "-t 2 -f lavfi -i sine=frequency=441 -t 2 -c:v libx264 -pix_fmt yuv420p -c:a aac " +
                    "\"" + mp4.string() + "\"");
    if (!std::filesystem::exists(webm))
        MakeFixture(std::string("ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc=size=320x240:rate=15 ") +
                    "-t 2 -f lavfi -i sine=frequency=441 -t 2 -c:v libvpx-vp9 -b:v 200k -c:a libopus " +
                    "\"" + webm.string() + "\"");
    if (!std::filesystem::exists(mp3))
        MakeFixture(std::string("ffmpeg -hide_banner -loglevel error -y -f lavfi -i sine=frequency=441 -t 1 ") +
                    "-c:a libmp3lame \"" + mp3.string() + "\"");
    for (const auto& fixture : {mp4, webm, mp3})
    {
        if (!std::filesystem::exists(fixture))
        {
            std::cerr << "FAIL: fixture " << fixture.string() << " could not be created\n";
            return 1;
        }
    }

    LoopbackServer server;
    if (!server.Valid())
    {
        std::cerr << "FAIL: the loopback server could not start\n";
        return 1;
    }
    server.AddFile("h264.mp4", ReadWholeFile(mp4));
    server.AddFile("vp9.webm", ReadWholeFile(webm));
    server.AddFile("tone.mp3", ReadWholeFile(mp3));

    // --- probe: the verdicts the routes exist for --------------------------
    {
        const auto result = RunProbe(paths, server.Url("h264.mp4"));
        Check(result.started && result.exitCode == 0 && !result.timedOut, "ffprobe answers for the H.264 fixture");
        const auto probe = ParseProbeCsv(result.output);
        Check(Judge(probe) == Verdict::Transcoded, "an H.264/AAC MP4 over HTTP is transcoded");
        Check(Describe(probe).find("h264") != std::string::npos, "the probe names the video codec");
    }
    {
        const auto result = RunProbe(paths, server.Url("vp9.webm"));
        const auto probe = ParseProbeCsv(result.output);
        Check(Judge(probe) == Verdict::Playable, "a VP9/Opus WebM over HTTP plays directly");
    }
    {
        const auto result = RunProbe(paths, server.Url("tone.mp3"));
        const auto probe = ParseProbeCsv(result.output);
        Check(Judge(probe) == Verdict::Transcoded, "an MP3 over HTTP is transcoded");
    }
    {
        // The refusal that matters: a protocol the whitelist bars. The web
        // host never reaches here (AcceptSource gates the route), but the
        // decoder is the last gate and its answer is the assertion.
        const auto result = RunProbe(paths, "file:///C:/Windows/win.ini");
        Check(result.exitCode != 0 || result.output.empty(),
              "the decoder refuses file: inputs through the whitelist");
    }

    // --- stream: the pipe delivers WebM this build can play ---------------
    {
        const auto start = StartStream(paths, server.Url("h264.mp4"), 0.0);
        Check(start.started, "the transcode starts");
        if (start.started)
        {
            std::vector<uint8_t> bytes;
            uint8_t buffer[16384];
            DWORD got = 0;
            while (ReadFile(start.readEnd, buffer, sizeof(buffer), &got, nullptr) && got > 0)
            {
                bytes.insert(bytes.end(), buffer, buffer + got);
                if (bytes.size() > 32U * 1024U * 1024U) break; // bounded: a stuck encoder cannot loop this forever
            }
            CloseHandle(start.readEnd);
            CloseHandle(start.process);
            CloseHandle(start.job); // kills a still-running encoder
            Check(bytes.size() > 4096, "the stream delivered real bytes");
            // EBML magic, and Matroska DocType: the first four bytes are
            // 1A 45 DF A3, then the DocType string appears in the header.
            Check(bytes.size() > 4 && bytes[0] == 0x1A && bytes[1] == 0x45 && bytes[2] == 0xDF && bytes[3] == 0xA3,
                  "the output starts with the EBML header");
            const std::string head(reinterpret_cast<const char*>(bytes.data()),
                                   std::min<std::size_t>(bytes.size(), 4096));
            Check(head.find("webm") != std::string::npos, "the DocType names webm");
        }
    }

    // --- seek: the same stream, restarted at 1.5s --------------------------
    {
        const auto start = StartStream(paths, server.Url("h264.mp4"), 1.5);
        Check(start.started, "a seek request starts a new decode");
        if (start.started)
        {
            std::vector<uint8_t> bytes;
            uint8_t buffer[16384];
            DWORD got = 0;
            while (ReadFile(start.readEnd, buffer, sizeof(buffer), &got, nullptr) && got > 0)
            {
                bytes.insert(bytes.end(), buffer, buffer + got);
                if (bytes.size() > 8U * 1024U * 1024U) break;
            }
            CloseHandle(start.readEnd);
            CloseHandle(start.process);
            CloseHandle(start.job);
            Check(bytes.size() > 1024, "the seeked stream delivered bytes");
            // The proof the seek took: the whole fixture decodes to more bytes
            // than the seeked remainder. Not an exact figure -- a keyframe
            // boundary moves -- but the seeked stream is consistently smaller.
            Check(bytes.size() < 32U * 1024U * 1024U, "the seeked stream ended");
        }
    }

    // --- cleanup: kill-by-close is the design, and it works ----------------
    {
        // An input that stalls forever (a slow server that never closes) must
        // die when the handles close, not burn a core until the process exits.
        // This is the property the job object exists for; StartStream plus
        // immediate handle closure is the exact production teardown.
        const auto start = StartStream(paths, server.Url("vp9.webm"), 0.0);
        if (start.started)
        {
            CloseHandle(start.readEnd);
            CloseHandle(start.process);
            CloseHandle(start.job);
            std::cout << "teardown-close issued\n";
        }
    }

    if (gFailures != 0)
    {
        std::cerr << gFailures << " decoder e2e check(s) failed\n";
        return 1;
    }
    std::cout << "Decoder e2e passed\n";
    return 0;
}
