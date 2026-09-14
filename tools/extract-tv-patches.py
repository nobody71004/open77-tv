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
    r"|Style::Screen|kMaximumWebSurfaces|MediaScreen",
)

SEAMS = [
    "client/src/api/MediaScreens.hpp",
    "client/src/api/MediaScreens.cpp",
    "client/src/webui/WorldOverlay.hpp",
    "client/src/webui/WorldOverlay.cpp",
    "client/src/webui/WebUiService.hpp",
    "client/src/webui/WebUiService.cpp",
    "client/src/scripting/ClientResourceHost.cpp",
    "scripting/src/ResourceHost.cpp",
    "scripting/include/op77/Scripting/ResourceHost.hpp",
    "client/CMakeLists.txt",
    "resources/gamemodes/freeroam/web/index.html",
    "resources/gamemodes/freeroam/web/app.js",
    "resources/gamemodes/freeroam/client/main.lua",
    "tools/lua-test/run.lua",
    "server/tests/Open77.Server.Tests/Resources/MediaRecordsTests.cs",
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
