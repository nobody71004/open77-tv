// Pins the two things about making somebody else's video playable that fail
// silently.
//
// The first is argument quoting. A link a player pastes reaches an external
// process, and the quoting rule it has to survive is `CommandLineToArgvW`'s,
// which is not "wrap it in quotes". A URL may legally contain a space, a quote
// and a trailing backslash, and the failure mode of getting this wrong is not a
// crash: it is *extra arguments*. `--foo bar` becomes two arguments, an
// operator sees a decoder error about an option nobody typed, and the link is
// blamed. The round-trip cases below are the assertion that matters -- take the
// command line apart the way Windows does and check the arguments come back
// identical -- with the specific shapes that break naive quoting named
// individually so a regression says which one it was.
//
// The second is the verdict. "Can this play" is answered by the decoder, read
// back from ffprobe, and the answer decides between handing a link to a
// `<video>` element and re-encoding it. Getting it wrong in one direction is a
// black rectangle; in the other, every video on every television is needlessly
// decoded twice. Both are unreachable from a page's own log, which is why the
// judgement is a pure function with its cases written down.
//
// The ffprobe outputs below are not invented. They are the real bytes the tool
// emits for a synthetic H.264/AAC MP4, a VP9/Opus WebM and an MP3, captured
// from the run recorded in `docs/research/webui-media-and-audio.md`.

#include <op77/WebUI/TranscodePlan.hpp>
#include <op77/WebUI/Decoder.hpp>

#include <Windows.h>
#include <shellapi.h>

#include <iostream>
#include <string>
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

void CheckEqual(const std::string& aActual, const std::string& aExpected, const char* const aMessage)
{
    if (aActual != aExpected)
    {
        std::cerr << "FAIL: " << aMessage << "\n  expected: [" << aExpected << "]\n  actual:   [" << aActual
                  << "]\n";
        ++gFailures;
    }
}

/// Splits a command line exactly the way Windows does.
///
/// `CommandLineToArgvW` is the authority this quoting has to satisfy, so the
/// test asks it rather than re-implementing the rule on the other side -- a
/// hand-written inverse would agree with a wrong implementation as easily as a
/// right one.
std::vector<std::string> SplitLikeWindows(const std::string& aCommandLine)
{
    const std::wstring wide(aCommandLine.begin(), aCommandLine.end());
    int count = 0;
    auto* argv = CommandLineToArgvW(wide.c_str(), &count);
    std::vector<std::string> arguments;
    if (argv == nullptr) return arguments;
    for (int index = 0; index < count; ++index)
    {
        const std::wstring value(argv[index]);
        arguments.emplace_back(value.begin(), value.end());
    }
    LocalFree(argv);
    return arguments;
}

void RoundTrips(const std::vector<std::string>& aArguments, const char* const aMessage)
{
    const auto recovered = SplitLikeWindows(BuildCommandLine(aArguments));
    if (recovered.size() != aArguments.size())
    {
        std::cerr << "FAIL: " << aMessage << ": " << aArguments.size() << " arguments became "
                  << recovered.size() << " -- one of them was split\n";
        ++gFailures;
        return;
    }
    for (std::size_t index = 0; index < aArguments.size(); ++index)
    {
        if (recovered[index] != aArguments[index])
        {
            std::cerr << "FAIL: " << aMessage << ": argument " << index << " changed\n  expected: ["
                      << aArguments[index] << "]\n  actual:   [" << recovered[index] << "]\n";
            ++gFailures;
        }
    }
}

std::string Accepted(const char* const aUrl)
{
    std::string error;
    if (AcceptSource(aUrl, error)) return {};
    return error;
}

void TestSourcePolicy()
{
    std::string error;
    Check(AcceptSource("http://example.com/a.mp4", error), "http is fetchable");
    Check(AcceptSource("https://example.com/a.mp4", error), "https is fetchable");
    Check(AcceptSource("HTTPS://EXAMPLE.COM/a.mp4", error), "the scheme is case-insensitive");
    Check(AcceptSource("https://example.com", error), "a bare host is fetchable");
    Check(AcceptSource("https://example.com:8443/live/index.m3u8?t=1&u=2", error),
          "a port, query and path are fetchable");
    // The addresses that exist to make a decoder read the local machine.
    Check(!AcceptSource("file:///C:/Windows/win.ini", error), "file: must be refused");
    Check(!AcceptSource("concat:http://a|http://b", error), "concat: must be refused");
    Check(!AcceptSource("subfile:http://example.com/a,0,100", error), "subfile: must be refused");
    Check(!AcceptSource("data:video/mp4;base64,AAAA", error), "data: must be refused");
    Check(!AcceptSource("", error), "an empty link must be refused");
    CheckEqual(Accepted(""), "no link was given", "the empty-link reason is the page's to show");
    // A URL that reads as one host and resolves to another.
    Check(!AcceptSource("https://good.example@evil.example/x", error),
          "a user-information @ in the authority must be refused");
    Check(!AcceptSource("https://exa mple.com/a", error), "a spaced host must be refused");
    Check(!AcceptSource(std::string("https://") + std::string(5000, 'a'), error),
          "an over-long link must be refused");
    // A control character is how a command line gets a new line into it.
    Check(!AcceptSource("https://example.com/a\nb", error), "a newline in a link must be refused");
    Check(!AcceptSource("https://example.com/a\tb", error), "a tab in a link must be refused");
    // Note what is NOT a host check: a path may contain anything.
    Check(AcceptSource("https://example.com/a b/c\"d/e\\f", error),
          "spaces, quotes and backslashes in the PATH are legal and must survive");
}

void TestQuoting()
{
    CheckEqual(QuoteWindowsArgument("plain"), "plain", "an argument needing no quoting is untouched");
    CheckEqual(QuoteWindowsArgument("has space"), "\"has space\"", "a space forces quotes");
    CheckEqual(QuoteWindowsArgument(""), "\"\"", "an empty argument stays an argument");
    CheckEqual(QuoteWindowsArgument("say \"hi\""), "\"say \\\"hi\\\"\"", "a quote is escaped");
    CheckEqual(QuoteWindowsArgument("trailing\\"), "trailing\\",
               "a lone backslash with nothing to quote needs no quoting");
    CheckEqual(QuoteWindowsArgument("quoted trailing\\"), "\"quoted trailing\\\\\"",
               "a trailing backslash inside quotes is doubled so it cannot eat the closing quote");
    CheckEqual(QuoteWindowsArgument("a\"b\\"), "\"a\\\"b\\\\\"", "quote then backslash");
    CheckEqual(QuoteWindowsArgument("\\\\server\\share"), "\\\\server\\share",
               "a backslash with no space or quote needs no quoting");

    // The property, over the shapes that actually break naive quoting.
    RoundTrips({"ffmpeg", "-i", "https://example.com/a.mp4", "-f", "webm", "-"}, "a plain argv");
    RoundTrips({"ffmpeg", "-i", "https://example.com/my video.mp4"}, "a URL with a space");
    RoundTrips({"ffmpeg", "-i", "https://example.com/a\"b.mp4"}, "a URL with a quote");
    RoundTrips({"ffmpeg", "-i", "https://example.com/a\\"}, "a URL ending in a backslash");
    RoundTrips({"ffmpeg", "-i", "https://example.com/a\\\"b\\"}, "a URL with backslash-quote-backslash");
    RoundTrips({"ffmpeg", "-vf", "scale=-2:min(1080\\,ih)"}, "the escaped-comma filter");
    RoundTrips({"ffmpeg", "-i", ""}, "an empty argument in the middle");
    RoundTrips({"-i", "https://example.com/\"\"\"\"", "-y", "-"}, "a run of quotes");

    // A URL that would have become extra arguments if the quoting were naive.
    const std::string hostile = "https://example.com/a.mp4 -f lavfi -i evil";
    const auto line = BuildCommandLine({"ffmpeg", "-i", hostile, "-f", "webm", "-"});
    const auto recovered = SplitLikeWindows(line);
    Check(recovered.size() == 6, "a link that looks like options must stay one argument");
    // The one place this test may not be neutral: the argv builder must never be
    // handed a link this check would have refused, so the two are asserted together.
    std::string error;
    Check(AcceptSource(hostile, error), "the hostile sample is not itself refused for other reasons");
}

void TestRouteQueries()
{
    // The routes are parsed by the same plan the pages uses, so the shapes a
    // pasted link takes through encodeURIComponent are pinned here.
    const auto plain = ParseRouteQuery("u=https%3A%2F%2Fexample.com%2Fa.mp4");
    Check(plain.error.empty(), "a plain encoded link parses");
    CheckEqual(plain.source, "https://example.com/a.mp4", "percent-decoding round-trips");
    Check(plain.startSeconds == 0.0, "no start parameter means none");

    const auto seek = ParseRouteQuery("u=https%3A%2F%2Fexample.com%2Fa.mp4&ss=12.5");
    Check(seek.startSeconds > 12.49 && seek.startSeconds < 12.51, "a start parameter parses");

    // `+` is a plus: form-post conventions do not apply to a query this host
    // builds itself from encodeURIComponent.
    const auto plus = ParseRouteQuery("u=https%3A%2F%2Fexample.com%2Fa%2Bb.mp4");
    CheckEqual(plus.source, "https://example.com/a+b.mp4", "a plus survives as a plus");

    // The plan's own refusals hold at the route: the origin gate is not the
    // only gate, because the same handler serves files for pages that never
    // asked for media privileges.
    Check(!ParseRouteQuery("u=file%3A%2F%2F%2FC%3A%2FWindows%2Fwin.ini").source.empty() == false,
          "file: is refused at the route");
    Check(!ParseRouteQuery("u=").error.empty(), "an empty u= is refused with a reason");
    Check(!ParseRouteQuery("").error.empty(), "an absent u= is refused with a reason");
    Check(ParseRouteQuery("u=https%3A%2F%2Fexample.com%2Fa.mp4&ss=999999999").error.empty() == false,
          "an absurd start position is refused");

    // The probe JSON the page reads is valid JSON for the inputs that reach it.
    const Probe p;
    const std::string json = BuildProbeJson(p, Verdict::Nothing, "said \"no\"\nline two");
    Check(json.find("\"verdict\":\"nothing\"") != std::string::npos, "the verdict is present");
    Check(json.find("\\\"no\\\"") != std::string::npos, "quotes in the detail are escaped");
    Check(json.find('\n') == std::string::npos, "no raw newline reaches the JSON body");
}

void TestFfmpegArguments()
{
    const auto stream = StreamArguments("https://example.com/a.mp4", 0.0);
    const std::string rendered = BuildCommandLine(stream);
    // The two switches measurement put in and then took out again.
    Check(rendered.find("-dash") == std::string::npos,
          "-dash must not appear: it fails the webm header and writes zero bytes");
    Check(rendered.find("scale=-2:min(1080\\,ih)") != std::string::npos,
          "the scale filter must carry its escaped comma");
    Check(rendered.find("libvpx-vp9") != std::string::npos, "video must be re-encoded to VP9");
    Check(rendered.find("libopus") != std::string::npos, "audio must be re-encoded to Opus");
    Check(rendered.find("-f webm -") != std::string::npos, "the output must be WebM on stdout");
    Check(rendered.find("file") == std::string::npos,
          "the protocol whitelist must not permit file:");
    Check(rendered.find("hls") != std::string::npos, "a live .m3u8 must still be reachable");

    // The URL is one argument, always, and it is the argument after -i.
    std::size_t index = 0;
    for (; index < stream.size(); ++index)
    {
        if (stream[index] == "-i") break;
    }
    Check(index + 1 < stream.size() && stream[index + 1] == "https://example.com/a.mp4",
          "the link must be the single argument after -i");

    // A start time is emitted before -i or not at all, never after it.
    const auto seek = StreamArguments("https://example.com/a.mp4", 42.5);
    std::size_t seekIndex = 0;
    for (; seekIndex < seek.size(); ++seekIndex)
    {
        if (seek[seekIndex] == "-ss") break;
    }
    std::size_t inputIndex = 0;
    for (; inputIndex < seek.size(); ++inputIndex)
    {
        if (seek[inputIndex] == "-i") break;
    }
    Check(seekIndex < seek.size() && seekIndex < inputIndex,
          "-ss must come before -i, or seeking decodes the whole film first");
    Check(seek[seekIndex + 1] == "42.500000", "the start time reaches ffmpeg");
    Check(BuildCommandLine(StreamArguments("https://example.com/a.mp4", 0.0)).find("-ss") ==
              std::string::npos,
          "a play from the start must not carry an -ss at all");

    // ffprobe's own argv asks for names, never frames.
    const auto probe = ProbeArguments("https://example.com/a.mp4");
    const std::string probeLine = BuildCommandLine(probe);
    Check(probeLine.find("csv=p=0") != std::string::npos, "ffprobe output must be line-oriented");
    Check(probeLine.find("stream=codec_name,codec_type") != std::string::npos, "ffprobe must report streams");
    Check(probeLine.find("format=format_name,duration") != std::string::npos,
          "ffprobe must report the container and its duration");
}

void TestProbeParsing()
{
    // Real output, three real files.
    const auto h264 = ParseProbeCsv(
        "h264,video\r\naac,audio\r\n\"mov,mp4,m4a,3gp,3g2,mj2\",3.000000\r\n");
    CheckEqual(h264.videoCodec, "h264", "the H.264 sample's video codec");
    CheckEqual(h264.audioCodec, "aac", "the H.264 sample's audio codec");
    CheckEqual(h264.formatName, "mov,mp4,m4a,3gp,3g2,mj2", "a quoted container name keeps its commas");
    Check(h264.duration > 2.999 && h264.duration < 3.001, "the duration is read");
    Check(Judge(h264) == Verdict::Transcoded, "an H.264/AAC MP4 must be transcoded");

    const auto vp9 = ParseProbeCsv("vp9,video\r\nopus,audio\r\n\"matroska,webm\",2.008000\r\n");
    Check(Judge(vp9) == Verdict::Playable, "a VP9/Opus WebM plays as it is");

    const auto audioOnly = ParseProbeCsv("mp3,audio\r\nmp3,1.000000\r\n");
    CheckEqual(audioOnly.videoCodec, "", "an MP3 has no video codec");
    Check(Judge(audioOnly) == Verdict::Transcoded, "an MP3 must become Opus");

    // A live stream states no duration, and that must not read as a failure.
    const auto live = ParseProbeCsv("h264,video\r\n\"mov,mp4,m4a,3gp,3g2,mj2\",N/A\r\n");
    Check(live.duration == 0.0, "a container with no duration must parse to zero, not fail");

    // ffprobe writes nothing at all for a URL it could not open -- the shape a
    // 404 or an HTML page takes.
    Check(Judge(ParseProbeCsv("")) == Verdict::Nothing, "empty output is nothing to play");
    Check(Judge(ParseProbeCsv("\r\n")) == Verdict::Nothing, "blank output is nothing to play");

    // A file ffprobe refuses but still describes must not be called playable.
    const auto broken = ParseProbeCsv("h264,video\r\n");
    Check(Judge(broken) == Verdict::Transcoded, "a video-only answer still needs its codec judged");

    // The judgements that are not about H.264 at all.
    Check(Judge(ParseProbeCsv("vp9,video\r\nopus,audio\r\nogg,2.0\r\n")) == Verdict::Playable,
          "Ogg with Theora/Opus-era codecs is playable");
    Check(Judge(ParseProbeCsv("vp9,video\r\nopus,audio\r\n\"mpegts\",2.0\r\n")) == Verdict::Transcoded,
          "the right codecs in a container this build cannot demux still need a transcode");
    Check(Judge(ParseProbeCsv("av1,video\r\nopus,audio\r\n\"matroska,webm\",2.0\r\n")) == Verdict::Playable,
          "AV1 plays in this build");
    Check(Judge(ParseProbeCsv("vp9,video\r\nac3,audio\r\n\"matroska,webm\",2.0\r\n")) == Verdict::Transcoded,
          "VP9 with an undecodable audio track is still a transcode");
    Check(Judge(ParseProbeCsv("vp9,video\r\n\"matroska,webm\",2.0\r\n")) == Verdict::Playable,
          "a silent VP9 clip plays without a needless re-encode");
    Check(Judge(ParseProbeCsv("flac,audio\r\nogg,2.0\r\n")) == Verdict::Playable, "FLAC audio alone plays");
    // A name that merely starts like a decoder is not that decoder.
    Check(!AudioCodecPlaysDirectly("vorbisx"), "an unknown codec must not match by prefix");
    Check(!VideoCodecPlaysDirectly("h265"), "HEVC must not be mistaken for a supported codec");
    Check(VideoCodecPlaysDirectly("vp8") && VideoCodecPlaysDirectly("theora"), "VP8 and Theora decode");

    // The log line has to name what was found, because "why is this re-encoding"
    // is the question it exists to answer.
    const std::string described = Describe(h264);
    Check(described.find("h264") != std::string::npos && described.find("aac") != std::string::npos &&
              described.find("duration=3s") != std::string::npos,
          "the probe summary must name the container, both codecs and the duration");
}
} // namespace

int main()
{
    TestSourcePolicy();
    TestQuoting();
    TestRouteQueries();
    TestFfmpegArguments();
    TestProbeParsing();

    if (gFailures != 0)
    {
        std::cerr << gFailures << " transcode-plan check(s) failed\n";
        return 1;
    }
    std::cout << "Transcode plan tests passed\n";
    return 0;
}
