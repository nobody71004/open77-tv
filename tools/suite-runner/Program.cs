using System.Text;
using KeraLua;

// The four open77_media suites, run with no Lua interpreter installed.
//
// The suites are pure Lua, and the monorepo runs them through a real `lua5.4`:
// `tools/lua-test/run.lua` and `tools/run-suite.py` both need one on PATH, and so
// does the fixture refresh. That is a dependency a Windows machine with only the
// .NET SDK does not have, which made "the suite passes" something a contributor
// could not check and CI could not check at all -- this repository had no CI.
//
// So this is the same four files executed through KeraLua, the Lua 5.4 binding
// (and the pinned version) that `Open77.Server.Tests` uses, compiled into a tool
// that runs wherever `dotnet` does. It is deliberately not a second definition of
// the suite: it preloads exactly what `tools/run-suite.py` preloads, in the same
// order, and reads the same `TestResult` table.
//
// One state per suite, which is what `tests/MediaRecordsTests.cs` does -- the
// monorepo's own C# harness. `tools/run-suite.py` shares a single state instead,
// and the difference is not academic: shared state can let a suite pass on a
// global a previous suite happened to leave behind, while isolation can only ever
// fail a suite for a dependency that really is missing. A gate should not be able
// to go green because of run order.

namespace Open77.Tv.SuiteRunner;

internal static class Program
{
    private const string Resource = "open77_media";
    private const string Manifest = "open77_media/open77.lua";

    private const string Records = "open77_media/shared/records.lua";
    private const string Placement = "open77_media/shared/placement.lua";
    private const string ServerConfig = "open77_media/server/config.lua";
    private const string ServerAdblock = "open77_media/server/adblock.lua";

    private const string RecordsSuite = "open77_media/tests/records_test.lua";
    private const string PlacementSuite = "open77_media/tests/placement_test.lua";
    private const string AdblockSuite = "open77_media/tests/adblock_test.lua";
    private const string ClientSuite = "open77_media/tests/client_test.lua";

    private const string Fixture = "tests/fixtures/open77_admin-props-models.lua";
    private const string AdminConfig = "resources/system/open77_admin/shared/config.lua";

    /// <summary>
    /// The four suites, in the order they are authored for.
    ///
    /// The catalogue suite comes first because it is the only one that needs the
    /// admin alias list. The ad-block suite is the server's half of a policy the
    /// browser host validates again, so it loads the two server modules it is the
    /// grammar of. The client suite comes LAST because it installs process-wide
    /// `Open77`, `CreateThread` and `Wait` stubs so the resource can be loaded
    /// outside the game; one state per suite already keeps those out of any other
    /// suite, but the order is the contract the monorepo's `run.lua` follows and
    /// nothing here should quietly disagree with it.
    /// </summary>
    private static readonly Suite[] Suites =
    [
        new("open77_media / records", [Records], RecordsSuite, NeedsRepoRoot: false),
        new("open77_media / placement", [Placement], PlacementSuite, NeedsRepoRoot: false),
        new("open77_media / adblock", [ServerConfig, ServerAdblock], AdblockSuite, NeedsRepoRoot: false),
        new("open77_media / client", [Records], ClientSuite, NeedsRepoRoot: true),
    ];

    private static int Main(string[] args)
    {
        string? repo = null;
        string? from = null;

        for (var i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--repo":
                    if (++i >= args.Length) return Fail("--repo needs a path");
                    repo = args[i];
                    break;
                case "--from":
                    if (++i >= args.Length) return Fail("--from needs a path to an open77-base checkout");
                    from = args[i];
                    break;
                case "-h":
                case "--help":
                    Usage(Console.Out);
                    return 0;
                default:
                    Console.Error.WriteLine($"unknown argument '{args[i]}'");
                    Usage(Console.Error);
                    return 2;
            }
        }

        try
        {
            return Run(repo, from);
        }
        catch (SuiteRunnerException error)
        {
            Console.Error.WriteLine($"open77_media: {error.Message}");
            return 2;
        }
    }

    private static int Run(string? repoPath, string? checkout)
    {
        var repo = FindRepository(repoPath);
        var admin = ResolveAdminConfig(repo, checkout);

        // The client suite resolves the resource through `OPEN77_REPO_ROOT` (the
        // monorepo layout, `resources/system/open77_media/...`). This repository is
        // not that layout, so one is staged in a temp directory rather than
        // teaching the suite a second way to find its own resource -- the suite is
        // a copy of the file that runs in the monorepo and has to stay one.
        var staged = StageResource(repo);
        try
        {
            Console.WriteLine($"staged the resource at {Path.Combine(staged, "resources", "system", Resource)} for the client suite");

            var failed = 0;
            var assertions = 0;
            foreach (var suite in Suites)
            {
                var (passed, bad, failures) = Execute(admin, suite, repo, staged);
                assertions += passed;
                Console.WriteLine($"{suite.Name}: {passed} passed, {bad} failed");
                foreach (var failure in failures) Console.WriteLine($"  FAIL: {failure}");
                if (bad > 0 || passed == 0) failed++;
            }

            if (failed == 0)
            {
                Console.WriteLine($"open77_media: PASS ({assertions} assertions)");
                return 0;
            }

            Console.Error.WriteLine($"open77_media: FAIL ({failed} of {Suites.Length} suites)");
            return 1;
        }
        finally
        {
            try
            {
                Directory.Delete(staged, recursive: true);
            }
            catch (IOException)
            {
                // Best effort, as the Python runner's `ignore_errors` is: a temp
                // directory that outlives the run must not turn a pass into a fail.
            }
        }
    }

    /// <summary>
    /// Runs one suite in its own Lua state and reads the <c>TestResult</c> table it
    /// publishes. A suite that raises, or that publishes nothing, is a failure of
    /// that suite rather than of the run, so the remaining suites still report.
    /// </summary>
    private static (int Passed, int Failed, List<string> Failures) Execute(
        string admin, Suite suite, string repo, string staged)
    {
        using var lua = new Lua(true) { Encoding = Encoding.UTF8 };
        try
        {
            Run(lua, admin);

            if (suite.NeedsRepoRoot)
            {
                lua.PushString(staged);
                lua.SetGlobal("OPEN77_REPO_ROOT");
            }

            foreach (var preload in suite.Preload) Run(lua, Path.Combine(repo, preload));
            Run(lua, Path.Combine(repo, suite.File));

            if (lua.GetGlobal("TestResult") != LuaType.Table)
            {
                return (0, 1, [$"{suite.Name} published no TestResult"]);
            }

            var passed = (int)ReadNumber(lua, "passed");
            var failed = (int)ReadNumber(lua, "failed");

            var failures = new List<string>();
            lua.GetField(-1, "failures");
            if (lua.Type(-1) == LuaType.Table)
            {
                var count = lua.RawLen(-1);
                for (var i = 1; i <= count; i++)
                {
                    lua.RawGetInteger(-1, i);
                    failures.Add(lua.ToString(-1) ?? "(unprintable failure)");
                    lua.Pop(1);
                }
            }

            lua.Pop(1); // failures
            lua.Pop(1); // TestResult
            return (passed, failed, failures);
        }
        catch (SuiteRunnerException error)
        {
            return (0, 1, [error.Message]);
        }
    }

    private static double ReadNumber(Lua lua, string field)
    {
        lua.GetField(-1, field);
        var value = lua.ToNumber(-1);
        lua.Pop(1);
        return value;
    }

    /// <summary>
    /// Loads and runs a chunk from disk, failing loudly with the path and the Lua
    /// message rather than returning a status nobody looks at.
    /// </summary>
    private static void Run(Lua lua, string path)
    {
        var status = lua.LoadBuffer(File.ReadAllBytes(path), "@" + Path.GetFileName(path), "t");
        if (status != LuaStatus.OK)
        {
            throw new SuiteRunnerException($"{path}: could not load: {lua.ToString(-1)}");
        }

        status = lua.PCall(0, 0, 0);
        if (status != LuaStatus.OK)
        {
            throw new SuiteRunnerException($"{path}: {lua.ToString(-1)}");
        }
    }

    /// <summary>
    /// Where `open77_media` lives: `--repo`, else the nearest ancestor of the
    /// working directory that holds the resource, so `dotnet run --project
    /// tools/suite-runner` works from the repository root and from anywhere under
    /// it.
    /// </summary>
    private static string FindRepository(string? explicitPath)
    {
        if (explicitPath is not null)
        {
            var full = Path.GetFullPath(explicitPath);
            if (!File.Exists(Path.Combine(full, Resource, "open77.lua")))
            {
                throw new SuiteRunnerException($"{full} does not hold {Manifest}");
            }
            return full;
        }

        for (var directory = new DirectoryInfo(Environment.CurrentDirectory);
             directory is not null;
             directory = directory.Parent)
        {
            if (File.Exists(Path.Combine(directory.FullName, Resource, "open77.lua")))
            {
                return directory.FullName;
            }
        }

        throw new SuiteRunnerException(
            $"no {Manifest} in {Environment.CurrentDirectory} or any parent; pass --repo <path>");
    }

    /// <summary>
    /// The prop-model alias list the catalogue suite cross-checks every record
    /// against. Two sources, exactly as `tools/run-suite.py` has them: `--from`
    /// preloads the live config from a real open77-base checkout and cannot drift,
    /// otherwise the vendored snapshot runs and only proves the records agree with
    /// the list as of the snapshot date.
    /// </summary>
    private static string ResolveAdminConfig(string repo, string? checkout)
    {
        if (checkout is not null)
        {
            var live = Path.Combine(Path.GetFullPath(checkout), AdminConfig);
            if (!File.Exists(live))
            {
                throw new SuiteRunnerException($"{live} does not exist; is --from pointing at a checkout?");
            }

            Console.WriteLine($"preloading the live admin config from {checkout}");
            return live;
        }

        var fixture = Path.Combine(repo, Fixture);
        if (!File.Exists(fixture))
        {
            throw new SuiteRunnerException(
                $"{fixture} is missing. Vendor it with:\n" +
                "  python tools/run-suite.py --refresh-fixture --from <checkout>");
        }

        Console.WriteLine("preloading the vendored admin-model snapshot (pass --from <checkout> for the live list)");
        return fixture;
    }

    private static string StageResource(string repo)
    {
        var staged = Path.Combine(Path.GetTempPath(), "open77-tv-" + Guid.NewGuid().ToString("N")[..8]);
        var layout = Path.Combine(staged, "resources", "system");
        Directory.CreateDirectory(layout);
        CopyDirectory(Path.Combine(repo, Resource), Path.Combine(layout, Resource));
        return staged;
    }

    private static void CopyDirectory(string source, string destination)
    {
        Directory.CreateDirectory(destination);
        foreach (var file in Directory.GetFiles(source))
        {
            File.Copy(file, Path.Combine(destination, Path.GetFileName(file)), overwrite: true);
        }

        foreach (var directory in Directory.GetDirectories(source))
        {
            CopyDirectory(directory, Path.Combine(destination, Path.GetFileName(directory)));
        }
    }

    private static int Fail(string message)
    {
        Console.Error.WriteLine(message);
        return 2;
    }

    private static void Usage(TextWriter writer)
    {
        writer.WriteLine("Runs the four open77_media Lua suites with no Lua interpreter installed.");
        writer.WriteLine();
        writer.WriteLine("usage:");
        writer.WriteLine("  dotnet run --project tools/suite-runner [-- --repo <path>] [--from <checkout>]");
        writer.WriteLine();
        writer.WriteLine("  --repo <path>     the checkout to run against (default: the nearest");
        writer.WriteLine("                    ancestor of the working directory holding open77_media)");
        writer.WriteLine("  --from <checkout> preload the live open77_admin prop-model list from an");
        writer.WriteLine("                    open77-base checkout instead of the vendored snapshot");
    }

    private sealed record Suite(string Name, string[] Preload, string File, bool NeedsRepoRoot);

    private sealed class SuiteRunnerException(string message) : Exception(message);
}
