#pragma once

// What a surface's page is allowed to reach.
//
// ---------------------------------------------------------------------------
// WHY THE CLIENT OWNS THIS, AND THE RESOURCE ONLY ASKS
// ---------------------------------------------------------------------------
//
// Every file the web host serves on behalf of a resource carries a
// `Content-Security-Policy` header. That header is written HERE, by the client,
// and not by the resource that ships the page -- a resource is code downloaded
// from a server, and its page runs in the game's own browser process, so a page
// cannot be trusted to describe its own privileges. What a page can do is say
// *which* of a small, closed set of policies it needs, and the directives behind
// each name are only ever edited in this file.
//
// ---------------------------------------------------------------------------
// WHAT THE TWO POLICIES ARE FOR
// ---------------------------------------------------------------------------
//
// `Strict` is everything: a resource UI is a menu, a HUD, a panel. It draws its
// own shipped files and talks to the client through CEF's own query channel,
// which is not a network fetch and is deliberately unaffected by `connect-src`.
// It has no business loading a remote script, framing a site, or fetching a URL.
//
// `Media` exists because of one page and is honest about it: a television whose
// whole purpose is to play a link somebody pasted. That means YouTube's player
// script and its embed frame, thumbnails for a link's poster, and media from
// whatever host a link names.
//
// ---------------------------------------------------------------------------
// HOW THIS WAS FOUND
// ---------------------------------------------------------------------------
//
// The media page was written to fetch YouTube's IFrame API and fall back to a
// bare `<iframe>` embed when it could not. In a live session it reported, in
// order:
//
//     television 3: loading (youtube embed (player api))
//     television 3: youtube_api_failed (the player script refused to load)
//     television 3: loading (youtube embed (no player api))
//     television 3: youtube_loaded (8scL5oJX6CM)
//
// and then nothing at all -- no state, no error, no picture, and a screen showing
// the page's own colour-bar idle pattern. Every one of those lines is the page
// reporting the consequence of a header it never saw. The header was this:
//
//     default-src 'self'; img-src 'self' data:; media-src 'self';
//     style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline';
//     connect-src 'none'; object-src 'none'; frame-src 'none'; base-uri 'none'
//
// `script-src 'self'` refuses `https://www.youtube.com/iframe_api`, and
// `frame-src 'none'` refuses the embed the fallback then builds. The page was not
// broken; it was never allowed to reach the two origins it needs.
//
// The lesson worth keeping: a page cannot report a policy it cannot read. When a
// page says "the script refused to load", the refusal may have happened before
// the request left.

#include <cstdint>

namespace op77::WebUI
{
/// A page's network privileges, as a closed set. The numeric values are the wire
/// values carried by `CreateSurfacePayload`; append only.
enum class WebPagePolicy : uint8_t
{
    /// Same-origin files only. No remote script, no frame, no fetch. Every
    /// resource UI that is not a media surface wants this and gets it by
    /// default.
    Strict = 0,
    /// A page that plays links: YouTube's player script and embed frame,
    /// thumbnails, and media from any https host. See the directives' own notes
    /// for why each one is there.
    Media = 1,
};

/// The highest value a payload may legitimately carry.
inline constexpr uint8_t kMaximumWebPagePolicy = static_cast<uint8_t>(WebPagePolicy::Media);

/// The policy a surface asked for, from the wire value. Anything the host does
/// not recognise is `Strict` -- an unknown policy must fail closed.
[[nodiscard]] constexpr WebPagePolicy PolicyFromWire(const uint8_t aValue)
{
    return aValue == static_cast<uint8_t>(WebPagePolicy::Media) ? WebPagePolicy::Media
                                                                : WebPagePolicy::Strict;
}

/// The header for a page that draws its own shipped files and nothing else.
///
/// Unchanged from the policy every surface had before a media page needed one,
/// and deliberately spelled out rather than assumed: this string is the whole
/// security property of every menu, HUD and panel the client renders.
inline constexpr const char* kStrictContentSecurityPolicy =
    "default-src 'self'; img-src 'self' data:; media-src 'self'; "
    "style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; "
    "connect-src 'none'; object-src 'none'; frame-src 'none'; base-uri 'none'";

/// The header for a page whose job is to play a link.
///
/// Every origin here is named rather than wildcarded where it can be:
///
///   * `script-src` gets YouTube, because the IFrame API is the only way a player
///     reports its own state -- without it a page cannot tell a refused video
///     from a player that never started, which is exactly the blind spot that
///     made the original failure undiagnosable.
///   * `frame-src` gets YouTube and its no-cookie host, and nothing else. This is
///     the directive that was `'none'`; it is what refused the embed itself.
///   * `media-src` is deliberately open over `https:`, because "plays any link
///     somebody pastes" is this page's whole specification and a direct media URL
///     has to load from its own host. `blob:` is for the page's own demuxing and
///     object URLs.
///   * `connect-src` stays narrow: the player API talks to its frame by
///     `postMessage`, so the fetch surface only needs to cover YouTube's own
///     endpoints.
///   * `object-src`, `base-uri`, `form-action` and `worker-src` stay shut. A page
///     that plays links has no reason to inject a plugin, rewrite its base URL,
///     post a form, or start a worker, and none of those are transitional.
inline constexpr const char* kMediaContentSecurityPolicy =
    "default-src 'self'; "
    "img-src 'self' data: https://i.ytimg.com https://*.ytimg.com https://*.ggpht.com; "
    "media-src 'self' blob: https:; "
    "style-src 'self' 'unsafe-inline' https://www.youtube.com; "
    "script-src 'self' 'unsafe-inline' https://www.youtube.com https://s.ytimg.com; "
    "connect-src https://www.youtube.com https://*.googlevideo.com https://*.ytimg.com; "
    "frame-src https://www.youtube.com https://www.youtube-nocookie.com; "
    "worker-src 'none'; object-src 'none'; base-uri 'none'; form-action 'none'";

/// The directives a page is served under.
[[nodiscard]] constexpr const char* ContentSecurityPolicy(const WebPagePolicy aPolicy)
{
    return aPolicy == WebPagePolicy::Media ? kMediaContentSecurityPolicy
                                           : kStrictContentSecurityPolicy;
}

/// Whether a surface under this policy may reach anything that is not one of the
/// client's own page files.
///
/// This and the header above are two halves of ONE decision, and this is the half
/// that was missing for a while. `Content-Security-Policy` tells a page what it is
/// allowed to load; it cannot make a request *succeed*. The web host hands CEF only
/// the files it resolves itself and leaves the rest of the network to the default
/// handler, which it disables -- and `SurfaceClient::OnBeforeBrowse` cancels every
/// navigation off the page's own origin. So a television asked for a policy that
/// permits YouTube, was granted the header, and still could not fetch the player
/// API or start a single embed: `youtube_api_failed (the player script refused to
/// load)` with the whole internet reachable and nothing wrong with the page. A
/// header is a permission; this is the capability behind it, and the two now move
/// together so a resource can never be granted one without the other.
[[nodiscard]] constexpr bool AllowsRemoteContent(const WebPagePolicy aPolicy)
{
    return aPolicy == WebPagePolicy::Media;
}

/// The policy's name, for logs and tests.
[[nodiscard]] constexpr const char* Describe(const WebPagePolicy aPolicy)
{
    return aPolicy == WebPagePolicy::Media ? "media" : "strict";
}
} // namespace op77::WebUI
