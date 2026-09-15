#!/usr/bin/env python3
"""Extract the television-relevant hunks from an open77-base working tree.

The television feature is a slice of a monorepo: the resource is standalone, but
it needs a handful of host-side seams (a native module, its Lua bindings, a
world-quad render style, a menu page). Those seam FILES also carry unrelated
work from the same branch, so a plain `git diff -- <file>` would drag it in.

This script keeps only the hunks that mention the feature and writes one .diff
per seam file. The result is a reading aid for review and re-application, not a
clean patch series: hunks are selected by keyword, so a hunk that touches both
TV and unrelated code is kept whole.

usage: python tools/extract-tv-patches.py <path-to-open77-base-worktree>
"""

import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
PATCHES = HERE / "patches"

# Every identifier this feature introduced or depends on. Deliberately specific:
# a bare `screen` would match unrelated viewport code elsewhere in these files.
FEATURE = re.compile(
    r"media|Media|television|Television|\btv\b|\bTV\b"
    r"|hudSuppressed|SurfaceTexture|screenQuad|screenSurface"
    r"|web_ui_page|WebUiPage|webUiPage"
    r"|Style::Screen|kMaximumWebSurfaces|MediaScreen"
    # The placement controls, the per-frame quad blending and the page policy.
    r"|placement|Placement|ScreenMotion|Nudge|MediaPlacement"
    r"|PagePolicy|AllowsRemoteContent|pagePolicy"
    r"|AudioSink|allowsRemoteContent"
    # A world screen's teardown: the session ending, the native registry no
    # longer holding a screen, and the reason a page is released.
    r"|materialised|destroyPage|session:ended"
    # Playing a link this build cannot decode: the host's own probe and stream
    # routes, the decoder tools that answer them, the launch switch that stages
    # those tools, and the frame grant that lets a pasted SITE into the page at
    # all. The last group is the vocabulary of the page's own verdict line --
    # `not_media` vs `not_stream_site` is exactly the distinction that decided
    # whether a website link was framed or thrown away.
    r"|TranscodePlan|Transcode|transcode|Decoder|decoder|ffprobe|ffmpeg"
    r"|media/probe|media/stream|decoderDir"
    r"|frame-src|embed_framed|embed_unverified|not_media|not_stream_site"
    # The second half of a pasted SITE: a shell that refuses framing has to be
    # resolved before it is framed, and the page may not navigate itself to an
    # advertisement. `popup` and `gesture` are the vocabulary that separates the
    # two, and the blocklist is the same decision written a third time.
    r"|AdBlock|adblock|blocklist|blockedHost|Framing|frameable|FindEmbeds"
    r"|EvaluateFraming|FrameResolver|frame_resolve|/op77/web/frame|violation"
    r"|popup|Popup|OnBeforePopup|gesture"
    # One pointer instead of two: the panel publishes where its quad landed so
    # the cursor the player aims with is the cursor the click lands under.
    r"|ScreenInput|PublishPointerOnScreen|screen input"
    # The scaled catalogue family: a 100 ft panel is a prop scale and a quad
    # written twice, and the stand-off that keeps it off the player.
    r"|cinema|Cinema|StandOff|standoff",
)

# The seam files. Three kinds are in here and each is treated differently:
#
#   * files that exist ONLY for this feature (MediaScreens, ScreenQuad,
#     ScreenMotion, PagePolicy, AudioSink) are copied verbatim into native/ or
#     tests/ by the extraction, so `git diff` on a fresh tree reports "no
#     changes" for the untracked ones and there is nothing to keep here.
#   * files this feature MODIFIED are what the keyword filter below is for.
#   * files that merely carry a TV hunk and a lot of unrelated work
#     (freeroam's menu, the Lua test runner, the admin prop-model list) are the
#     reason a plain `git diff` cannot be shipped as-is.
SEAMS = [
    "client/src/api/MediaScreens.hpp",
    "client/src/api/MediaScreens.cpp",
    "client/src/api/ScreenQuad.hpp",
    # The model aliases the catalogue is authored against: the game's own
    # televisions and monitors, added so a "Framed panel" is a television rather
    # than a painting.
    "client/src/api/Props.cpp",
    # The debug row that names the world-screen input session, which is the only
    # input state with no window to look at.
    "client/src/debug/bridge/ResourceCommands.cpp",
    "client/src/webui/ScreenMotion.hpp",
    "client/src/webui/WorldOverlay.hpp",
    "client/src/webui/WorldOverlay.cpp",
    "client/src/webui/WebUiService.hpp",
    "client/src/webui/WebUiService.cpp",
    "client/src/scripting/ClientResourceHost.cpp",
    "client/src/Plugin.cpp",
    "scripting/src/ResourceHost.cpp",
    "scripting/include/op77/Scripting/ResourceHost.hpp",
    "client/CMakeLists.txt",
    # The page policy and the host's own request gate are one decision in two
    # files: a directive in the header is a privilege the host has to grant.
    "webui/include/op77/WebUI/PagePolicy.hpp",
    "webui/include/op77/WebUI/Messages.hpp",
    "webui/include/op77/WebUI/Protocol.hpp",
    "webui/src/Messages.cpp",
    "webui/CMakeLists.txt",
    "webui/tests/WebCoreTests.cpp",
    # Audio out of a browser page into the game's mixer, and the remote-content
    # gate in front of it.
    "webhost/src/AudioSink.hpp",
    "webhost/src/AudioSink.cpp",
    "webhost/src/SurfaceClient.hpp",
    "webhost/src/SurfaceClient.cpp",
    "webhost/src/WebHostApp.cpp",
    "webhost/src/SelfTestApp.hpp",
    "webhost/src/SelfTestApp.cpp",
    "webhost/src/Main.cpp",
    "webhost/CMakeLists.txt",
    # Where the decoder tools are staged from and where the frame-grant probe
    # lives: two files whose only TV content is the launch switch and the
    # integration test that observes a real frame being allowed in.
    "webhost/src/HostRuntime.hpp",
    "webhost/src/HostRuntime.cpp",
    "webhost/tests/WebHostTests.cpp",
    # The menu tab, the placement controls, and the catalogue cross-check's
    # source of truth.
    "resources/gamemodes/freeroam/web/index.html",
    "resources/gamemodes/freeroam/web/app.js",
    "resources/gamemodes/freeroam/web/app.css",
    "resources/gamemodes/freeroam/client/main.lua",
    "resources/system/open77_admin/shared/config.lua",
    "tools/lua-test/run.lua",
    "server/tests/Open77.Server.Tests/Resources/MediaRecordsTests.cs",
    "server/tests/Open77.Server.Tests/Resources/MediaPlacementIntegrationTests.cs",
    # The wire between the page and the host's frame probe: three files, three
    # languages, and no way to run any of them without a game, so the strings
    # that join them are pinned where all three are on disk.
    "server/tests/Open77.Server.Tests/Resources/WebUiAssetTests.cs",
]


def run(root, *args):
    return subprocess.run(
        ["git", "-C", str(root), *args],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    ).stdout


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    root = Path(sys.argv[1]).resolve()
    PATCHES.mkdir(exist_ok=True)
    total = 0

    for seam in SEAMS:
        diff = run(root, "diff", "-U6", "--", seam)
        if not diff.strip():
            print(f"  {seam}: no changes in the tree")
            continue

        lines = diff.splitlines(keepends=True)
        header, hunks, current = [], [], None
        for line in lines:
            if line.startswith("@@"):
                if current is not None:
                    hunks.append(current)
                current = [line]
            elif current is None:
                header.append(line)
            else:
                current.append(line)
        if current is not None:
            hunks.append(current)

        kept = [h for h in hunks if FEATURE.search("".join(h))]
        if not kept:
            print(f"  {seam}: {len(hunks)} hunks, none TV-related")
            continue

        out = PATCHES / (seam.replace("/", "__") + ".diff")
        banner = (
            "# TV-relevant hunks only, extracted from a working tree by\n"
            "# tools/extract-tv-patches.py. Apply by hand: this is not a patch\n"
            "# series, and the surrounding file has other unreleased work in it.\n"
        )
        out.write_text(banner + "".join(header) + "".join("".join(h) for h in kept),
                       encoding="utf-8")
        total += len(kept)
        print(f"  {seam}: kept {len(kept)}/{len(hunks)} hunks -> {out.name}")

    print(f"\n{total} hunks written to {PATCHES}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
