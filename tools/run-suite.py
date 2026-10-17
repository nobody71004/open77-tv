#!/usr/bin/env python3
"""Run the open77_media suite from this repository, with no monorepo needed.

The catalogue test cross-checks every `record.model` against the prop-model
alias list that `open77_admin` publishes as `Open77AdminConfig.props.models`,
so that a television can never be authored against a model that does not exist.
That cross-check is the point of the suite, so the list has to be available.

Two sources, in order of preference:

  * `--from <open77-base checkout>` preloads the live `open77_admin` config from
    a real tree. Use this when you have one; it is the only form that cannot
    drift.
  * otherwise `tests/fixtures/open77_admin-props-models.lua` is preloaded. It is
    a SNAPSHOT, so it proves the records agree with the list as of the snapshot
    date -- not with whatever a given server is running today.

`--refresh-fixture --from <checkout>` rewrites the snapshot from a real tree.

The catalogue suite is not the only one: the placement arithmetic (which way
"left" is on a set that is not axis-aligned), the playhead arithmetic (where a set
is in its programme, so two screens showing one link stay together), the
ad-blocklist grammar a server pushes (and the receipt that says whether the host
took it) and the client half (which screens a client materialises, and what it
releases when the session ends) are pure Lua too, and so is the server half of the
browser cinema (a record whose picture another resource provides, asked for at
spawn): all six run here.

usage:
  python tools/run-suite.py [--lua <interpreter>] [--from <checkout>]
  python tools/run-suite.py --refresh-fixture --from <checkout>
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
FIXTURE = HERE / "tests" / "fixtures" / "open77_admin-props-models.lua"
ADMIN_CONFIG = "resources/system/open77_admin/shared/config.lua"
RECORDS = "open77_media/shared/records.lua"
PLACEMENT = "open77_media/shared/placement.lua"
KEYS = "open77_media/shared/keys.lua"
CLOCK = "open77_media/shared/clock.lua"
ACL = "open77_media/shared/acl.lua"
SUITE = "open77_media/tests/records_test.lua"
PLACEMENT_SUITE = "open77_media/tests/placement_test.lua"
KEYS_SUITE = "open77_media/tests/keys_test.lua"
CLOCK_SUITE = "open77_media/tests/clock_test.lua"
ACL_SUITE = "open77_media/tests/acl_test.lua"
ADBLOCK_SUITE = "open77_media/tests/adblock_test.lua"
CLIENT_SUITE = "open77_media/tests/client_test.lua"
LINKED_SUITE = "open77_media/tests/linked_test.lua"
VENUESETS_SUITE = "open77_media/tests/venuesets_test.lua"
SERVER_CONFIG = "open77_media/server/config.lua"
SERVER_ADBLOCK = "open77_media/server/adblock.lua"
SERVER_VENUESETS = "open77_media/server/venuesets.lua"
RESOURCE = "open77_media"

# The pure suites, in the order they have to run.
#
# The catalogue suite comes first because it is the only one that needs the
# admin alias list. The ad-block suite is the server's half of a policy the
# browser host validates again -- so it loads the two server modules it is the
# grammar of. The key suite and the linked suite each load the whole server half
# against stubs (so they preload everything that half reads as it loads) and put
# every global they stubbed back when they are done. The client suite comes LAST because it installs process-wide
# `Open77`, `CreateThread` and `Wait` stubs so the resource can be loaded outside
# the game -- a suite that ran after it would see those instead of nothing.
# `tools/lua-test/run.lua` in the monorepo makes the same point.
SUITES = [
    ("open77_media / acl", [ACL], ACL_SUITE),
    ("open77_media / records", [RECORDS], SUITE),
    ("open77_media / placement", [PLACEMENT], PLACEMENT_SUITE),
    ("open77_media / clock", [CLOCK], CLOCK_SUITE),
    ("open77_media / adblock", [SERVER_CONFIG, SERVER_ADBLOCK], ADBLOCK_SUITE),
    ("open77_media / keys", [RECORDS, PLACEMENT, KEYS, CLOCK, SERVER_CONFIG, SERVER_ADBLOCK,
                             SERVER_VENUESETS], KEYS_SUITE),
    ("open77_media / linked", [RECORDS, PLACEMENT, KEYS, CLOCK, SERVER_CONFIG, SERVER_ADBLOCK,
                               SERVER_VENUESETS], LINKED_SUITE),
    ("open77_media / venuesets", [RECORDS, PLACEMENT, KEYS, CLOCK, SERVER_CONFIG, SERVER_ADBLOCK,
                                  SERVER_VENUESETS], VENUESETS_SUITE),
    ("open77_media / client", [RECORDS], CLIENT_SUITE),
]

# Dumps the live models list as the fixture body, so vendoring is a copy of what
# the server actually publishes rather than a transcription.
#
# The config path is substituted into the chunk rather than passed as an
# argument: after `lua -e <chunk>`, a following argument is taken as the SCRIPT
# to run, so `arg[1]` is never what you passed -- and the interpreter tries to
# open your path as a program.
DUMP = r"""
local path = %s
local source = assert(loadfile(path), path .. ': not found')
source()
local models = assert(Open77AdminConfig and Open77AdminConfig.props
    and Open77AdminConfig.props.models, 'Open77AdminConfig.props.models missing')
local out = {}
for _, name in ipairs(models) do out[#out + 1] = string.format('    "%%s",', name) end
io.write(table.concat(out, '\n'))
"""


def lua_literal(path):
    # Long-bracket string: no escape processing, so a Windows path survives as-is
    # once its separators are forward slashes.
    text = str(path).replace("\\", "/")
    assert "]]" not in text, "path contains ]], which a long bracket cannot hold"
    return f"[[{text}]]"


def find_lua(explicit):
    if explicit:
        return explicit
    for name in ("lua", "lua5.4", "lua54", "lua.exe", "luajit"):
        found = shutil.which(name)
        if found:
            return found
    sys.exit("no Lua interpreter found; pass --lua <path> (Lua 5.4 is expected)")


def run(lua, body):
    result = subprocess.run([lua, "-e", body],
                            capture_output=True, text=True, encoding="utf-8",
                            stdin=subprocess.DEVNULL)
    return result.returncode, result.stdout, result.stderr


def refresh_fixture(lua, checkout):
    config = Path(checkout) / ADMIN_CONFIG
    code, out, err = run(lua, DUMP % lua_literal(config))
    if code != 0:
        sys.exit(f"could not read {config}:\n{err}")
    names = [line.strip().strip(',"') for line in out.splitlines() if line.strip()]
    FIXTURE.parent.mkdir(parents=True, exist_ok=True)
    FIXTURE.write_text(
        "-- A SNAPSHOT of the prop-model aliases that `open77_admin` publishes as\n"
        "-- `Open77AdminConfig.props.models`.\n"
        "--\n"
        "-- The open77_media suite cross-checks every record's `model` against this\n"
        "-- list, so a television can never name a model that does not exist. This\n"
        "-- file exists only so that check can run without an open77-base checkout;\n"
        "-- regenerate it from a real tree with:\n"
        "--\n"
        f"--   python tools/run-suite.py --refresh-fixture --from <checkout>\n"
        "--\n"
        f"-- {len(names)} aliases, vendored as-is; nothing here is hand-edited.\n"
        "Open77AdminConfig = {\n"
        "  props = {\n"
        "    models = {\n"
        + "\n".join(f'      "{name}",' for name in names) + "\n"
        "    },\n"
        "  },\n"
        "}\n",
        encoding="utf-8")
    print(f"wrote {FIXTURE.relative_to(HERE)} with {len(names)} aliases")
    return 0


# ── the manifests ──────────────────────────────────────────────────────────────
# `Open77.acl` IS INSTALLED PER RESOURCE, and only for a resource whose manifest
# declares `acl.read`. open77_media asks it for the admin-only panel switch and
# the screen-lock (`shared/acl.lua`); opx_tvbrowser asks it for `/browser`'s gate
# (`server/main.lua`). Both shipped without the line, so the host handed them no
# `acl` table, the wrapped read raised, and every player -- the owner holding
# `command.media.lock` included -- was told the panel was admin-only. The failure
# was invisible from Lua because the question and the refusal both look correct
# from inside; only the manifest was wrong. A Lua suite cannot read a manifest,
# so the check lives here, where the suite is run.
ACL_READ = "acl.read"
ACL_ASKERS = [
    ("open77_media", HERE / "open77_media" / "open77.lua", "shared/acl.lua"),
    ("opx_tvbrowser", HERE / "shared-browser" / "opx_tvbrowser" / "open77.lua",
     "server/main.lua"),
]


def permissions_block(manifest):
    """The body of the manifest's `permissions { ... }` block, or None.

    Counted rather than matched, and with the `--` comments stripped before the
    count: the block is full of comment prose that may itself contain braces (the
    `acl.read` note in open77_media mentions `Open77.acl` and a `}`), and a count
    that reads those would close the block early and report a grant that is
    written below one as missing.
    """
    lines = manifest.read_text(encoding="utf-8").splitlines()
    depth = 0
    body = []
    inside = False
    for line in lines:
        code = line.split("--", 1)[0]
        if not inside:
            if code.strip().startswith("permissions") and "{" in code:
                inside = True
                depth = code.count("{") - code.count("}")
            continue
        depth += code.count("{") - code.count("}")
        body.append(code)
        if depth <= 0:
            break
    return "\n".join(body) if inside else None


def check_manifests():
    """Every manifest that asks the ACL declares the grant that installs it."""
    failures = []
    for name, manifest, asker in ACL_ASKERS:
        if not manifest.exists():
            failures.append(f"{name}: {manifest} is missing")
            continue
        block = permissions_block(manifest)
        if block is None:
            failures.append(f"{name}: open77.lua has no permissions block")
        elif f'"{ACL_READ}"' not in block:
            failures.append(
                f"{name}: asks the ACL in {asker} but does not declare "
                f"\"{ACL_READ}\" -- the host installs no Open77.acl, and every "
                f"player is refused")
    for line in failures:
        print(f"  FAIL: {line}", file=sys.stderr)
    if failures:
        print("manifests: FAIL", file=sys.stderr)
    else:
        print(f"manifests: {len(ACL_ASKERS)} checked, {ACL_READ} declared by every ACL asker")
    return not failures


def main():
    parser = argparse.ArgumentParser(description="Run the open77_media suite.")
    parser.add_argument("--lua", help="Lua 5.4 interpreter to use")
    parser.add_argument("--from", dest="checkout",
                        help="an open77-base checkout to take the admin config from")
    parser.add_argument("--refresh-fixture", action="store_true",
                        help="rewrite the vendored snapshot and exit")
    args = parser.parse_args()

    manifests_ok = check_manifests()
    if not manifests_ok:
        print("open77_media: FAIL", file=sys.stderr)
        return 1

    lua = find_lua(args.lua)

    if args.refresh_fixture:
        if not args.checkout:
            sys.exit("--refresh-fixture needs --from <checkout>")
        return refresh_fixture(lua, args.checkout)

    if args.checkout:
        admin = Path(args.checkout) / ADMIN_CONFIG
        if not admin.exists():
            sys.exit(f"{admin} does not exist; is --from pointing at a checkout?")
        print(f"preloading the live admin config from {args.checkout}")
    else:
        admin = FIXTURE
        if not admin.exists():
            sys.exit(f"{admin} is missing. Vendor it with:\n"
                     "  python tools/run-suite.py --refresh-fixture --from <checkout>")
        print("preloading the vendored admin-model snapshot "
              "(pass --from <checkout> for the live list)")

    # The client, key and linked suites resolve the resource through `OPEN77_REPO_ROOT` (the
    # monorepo layout, `resources/system/open77_media/...`). This repository is
    # not that layout, so one is staged in a temp directory rather than teaching
    # the suite a second way to find its own resource -- the suite is a copy of
    # the file that runs in the monorepo and has to stay one.
    staged = Path(tempfile.mkdtemp(prefix="open77-tv-"))
    layout = staged / "resources" / "system"
    layout.mkdir(parents=True)
    shutil.copytree(HERE / RESOURCE, layout / RESOURCE)
    print(f"staged the resource at {layout / RESOURCE} for the client, key and linked suites")

    body = [f"assert(loadfile({lua_literal(admin)}))()\n"]
    for name, preload, suite in SUITES:
        chunk = []
        # The client, key and linked suites are the ones that need to know where the
        # resource lives, and they read that from a global rather than from the CWD.
        if suite in (CLIENT_SUITE, LINKED_SUITE, KEYS_SUITE, VENUESETS_SUITE):
            chunk.append(f"OPEN77_REPO_ROOT = {lua_literal(staged)}\n")
        for path in preload:
            chunk.append(f"assert(loadfile({lua_literal(HERE / path)}))()\n")
        chunk.append("TestResult = nil\n")
        chunk.append(f"assert(loadfile({lua_literal(HERE / suite)}))()\n")
        chunk.append(
            "local r = TestResult\n"
            "assert(type(r) == 'table', 'the suite published no TestResult')\n"
            f"print(('{name}: %d passed, %d failed'):format(r.passed, r.failed))\n"
            "for _, f in ipairs(r.failures or {}) do print('  FAIL: ' .. f) end\n"
            "if r.failed > 0 or r.passed == 0 then failed = true end\n"
        )
        body.extend(chunk)
    body.append("os.exit(failed and 1 or 0)\n")

    code, out, err = run(lua, "failed = false\n" + "".join(body))
    shutil.rmtree(staged, ignore_errors=True)
    sys.stdout.write(out)
    if err.strip():
        sys.stderr.write(err)
    if code == 0:
        print("open77_media: PASS")
    else:
        print("open77_media: FAIL", file=sys.stderr)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
