#pragma once

// What a framed page may open, and what it may reach.
//
// ---------------------------------------------------------------------------
// WHY A TELEVISION NEEDS THIS AT ALL
// ---------------------------------------------------------------------------
//
// The media page is the one surface whose specification is "show a page somebody
// pasted", and the sites that publish a free player pay for it with popups. The
// first live session on one of them (`123movie-tv.it.com`) produced this, which
// is the whole complaint in one measurement:
//
//     the shell page is 5.7 KB and carries FIVE ad scripts
//         https://focusameneducation.com/<id>/invoke.js   (x5)
//     and frames two real apps
//         https://v3.freemovies.lol/?logo=...&brand=Soap2day
//         https://moviestv.my/
//
// The five scripts do one thing: open windows. In a game there is no window to
// open -- CEF renders a popup as an overlay on the same surface (`OnPopupShow`
// and `OnPopupSize` are how this host paints one) -- so what a player sees is a
// panel of somebody else's advertising dropped on top of the film, and there is
// no title bar, no tab and no close button to get out of it. That is why the
// report was "i cant even navigate these pages from random popups": every click
// produced one, and the page behind it became unreachable.
//
// ---------------------------------------------------------------------------
// THE TWO HALVES, AND WHY THEY ARE DIFFERENT KINDS OF THING
// ---------------------------------------------------------------------------
//
// **A popup is a decision.** `DecidePopup` below is asked what to do with a
// window a framed page has asked for, and it has three answers:
//
//   * `Drop`   -- refuse it. This is the ad case, and it is the default.
//   * `Follow` -- load it in the frame that asked, in place. This is the case
//                 that makes a site *usable* rather than merely quiet: a player
//                 that opens its video in a popup (most of them do) still plays,
//                 because the URL lands on the screen instead of nowhere.
//   * `Allow`  -- let CEF create the popup. Nothing uses this today; it exists
//                 so the policy has a name for "this one was legitimate" rather
//                 than overloading `Follow`.
//
// The rule that separates the ad from the player is **two questions, not one**,
// and the second of them was measured the hard way:
//
//   * WAS THERE A CLICK? A window opened with nothing behind it is advertising by
//     definition -- a player that needs a window asks for it when you press play
//     -- while a window opened by a click is what the person in front of the
//     television just asked for. A blocklist alone cannot make that distinction.
//
//   * DID THE WINDOW STAY INSIDE THE SITE THAT ASKED? A gesture check alone cannot
//     decide it, because these players are embedded in pages whose ad overlays sit
//     over the player and eat the click themselves: the gesture is real and the
//     window is still an advertisement. Measured in one live session, four windows
//     followed and every one of them was advertising:
//
//         popup:follow https://sorrowfulpsychology.com/iLmSbj
//         popup:follow https://ay267.com/?rb=...
//         popup:follow https://yz.woolderstrolld.qpon/cx/...
//         popup:follow https://ro.dogfootpalmo.cfd/cx/...
//
//     and the frames were not idle about it:
//
//         browser_console:chrome-error://chromewebdata/:1:Refused to display
//             'https://sorrowfulpsychology.com/' in a frame because it set
//             'X-Frame-Options' to 'deny'.
//
//     The frame that asked was the provider's own top document, so following it
//     replaced the player with an ad landing page that refuses to be framed. That
//     refusal page -- reading "refused to connect" -- is what the person watching
//     saw instead of the film, on every server they tried, because every provider's
//     player sits in the same kind of page. A window that would leave the site that
//     asked for it is therefore dropped: only a same-site window, or one asked for
//     by a page this host serves itself, is followed.
//
// **A host is a fact.** `IsBlocked` is the blocklist half: named networks whose
// entire business is the above, refused at the request layer as well as at the
// window layer, because a script refused its window will keep trying and a script
// that never loads cannot. This list is deliberately short and legible. It is not
// EasyList and does not pretend to be: it is the ad and popup networks a
// television meets in practice, spelled out so that every entry can be argued
// with, and it is paired with a *behavioural* rule (the gesture test) precisely
// because a hand-maintained host list is always incomplete.
//
// ---------------------------------------------------------------------------
// WHY NONE OF THIS IS A SECURITY BOUNDARY
// ---------------------------------------------------------------------------
//
// The security boundary is the surface's page policy in the client -- a framed
// site cannot reach this process, cannot call the bridge, and cannot load what
// the host does not serve it -- and the origin: the frame is another origin, so
// it cannot touch the page that framed it either. It is deliberately NOT the
// iframe's `sandbox` attribute, which the page no longer sets: the players these
// sites ship refuse to play inside one (their own `sandboxVerdict()` calls it
// `sandboxed`), which is measured in `docs/research/webui-media-and-audio.md`.
// This file is a *quality* decision: it decides whether the thing on screen is
// the film or the advertising around it. It is allowed to be wrong occasionally, and it is
// kept pure and tested because being wrong in the other direction -- dropping a
// real player's window -- looks exactly like a broken site.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace op77::WebUI::Ads
{
/// Networks whose only business on a television is a window nobody asked for.
///
/// Suffix-matched against the host, so an entry covers its own subdomains. The
/// list is deliberately networks and not "things called ad": a content
/// recommendation widget on a page is not what this exists for, and every entry
/// here is something that opens windows, redirects the page, or does both.
inline constexpr std::string_view kBlockedHosts[] = {
    // The pair measured on the reporting site, and its siblings.
    "focusameneducation.com",
    "invokejs.com",
    // Popunder/popup networks.
    "popads.net",
    "popadscdn.net",
    "popcash.net",
    "propellerads.com",
    "propellerpops.com",
    "onclickalgo.com",
    "onclickprediction.com",
    "onclckds.com",
    "adsterra.com",
    "adsterracdn.com",
    "hilltopads.net",
    "hilltopads.com",
    "clickadu.com",
    "clickadilla.com",
    "adcash.com",
    // Adult-network redirect chains, which free-streaming sites use heavily.
    "exoclick.com",
    "exosrv.com",
    "exdynsrv.com",
    "exclusiveincome.com",
    "juicyads.com",
    "juicyads.net",
    "trafficjunky.com",
    "trafficjunky.net",
    "realsrv.com",
    "tsyndicate.com",
    // Exchanges and the ad-tech chain, which is where a redirect lands.
    "doubleclick.net",
    "googlesyndication.com",
    "googleadservices.com",
    "adnxs.com",
    "adsrvr.org",
    "casalemedia.com",
    "criteo.com",
    "criteo.net",
    "media.net",
    "mgid.com",
    "openx.net",
    "outbrain.com",
    "pubmatic.com",
    "revcontent.com",
    "rubiconproject.com",
    "sharethrough.com",
    "smartadserver.com",
    "taboola.com",
    "yieldmo.com",
    "zedo.com",
    "amazon-adsystem.com",
};

/// Host fragments that name a business rather than a brand: a host that *calls
/// itself* one of these is an advertising host whatever domain it sits under,
/// which is how a campaign rotates through fresh domains faster than a list can
/// be written.
inline constexpr std::string_view kBlockedHostTokens[] = {
    "popads",
    "popcash",
    "popunder",
    "adsterra",
    "propellerads",
    "adservice",
    "adserver",
    "clickadu",
    "adnetwork",
    "adsystem",
    "adnxs",
    "ad-delivery",
};

/// The scheme in lowercase, or empty when the URL has none.
[[nodiscard]] inline std::string SchemeOf(const std::string_view aUrl)
{
    const auto colon = aUrl.find(':');
    if (colon == std::string_view::npos || colon == 0) return {};
    for (std::size_t i = 0; i < colon; ++i)
    {
        const char c = aUrl[i];
        if (i == 0 ? (std::isalpha(static_cast<unsigned char>(c)) == 0)
                   : (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '+' && c != '-' && c != '.'))
            return {};
    }
    std::string scheme(aUrl.substr(0, colon));
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
        [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return scheme;
}

/// The authority's host, lowercased and without a port or userinfo.
///
/// `https://User@Example.COM:8443/a` -> `example.com`. Written by hand rather
/// than by a URL library because everything that consumes this is a decision
/// about a host, and a function that cannot fail -- it returns an empty string
/// for input it cannot read -- is easier to be right about than one that throws.
[[nodiscard]] inline std::string LowerHost(const std::string_view aUrl)
{
    const auto scheme = aUrl.find("://");
    if (scheme == std::string_view::npos) return {};
    std::string_view rest = aUrl.substr(scheme + 3);
    const auto end = rest.find_first_of("/?#");
    if (end != std::string_view::npos) rest = rest.substr(0, end);
    // Userinfo, if any -- the last `@` before the host, per RFC 3986.
    const auto at = rest.rfind('@');
    if (at != std::string_view::npos) rest = rest.substr(at + 1);
    // A port, unless the colons are inside a bracketed IPv6 literal -- where the
    // host ends at the bracket and the port follows it, so a rfind would cut the
    // literal in half.
    if (!rest.empty() && rest.front() == '[')
    {
        const auto close = rest.find(']');
        if (close != std::string_view::npos) rest = rest.substr(0, close + 1);
    }
    else
    {
        const auto colon = rest.rfind(':');
        if (colon != std::string_view::npos) rest = rest.substr(0, colon);
    }
    std::string host(rest);
    std::transform(host.begin(), host.end(), host.begin(),
        [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return host;
}

/// `aHost` is `aRule`, or a subdomain of it -- so a rule covers `cdn.rule` and
/// `rule` but never `notrule` and never `rule.example.com`.
[[nodiscard]] inline bool HostMatchesRule(const std::string_view aHost, const std::string_view aRule)
{
    if (aRule.empty() || aHost.size() < aRule.size()) return false;
    const std::size_t offset = aHost.size() - aRule.size();
    if (aHost.compare(offset, aRule.size(), aRule) != 0) return false;
    return offset == 0 || aHost[offset - 1] == '.';
}

/// Whether a host names something this build refuses to load.
[[nodiscard]] inline bool IsBlockedHost(const std::string_view aLowerHost)
{
    if (aLowerHost.empty()) return false;
    for (const auto rule : kBlockedHosts)
        if (HostMatchesRule(aLowerHost, rule)) return true;
    for (const auto token : kBlockedHostTokens)
        if (aLowerHost.find(token) != std::string_view::npos) return true;
    return false;
}

/// Whether a URL does. The whole URL, not just its host, so a path-addressed
/// campaign on an otherwise ordinary domain has somewhere to be listed later;
/// today every rule is a host rule.
[[nodiscard]] inline bool IsBlocked(const std::string_view aUrl)
{
    const std::string scheme = SchemeOf(aUrl);
    if (scheme != "http" && scheme != "https") return false;
    return IsBlockedHost(LowerHost(aUrl));
}

/// How many rules the list carries, for a log line and for the test that pins
/// that the list is not silently emptied.
[[nodiscard]] inline constexpr std::size_t RuleCount()
{
    return std::size(kBlockedHosts) + std::size(kBlockedHostTokens);
}

// ---------------------------------------------------------------------------
// THE OPERATOR LAYER
// ---------------------------------------------------------------------------
//
// The list above is compiled in, which is the right default and the wrong
// ceiling: a new popup network is a domain registered on Tuesday, and the
// person who owns the server is the one who sees it in the log that evening.
// Waiting for a build, a signed catalog and every client updating is how a
// television ends up showing advertisements for a fortnight.
//
// So the list has a second layer, and it is owned by the server: an operator
// adds a host, the server pushes it to the clients in that session, and each
// client's host process starts refusing requests to it. The whole path is
// `docs/research/webui-media-and-audio.md`; what matters here is the shape of
// the layer itself, and it has three properties worth stating together because
// each one is a decision:
//
//   * IT ONLY ADDS. An operator rule joins the compiled list; nothing the server
//     sends can remove one. That asymmetry is the security story of the whole
//     feature: a server -- or anyone who can talk to a client pretending to be
//     one -- may make a television MORE conservative than the build, never less.
//     A rule that tried to unblock `doubleclick.net` would need a code change
//     reviewed by whoever ships the client, which is exactly the friction the
//     compiled list is for.
//
//   * IT IS VALIDATED HERE, NOT TRUSTED. Every incoming rule is normalised and
//     checked by `NormaliseRule` / `NormaliseToken` below before it is stored or
//     matched, and the ones refused are reported back to the server with the
//     reason. This is not politeness: a rule of `com` or `co.uk` would turn one
//     operator typo into a client that cannot load any page at all, and a rule
//     of `*` would be a denial of service on the feature the feature exists for.
//
//   * IT IS DATA, NOT CODE. A rule is a string; nothing is compiled, evaluated
//     or pattern-matched. That is why the list is legible and why every entry
//     can be argued with.

/// A multi-label suffix that is a registry, not a company. Refusing these is
/// what stops an operator meaning `ads.example.co.uk` from writing `co.uk` and
/// blocking a country.
inline constexpr std::string_view kPublicSuffixes[] = {
    "co.uk", "org.uk", "ac.uk", "gov.uk", "me.uk", "net.uk", "sch.uk",
    "com.au", "net.au", "org.au", "edu.au", "gov.au", "id.au",
    "co.nz", "net.nz", "org.nz", "govt.nz",
    "com.br", "net.br", "org.br", "gov.br",
    "com.cn", "net.cn", "org.cn", "gov.cn", "edu.cn",
    "co.jp", "or.jp", "ne.jp", "ac.jp", "go.jp",
    "co.kr", "or.kr", "ne.kr", "go.kr",
    "co.in", "net.in", "org.in", "gov.in", "ac.in",
    "com.mx", "com.ar", "com.co", "com.pe", "com.ve", "com.uy",
    "co.za", "org.za", "net.za", "gov.za",
    "com.tr", "com.tw", "com.hk", "com.sg", "com.my", "com.ph", "com.vn",
    "co.id", "or.id", "ac.id", "go.id", "web.id",
    "co.il", "org.il", "ac.il", "gov.il",
    "com.pl", "com.ua", "com.ru", "co.th", "in.th", "go.th",
    "com.eg", "com.sa", "com.pk", "com.bd", "com.ng", "com.gh", "com.ke",
    "co.ke", "co.tz", "com.et", "co.ao", "co.mz", "co.zw", "com.na",
    "com.es", "com.pt", "com.it", "com.de", "com.fr", "co.at", "co.hu",
};

/// Labels that are a top-level registry rather than a business. A *token* rule
/// matches anywhere in a host, so a token of `com` would refuse every `.com` on
/// the internet; a *host* rule needs two labels, which already makes this
/// impossible there. The token case is why this list exists.
inline constexpr std::string_view kRegistryLabels[] = {
    "com", "net", "org", "gov", "edu", "mil", "int", "www", "http", "https",
    "local", "internal", "arpa", "info", "biz", "name", "mobi", "online", "site",
};

/// A rule that is already a host: lowercase, no wildcard, at least two labels,
/// and not a registry. `""` when the entry is not one, with `aReason` set to a
/// word the operator can act on.
///
/// It is deliberately forgiving about the SHAPE a person actually pastes -- a
/// full URL, a leading `*.`, a trailing slash, a trailing dot, mixed case --
/// because the alternative is an operator whose blocking rule silently does
/// nothing, and being strict about the *acceptance* is what keeps the refusal a
/// thing that is said rather than a thing that happens.
[[nodiscard]] inline std::string NormaliseRule(const std::string_view aRaw, std::string& aReason)
{
    aReason.clear();
    std::string text(aRaw);
    const auto notSpace = [](const unsigned char c) { return std::isspace(c) == 0; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    if (text.empty()) { aReason = "empty"; return {}; }
    if (text.size() > 512) { aReason = "too_long"; return {}; }

    // A whole URL. `LowerHost` reads the authority and nothing else, which is
    // the only part of a link that is a host.
    if (text.find("://") != std::string::npos)
    {
        std::string host = LowerHost(text);
        if (host.empty()) { aReason = "unreadable_url"; return {}; }
        text = std::move(host);
    }
    else
    {
        // A bare host with a path or a query stuck to it: cut at the first
        // separator, then lowercase what is left.
        const auto cut = text.find_first_of("/?#");
        if (cut != std::string::npos) text.resize(cut);
        std::transform(text.begin(), text.end(), text.begin(),
            [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }

    // A leading wildcard, or a leading dot, both of which people write to mean
    // "this host and its subdomains" -- which is what a rule already means.
    while (!text.empty() && (text.front() == '*' || text.front() == '.')) text.erase(text.begin());
    // A port, which a rule does not express.
    if (const auto colon = text.rfind(':'); colon != std::string::npos && text.find('[') == std::string::npos)
        text.resize(colon);
    // A trailing dot: `example.com.` is the same host, spelled absolutely.
    while (!text.empty() && text.back() == '.') text.pop_back();

    if (text.empty()) { aReason = "empty"; return {}; }
    if (text.size() > 253) { aReason = "too_long"; return {}; }

    // Shape: labels of letters, digits and inner hyphens, joined by single dots.
    // Anything else -- a space, a `*`, a `?`, a slash, a `:` -- is refused rather
    // than partially interpreted, because a rule that half-matches is a rule the
    // operator cannot predict.
    std::size_t labels = 1;
    bool labelHasChar = false;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '.')
        {
            if (!labelHasChar) { aReason = "empty_label"; return {}; }
            ++labels;
            labelHasChar = false;
            continue;
        }
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-';
        if (!ok) { aReason = "invalid_character"; return {}; }
        if (c == '-' && !labelHasChar) { aReason = "invalid_hyphen"; return {}; }
        if (c == '-' && (i + 1 >= text.size() || text[i + 1] == '.')) { aReason = "invalid_hyphen"; return {}; }
        labelHasChar = true;
    }
    if (!labelHasChar) { aReason = "empty_label"; return {}; }
    if (labels < 2) { aReason = "needs_a_domain"; return {}; }

    for (const auto suffix : kPublicSuffixes)
        if (text == suffix) { aReason = "public_suffix"; return {}; }

    return text;
}

/// A rule that is matched anywhere inside a host. Same forgiving shape, plus one
/// extra refusal: a token must not be a registry label, because `com` as a
/// substring is every `.com` there is.
[[nodiscard]] inline std::string NormaliseToken(const std::string_view aRaw, std::string& aReason)
{
    aReason.clear();
    std::string text(aRaw);
    const auto notSpace = [](const unsigned char c) { return std::isspace(c) == 0; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    std::transform(text.begin(), text.end(), text.begin(),
        [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (text.empty()) { aReason = "empty"; return {}; }
    // Before the length rule, so that `com` is refused for the reason that
    // matters rather than for being three characters long: the two are both
    // refusals, but only one of them explains why the entry could never be
    // honoured at any length.
    for (const auto label : kRegistryLabels)
        if (text == label) { aReason = "registry_label"; return {}; }
    if (text.size() < 4) { aReason = "token_too_short"; return {}; }
    if (text.size() > 63) { aReason = "token_too_long"; return {}; }
    if (text.find('.') != std::string::npos) { aReason = "token_is_a_host"; return {}; }
    for (const char c : text)
    {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-';
        if (!ok) { aReason = "invalid_character"; return {}; }
    }
    return text;
}

/// The compiled list plus whatever the operator's server has added.
///
/// Not thread-safe on its own -- a host hands the whole object to its readers as
/// one immutable snapshot instead (see `HostRuntime`), so a rule arriving mid-
/// request can never be seen half-applied.
class Blocklist
{
public:
    /// One entry that was not accepted, with the reason, so the operator is told
    /// which of their rules did nothing and why.
    struct Refusal
    {
        std::string rule;
        std::string reason;
    };

    Blocklist() = default;

    /// Adds host rules. Returns how many were accepted; every refusal is
    /// appended to `aRefused`. A duplicate -- of an earlier operator rule or of
    /// one compiled in -- is accepted and stored once.
    std::size_t AddHostRules(const std::span<const std::string> aRules,
                             std::vector<Refusal>& aRefused)
    {
        std::size_t accepted = 0;
        for (const auto& raw : aRules)
        {
            std::string reason;
            const std::string rule = NormaliseRule(raw, reason);
            if (rule.empty())
            {
                if (aRefused.size() < kMaximumRefusals) aRefused.push_back({raw, reason});
                continue;
            }
            if (std::find(m_hosts.begin(), m_hosts.end(), rule) != m_hosts.end()) continue;
            m_hosts.push_back(rule);
            ++accepted;
        }
        return accepted;
    }

    std::size_t AddTokenRules(const std::span<const std::string> aRules,
                              std::vector<Refusal>& aRefused)
    {
        std::size_t accepted = 0;
        for (const auto& raw : aRules)
        {
            std::string reason;
            const std::string token = NormaliseToken(raw, reason);
            if (token.empty())
            {
                if (aRefused.size() < kMaximumRefusals) aRefused.push_back({raw, reason});
                continue;
            }
            if (std::find(m_tokens.begin(), m_tokens.end(), token) != m_tokens.end()) continue;
            m_tokens.push_back(token);
            ++accepted;
        }
        return accepted;
    }

    /// Drops every operator rule. The compiled list is untouched, which is what
    /// makes "clear" a safe verb for an operator command to expose.
    void ClearOperatorRules()
    {
        m_hosts.clear();
        m_tokens.clear();
    }

    [[nodiscard]] bool IsBlocked(const std::string_view aUrl) const
    {
        const std::string scheme = SchemeOf(aUrl);
        if (scheme != "http" && scheme != "https") return false;
        const std::string host = LowerHost(aUrl);
        if (host.empty()) return false;
        if (op77::WebUI::Ads::IsBlockedHost(host)) return true;
        for (const auto& rule : m_hosts)
            if (HostMatchesRule(host, rule)) return true;
        for (const auto& token : m_tokens)
            if (host.find(token) != std::string::npos) return true;
        return false;
    }

    [[nodiscard]] const std::vector<std::string>& OperatorHosts() const { return m_hosts; }
    [[nodiscard]] const std::vector<std::string>& OperatorTokens() const { return m_tokens; }
    [[nodiscard]] std::size_t OperatorRuleCount() const { return m_hosts.size() + m_tokens.size(); }
    /// Compiled rules plus operator rules: what `IsBlocked` actually consults.
    [[nodiscard]] std::size_t Count() const { return RuleCount() + OperatorRuleCount(); }

    /// `193 compiled + 4 operator` -- one string for a trace line and for the
    /// receipt a server prints back to its operator.
    [[nodiscard]] std::string Describe() const
    {
        return std::to_string(RuleCount()) + " compiled + " +
            std::to_string(OperatorRuleCount()) + " operator";
    }

private:
    /// Enough to report the shape of a mistake without turning a payload into a
    /// log flood.
    static constexpr std::size_t kMaximumRefusals = 32;

    std::vector<std::string> m_hosts;
    std::vector<std::string> m_tokens;
};

/// What to do with a window a framed page asked to open.
enum class PopupAction
{
    /// Refuse it. The page keeps running; only the window is lost.
    Drop = 0,
    /// Load it in the frame that asked, replacing the page in place. What makes
    /// a player that opens its video in a popup still playable.
    Follow = 1,
    /// Let CEF create it. Unused today, and named so the policy has a value for
    /// "this one was legitimate" instead of overloading `Follow`.
    Allow = 2,
};

/// The site a host belongs to: its last two labels, or its last three when the
/// final two are a multi-label registry (`co.uk`), because `foo.co.uk` and
/// `bar.co.uk` are two companies and not one site.
///
/// An address is returned whole -- `10.0.0.7` is not `0.7`, and a television may
/// well be pointed at one -- and a single label (`localhost`) has no site to
/// speak of and returns nothing, which the caller reads as "cannot be compared".
[[nodiscard]] inline std::string RegistrableDomain(const std::string_view aLowerHost)
{
    if (aLowerHost.empty()) return {};
    std::vector<std::string_view> labels;
    for (std::size_t start = 0; start <= aLowerHost.size();)
    {
        const auto dot = aLowerHost.find('.', start);
        const auto end = dot == std::string_view::npos ? aLowerHost.size() : dot;
        labels.push_back(aLowerHost.substr(start, end - start));
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    for (const auto& label : labels)
        if (label.empty()) return {};
    if (labels.size() < 2) return {};
    bool allNumeric = true;
    for (const auto& label : labels)
    {
        for (const char c : label)
            if (std::isdigit(static_cast<unsigned char>(c)) == 0) { allNumeric = false; break; }
        if (!allNumeric) break;
    }
    if (allNumeric) return std::string(aLowerHost);
    std::size_t take = 2;
    const std::string_view tail = aLowerHost.substr(
        aLowerHost.size() - labels[labels.size() - 2].size() - labels.back().size() - 1);
    for (const auto suffix : kPublicSuffixes)
        if (tail == suffix && labels.size() >= 3) { take = 3; break; }
    std::string domain(labels[labels.size() - take]);
    for (std::size_t i = labels.size() - take + 1; i < labels.size(); ++i)
    {
        domain.push_back('.');
        domain.append(labels[i]);
    }
    return domain;
}

/// Whether two URLs name the same site, by registrable domain. False whenever
/// either side has no site to compare -- an opaque origin, a custom scheme, an
/// empty string -- because "cannot be shown to be the same site" is not the same
/// fact as "is the same site", and only the second one earns a window.
[[nodiscard]] inline bool SameSite(const std::string_view aUrl, const std::string_view aOther)
{
    const std::string mine = RegistrableDomain(LowerHost(aUrl));
    const std::string theirs = RegistrableDomain(LowerHost(aOther));
    return !mine.empty() && mine == theirs;
}

/// A window a framed page has asked for, with everything the policy decides it
/// by. A struct rather than five positional arguments because three of the five
/// are booleans, and `DecidePopup(url, asker, false, true, false)` at a call site
/// is a question nobody can read the answer to.
struct PopupRequest
{
    /// What the window would load.
    std::string_view url;
    /// The frame that asked for it, as the browser reports it. Empty when the
    /// caller has none to offer, which is dropped rather than followed: a window
    /// whose origin cannot be established has not been shown to stay anywhere.
    std::string_view asker;
    /// Whether that frame is a document THIS HOST serves. The host computes it
    /// (`SameOrigin` against the surface's own origin) because only the host knows
    /// which origins are its own, and it is the one case where "did the window
    /// stay inside the site that asked" has no site to mean: the surfaces' own
    /// documents -- the television page, the companion, the shell -- are served by
    /// this process and are the pages that may legitimately open a window.
    bool askerIsHostPage = false;
    /// Whether the navigation was in response to a click.
    bool userGesture = false;
    /// Whether the blocklist already refused the URL, when the caller looked.
    bool blocked = false;
};

/// Why a window was dropped, so a trace line says which rule did it rather than
/// leaving a reader to re-derive the policy from the URL.
[[nodiscard]] inline const char* RefusalReason(const PopupRequest& aRequest)
{
    if (aRequest.url.empty()) return "empty";
    const std::string scheme = SchemeOf(aRequest.url);
    if (scheme != "http" && scheme != "https") return "not_a_window";
    if (aRequest.blocked || IsBlocked(aRequest.url)) return "blocked_host";
    if (!aRequest.userGesture) return "no_gesture";
    return "cross_site";
}

/// The window policy.
///
/// Order matters and is the whole policy:
///
///   1. Anything that is not `http` or `https` is dropped. A framed page asking
///      for `javascript:`, `about:`, `data:` or a custom scheme is not asking
///      for a window, and `OnProtocolExecution` would refuse it anyway.
///   2. A blocked host is dropped, gesture or not. A click on an ad is still an
///      ad, and a site that has talked you into one has not earned the window.
///   3. No user gesture is dropped. This is the rule that catches the actual
///      complaint -- an on-load script opening five windows -- because a window
///      with no click behind it is advertising by definition.
///   4. A window that would leave the site that asked for it is dropped, which is
///      the rule the second live session added: the click is real, the ad overlay
///      ate it, and following the window destroys the player. A page this host
///      serves itself is exempt, because there is no other site for its window to
///      leave -- see `PopupRequest::askerIsHostPage`.
///   5. Anything left follows in place. A player that opens its video on a click
///      wants a window it cannot have, and a television's answer is to put that
///      URL on the screen rather than nowhere.
[[nodiscard]] inline PopupAction DecidePopup(const PopupRequest& aRequest)
{
    const std::string scheme = SchemeOf(aRequest.url);
    if (aRequest.url.empty() || (scheme != "http" && scheme != "https")) return PopupAction::Drop;
    if (aRequest.blocked || IsBlocked(aRequest.url)) return PopupAction::Drop;
    if (!aRequest.userGesture) return PopupAction::Drop;
    if (aRequest.askerIsHostPage) return PopupAction::Follow;
    if (SameSite(aRequest.url, aRequest.asker)) return PopupAction::Follow;
    return PopupAction::Drop;
}

/// The same policy against the compiled list AND the operator's additions -- the
/// overload every host call site uses, because the operator's layer is the one
/// that exists to be current.
[[nodiscard]] inline PopupAction DecidePopup(PopupRequest aRequest, const Blocklist& aBlocklist)
{
    aRequest.blocked = aRequest.blocked || aBlocklist.IsBlocked(aRequest.url);
    return DecidePopup(aRequest);
}

/// The policy's name, for the trace line every drop and follow writes.
[[nodiscard]] inline const char* Describe(const PopupAction aAction)
{
    switch (aAction)
    {
    case PopupAction::Drop: return "drop";
    case PopupAction::Follow: return "follow";
    case PopupAction::Allow: return "allow";
    }
    return "drop";
}
} // namespace op77::WebUI::Ads
