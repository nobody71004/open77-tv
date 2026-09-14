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

usage:
  python tools/run-suite.py [--lua <interpreter>] [--from <checkout>]
  python tools/run-suite.py --refresh-fixture --from <checkout>
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
FIXTURE = HERE / "tests" / "fixtures" / "open77_admin-props-models.lua"
ADMIN_CONFIG = "resources/system/open77_admin/shared/config.lua"
RECORDS = "open77_media/shared/records.lua"
SUITE = "open77_media/tests/records_test.lua"

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

    body = (
        f"assert(loadfile({lua_literal(admin)}))()\n"
        f"assert(loadfile({lua_literal(HERE / RECORDS)}))()\n"
        f"assert(loadfile({lua_literal(HERE / SUITE)}))()\n"
        "local r = TestResult\n"
        "assert(type(r) == 'table', 'the suite published no TestResult')\n"
        "print(('assertions passed: %d, failed: %d'):format(r.passed, r.failed))\n"
        "for _, f in ipairs(r.failures or {}) do print('  FAIL: ' .. f) end\n"
        "os.exit(r.failed == 0 and r.passed > 0 and 0 or 1)\n"
    )
    code, out, err = run(lua, body)
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
