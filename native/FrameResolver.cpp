#include "FrameResolver.hpp"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

namespace op77::WebHost::FrameResolver
{
namespace
{
/// The most of a page that is ever held: a shell that wraps an application is
/// kilobytes, and the whole point of the cap is that a URL which turns out to be
/// a film is not downloaded in order to find out it has no iframes in it.
constexpr std::size_t kMaximumBodyBytes = 256U * 1024U;
/// Refuse to even look at a URL longer than any real one, so a page cannot make
/// the host build an unbounded request.
constexpr std::size_t kMaximumUrlLength = 2048;

std::wstring Utf8ToWide(const std::string& aValue)
{
    if (aValue.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, aValue.data(),
                                          static_cast<int>(aValue.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, aValue.data(), static_cast<int>(aValue.size()),
                            result.data(), count) != count)
        return {};
    return result;
}

std::string WideToUtf8(const std::wstring_view aValue)
{
    if (aValue.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, aValue.data(),
                                          static_cast<int>(aValue.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, aValue.data(), static_cast<int>(aValue.size()),
                            result.data(), count, nullptr, nullptr) != count)
        return {};
    return result;
}

std::string Lower(std::string_view aText)
{
    std::string result(aText);
    std::transform(result.begin(), result.end(), result.begin(),
        [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::string Trim(std::string_view aText)
{
    std::size_t begin = 0;
    std::size_t end = aText.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(aText[begin])) != 0) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(aText[end - 1])) != 0) --end;
    return std::string(aText.substr(begin, end - begin));
}

std::string HeaderValue(const std::vector<op77::WebUI::Framing::Header>& aHeaders, const std::string_view aName)
{
    for (const auto& header : aHeaders)
        if (Lower(header.name) == aName) return header.value;
    return {};
}

std::string JsonQuote(const std::string_view aText)
{
    std::ostringstream output;
    output << '"';
    for (const unsigned char value : aText)
    {
        switch (value)
        {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (value < 0x20) output << "\\u00" << "0123456789abcdef"[(value >> 4U) & 0xFU]
                                     << "0123456789abcdef"[value & 0xFU];
            else output << static_cast<char>(value);
        }
    }
    output << '"';
    return output.str();
}

struct Response
{
    bool ok{};
    std::string error;
    int status{};
    std::string finalUrl;
    std::vector<op77::WebUI::Framing::Header> headers;
    std::string body;
};

/// The handles of one request, closed in the order WinHTTP wants whichever way
/// the function leaves.
struct Handles
{
    HINTERNET session{};
    HINTERNET connection{};
    HINTERNET request{};

    ~Handles()
    {
        if (request != nullptr) WinHttpCloseHandle(request);
        if (connection != nullptr) WinHttpCloseHandle(connection);
        if (session != nullptr) WinHttpCloseHandle(session);
    }

    Handles(const Handles&) = delete;
    Handles& operator=(const Handles&) = delete;
    Handles() = default;
};

/// One GET, with redirects followed and the body read only when it could be
/// markup.
///
/// The content type decides whether the body is read at all, and that is not an
/// optimisation: a pasted link is frequently a film, and reading a film into
/// memory to discover it has no `<iframe>` in it is how a probe becomes a
/// download. The check happens after the response arrives, so a shell page still
/// costs exactly one round trip.
Response Fetch(const std::string& aUrl, const bool aWantBody, const int aTimeoutMilliseconds)
{
    Response response;
    const auto wideUrl = Utf8ToWide(aUrl);
    if (wideUrl.empty())
    {
        response.error = "unreadable_url";
        return response;
    }

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    std::wstring scheme(16, L'\0');
    std::wstring host(256, L'\0');
    std::wstring path(4096, L'\0');
    std::wstring extra(2048, L'\0');
    parts.lpszScheme = scheme.data();
    parts.dwSchemeLength = static_cast<DWORD>(scheme.size());
    parts.lpszHostName = host.data();
    parts.dwHostNameLength = static_cast<DWORD>(host.size());
    parts.lpszUrlPath = path.data();
    parts.dwUrlPathLength = static_cast<DWORD>(path.size());
    parts.lpszExtraInfo = extra.data();
    parts.dwExtraInfoLength = static_cast<DWORD>(extra.size());
    if (WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &parts) != TRUE ||
        parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS)
    {
        response.error = "malformed_url";
        return response;
    }
    scheme.resize(parts.dwSchemeLength);
    host.resize(parts.dwHostNameLength);
    path.resize(parts.dwUrlPathLength);
    extra.resize(parts.dwExtraInfoLength);

    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    Handles handles;
    handles.session = WinHttpOpen(L"Open77/1.0 (media probe)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (handles.session == nullptr)
    {
        response.error = "network_unavailable";
        return response;
    }
    // All four of resolve/connect/send/receive get the same budget, and the
    // caller's timeout is the whole request's, not each hop's: a probe that can
    // take four times as long as it claims is how a screen sits on "checking".
    WinHttpSetTimeouts(handles.session, aTimeoutMilliseconds, aTimeoutMilliseconds, aTimeoutMilliseconds,
                       aTimeoutMilliseconds);
    handles.connection = WinHttpConnect(handles.session, host.c_str(), parts.nPort, 0);
    if (handles.connection == nullptr)
    {
        response.error = "connect_failed";
        return response;
    }
    std::wstring target = path;
    target += extra;
    handles.request = WinHttpOpenRequest(handles.connection, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (handles.request == nullptr)
    {
        response.error = "request_failed";
        return response;
    }
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(handles.request, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));
    // An unknown client is served a different page -- or no page -- by enough
    // origins that a probe without a browser's own headers measures the wrong
    // thing. This identifies the client and nothing else: no cookies are sent,
    // because there is no session for a cookie to belong to.
    WinHttpAddRequestHeaders(handles.request,
        L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
        L"Chrome/144.0.0.0 Safari/537.36",
        static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    WinHttpAddRequestHeaders(handles.request,
        L"Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8", static_cast<DWORD>(-1),
        WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    WinHttpAddRequestHeaders(handles.request, L"Accept-Language: en-US,en;q=0.9", static_cast<DWORD>(-1),
        WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    if (WinHttpSendRequest(handles.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) !=
            TRUE ||
        WinHttpReceiveResponse(handles.request, nullptr) != TRUE)
    {
        const DWORD code = GetLastError();
        response.error = code == ERROR_WINHTTP_TIMEOUT ? "timed_out" : "unreachable";
        return response;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (WinHttpQueryHeaders(handles.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) == TRUE)
        response.status = static_cast<int>(status);

    DWORD rawSize = 0;
    WinHttpQueryHeaders(handles.request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr,
                        &rawSize, WINHTTP_NO_HEADER_INDEX);
    if (rawSize > 0)
    {
        std::wstring raw(rawSize / sizeof(wchar_t) + 1, L'\0');
        if (WinHttpQueryHeaders(handles.request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                                raw.data(), &rawSize, WINHTTP_NO_HEADER_INDEX) == TRUE)
        {
            // The first line is the status line; every other is `Name: value`.
            std::size_t at = raw.find(L"\r\n");
            while (at != std::wstring::npos)
            {
                const std::size_t begin = at + 2;
                const std::size_t end = raw.find(L"\r\n", begin);
                const std::wstring line = raw.substr(begin, end == std::wstring::npos ? std::wstring::npos
                                                                                     : end - begin);
                const std::size_t colon = line.find(L':');
                if (colon != std::wstring::npos)
                    response.headers.push_back(op77::WebUI::Framing::Header{
                        WideToUtf8(line.substr(0, colon)), Trim(WideToUtf8(line.substr(colon + 1)))});
                if (end == std::wstring::npos) break;
                at = end;
            }
        }
    }

    DWORD urlSize = 0;
    WinHttpQueryOption(handles.request, WINHTTP_OPTION_URL, nullptr, &urlSize);
    if (urlSize > 0)
    {
        std::wstring finalUrl(urlSize / sizeof(wchar_t) + 1, L'\0');
        if (WinHttpQueryOption(handles.request, WINHTTP_OPTION_URL, finalUrl.data(), &urlSize) == TRUE)
            response.finalUrl = WideToUtf8(finalUrl.c_str());
    }

    const std::string contentType = Lower(HeaderValue(response.headers, "content-type"));
    const bool markup = contentType.find("html") != std::string::npos || contentType.find("xml") != std::string::npos ||
        contentType.empty();
    if (aWantBody && markup)
    {
        for (;;)
        {
            DWORD available = 0;
            if (WinHttpQueryDataAvailable(handles.request, &available) != TRUE || available == 0) break;
            if (response.body.size() >= kMaximumBodyBytes) break;
            const DWORD want = static_cast<DWORD>(
                std::min<std::size_t>(available, kMaximumBodyBytes - response.body.size()));
            std::string chunk(want, '\0');
            DWORD read = 0;
            if (WinHttpReadData(handles.request, chunk.data(), want, &read) != TRUE || read == 0) break;
            response.body.append(chunk.data(), read);
        }
    }

    response.ok = response.status > 0;
    if (!response.ok) response.error = "no_status";
    return response;
}
} // namespace

Answer Probe(const std::string& aUrl, const std::string& aOurOrigin, const int aDocumentTimeoutMilliseconds,
             const int aCandidateTimeoutMilliseconds)
{
    Answer answer;
    const std::string url = Trim(aUrl);
    if (url.empty() || url.size() > kMaximumUrlLength)
    {
        answer.error = "unusable_url";
        return answer;
    }
    const std::string scheme = WebUI::Ads::SchemeOf(url);
    if (scheme != "http" && scheme != "https")
    {
        answer.error = "unsupported_scheme";
        return answer;
    }
    // Refused without a request being made. A pasted advertising host is not
    // something to ask politely, and the page is told the same thing it would be
    // told by a site that refused framing.
    if (WebUI::Ads::IsBlocked(url))
    {
        answer.ok = true;
        answer.documentUrl = url;
        answer.violation = "blocked";
        return answer;
    }

    const Response document = Fetch(url, true, aDocumentTimeoutMilliseconds);
    if (!document.ok)
    {
        answer.error = document.error;
        return answer;
    }
    answer.ok = true;
    answer.documentUrl = document.finalUrl.empty() ? url : document.finalUrl;

    const auto verdict = WebUI::Framing::EvaluateFraming(document.headers, aOurOrigin);
    answer.frameable = verdict.allowed;
    answer.violation = verdict.violation;
    if (verdict.allowed)
    {
        answer.best = answer.documentUrl;
        answer.via = "direct";
        return answer;
    }

    // Refused. The link is a shell; what it wraps is what the screen wants, and
    // the probe stops at the first nested document that may be framed rather
    // than asking about the rest -- finding one is the answer, and each extra
    // question is seconds the player spends looking at "checking".
    for (const auto& embed : WebUI::Framing::FindEmbeds(document.body, answer.documentUrl))
    {
        if (answer.candidates.size() >= kMaximumProbedCandidates) break;
        Candidate candidate;
        candidate.url = embed.url;
        candidate.kind = embed.kind;
        const Response nested = Fetch(embed.url, false, aCandidateTimeoutMilliseconds);
        if (!nested.ok)
        {
            candidate.violation = nested.error;
        }
        else
        {
            const auto nestedVerdict = WebUI::Framing::EvaluateFraming(nested.headers, aOurOrigin);
            candidate.frameable = nestedVerdict.allowed;
            candidate.violation = nestedVerdict.violation;
        }
        const bool winner = candidate.frameable && answer.best.empty();
        if (winner)
        {
            answer.best = candidate.url;
            answer.via = "embed";
        }
        answer.candidates.push_back(std::move(candidate));
        if (winner) break;
    }
    return answer;
}

std::string ToJson(const Answer& aAnswer)
{
    std::ostringstream output;
    output << "{\"ok\":" << (aAnswer.ok ? "true" : "false")
           << ",\"frameable\":" << (aAnswer.frameable ? "true" : "false")
           << ",\"url\":" << JsonQuote(aAnswer.documentUrl)
           << ",\"violation\":" << JsonQuote(aAnswer.violation)
           << ",\"best\":" << JsonQuote(aAnswer.best)
           << ",\"via\":" << JsonQuote(aAnswer.via)
           << ",\"error\":" << JsonQuote(aAnswer.error)
           << ",\"candidates\":[";
    for (std::size_t i = 0; i < aAnswer.candidates.size(); ++i)
    {
        const auto& candidate = aAnswer.candidates[i];
        if (i > 0) output << ',';
        output << "{\"url\":" << JsonQuote(candidate.url) << ",\"kind\":" << JsonQuote(candidate.kind)
               << ",\"frameable\":" << (candidate.frameable ? "true" : "false")
               << ",\"violation\":" << JsonQuote(candidate.violation) << "}";
    }
    output << "]}";
    return output.str();
}
} // namespace op77::WebHost::FrameResolver
