#pragma once

// Whether a page can be shown on a television, and where the page that actually
// plays is, when the link somebody pasted is only a shell around it.
//
// ---------------------------------------------------------------------------
// THE PROBLEM, MEASURED
// ---------------------------------------------------------------------------
//
// The media page frames whatever is pasted onto it, so a site that refuses to be
// framed shows nothing. That refusal is not a bug in the page and cannot be
// worked around from inside it: `X-Frame-Options` is read by the browser from
// the response, and an embedded document's refusal is deliberately invisible to
// its embedder -- the iframe loads an error page and no line the page can write
// distinguishes "the site said no" from "the site is empty".
//
// The pasted link that produced this file:
//
//     https://123movie-tv.it.com/          x-frame-options: SAMEORIGIN   <- refuses
//       https://v3.freemovies.lol/?logo=…  (no x-frame-options)          <- the real app
//       https://moviestv.my/               (no x-frame-options)          <- and another
//
// A 5.7 KB shell carrying no content of its own, refusing to be framed, wrapping
// two applications that are happy to be framed. From inside the page that is
// indistinguishable from a dead site; from *outside* it is one request away from
// being obvious. So the host makes that request, and this header is how it is
// judged.
//
// ---------------------------------------------------------------------------
// WHAT THIS IS AND IS NOT
// ---------------------------------------------------------------------------
//
// This is a PROBE, not a proxy. Nothing here fetches a site on the player's
// behalf, replays it, rewrites its URLs or forwards its cookies: the host reads
// a page's headers and, only when framing is refused, its markup -- and the real
// origin is then framed directly by CEF, with the site's own network, its own
// cookies and its own session. That distinction is the reason this is allowed to
// exist at all: a proxy would have to carry the player's credentials through a
// process the resource can talk to, and a probe carries none.
//
// Everything here is a pure function over strings -- no CEF, no sockets, no
// files -- which is what makes the two decisions that matter testable without a
// game: whether a set of headers refuses framing, and which of a shell's nested
// documents is the application rather than the furniture.
//
// ---------------------------------------------------------------------------
// WHY FRAMING IS REFUSED MORE OFTEN THAN IT IS ALLOWED
// ---------------------------------------------------------------------------
//
// `X-Frame-Options: SAMEORIGIN` means "same origin as the response", and a
// television is never that origin, so every value of the header refuses us:
// `DENY` explicitly, `SAMEORIGIN` by definition, and `ALLOW-FROM` because no
// browser has honoured it for years. `frame-ancestors` is the modern spelling
// and is stricter: a list of ancestors that does not name us refuses us, so the
// only values that permit framing are the ones that name everybody (`*` or a
// scheme source like `https:`).

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <op77/WebUI/AdBlock.hpp>

namespace op77::WebUI::Framing
{
/// One response header, exactly as the server sent it. A vector rather than a
/// map because headers repeat: a server that sends two `Content-Security-Policy`
/// headers means *both* policies apply, and folding them into one value would
/// read the pair as a single malformed directive list.
struct Header
{
    std::string name;
    std::string value;
};

/// Whether a document may be framed, and -- when it may not -- which header
/// refused it. `violation` is the string a log line and the page both use, and
/// it is spelled the way the page already spells the other refusal it can see
/// (`frame-src`), so the two are greppable together.
struct Verdict
{
    bool allowed{true};
    std::string violation;
};

namespace Detail
{
[[nodiscard]] inline std::string Lower(std::string_view aText)
{
    std::string result(aText);
    std::transform(result.begin(), result.end(), result.begin(),
        [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

[[nodiscard]] inline std::string Trim(std::string_view aText)
{
    std::size_t begin = 0;
    std::size_t end = aText.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(aText[begin])) != 0) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(aText[end - 1])) != 0) --end;
    return std::string(aText.substr(begin, end - begin));
}

/// A URL split the way the resolution below needs it. `valid` is false for
/// anything without an authority (`mailto:`, `about:`, a bare path), which is
/// exactly the set of references that cannot be resolved against a base.
struct Parts
{
    std::string scheme;
    std::string authority;
    std::string path;
    std::string query;
    std::string fragment;
    bool valid{};
};

[[nodiscard]] inline Parts Split(std::string_view aUrl)
{
    Parts parts;
    const auto schemeEnd = aUrl.find("://");
    if (schemeEnd == std::string_view::npos || schemeEnd == 0) return parts;
    parts.scheme = Lower(aUrl.substr(0, schemeEnd));
    std::string_view rest = aUrl.substr(schemeEnd + 3);
    const auto authorityEnd = rest.find_first_of("/?#");
    parts.authority = std::string(rest.substr(0, authorityEnd));
    if (authorityEnd == std::string_view::npos) rest = {};
    else rest = rest.substr(authorityEnd);
    const auto fragmentStart = rest.find('#');
    if (fragmentStart != std::string_view::npos)
    {
        parts.fragment = std::string(rest.substr(fragmentStart + 1));
        rest = rest.substr(0, fragmentStart);
    }
    const auto queryStart = rest.find('?');
    if (queryStart != std::string_view::npos)
    {
        parts.query = std::string(rest.substr(queryStart + 1));
        rest = rest.substr(0, queryStart);
    }
    parts.path = std::string(rest);
    parts.valid = !parts.authority.empty();
    return parts;
}

/// RFC 3986 section 5.2.4, because `../../` in a stylesheet-adjacent `src` is
/// exactly the shape that silently resolves to a different directory and puts a
/// candidate on the wrong host.
[[nodiscard]] inline std::string RemoveDotSegments(const std::string_view aPath)
{
    std::string output;
    std::string_view input = aPath;
    while (!input.empty())
    {
        if (input.starts_with("../")) input.remove_prefix(3);
        else if (input.starts_with("./")) input.remove_prefix(2);
        else if (input.starts_with("/./")) input.remove_prefix(2);
        else if (input == "/.") input = "/";
        else if (input.starts_with("/../"))
        {
            input.remove_prefix(3);
            const auto slash = output.rfind('/');
            output.resize(slash == std::string::npos ? 0 : slash);
        }
        else if (input == "/..")
        {
            input = "/";
            const auto slash = output.rfind('/');
            output.resize(slash == std::string::npos ? 0 : slash);
        }
        else if (input == "." || input == "..") input = {};
        else
        {
            const std::size_t next = input.starts_with('/') ? input.find('/', 1) : input.find('/');
            const std::size_t take = next == std::string_view::npos ? input.size() : next;
            output.append(input.substr(0, take));
            input.remove_prefix(take);
        }
    }
    return output;
}

[[nodiscard]] inline std::string Composed(const Parts& aParts)
{
    std::string url = aParts.scheme + "://" + aParts.authority + aParts.path;
    if (!aParts.query.empty()) url += "?" + aParts.query;
    if (!aParts.fragment.empty()) url += "#" + aParts.fragment;
    return url;
}
} // namespace Detail

/// Whether a response's headers permit this build to frame it.
///
/// `aOurOrigin` is the surface's own origin -- a name only Open77's pages carry,
/// so a `frame-ancestors` list that names it is a list a site wrote on purpose,
/// and every other list refuses us. It is passed in rather than assumed so this
/// stays a pure function and so the test can pin both directions.
[[nodiscard]] inline Verdict EvaluateFraming(const std::vector<Header>& aHeaders, const std::string_view aOurOrigin)
{
    const std::string origin = Detail::Lower(aOurOrigin);
    for (const auto& header : aHeaders)
    {
        const std::string name = Detail::Lower(header.name);
        const std::string value = Detail::Lower(header.value);
        if (name == "x-frame-options")
        {
            // Every value refuses us: DENY by name, SAMEORIGIN because the
            // response's origin is never ours, ALLOW-FROM because no browser
            // has honoured it since 2015. Some servers send the header twice in
            // one field (`SAMEORIGIN, SAMEORIGIN`); the first token decides.
            if (value.find("deny") != std::string::npos || value.find("sameorigin") != std::string::npos ||
                value.find("allow-from") != std::string::npos)
                return Verdict{false, "x-frame-options"};
        }
        if (name == "content-security-policy")
        {
            // Only the `frame-ancestors` directive is read here. A page's other
            // directives are its own business -- this build frames the document
            // and lets it run under its own policy, exactly as a browser tab
            // would, and a probe that policed them would be pretending to be a
            // proxy it is not.
            const std::size_t at = value.find("frame-ancestors");
            if (at == std::string::npos) continue;
            std::size_t end = value.find(';', at);
            if (end == std::string::npos) end = value.size();
            const std::string directive = value.substr(at + std::string_view("frame-ancestors").size(),
                                                       end - at - std::string_view("frame-ancestors").size());
            bool permits = false;
            std::string_view remaining = directive;
            while (!remaining.empty())
            {
                const auto space = remaining.find_first_of(" \t");
                std::string_view token = space == std::string_view::npos ? remaining : remaining.substr(0, space);
                remaining = space == std::string_view::npos ? std::string_view{} : remaining.substr(space + 1);
                if (token.empty()) continue;
                if (token == "*" || token == "https:" || token == "http:") permits = true;
                else if (!origin.empty() && token == origin) permits = true;
            }
            if (!permits) return Verdict{false, "frame-ancestors"};
        }
    }
    return Verdict{};
}

/// A reference resolved against the document it was written in.
///
/// Empty when the reference cannot be framed at all -- a scheme that is not
/// `http`/`https`, a fragment-only link, or anything the blocklist refuses --
/// because every caller here is choosing something to put in an iframe.
[[nodiscard]] inline std::string ResolveUrl(const std::string_view aDocumentUrl, const std::string_view aReference)
{
    const std::string reference = Detail::Trim(aReference);
    if (reference.empty()) return {};

    const std::string scheme = Ads::SchemeOf(reference);
    if (!scheme.empty())
    {
        if (scheme != "http" && scheme != "https") return {};
        return Detail::Composed(Detail::Split(reference));
    }
    if (reference.front() == '#') return {};

    const Detail::Parts base = Detail::Split(aDocumentUrl);
    if (!base.valid) return {};

    Detail::Parts target;
    target.scheme = base.scheme;
    target.authority = base.authority;

    if (reference.starts_with("//"))
    {
        const auto combined = Detail::Split(base.scheme + ":" + reference);
        if (!combined.valid) return {};
        target.authority = combined.authority;
        target.path = Detail::RemoveDotSegments(combined.path);
        target.query = combined.query;
        target.fragment = combined.fragment;
        return Detail::Composed(target);
    }

    // A reference carries its own fragment, which is never sent to a server but
    // is part of the URL a candidate is compared by.
    std::string referenceBody = reference;
    const auto hash = referenceBody.find('#');
    if (hash != std::string::npos)
    {
        target.fragment = referenceBody.substr(hash + 1);
        referenceBody.resize(hash);
    }
    if (referenceBody.empty())
    {
        target.path = base.path;
        target.query = base.query;
        return Detail::Composed(target);
    }

    if (referenceBody.starts_with("?"))
    {
        target.path = base.path;
        target.query = referenceBody.substr(1);
        return Detail::Composed(target);
    }

    std::string path;
    if (referenceBody.starts_with("/")) path = referenceBody;
    else
    {
        const auto slash = base.path.rfind('/');
        path = (slash == std::string::npos ? std::string("/") : base.path.substr(0, slash + 1)) + referenceBody;
    }
    const auto question = path.find('?');
    if (question != std::string::npos)
    {
        target.query = path.substr(question + 1);
        path.resize(question);
    }
    target.path = Detail::RemoveDotSegments(path);
    return Detail::Composed(target);
}

/// A document a page nested inside itself, and the element that nested it.
struct Embed
{
    std::string url;
    /// `iframe`, `embed`, `object` or `frame` -- kept so a log line can say what
    /// was found, and so a test can tell a `<base>`-relative `<embed src>` from
    /// an `<object data>`.
    std::string kind;
};

/// More than this and the page is an advertising grid, not a shell around an
/// application; the host probes at most this many before giving up.
inline constexpr std::size_t kMaximumEmbeds = 8;

namespace Detail
{
/// The value of one attribute in an already-extracted tag, or empty.
[[nodiscard]] inline std::string AttributeValue(const std::string_view aTag, const std::string_view aName)
{
    const std::string haystack = Lower(aTag);
    const std::string needle = Lower(aName);
    std::size_t at = 0;
    while ((at = haystack.find(needle, at)) != std::string::npos)
    {
        const std::size_t after = at + needle.size();
        // A prefix of a longer attribute name is not this attribute.
        if (at > 0 && (std::isalnum(static_cast<unsigned char>(haystack[at - 1])) != 0 || haystack[at - 1] == '-'))
        {
            at = after;
            continue;
        }
        std::size_t cursor = after;
        while (cursor < haystack.size() && std::isspace(static_cast<unsigned char>(haystack[cursor])) != 0) ++cursor;
        if (cursor >= haystack.size() || haystack[cursor] != '=')
        {
            at = after;
            continue;
        }
        ++cursor;
        while (cursor < haystack.size() && std::isspace(static_cast<unsigned char>(haystack[cursor])) != 0) ++cursor;
        if (cursor >= aTag.size()) return {};
        const char quote = aTag[cursor];
        if (quote == '"' || quote == '\'')
        {
            const auto close = aTag.find(quote, cursor + 1);
            if (close == std::string_view::npos) return {};
            return std::string(aTag.substr(cursor + 1, close - cursor - 1));
        }
        std::size_t close = cursor;
        while (close < aTag.size() && std::isspace(static_cast<unsigned char>(aTag[close])) == 0 &&
               aTag[close] != '>')
            ++close;
        return std::string(aTag.substr(cursor, close - cursor));
    }
    return {};
}
} // namespace Detail

/// Every document a page nests inside itself, in the order the page names them.
///
/// This is a scan, not a parser, and deliberately so: the input is a page this
/// build did not write, the answer only has to be *a* nested document that
/// frames, and a real HTML parser here would be a second implementation of the
/// one already in the browser for no gain. The scan reads four element names,
/// takes `src` (or `data` for `<object>`, which is where an object keeps its
/// URL), skips anything the blocklist refuses or that is not `http`/`https`, and
/// does not repeat a URL.
///
/// A `<base href>` is honoured, because a shell page that sets one has already
/// decided how its own relative URLs resolve and a candidate read without it
/// would point at the wrong host -- the failure mode that looks like "this site
/// has no playable page" on a site that has two.
[[nodiscard]] inline std::vector<Embed> FindEmbeds(const std::string_view aHtml, const std::string_view aDocumentUrl)
{
    std::vector<Embed> found;
    if (aHtml.empty()) return found;

    const std::string lower = Detail::Lower(aHtml);
    std::string documentUrl(aDocumentUrl);

    // A `<base href>` re-bases every relative reference in the document.
    const auto baseAt = lower.find("<base");
    if (baseAt != std::string::npos)
    {
        const auto tagEnd = aHtml.find('>', baseAt);
        if (tagEnd != std::string_view::npos)
        {
            const std::string href = Detail::AttributeValue(aHtml.substr(baseAt, tagEnd - baseAt + 1), "href");
            const std::string resolved = ResolveUrl(documentUrl, href);
            if (!resolved.empty()) documentUrl = resolved;
        }
    }

    struct Element
    {
        std::string_view tag;
        std::string_view attribute;
        std::string kind;
    };
    static constexpr Element kElements[] = {
        {"<iframe", "src", "iframe"},
        {"<frame", "src", "frame"},
        {"<embed", "src", "embed"},
        {"<object", "data", "object"},
    };

    // One pass, left to right, so the list is in the order the page names its
    // nested documents -- which is the order that matters, because the first one
    // is the one the page's own author put in front. A pass per element type
    // would read the same set and answer a different question.
    for (std::size_t at = 0; at < lower.size() && found.size() < kMaximumEmbeds;)
    {
        const std::size_t open = lower.find('<', at);
        if (open == std::string::npos) break;
        at = open;
        std::size_t advance = 0;
        for (const auto& element : kElements)
        {
            if (lower.compare(at, element.tag.size(), element.tag) != 0) continue;
            // `<frame>` must not match `<frameset>`, and no element here may
            // match a longer name that merely starts the same way.
            const std::size_t afterName = at + element.tag.size();
            if (afterName >= lower.size() ||
                (std::isspace(static_cast<unsigned char>(lower[afterName])) == 0 && lower[afterName] != '>' &&
                 lower[afterName] != '/'))
                continue;
            const auto tagEnd = aHtml.find('>', at);
            if (tagEnd == std::string_view::npos)
            {
                advance = lower.size() - at;
                break;
            }
            const std::string_view tag = aHtml.substr(at, tagEnd - at + 1);
            const std::string url = ResolveUrl(documentUrl, Detail::AttributeValue(tag, element.attribute));
            advance = tagEnd + 1 - at;
            if (url.empty() || url == documentUrl) break;
            if (Ads::IsBlocked(url)) break;
            if (std::none_of(found.begin(), found.end(), [&](const Embed& e) { return e.url == url; }))
                found.push_back(Embed{url, element.kind});
            break;
        }
        at += advance > 0 ? advance : 1;
    }
    return found;
}
} // namespace op77::WebUI::Framing
