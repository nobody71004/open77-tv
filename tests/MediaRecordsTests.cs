using System.Text;
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
/// admin config already publishes, and pins the quad geometry -- parallel axes,
/// a zero size and an aspect ratio that does not match the surface the client
/// creates are each a picture that is wrong in a way a screenshot cannot
/// explain.
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

        var type = lua.GetGlobal("TestResult");
        Assert.AreEqual(LuaType.Table, type, "TestResult global missing after running records_test.lua");
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
                $"records_test.lua reported {failed} failures ({passed} passed): {failureText}");
            Assert.IsTrue(passed > 0, "records_test.lua reported no assertions");
        }
        finally
        {
            lua.Pop(1); // TestResult
        }
    }

    /// <summary>
    /// Every `Open77.…` name the television resource uses must be one the host
    /// actually publishes.
    /// </summary>
    /// <remarks>
    /// This is the check that was missing when the feature first ran. The server
    /// half called `Open77.players.ids()`, which does not exist — the enumerator
    /// is `Open77.players.all()` — and the only visible symptom was a television
    /// that spawned, reported success, and reached no client, because the throw
    /// happened inside the broadcast and the command wrapper printed it as a
    /// usage line. A name that is not on the list below is either a typo or a new
    /// dependency; both belong in a test failure here rather than in a silent
    /// no-op during a session.
    ///
    /// The list is transcribed from the two registration sites:
    ///   client  scripting/src/ResourceHost.cpp
    ///   server  server/src/Open77.Server.Scripting/Runtime/LuaResourceRuntime.cs
    /// </remarks>
    [TestMethod]
    public void TelevisionResourceUsesRealHostApiNames()
    {
        var allowed = new Dictionary<string, string[]>(StringComparer.Ordinal)
        {
            ["players"] = ["all", "position"],
            ["props"] = ["all", "create", "remove"],
            ["media"] = ["bind", "update", "unbind", "clear", "list"],
            ["character"] = ["position"],
        };

        var sources = new[]
        {
            "resources/system/open77_media/client/main.lua",
            "resources/system/open77_media/server/main.lua",
        };

        var calls = 0;
        foreach (var relative in sources)
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
                foreach (Match match in Regex.Matches(code, @"Open77\.([A-Za-z_]+)(?:\.([A-Za-z_]+))?"))
                {
                    var table = match.Groups[1].Value;
                    if (!match.Groups[2].Success)
                    {
                        // A bare table reference is the presence check itself.
                        Assert.IsTrue(allowed.ContainsKey(table),
                            $"{relative}:{lineNumber} references Open77.{table}, which the host does not publish");
                        continue;
                    }

                    var member = match.Groups[2].Value;
                    calls++;
                    Assert.IsTrue(allowed.TryGetValue(table, out var members) && Array.IndexOf(members, member) >= 0,
                        $"{relative}:{lineNumber} calls Open77.{table}.{member}, which is not a host function");
                }
            }
        }

        Assert.IsTrue(calls > 0, "no host calls found: the resource no longer matches this check");
    }
}
