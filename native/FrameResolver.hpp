#pragma once

// The host half of "can this link be shown on a television, and if not, what is
// the page it wraps?".
//
// `op77/WebUI/FramePolicy.hpp` holds every decision; this holds the one thing a
// pure header cannot do, which is ask the server. The split is deliberate: the
// answering is a network call with a timeout and a failure mode, and the judging
// is a function over strings, so the judging is what the game-free tests pin.
//
// Called from a worker thread (the page's `fetch` arrives on CEF's IO thread,
// and a probe that blocked that thread would stall the whole browser), so
// nothing here may touch CEF, the runtime or a surface's state.

#include <op77/WebUI/FramePolicy.hpp>

#include <string>
#include <vector>

namespace op77::WebHost::FrameResolver
{
/// One nested document the shell page names, and what the server said about it.
struct Candidate
{
    std::string url;
    std::string kind;
    bool frameable{};
    std::string violation;
};

struct Answer
{
    /// False when the PROBE failed -- a timeout, a malformed URL, no network.
    /// Distinct from `frameable`, which is the site's answer rather than ours:
    /// "we could not ask" and "it said no" are different sentences on a screen.
    bool ok{};
    std::string error;
    /// The document's URL after redirects. A link that redirects is normal and
    /// the frame must be given where it settled, not where it started.
    std::string documentUrl;
    /// Whether the document itself may be framed.
    bool frameable{};
    /// `""`, `x-frame-options` or `frame-ancestors`.
    std::string violation;
    /// What to put in the frame, when something can go in it. Equals
    /// `documentUrl` when the document itself is frameable.
    std::string best;
    /// `direct` when `best` is the document, `embed` when it is a nested
    /// application found inside it.
    std::string via;
    /// Every nested document considered, frameable or not, in page order.
    std::vector<Candidate> candidates;
};

/// How long the whole probe may take: the document, then at most three of its
/// nested documents. Long enough for a slow origin, short enough that the page
/// can say "checking" and still be believed.
inline constexpr int kDocumentTimeoutMilliseconds = 6000;
inline constexpr int kCandidateTimeoutMilliseconds = 3500;
/// How many nested documents are probed before giving up. A shell page wraps one
/// or two; a page with more than three candidates is an advertising grid.
inline constexpr std::size_t kMaximumProbedCandidates = 3;

/// Reads the URL's headers and, when framing is refused, its markup.
///
/// `aOurOrigin` is the surface's own origin, which is what a `frame-ancestors`
/// list has to name for the document to permit framing. It is passed in rather
/// than assumed because only the surface knows it, and a name nothing can guess
/// is the reason a site that permits framing says so on purpose.
[[nodiscard]] Answer Probe(const std::string& aUrl, const std::string& aOurOrigin,
                           int aDocumentTimeoutMilliseconds = kDocumentTimeoutMilliseconds,
                           int aCandidateTimeoutMilliseconds = kCandidateTimeoutMilliseconds);

/// The answer as the page's JSON, on one line, with every string escaped.
[[nodiscard]] std::string ToJson(const Answer& aAnswer);
} // namespace op77::WebHost::FrameResolver
