#pragma once

// How somebody else's video becomes playable on a television.
//
// ---------------------------------------------------------------------------
// THE PROBLEM THIS EXISTS FOR
// ---------------------------------------------------------------------------
//
// The runtime beside the game is a CEF build without proprietary codecs. That
// is a property of the *build*, not of the platform: the decoders are absent
// from libcef, so an H.264/AAC stream is refused at the demuxer and the usual
// symptom is a black rectangle and a page reporting `playing`. Measured, in
// `docs/research/evidence/webui-media-2026-09-14.json`:
//
//     canPlayType('video/mp4; codecs="avc1.42E01E"')  -> ""     (empty)
//     MSE.isTypeSupported('...avc1...,mp4a.40.2')      -> false
//     a real <video> load of an .mp4                   -> NotSupportedError
//
// Two ways out. Build a libcef with `proprietary_codecs` (a Chromium checkout,
// ~150 GB and hours of build time, and nothing to download that already has it),
// or make the stream playable in the runtime that is already shipped. This file
// is the second: the link is handed to a real decoder, and what comes back out
// is WebM -- VP9 and Opus, both of which this build plays natively.
//
// ---------------------------------------------------------------------------
// WHY THE DECISION IS PURE AND TESTED
// ---------------------------------------------------------------------------
//
// Everything here is a string function over strings. It spawns nothing, opens
// nothing, and knows no CEF, which is what makes the two things that can go
// badly wrong here testable without a game, a network or a browser:
//
//   * **Argument injection.** A link a player pastes reaches an external
//     process. It is passed as ONE argv element and never as part of a command
//     line, and `QuoteWindowsArgument` below exists because a URL containing a
//     quote and a backslash is exactly the shape that breaks naive quoting.
//   * **Playing a stream this build cannot decode.** "Is this H.264" is not a
//     guess about a file extension; it is the decoder's own answer, read back
//     from ffprobe and judged here.
//
// ---------------------------------------------------------------------------
// WHAT COUNTS AS PLAYABLE, AND WHERE THAT LIST COMES FROM
// ---------------------------------------------------------------------------
//
// Three independent things have to hold before a link can be handed straight to
// a `<video>` element, and all three are checked:
//
//   1. the *container* is one this runtime demuxes -- WebM/Matroska and Ogg;
//   2. the video codec has a decoder -- VP8, VP9, AV1, Theora;
//   3. the audio codec has a decoder -- Opus, Vorbis, FLAC, PCM.
//
// The container check is not redundant with the codec check. A Matroska file
// full of VP9 plays; the same VP9 inside an MPEG-TS or an AVI does not, because
// the demuxer is missing, and that failure looks identical from the page.
//
// A link that fails any of the three is *transcoded*, not refused. Refusing was
// the previous behaviour and it was wrong for a reason worth recording: the page
// refused `.mp4` out loud while YouTube played, so the rule people learned was
// "some links work and some do not", with no way to tell which from the address.

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace op77::WebUI::Transcode
{
/// Longer than any real link, short enough that nothing here can be fed a
/// megabyte of "URL" by a page that has been talked into it.
inline constexpr std::size_t kMaximumSourceLength = 4096;

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

[[nodiscard]] inline std::string LowerAscii(const std::string_view aText)
{
    std::string result(aText);
    for (char& value : result)
    {
        if (value >= 'A' && value <= 'Z') value = static_cast<char>(value - 'A' + 'a');
    }
    return result;
}

[[nodiscard]] inline std::string Trim(const std::string_view aText)
{
    std::size_t begin = 0;
    std::size_t end = aText.size();
    const auto space = [](const char aValue) {
        return aValue == ' ' || aValue == '\t' || aValue == '\r' || aValue == '\n';
    };
    while (begin < end && space(aText[begin])) ++begin;
    while (end > begin && space(aText[end - 1])) --end;
    return std::string(aText.substr(begin, end - begin));
}

/// Splits on a single character, with no CSV quoting. Used for `format_name`,
/// which ffprobe has already unquoted by the time it reaches us: the field is
/// `mov,mp4,m4a,3gp,3g2,mj2` -- a list of the demuxers that matched, and a
/// Matroska file that reports `matroska,webm` is genuinely either.
[[nodiscard]] inline std::vector<std::string> SplitOn(const std::string_view aText, const char aSeparator)
{
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true)
    {
        const std::size_t at = aText.find(aSeparator, start);
        if (at == std::string_view::npos)
        {
            fields.emplace_back(aText.substr(start));
            break;
        }
        fields.emplace_back(aText.substr(start, at - start));
        start = at + 1;
    }
    return fields;
}

/// One CSV record, honouring the double-quote rule ffprobe's writer uses. A
/// `format_name` containing commas is emitted quoted -- `"mov,mp4,...,mj2"` --
/// so splitting the line on commas without this would read the container's own
/// name as four bogus fields and lose the duration with them.
[[nodiscard]] inline std::vector<std::string> SplitCsvRecord(const std::string_view aLine)
{
    std::vector<std::string> fields;
    std::string current;
    bool quoted = false;
    for (std::size_t index = 0; index < aLine.size(); ++index)
    {
        const char value = aLine[index];
        if (quoted)
        {
            if (value != '"')
            {
                current.push_back(value);
                continue;
            }
            if (index + 1 < aLine.size() && aLine[index + 1] == '"')
            {
                current.push_back('"');
                ++index;
                continue;
            }
            quoted = false;
            continue;
        }
        if (value == '"')
        {
            quoted = true;
        }
        else if (value == ',')
        {
            fields.push_back(current);
            current.clear();
        }
        else
        {
            current.push_back(value);
        }
    }
    fields.push_back(current);
    return fields;
}

// ---------------------------------------------------------------------------
// What may be fetched
// ---------------------------------------------------------------------------
// A television is not a general-purpose fetcher. `http` and `https` only, and
// the reason is the same one `-protocol_whitelist` below exists: the URL is
// handed to an external process, and `file:`, `concat:` and `subfile:` are
// accepted by that process and read the machine it runs on. A page that could
// ask for `file:///C:/...` would be a page that can read any file the game can.
[[nodiscard]] inline bool AcceptSource(const std::string_view aUrl, std::string& aError)
{
    aError.clear();
    if (aUrl.empty())
    {
        aError = "no link was given";
        return false;
    }
    if (aUrl.size() > kMaximumSourceLength)
    {
        aError = "the link is longer than this host will hand to a decoder";
        return false;
    }
    for (const char value : aUrl)
    {
        const auto code = static_cast<unsigned char>(value);
        if (code < 0x20 || code == 0x7F)
        {
            aError = "the link contains a control character";
            return false;
        }
    }
    const std::string lowered = LowerAscii(aUrl);
    const bool secure = lowered.rfind("https://", 0) == 0;
    const bool plain = lowered.rfind("http://", 0) == 0;
    if (!secure && !plain)
    {
        aError = "only http:// and https:// links can be played here";
        return false;
    }
    const std::size_t hostStart = secure ? 8 : 7;
    const std::size_t slash = lowered.find('/', hostStart);
    const std::size_t hostEnd = slash == std::string::npos ? aUrl.size() : slash;
    if (hostEnd <= hostStart)
    {
        aError = "the link names no host";
        return false;
    }
    for (std::size_t index = hostStart; index < hostEnd; ++index)
    {
        const char value = aUrl[index];
        if (value == ' ' || value == '\t' || value == '\\' || value == '@')
        {
            // `@` inside the authority is how `https://good.example@evil.example`
            // is written, and the whole point of such a link is that a reader
            // stops at the wrong host. Refused rather than explained away.
            aError = "the link's host is not a plain host name";
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Command lines
// ---------------------------------------------------------------------------

/// One argument, quoted by the rule `CommandLineToArgvW` actually applies.
///
/// Written out rather than approximated because the failure is silent and the
/// input is hostile by construction: a link with a space, a quote and a
/// trailing backslash is a legal URL, and a quoting pass that only wraps the
/// argument in quotes turns the rest of the command line into arguments the
/// caller never built. The rule is: a run of backslashes before a quote is
/// doubled with the quote escaped, and a run at the very end of the argument is
/// doubled, because otherwise the closing quote would be escaped away.
[[nodiscard]] inline std::string QuoteWindowsArgument(const std::string_view aArgument)
{
    if (!aArgument.empty() && aArgument.find_first_of(" \t\n\v\"") == std::string_view::npos)
    {
        return std::string(aArgument);
    }
    std::string result;
    result.reserve(aArgument.size() + 2);
    result.push_back('"');
    auto cursor = aArgument.begin();
    while (true)
    {
        std::size_t backslashes = 0;
        while (cursor != aArgument.end() && *cursor == '\\')
        {
            ++cursor;
            ++backslashes;
        }
        if (cursor == aArgument.end())
        {
            result.append(backslashes * 2, '\\');
            break;
        }
        if (*cursor == '"')
        {
            result.append(backslashes * 2 + 1, '\\');
            result.push_back('"');
        }
        else
        {
            result.append(backslashes, '\\');
            result.push_back(*cursor);
        }
        ++cursor;
    }
    result.push_back('"');
    return result;
}

[[nodiscard]] inline std::string BuildCommandLine(const std::vector<std::string>& aArguments)
{
    std::string line;
    for (const auto& argument : aArguments)
    {
        if (!line.empty()) line.push_back(' ');
        line += QuoteWindowsArgument(argument);
    }
    return line;
}

/// The protocols the decoder may use to reach its input.
///
/// `file` is deliberately absent and `concat`/`subfile` with it: those are how a
/// link stops being a link and starts being a read of the local disk. `hls` and
/// `crypto` are present because a live `.m3u8` is a real thing to paste and it
/// fetches its own segments over the same whitelist.
[[nodiscard]] inline const std::vector<std::string>& ProtocolWhitelist()
{
    static const std::vector<std::string> value{
        "-protocol_whitelist", "http,https,tcp,tls,crypto,hls,httpproxy"};
    return value;
}

/// ffprobe's argv: codec and container names only, never a frame.
///
/// The output is deliberately line-oriented (`csv=p=0`) rather than JSON, so
/// reading it needs no parser: one line per stream, `codec_name,codec_type`,
/// then a final line for the container, `format_name,duration`.
[[nodiscard]] inline std::vector<std::string> ProbeArguments(const std::string_view aUrl)
{
    std::vector<std::string> arguments{"-v", "error", "-of", "csv=p=0"};
    const auto& whitelist = ProtocolWhitelist();
    arguments.insert(arguments.end(), whitelist.begin(), whitelist.end());
    arguments.insert(arguments.end(), {"-show_entries", "stream=codec_name,codec_type"});
    arguments.insert(arguments.end(), {"-show_entries", "format=format_name,duration"});
    arguments.emplace_back(aUrl);
    return arguments;
}

/// ffmpeg's argv: WebM out of whatever came in, on stdout, live.
///
/// The encoder settings are chosen for a screen a few metres away that has to
/// keep up, not for an archive. `-deadline realtime` with `-cpu-used 5` is the
/// speed end of libvpx's scale; `-b:v 0 -crf 32` is its constant-quality mode,
/// which is the only mode where the bitrate follows the picture.
///
/// **There is no `-dash 1` here, and there was.** DASH-style output looks like
/// the right switch for a stream that is consumed as it arrives -- clusters
/// opened on keyframes, cue points for a reader that seeks. It is not: measured
/// against a real H.264 input, `-f webm -dash 1 -` fails at the header with
/// `Could not write header (incorrect codec parameters ?)` and writes zero
/// bytes, while the same argv without it writes a VP9/Opus stream. The plain
/// muxer already puts the EBML header and the Tracks section before the first
/// cluster, which is all a progressive reader needs, so the flag bought nothing
/// and broke everything.
///
/// `-ss` goes BEFORE `-i` on purpose. After `-i` it seeks by decoding from the
/// start and throwing frames away, which on a two-hour film is a minute of
/// nothing; before `-i` it seeks by asking the demuxer for the nearest
/// keyframe. The difference is the whole reason seeking a transcoded stream is
/// usable at all.
///
/// The scale expression's comma is escaped (`min(1080\,ih)`) because it is
/// passed as one argv element with no shell in between, and `,` is what
/// separates filters in a filtergraph: unescaped, ffmpeg reads the filter as a
/// name ending in `ih)` and reports `No such filter`.
[[nodiscard]] inline std::vector<std::string> StreamArguments(const std::string_view aUrl,
                                                              const double aStartSeconds)
{
    std::vector<std::string> arguments{"-hide_banner", "-nostdin", "-loglevel", "error"};
    const auto& whitelist = ProtocolWhitelist();
    arguments.insert(arguments.end(), whitelist.begin(), whitelist.end());
    if (aStartSeconds > 0.0)
    {
        arguments.insert(arguments.end(), {"-ss", std::to_string(aStartSeconds)});
    }
    arguments.insert(arguments.end(), {"-i", std::string(aUrl)});
    arguments.insert(arguments.end(), {"-map", "0:v:0?", "-map", "0:a:0?"});
    // A television's own texture is at most a few hundred pixels across, and a
    // 4K decode per viewer is how a feature becomes the reason a game stutters.
    // A source with no video stream passes through this untouched -- a video
    // filter with nothing to filter is not an error in ffmpeg.
    arguments.insert(arguments.end(), {"-vf", "scale=-2:min(1080\\,ih)"});
    arguments.insert(arguments.end(),
                     {"-c:v", "libvpx-vp9", "-deadline", "realtime", "-cpu-used", "5", "-row-mt", "1",
                      "-b:v", "0", "-crf", "32", "-g", "48"});
    arguments.insert(arguments.end(), {"-c:a", "libopus", "-b:a", "96k", "-ac", "2", "-ar", "48000"});
    arguments.insert(arguments.end(), {"-f", "webm", "-"});
    return arguments;
}

// ---------------------------------------------------------------------------
// Reading ffprobe's answer
// ---------------------------------------------------------------------------

struct Probe
{
    std::string videoCodec;
    std::string audioCodec;
    std::string formatName;
    /// Seconds, `0` when the container does not state one (a live stream, or a
    /// format that has no duration field at all).
    double duration{};
};

/// The duration field is `N/A` for anything without one, so this parses rather
/// than assumes. `strtod` is locale-sensitive in principle; `N/A` fails it
/// cleanly, which is the only outcome that matters here.
[[nodiscard]] inline double ParseSeconds(const std::string_view aText)
{
    const std::string text = Trim(aText);
    if (text.empty()) return 0.0;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || value < 0.0) return 0.0;
    return value;
}

[[nodiscard]] inline Probe ParseProbeCsv(const std::string_view aOutput)
{
    Probe probe;
    for (const auto& raw : SplitOn(aOutput, '\n'))
    {
        const std::string line = Trim(raw);
        if (line.empty()) continue;
        const auto fields = SplitCsvRecord(line);
        if (fields.size() < 2) continue;
        const std::string kind = LowerAscii(Trim(fields[1]));
        // A stream record's second field names the stream's kind; the format
        // record's second field is the duration ("3.000000" or "N/A"). The
        // distinction is made by shape, not by position, so a probe that fails
        // part-way -- streams answered, format section never reached -- still
        // reads the stream it was given instead of misreading it as a format.
        if (kind == "video" && probe.videoCodec.empty())
        {
            probe.videoCodec = LowerAscii(Trim(fields[0]));
        }
        else if (kind == "audio" && probe.audioCodec.empty())
        {
            probe.audioCodec = LowerAscii(Trim(fields[0]));
        }
        else if (kind != "subtitle" && kind != "data" && kind != "attachment")
        {
            // The format record. Its name field may hold a quoted list of the
            // demuxers that matched, already unquoted by SplitCsvRecord.
            probe.formatName = LowerAscii(Trim(fields[0]));
            probe.duration = ParseSeconds(fields[1]);
        }
    }
    return probe;
}

// ---------------------------------------------------------------------------
// The verdict
// ---------------------------------------------------------------------------

[[nodiscard]] inline bool VideoCodecPlaysDirectly(const std::string_view aCodec)
{
    return aCodec == "vp8" || aCodec == "vp9" || aCodec == "av1" || aCodec == "theora";
}

/// An empty codec is not a refusal: a video with no audio track has nothing to
/// be unplayable, and saying otherwise would send every silent clip through a
/// transcode it does not need.
[[nodiscard]] inline bool AudioCodecPlaysDirectly(const std::string_view aCodec)
{
    if (aCodec.empty()) return true;
    return aCodec == "opus" || aCodec == "vorbis" || aCodec == "flac" || aCodec.rfind("pcm_", 0) == 0;
}

[[nodiscard]] inline bool ContainerPlaysDirectly(const std::string_view aFormatName)
{
    const std::string lowered = LowerAscii(aFormatName);
    for (const auto& name : SplitOn(lowered, ','))
    {
        const std::string trimmed = Trim(name);
        if (trimmed == "matroska" || trimmed == "webm" || trimmed == "ogg") return true;
    }
    return false;
}

enum class Verdict : uint8_t
{
    /// Hand the link straight to the element.
    Playable = 0,
    /// A real stream, in a shape this build cannot decode. Send it through the
    /// local decoder and hand the element the result.
    Transcoded = 1,
    /// No stream at all -- an HTML page, a 404, or a file ffprobe could not
    /// open. Nothing to play, and the page should say so instead of showing a
    /// rectangle.
    Nothing = 2,
};

[[nodiscard]] inline Verdict Judge(const Probe& aProbe)
{
    if (aProbe.videoCodec.empty() && aProbe.audioCodec.empty()) return Verdict::Nothing;
    if (ContainerPlaysDirectly(aProbe.formatName) &&
        (aProbe.videoCodec.empty() || VideoCodecPlaysDirectly(aProbe.videoCodec)) &&
        AudioCodecPlaysDirectly(aProbe.audioCodec))
    {
        return Verdict::Playable;
    }
    return Verdict::Transcoded;
}

[[nodiscard]] inline const char* Describe(const Verdict aVerdict)
{
    switch (aVerdict)
    {
    case Verdict::Playable: return "playable";
    case Verdict::Transcoded: return "transcoded";
    case Verdict::Nothing: return "nothing";
    }
    return "unknown";
}

/// One line naming what the decoder found, for a log that has to answer "why is
/// this link being re-encoded" without anybody re-running ffprobe.
[[nodiscard]] inline std::string Describe(const Probe& aProbe)
{
    std::string text = "container=" + (aProbe.formatName.empty() ? std::string("?") : aProbe.formatName);
    text += " video=" + (aProbe.videoCodec.empty() ? std::string("none") : aProbe.videoCodec);
    text += " audio=" + (aProbe.audioCodec.empty() ? std::string("none") : aProbe.audioCodec);
    if (aProbe.duration > 0.0)
    {
        text += " duration=" + std::to_string(static_cast<long long>(aProbe.duration * 1000.0) / 1000) + "s";
    }
    return text;
}

// ---------------------------------------------------------------------------
// The two routes a media page is offered
// ---------------------------------------------------------------------------
//
// Both live under the page's own origin, served by the host from inside the
// browser process -- no listener, no port, nothing externally reachable. The
// origin gate is the authentication: only the page itself can name them.
//
//     /op77/media/probe?u=...      what does the decoder say this link is?
//     /op77/media/stream?u=...&ss= the link, decoded into WebM, from ss seconds

/// Decodes the %XX escapes `encodeURIComponent` produces. `+` is left alone:
/// that convention belongs to form posts, and a literal plus in a URL path is
/// a plus.
[[nodiscard]] inline std::string PercentDecode(const std::string_view aText)
{
    static constexpr const char* kDigits = "0123456789abcdef";
    std::string result;
    result.reserve(aText.size());
    for (std::size_t index = 0; index < aText.size(); ++index)
    {
        if (aText[index] != '%' || index + 2 >= aText.size())
        {
            result.push_back(aText[index]);
            continue;
        }
        const auto high = static_cast<char>(LowerAscii(aText.substr(index + 1, 1))[0]);
        const auto low = static_cast<char>(LowerAscii(aText.substr(index + 2, 1))[0]);
        const char* highAt = std::strchr(kDigits, high);
        const char* lowAt = std::strchr(kDigits, low);
        if (highAt == nullptr || lowAt == nullptr)
        {
            result.push_back(aText[index]);
            continue;
        }
        result.push_back(static_cast<char>((highAt - kDigits) * 16 + (lowAt - kDigits)));
        index += 2;
    }
    return result;
}

struct RouteQuery
{
    std::string source;
    double startSeconds{};
    std::string error;
};

/// Reads the query part of one of the two routes. Everything else about the URL
/// (scheme, host) has already been checked by the caller's same-origin gate.
[[nodiscard]] inline RouteQuery ParseRouteQuery(const std::string_view aQuery)
{
    RouteQuery query;
    std::string sourceParameter;
    std::string startParameter;
    std::size_t start = 0;
    while (start <= aQuery.size())
    {
        const std::size_t at = aQuery.find('&', start);
        const std::string_view pair = aQuery.substr(
            start, at == std::string_view::npos ? aQuery.size() - start : at - start);
        if (!pair.empty())
        {
            const std::size_t equals = pair.find('=');
            const std::string key = LowerAscii(Trim(equals == std::string_view::npos
                                                        ? pair
                                                        : pair.substr(0, equals)));
            const std::string value = PercentDecode(equals == std::string_view::npos
                                                        ? std::string_view{}
                                                        : pair.substr(equals + 1));
            if (key == "u") sourceParameter = value;
            else if (key == "ss") startParameter = value;
        }
        if (at == std::string_view::npos) break;
        start = at + 1;
    }
    if (!AcceptSource(sourceParameter, query.error))
    {
        query.source.clear();
        return query;
    }
    if (!startParameter.empty())
    {
        query.startSeconds = ParseSeconds(startParameter);
        if (query.startSeconds < 0.0 || query.startSeconds > 86400.0 * 14.0)
        {
            query.error = "the start position is out of range";
            query.source.clear();
            return query;
        }
    }
    query.source = sourceParameter;
    return query;
}

/// The probe route's answer, as JSON the page reads with `fetch`.
[[nodiscard]] inline std::string BuildProbeJson(const Probe& aProbe, const Verdict aVerdict,
                                                const std::string_view aDetail)
{
    std::string json = "{\"verdict\":\"";
    json += Describe(aVerdict);
    json += "\"";
    if (!aProbe.videoCodec.empty())
    {
        json += ",\"video\":\"";
        json += aProbe.videoCodec;
        json += "\"";
    }
    if (!aProbe.audioCodec.empty())
    {
        json += ",\"audio\":\"";
        json += aProbe.audioCodec;
        json += "\"";
    }
    if (!aProbe.formatName.empty())
    {
        json += ",\"container\":\"";
        json += aProbe.formatName;
        json += "\"";
    }
    if (aProbe.duration > 0.0)
    {
        json += ",\"duration\":";
        json += std::to_string(aProbe.duration);
    }
    if (!aDetail.empty())
    {
        json += ",\"detail\":\"";
        for (const char value : aDetail)
        {
            switch (value)
            {
            case '"': json += "\\\""; break;
            case '\\': json += "\\\\"; break;
            case '\n': json += " "; break;
            case '\r': break;
            default: json.push_back(value); break;
            }
        }
        json += "\"";
    }
    json += "}";
    return json;
}
} // namespace op77::WebUI::Transcode
