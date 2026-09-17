#!/usr/bin/env python3
"""Does the installed Open77.archive carry the prop hosts the television needs?

Why this exists
---------------
A television is spawned as a *host entity*: `Props::Project` hands the engine a
depot path such as

    cyberm\\entities\\props\\open77_prop_electronics_tv_screen_16x9.ent

and the entity geometry is baked into that `.ent` at asset-build time. If the
installed `Open77.archive` does not contain it, the engine still *accepts* the
spawn request and registers it in the population system -- it simply never
materialises the entity, so no spawner event fires and the props entry never
adopts an entity. The client then reports

    television 1 (cinema.150ft): not drawing -- prop_not_projected

forever, the panel is never visible, and nothing in any log names the missing
asset. Diagnosed 2026-09-15: the Open77 launcher's modstack re-projected its own
CDN `Open77.archive` (blob `8484cbc2...`) over the local asset build, and that
generation predates the TV host entities.

Why a hash probe and not a string search
----------------------------------------
CP77 (KARK / `RDAR`) archives mask the path strings, so grepping an archive for
a path finds nothing even when the entry is present. Each entry's depot path is
kept in the clear only as its FNV1a64 hash, which is what this checks.

Usage
-----
    python tools/check-prop-hosts.py --archive "<game>\\archive\\pc\\mod\\Open77.archive"
    python tools/check-prop-hosts.py --archive ... --preset tv --require cyberm\\entities\\props\\x.ent
    python tools/check-prop-hosts.py --archive ... --quiet

Exit 0 when every required path is present, 1 when any is missing (each missing
path is named), 2 on a usage or I/O error.
"""

from __future__ import annotations

import argparse
import os
import sys

MASK = 0xFFFFFFFFFFFFFFFF
PRIME = 0x100000001B3
OFFSET_BASIS = 0xCBF29CE484222325

# The nine television hosts, taken from docs/generated/prop-hosts.json (alias ->
# entity). `electronics.tv.screen.16x9` is the one `cinema.150ft` resolves to,
# which is the set the media resource actually spawns.
PRESETS = {
    "tv": [
        r"cyberm\entities\props\open77_prop_electronics_tv_16x9.ent",
        r"cyberm\entities\props\open77_prop_electronics_tv_21x9.ent",
        r"cyberm\entities\props\open77_prop_electronics_tv_neokitsch_16x9.ent",
        r"cyberm\entities\props\open77_prop_electronics_tv_neokitsch_21x9.ent",
        r"cyberm\entities\props\open77_prop_electronics_tv_screen_16x9.ent",
        r"cyberm\entities\props\open77_prop_electronics_tv_screen_21x9.ent",
        r"cyberm\entities\props\open77_prop_electronics_tv_screen_neokitsch_16x9.ent",
        r"cyberm\entities\props\open77_prop_electronics_tv_screen_neokitsch_21x9.ent",
        r"cyberm\entities\props\open77_prop_electronics_tv_large.ent",
    ],
}


def fnv1a64(text: str) -> int:
    """FNV1a64 over the lowercased depot path, as CP77 archives store it."""
    value = OFFSET_BASIS
    for byte in text.lower().encode("ascii"):
        value ^= byte
        value = (value * PRIME) & MASK
    return value


def contains_path(data: bytes, depot_path: str) -> bool:
    digest = fnv1a64(depot_path)
    return digest.to_bytes(8, "little") in data or digest.to_bytes(8, "big") in data


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Check a deployed Open77.archive for required prop host entities.",
    )
    parser.add_argument("--archive", required=True, help="path to Open77.archive")
    parser.add_argument(
        "--preset",
        action="append",
        choices=sorted(PRESETS),
        default=None,
        help="named set of hosts to require (default: tv)",
    )
    parser.add_argument(
        "--require",
        action="append",
        default=[],
        metavar="DEPOT_PATH",
        help="an extra depot path that must be present (repeatable)",
    )
    parser.add_argument("--quiet", action="store_true", help="only report failures")
    args = parser.parse_args(argv)

    presets = args.preset or ["tv"]
    wanted: list[str] = []
    for name in presets:
        wanted.extend(PRESETS[name])
    wanted.extend(args.require)

    path = os.path.abspath(args.archive)
    if not os.path.isfile(path):
        print(f"check-prop-hosts: no archive at {path}", file=sys.stderr)
        return 2
    try:
        with open(path, "rb") as handle:
            data = handle.read()
    except OSError as exc:
        print(f"check-prop-hosts: cannot read {path}: {exc}", file=sys.stderr)
        return 2

    if len(data) < 8:
        print(f"check-prop-hosts: {path} is not an archive (too short)", file=sys.stderr)
        return 2
    if data[:4] not in (b"RDAR", b"KARK"):
        print(
            f"check-prop-hosts: {path} does not look like a CP77 archive "
            f"(magic {data[:4]!r}, expected b'RDAR' or b'KARK')",
            file=sys.stderr,
        )
        return 2

    missing = [depot for depot in wanted if not contains_path(data, depot)]
    if not args.quiet:
        print(
            f"check-prop-hosts: {os.path.basename(path)} ({len(data)} bytes), "
            f"{len(wanted) - len(missing)}/{len(wanted)} required host(s) present"
        )
        for depot in wanted:
            state = "ABSENT " if depot in missing else "present"
            print(f"  {state}  {depot}")

    if missing:
        print(
            f"check-prop-hosts: {len(missing)} required prop host(s) missing from {path}:"
        )
        for depot in missing:
            print(f"  - {depot}")
        print(
            "A spawn request for a missing host is accepted by the engine and then "
            "never materialised, so the prop stays invisible with no error. Re-deploy "
            "the asset archive (scripts/build-assets.ps1), or restore the build's "
            "artifacts/Release/Open77.archive over archive/pc/mod/Open77.archive."
        )
        return 1

    if not args.quiet:
        print("check-prop-hosts: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
