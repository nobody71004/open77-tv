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
"left" is on a set that is not axis-aligned) and the client half (which screens a
client materialises, and what it releases when the session ends) are pure Lua
too, and all three run here.

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
SUITE = "open77_media/tests/records_test.lua"
PLACEMENT_SUITE = "open77_media/tests/placement_test.lua"
CLIENT_SUITE = "open77_media/tests/client_test.lua"
RESOURCE = "open77_media"

# The three pure suites, in the order they have to run.
#
# The catalogue suite comes first because it is the only one that needs the
# admin alias list, and the client suite comes LAST because it installs
# process-wide `Open77`, `CreateThread` and `Wait` stubs so the resource can be
# loaded outside the game -- a suite that ran after it would see those instead of
# nothing. `tools/lua-test/run.lua` in the monorepo makes the same point.
SUITES = [
    ("open77_media / records", [RECORDS], SUITE),
    ("open77_media / placement", [PLACEMENT], PLACEMENT_SUITE),
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


def main():
    parser = argparse.ArgumentParser(description="Run the open77_media suite.")
    parser.add_argument("--lua", help="Lua 5.4 interpreter to use")
    parser.add_argument("--from", dest="checkout",
                        help="an open77-base checkout to take the admin config from")
    parser.add_argument("--refresh-fixture", action="store_true",
                        help="rewrite the vendored snapshot and exit")
    args = parser.parse_args()

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

    # The client suite resolves the resource through `OPEN77_REPO_ROOT` (the
    # monorepo layout, `resources/system/open77_media/...`). This repository is
    # not that layout, so one is staged in a temp directory rather than teaching
    # the suite a second way to find its own resource -- the suite is a copy of
    # the file that runs in the monorepo and has to stay one.
    staged = Path(tempfile.mkdtemp(prefix="open77-tv-"))
    layout = staged / "resources" / "system"
    layout.mkdir(parents=True)
    shutil.copytree(HERE / RESOURCE, layout / RESOURCE)
    print(f"staged the resource at {layout / RESOURCE} for the client suite")

    body = [f"assert(loadfile({lua_literal(admin)}))()\n"]
    for name, preload, suite in SUITES:
        chunk = []
        # The client suite is the only one that needs to know where the resource
        # lives, and it reads that from a global rather than from the CWD.
        if suite == CLIENT_SUITE:
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
