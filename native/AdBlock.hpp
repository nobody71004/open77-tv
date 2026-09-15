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
// The rule that separates the ad from the player is the **user gesture**, and it
// is the reason this is a policy and not a filter: a window opened with no click
// behind it is advertising by definition -- a player that needs a window asks for
// it when you press play -- while a window opened by a click is what the person
// in front of the television just asked for. A blocklist alone cannot make that
// distinction, and a gesture check alone would let a click on a page's own ad
// inventory through.
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
// The security boundary is the iframe's `sandbox` in the page and the surface's
// page policy in the client -- a framed site cannot reach this process, cannot
// call the bridge, and cannot navigate the top level without a click. This file
// is a *quality* decision: it decides whether the thing on screen is the film or
// the advertising around it. It is allowed to be wrong occasionally, and it is
// kept pure and tested because being wrong in the other direction -- dropping a
// real player's window -- looks exactly like a broken site.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

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
///   4. Anything else follows in place. A player that opens its video on a click
///      wants a window it cannot have, and a television's answer is to put that
///      URL on the screen rather than nowhere.
[[nodiscard]] inline PopupAction DecidePopup(const std::string_view aUrl, const bool aUserGesture, const bool aBlocked)
{
    const std::string scheme = SchemeOf(aUrl);
    if (aUrl.empty() || (scheme != "http" && scheme != "https")) return PopupAction::Drop;
    if (aBlocked || IsBlocked(aUrl)) return PopupAction::Drop;
    if (!aUserGesture) return PopupAction::Drop;
    return PopupAction::Follow;
}

/// Overload for callers holding only the URL and the gesture.
[[nodiscard]] inline PopupAction DecidePopup(const std::string_view aUrl, const bool aUserGesture)
{
    return DecidePopup(aUrl, aUserGesture, false);
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
