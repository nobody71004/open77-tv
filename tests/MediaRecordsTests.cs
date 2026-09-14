using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using KeraLua;

namespace Open77.Server.Tests.Resources;

/// <summary>
/// Runs the open77_media television-catalogue suite inside the server test
/// process via KeraLua, the same engine `tools/lua-test/run.lua` uses standalone.
///
/// The catalogue is the one part of the television feature that cannot report
/// its own mistakes. A record naming a prop alias that does not exist builds a
/// television whose prop never spawns: the screen binds, projects, and is simply
/// never seen, and nothing in the engine, the server or the client says so. The
/// suite cross-checks every record's model against the props alias table the
/// admin config already publishes and against the asset build's host manifest,
/// and pins the quad geometry -- parallel axes, a zero size, an id whose shape
/// claim disagrees with the rectangle, and a screen that is not inside the
/// television it is attached to are each a picture that is wrong in a way a
/// screenshot cannot explain.
/// </summary>
[TestClass]
public sealed class MediaRecordsTests
{
    private static string RepoFile(string relative)
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null)
        {
            var candidate = Path.Combine(dir.FullName, relative.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(candidate))
            {
                return candidate;
            }
            dir = dir.Parent;
        }
        throw new FileNotFoundException(
            $"could not locate {relative} from {AppContext.BaseDirectory}");
    }

    private static void RunChunk(Lua lua, string path, string chunkName)
    {
        var source = File.ReadAllBytes(path);
        var status = lua.LoadBuffer(source, chunkName, "t");
        Assert.AreEqual(LuaStatus.OK, status, $"{chunkName}: load failed: {lua.ToString(-1)}");
        status = lua.PCall(0, 0, 0);
        Assert.AreEqual(LuaStatus.OK, status, $"{chunkName}: run failed: {lua.ToString(-1)}");
    }

    [TestMethod]
    public void TelevisionRecordSuitePasses()
    {
        using var lua = new Lua(true) { Encoding = Encoding.UTF8 };

        // The alias table the check runs against, then the catalogue. Manifest
        // order is not load order here -- both files only publish globals -- but
        // the props table must exist before the suite reads it.
        RunChunk(lua, RepoFile("resources/system/open77_admin/shared/config.lua"),
            "@open77_admin/shared/config.lua");
        RunChunk(lua, RepoFile("resources/system/open77_media/shared/records.lua"),
            "@open77_media/shared/records.lua");
        RunChunk(lua, RepoFile("resources/system/open77_media/tests/records_test.lua"),
            "@open77_media/tests/records_test.lua");

        EnsureTestPassed(lua, "records_test.lua");
    }

    /// <summary>
    /// The placement arithmetic behind the menu's nudge and turn controls.
    /// </summary>
    /// <remarks>
    /// A nudge that moves a set the wrong way is not a crash, a log line or an
    /// exception: it is a cabinet that slides right while the operator holds
    /// LEFT, on a set that may be kilometres away, and the only instrument that
    /// can see it is a person standing in front of it. So the axis convention
    /// the whole feature depends on -- yaw 0 faces +Y and grows
    /// counter-clockwise, the same one `facingPlacement` places a set with -- is
    /// pinned here rather than trusted.
    /// </remarks>
    [TestMethod]
    public void TelevisionPlacementSuitePasses()
    {
        using var lua = new Lua(true) { Encoding = Encoding.UTF8 };

        RunChunk(lua, RepoFile("resources/system/open77_media/shared/placement.lua"),
            "@open77_media/shared/placement.lua");
        RunChunk(lua, RepoFile("resources/system/open77_media/tests/placement_test.lua"),
            "@open77_media/tests/placement_test.lua");

        EnsureTestPassed(lua, "placement_test.lua");
    }

    /// <summary>
    /// The client half of the television resource, run against a stub of the
    /// native surface whose shape is the native's own.
    /// </summary>
    /// <remarks>
    /// A spawned television used to draw nothing at all while every other part
    /// of the feature reported success -- the server said `OK television 1
    /// created`, the prop was in the world with the right mesh, and the
    /// catalogue was consistent. The client's one-second selection thread threw
    /// on its first line instead, because `Open77.character.position()` returns
    /// three numbers and the client indexed `.x` on the first one.
    ///
    /// The second failure of the same kind is pinned here too: the catalogue and
    /// the server both write a quad as positional arrays (`{ 0, 0.1154, 0.42 }`)
    /// while the native reads x/y/z BY NAME and refuses the rest with
    /// `invalid_offset`. The stub below restates that rule rather than accepting
    /// any table, because a permissive stub is exactly what let a broken payload
    /// look healthy in every test while every screen in the game logged
    /// `bind failed: invalid_offset` once a second.
    ///
    /// Run by `tools/lua-test/run.lua` standalone and by this method inside the
    /// server test process: the suite is one file, both harnesses load it.
    /// </remarks>
    [TestMethod]
    public void TelevisionClientSuitePasses()
    {
        const string clientRelative = "resources/system/open77_media/client/main.lua";
        var clientPath = RepoFile(clientRelative);
        // The suite loads the file under test itself, so it is told the root
        // rather than being left to the process's working directory -- the same
        // convention `director_test.lua` uses with `DIRECTOR_PATH`.
        var root = clientPath[..^clientRelative.Length]
            .TrimEnd(Path.DirectorySeparatorChar);

        using var lua = new Lua(true) { Encoding = Encoding.UTF8 };
        lua.PushString(root);
        lua.SetGlobal("OPEN77_REPO_ROOT");

        // The alias table and the catalogue, for `Open77MediaSurfaceFor`, then
        // the suite. The suite installs its own `Open77`/`WebUI`/thread stubs.
        RunChunk(lua, RepoFile("resources/system/open77_admin/shared/config.lua"),
            "@open77_admin/shared/config.lua");
        RunChunk(lua, RepoFile("resources/system/open77_media/shared/records.lua"),
            "@open77_media/shared/records.lua");
        RunChunk(lua, RepoFile("resources/system/open77_media/tests/client_test.lua"),
            "@open77_media/tests/client_test.lua");

        EnsureTestPassed(lua, "client_test.lua");
    }

    /// <summary>
    /// Reads the `TestResult` table a suite publishes and fails with the
    /// assertions it recorded, rather than with the first one.
    /// </summary>
    private static void EnsureTestPassed(Lua lua, string suiteName)
    {
        var type = lua.GetGlobal("TestResult");
        Assert.AreEqual(LuaType.Table, type, $"TestResult global missing after running {suiteName}");
        try
        {
            lua.GetField(-1, "failed");
            var failed = (int)lua.ToNumber(-1);
            lua.Pop(1);

            lua.GetField(-1, "passed");
            var passed = (int)lua.ToNumber(-1);
            lua.Pop(1);

            lua.GetField(-1, "failures");
            string? failureText = null;
            if (lua.IsTable(-1))
            {
                var sb = new StringBuilder();
                var length = (int)lua.RawLen(-1);
                for (var i = 1; i <= length; i++)
                {
                    lua.RawGetInteger(-1, i);
                    sb.Append(lua.ToString(-1)).Append("; ");
                    lua.Pop(1);
                }
                failureText = sb.ToString();
            }
            lua.Pop(1);

            Assert.AreEqual(0, failed,
                $"{suiteName} reported {failed} failures ({passed} passed): {failureText}");
            Assert.IsTrue(passed > 0, $"{suiteName} reported no assertions");
        }
        finally
        {
            lua.Pop(1); // TestResult
        }
    }

    /// <summary>
    /// Every prop the television catalogue names must have a generated host
    /// entity on disk.
    /// </summary>
    /// <remarks>
    /// This is the check that was missing, and it cost a whole afternoon of "the
    /// records are wrong". A catalogue can be perfectly consistent -- every alias
    /// spelled correctly and present in `Props.cpp` -- and still be unable to
    /// build a single television, because an alias only becomes a spawnable prop
    /// once the asset build has emitted `open77_prop_&lt;slug&gt;.ent` for it.
    ///
    /// The failure mode is silent in the worst way: the prop creation succeeds by
    /// falling back to the marker mesh, so a record named in the catalogue
    /// spawns a floor decal with a web page stretched over it rather than
    /// reporting that the alias has no geometry.
    ///
    /// `docs/generated/prop-hosts.json` is the asset build's own manifest, so
    /// this asserts agreement between the two generated things rather than
    /// duplicating a list by hand. Adding an alias to `Props.cpp` and a record
    /// that uses it now fails here until `scripts/build-prop-hosts.ps1` (or a
    /// full `build-assets.ps1`) has been run -- which is the correct order and
    /// the one worth enforcing.
    /// </remarks>
    [TestMethod]
    public void EveryCataloguePropHasAGeneratedHost()
    {
        using var lua = new Lua(true) { Encoding = Encoding.UTF8 };
        RunChunk(lua, RepoFile("resources/system/open77_media/shared/records.lua"),
            "@open77_media/shared/records.lua");

        var models = new List<string>();
        lua.GetGlobal("Open77MediaCatalogue");
        Assert.AreEqual(LuaType.Function, lua.Type(-1),
            "Open77MediaCatalogue is not a function");
        Assert.AreEqual(LuaStatus.OK, lua.PCall(0, 1, 0),
            $"Open77MediaCatalogue failed: {lua.ToString(-1)}");
        Assert.AreEqual(LuaType.Table, lua.Type(-1), "the catalogue is not a table");
        var count = (int)lua.RawLen(-1);
        for (var i = 1; i <= count; i++)
        {
            lua.RawGetInteger(-1, i);
            lua.GetField(-1, "model");
            models.Add(lua.ToString(-1));
            lua.Pop(2);
        }
        lua.Pop(1);

        Assert.IsTrue(models.Count > 0, "the catalogue is empty");

        using var manifest = JsonDocument.Parse(
            File.ReadAllText(RepoFile("docs/generated/prop-hosts.json")));
        var hosted = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var entry in manifest.RootElement.EnumerateArray())
        {
            var alias = entry.GetProperty("alias").GetString() ?? "";
            var status = entry.TryGetProperty("status", out var s) ? s.GetString() : null;
            if (!hosted.ContainsKey(alias))
            {
                hosted[alias] = status ?? "";
            }
        }
        Assert.IsTrue(hosted.Count > 0, "the prop-host manifest is empty");

        var unhosted = models
            .Where(model => !hosted.TryGetValue(model, out var status) || status != "ok")
            .Distinct(StringComparer.Ordinal)
            .OrderBy(model => model, StringComparer.Ordinal)
            .ToArray();

        Assert.AreEqual(0, unhosted.Length,
            "the catalogue names props with no generated host entity (run scripts/build-prop-hosts.ps1): "
            + string.Join(", ", unhosted));
    }

    /// <summary>
    /// Every `Open77.…` name the television resource uses must be one the runtime
    /// the file runs in actually publishes — and the client and the server are
    /// two different surfaces.
    /// </summary>
    /// <remarks>
    /// This is the check that was missing when the feature first ran. The server
    /// half called `Open77.players.ids()`, which does not exist — the enumerator
    /// is `Open77.players.all()` — and the only visible symptom was a television
    /// that spawned, reported success, and reached no client, because the throw
    /// happened inside the broadcast and the command wrapper printed it as a
    /// usage line.
    ///
    /// The first version of this test carried a hand-written transcription of the
    /// two registration sites, and it went stale the way such a list always does:
    /// it named `props` as create/remove/all only, so the placement feature's
    /// `Open77.props.setTransform` — a real binding, registered on both sides —
    /// failed here. A check that reports a working call as a bug is worse than no
    /// check, because the next person deletes it.
    ///
    /// So the allowed surface is now DERIVED from the registration sites instead
    /// of transcribed: the client's table build in `scripting/src/ResourceHost.cpp`
    /// and the server's Lua prelude `Open77 = { … }` in
    /// `LuaResourceRuntime.cs`. A binding added to the host is allowed here the
    /// moment it exists, and a name that is not in the host fails here rather
    /// than as a silent no-op in a session. The two parsers assert on their own
    /// output (table counts, and the names this resource depends on) so a parser
    /// that stops matching fails loudly instead of passing everything.
    ///
    /// It is deliberately per-runtime. The two hosts overlap but are not equal --
    /// `Open77.props.nearest` is client-only, `Open77.io.writeJson` is server-only
    /// -- and the bug this test was written for was exactly a name used on the
    /// side that does not have it.
    /// </remarks>
    [TestMethod]
    public void TelevisionResourceUsesRealHostApiNames()
    {
        var client = ClientHostSurface();
        var server = ServerHostSurface();

        // A parser that silently stopped matching would make this test vacuous,
        // which is the one failure it cannot report on its own.
        Assert.IsTrue(client.Count > 20, $"the client host surface parsed as {client.Count} tables");
        Assert.IsTrue(server.Count > 20, $"the server runtime surface parsed as {server.Count} tables");

        static void Require(Dictionary<string, HashSet<string>> surface, string table, params string[] members)
        {
            Assert.IsTrue(surface.TryGetValue(table, out var found),
                $"Open77.{table} is not published by the host this resource runs in");
            foreach (var member in members)
            {
                Assert.IsTrue(found!.Contains(member), $"Open77.{table}.{member} is missing from the host");
            }
        }

        // The calls this resource makes, asserted against the parser's own
        // output: if any of these ever leaves the host, the failure above is the
        // one worth reading before the per-line failures below.
        Require(client, "media", "bind", "update", "unbind", "clear", "list");
        Require(client, "character", "position");
        Require(client, "props", "create", "remove", "setTransform");
        Require(server, "players", "all", "position");
        Require(server, "props", "create", "remove", "all", "setTransform");

        var sources = new (string Relative, Dictionary<string, HashSet<string>> Surface)[]
        {
            ("resources/system/open77_media/client/main.lua", client),
            ("resources/system/open77_media/server/main.lua", server),
        };

        var calls = 0;
        foreach (var (relative, surface) in sources)
        {
            var path = RepoFile(relative);
            var lineNumber = 0;
            foreach (var line in File.ReadAllLines(path))
            {
                lineNumber++;
                // Comments carry examples and prose; only code is a call.
                var code = line;
                var comment = code.IndexOf("--", StringComparison.Ordinal);
                if (comment >= 0) code = code[..comment];
                foreach (Match match in Regex.Matches(code, @"Open77\.([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*)"))
                {
                    var segments = match.Groups[1].Value.Split('.');
                    var table = segments[0];
                    var walk = table;
                    var resolved = true;
                    for (var index = 1; index < segments.Length; index++)
                    {
                        var next = walk + "." + segments[index];
                        var isLast = index == segments.Length - 1;
                        if (isLast)
                        {
                            // The last segment is read off the table that owns it.
                            resolved = surface.TryGetValue(walk, out var members) && members.Contains(segments[index]);
                            walk = next;
                        }
                        else if (surface.ContainsKey(next))
                        {
                            // An intermediate segment has to be a table of its own
                            // (`Open77.props.local_.create`).
                            walk = next;
                        }
                        else
                        {
                            resolved = false;
                            walk = next;
                        }
                    }

                    calls++;
                    Assert.IsTrue(resolved,
                        $"{relative}:{lineNumber} uses Open77.{match.Groups[1].Value}, which the " +
                        (relative.Contains("/client/") ? "client host" : "server runtime") + " does not publish");
                }
            }
        }

        Assert.IsTrue(calls > 0, "no host calls found: the resource no longer matches this check");
    }

    /// <summary>
    /// The tables and members the client host publishes, read out of the native
    /// registration itself.
    /// </summary>
    /// <remarks>
    /// The build is one flat sequence: `lua_newtable(state); // Open77.<path>`
    /// opens a table, `Set…Function(state, "member", …)` adds a member, and
    /// `lua_setfield(state, -2, "<leaf>")` names it in its parent. The comment on
    /// the open line is what carries the path, including for nested tables
    /// (`// Open77.props.local_`), and it is already load-bearing documentation —
    /// the file's own note about `Open77.world` being registered twice depends on
    /// it — so this parser is reading a convention the native keeps for its own
    /// readers.
    /// </remarks>
    private static Dictionary<string, HashSet<string>> ClientHostSurface()
    {
        var lines = File.ReadAllLines(RepoFile("scripting/src/ResourceHost.cpp"))
            .Select(line => line.TrimEnd('\r', ' ', '\t')).ToArray();

        var open = Array.FindIndex(lines, line => line.Trim() == "lua_newtable(state); // Open77");
        Assert.IsTrue(open >= 0,
            "scripting/src/ResourceHost.cpp no longer marks the Open77 table build with `lua_newtable(state); // Open77`");
        var end = Array.FindIndex(lines, open,
            line => line.Contains("lua_setglobal(state, \"Open77\")"));
        Assert.IsTrue(end > open,
            "scripting/src/ResourceHost.cpp no longer installs Open77 with `lua_setglobal(state, \"Open77\")`");

        var opens = new Regex(@"lua_newtable\(state\)\s*;\s*//\s*Open77\.([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*)");
        var blocks = new List<(string Path, int Open, int Close)>();
        for (var index = open + 1; index < end; index++)
        {
            var marker = opens.Match(lines[index]);
            if (!marker.Success) continue;
            var path = marker.Groups[1].Value;
            var leaf = path[(path.LastIndexOf('.') + 1)..];
            var closing = new Regex($@"lua_setfield\(state, -2, ""{Regex.Escape(leaf)}""\);");
            var close = Array.FindIndex(lines, index + 1, line => closing.IsMatch(line));
            Assert.IsTrue(close > index && close < end,
                $"Open77.{path} opens at ResourceHost.cpp:{index + 1} and never closes with " +
                $"lua_setfield(state, -2, \"{leaf}\")");
            blocks.Add((path, index, close));
        }

        var setFunction = new Regex(@"Set[A-Za-z]*Function\(state, ""([A-Za-z_][A-Za-z0-9_]*)""");
        var surface = new Dictionary<string, HashSet<string>>(StringComparer.Ordinal);
        foreach (var (path, blockOpen, blockClose) in blocks)
        {
            if (!surface.TryGetValue(path, out var members))
            {
                surface[path] = members = new HashSet<string>(StringComparer.Ordinal);
            }
            for (var index = blockOpen + 1; index < blockClose; index++)
            {
                // A nested table's members belong to the nested table.
                var nested = blocks.Any(other => other.Path != path &&
                    other.Open <= index && index < other.Close);
                if (nested) continue;
                var call = setFunction.Match(lines[index]);
                if (call.Success) members.Add(call.Groups[1].Value);
            }
        }
        return surface;
    }

    /// <summary>
    /// The tables and members the server runtime publishes, read out of its Lua
    /// prelude.
    /// </summary>
    /// <remarks>
    /// The prelude is one Lua table literal written to a strict indent: a table at
    /// two spaces, its members at four, a nested table's members at six. That
    /// regularity is what lets a member line be attributed to its owner, and the
    /// indent test is `== parent + 2` rather than `&gt;` on purpose — a function
    /// body inside a member sits deeper, and `local x = …`/`scale = scale or {}`
    /// lines in there are statements, not members. Anything that does not answer
    /// exactly loses at the "this parser found N tables" assertion above.
    /// </remarks>
    private static Dictionary<string, HashSet<string>> ServerHostSurface()
    {
        var lines = File.ReadAllLines(RepoFile("server/src/Open77.Server.Scripting/Runtime/LuaResourceRuntime.cs"))
            .Select(line => line.TrimEnd('\r')).ToArray();

        var open = Array.FindIndex(lines, line => line.Trim() == "Open77 = {");
        Assert.IsTrue(open >= 0,
            "LuaResourceRuntime.cs no longer opens its Open77 prelude with `Open77 = {` at column zero");
        var end = Array.FindIndex(lines, open + 1, line => line.Length > 0 && line[0] == '}');
        Assert.IsTrue(end > open,
            "LuaResourceRuntime.cs no longer closes its Open77 prelude with `}` at column zero");

        var table = new Regex(@"^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*\{");
        var member = new Regex(@"^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*[^=]");
        var surface = new Dictionary<string, HashSet<string>>(StringComparer.Ordinal);
        var stack = new List<(int Indent, string Path)>();
        for (var index = open + 1; index < end; index++)
        {
            var line = lines[index];
            if (line.Trim().Length == 0 || line.TrimStart().StartsWith("--", StringComparison.Ordinal)) continue;
            var indent = line.Length - line.TrimStart().Length;
            var code = line.TrimStart();
            while (stack.Count > 0 && indent <= stack[^1].Indent) stack.RemoveAt(stack.Count - 1);

            var opens = table.Match(code);
            if (opens.Success)
            {
                var path = stack.Count > 0 && indent == stack[^1].Indent + 2
                    ? stack[^1].Path + "." + opens.Groups[1].Value
                    : opens.Groups[1].Value;
                if (!surface.ContainsKey(path)) surface[path] = new HashSet<string>(StringComparer.Ordinal);
                stack.Add((indent, path));
                continue;
            }

            var isMember = member.Match(code);
            if (isMember.Success && stack.Count > 0 && indent == stack[^1].Indent + 2)
            {
                surface[stack[^1].Path].Add(isMember.Groups[1].Value);
            }
        }
        return surface;
    }
}
