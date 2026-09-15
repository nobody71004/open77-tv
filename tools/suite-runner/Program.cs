using System.Text;
using System.Text.RegularExpressions;
using KeraLua;

// The four open77_media suites, and the vendored alias snapshot they are checked
// against, run with no Lua interpreter installed.
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
// It also writes and checks that snapshot, because the snapshot is generated and
// a generated file that nobody regenerates is a lie with a date on it. The tool
// writes the bytes `tools/run-suite.py` writes -- including the CRLF endings
// `.gitattributes` pins with `* -text` -- so the two agree on one canonical form
// rather than each producing a subtly different file.
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
    /// The repository's own hunk of the file the snapshot is a copy of. This is the
    /// only statement of the alias list that lives in this repository, so it is
    /// what `--check-fixture` can hold the snapshot against without a checkout.
    /// </summary>
    private const string AdminConfigPatch = "patches/resources__system__open77_admin__shared__config.lua.diff";

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
        var refresh = false;
        var check = false;

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
                case "--refresh-fixture":
                    refresh = true;
                    break;
                case "--check-fixture":
                    check = true;
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

        if (refresh && check)
        {
            return Fail("--refresh-fixture and --check-fixture are different jobs; pass one");
        }

        try
        {
            var root = FindRepository(repo);
            if (refresh) return RefreshFixture(root, from);
            return check ? CheckFixture(root, from) : RunSuites(root);
        }
        catch (SuiteRunnerException error)
        {
            Console.Error.WriteLine($"open77_media: {error.Message}");
            return 2;
        }
    }

    private static int RunSuites(string repo)
    {
        var admin = ResolveAdminConfig(repo, checkout: null);

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
        using var lua = NewLua();
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

    // ---------------------------------------------------------------------
    // The vendored snapshot
    // ---------------------------------------------------------------------

    /// <summary>
    /// Rewrites <c>tests/fixtures/open77_admin-props-models.lua</c> from a live
    /// open77-base checkout. Needs `--from`: a refresh with no source would be a
    /// snapshot of the snapshot.
    /// </summary>
    private static int RefreshFixture(string repo, string? checkout)
    {
        if (checkout is null)
        {
            return Fail("--refresh-fixture needs --from <checkout>: the snapshot has to come from a live admin config");
        }

        var live = ResolveAdminConfig(repo, checkout);
        var models = ReadModels(live);
        var rendered = Render(models);
        var fixture = Path.Combine(repo, Fixture);

        var current = File.Exists(fixture) ? File.ReadAllBytes(fixture) : [];
        if (current.AsSpan().SequenceEqual(rendered))
        {
            Console.WriteLine($"{Fixture} already matches {checkout} at {models.Count} aliases; nothing written");
            return 0;
        }

        File.WriteAllBytes(fixture, rendered);
        Console.WriteLine($"wrote {Fixture} with {models.Count} aliases from {checkout}");
        return 0;
    }

    /// <summary>
    /// Checks the snapshot without needing a Lua interpreter, and without needing
    /// a checkout -- which is the only reason CI can run any part of this.
    ///
    /// With `--from <checkout>` it is the whole check: the snapshot must equal what
    /// the live admin config renders, byte for byte. Without one it checks the
    /// three things that are decidable from this repository alone:
    ///
    ///   * the file is exactly what the generator writes for the list it contains,
    ///     so a hand-edited alias, a declared count that no longer matches the list,
    ///     a lost line ending or a truncated file all fail here rather than in the
    ///     records suite's messages;
    ///   * no alias is listed twice;
    ///   * every alias this repository's own `open77_admin` hunk adds to that list
    ///     is in the snapshot.
    ///
    /// What it cannot see without a checkout is an alias that upstream grew and
    /// this repository's hunks do not carry -- that is what `--from` is for, and
    /// why the README says the snapshot can only ever be as fresh as its last
    /// regeneration.
    /// </summary>
    private static int CheckFixture(string repo, string? checkout)
    {
        var fixture = Path.Combine(repo, Fixture);
        if (!File.Exists(fixture))
        {
            throw new SuiteRunnerException($"{fixture} is missing. Vendor it with:\n" +
                "  dotnet run --project tools/suite-runner -- --refresh-fixture --from <checkout>");
        }

        var bytes = File.ReadAllBytes(fixture);
        var models = ReadModels(fixture);
        var problems = new List<string>();

        var canonical = Render(models);
        if (!bytes.AsSpan().SequenceEqual(canonical))
        {
            problems.Add(Describe(Relative(repo, fixture), bytes, canonical));
        }

        var duplicates = models
            .GroupBy(name => name, StringComparer.Ordinal)
            .Where(group => group.Count() > 1)
            .Select(group => group.Key)
            .ToList();
        if (duplicates.Count > 0)
        {
            problems.Add($"listed more than once: {string.Join(", ", duplicates)}");
        }

        var fromPatches = PatchAliases(repo);
        var missing = fromPatches.Where(alias => !models.Contains(alias, StringComparer.Ordinal)).ToList();
        if (missing.Count > 0)
        {
            problems.Add($"{missing.Count} alias(es) {AdminConfigPatch} adds are not in the snapshot: "
                + string.Join(", ", missing));
        }

        if (checkout is not null)
        {
            var live = ReadModels(ResolveAdminConfig(repo, checkout));
            if (!live.SequenceEqual(models, StringComparer.Ordinal))
            {
                problems.Add(DescribeLive(checkout, live, models));
            }
        }

        var scope = checkout is null
            ? $"{models.Count} aliases, {fromPatches.Count} alias(es) declared by {AdminConfigPatch}"
            : $"{models.Count} aliases, matching {checkout}";
        if (problems.Count == 0)
        {
            Console.WriteLine($"{Fixture}: OK ({scope})");
            return 0;
        }

        Console.Error.WriteLine($"{Fixture}: STALE ({scope})");
        foreach (var problem in problems) Console.Error.WriteLine($"  {problem}");
        Console.Error.WriteLine(checkout is null
            ? "  regenerate it from a tree that carries the work: --refresh-fixture --from <checkout>"
            : "  regenerate it: --refresh-fixture --from <checkout>");
        return 1;
    }

    /// <summary>
    /// The aliases this repository's own hunk of the admin config adds. The guard
    /// on the count is deliberate: if the patch is regenerated in a way this parse
    /// stops understanding, it says so instead of quietly checking nothing.
    /// </summary>
    private static List<string> PatchAliases(string repo)
    {
        var patch = Path.Combine(repo, AdminConfigPatch);
        if (!File.Exists(patch))
        {
            throw new SuiteRunnerException(
                $"{AdminConfigPatch} is missing; it is what tells the snapshot which aliases this repository adds");
        }

        var aliases = new List<string>();
        foreach (var line in File.ReadAllLines(patch))
        {
            // Additions only: context lines describe what the checkout already had,
            // and `+++`/`---` are the diff header.
            if (!line.StartsWith('+') || line.StartsWith("+++", StringComparison.Ordinal)) continue;
            if (line.TrimStart('+').TrimStart().StartsWith("--", StringComparison.Ordinal)) continue;

            foreach (Match match in LiteralPattern.Matches(line))
            {
                aliases.Add(match.Groups[1].Value);
            }
        }

        if (aliases.Count == 0)
        {
            throw new SuiteRunnerException(
                $"{AdminConfigPatch} yielded no aliases; the parse has stopped understanding it, which would make "
                + "this check pass by finding nothing");
        }

        return aliases;
    }

    /// <summary>
    /// Reads `Open77AdminConfig.props.models` out of a Lua file, in the order the
    /// list declares. Works on the live config and on the snapshot alike, because
    /// the snapshot is that same table written to disk.
    /// </summary>
    private static List<string> ReadModels(string path)
    {
        if (!File.Exists(path))
        {
            throw new SuiteRunnerException($"{path} does not exist");
        }

        using var lua = NewLua();
        Run(lua, path);

        if (lua.GetGlobal("Open77AdminConfig") != LuaType.Table)
        {
            throw new SuiteRunnerException($"{path} does not define Open77AdminConfig");
        }

        if (lua.GetField(-1, "props") != LuaType.Table)
        {
            throw new SuiteRunnerException($"{path}: Open77AdminConfig.props is missing or not a table");
        }

        if (lua.GetField(-1, "models") != LuaType.Table)
        {
            throw new SuiteRunnerException($"{path}: Open77AdminConfig.props.models is missing or not a table");
        }

        var count = lua.RawLen(-1);
        if (count == 0)
        {
            throw new SuiteRunnerException($"{path}: Open77AdminConfig.props.models is empty");
        }

        var models = new List<string>(count);
        for (var i = 1; i <= count; i++)
        {
            lua.RawGetInteger(-1, i);
            models.Add(lua.ToString(-1) ?? throw new SuiteRunnerException($"{path}: model {i} is not a string"));
            lua.Pop(1);
        }

        return models;
    }

    /// <summary>
    /// The file the generator writes, byte for byte -- the same header, order and
    /// CRLF endings `tools/run-suite.py` produces, because two tools writing one
    /// generated file have to agree on what it looks like or the freshness check
    /// becomes a diff of formatting.
    /// </summary>
    private static byte[] Render(List<string> models)
    {
        var text = new StringBuilder();
        text.Append("-- A SNAPSHOT of the prop-model aliases that `open77_admin` publishes as\r\n");
        text.Append("-- `Open77AdminConfig.props.models`.\r\n");
        text.Append("--\r\n");
        text.Append("-- The open77_media suite cross-checks every record's `model` against this\r\n");
        text.Append("-- list, so a television can never name a model that does not exist. This\r\n");
        text.Append("-- file exists only so that check can run without an open77-base checkout;\r\n");
        text.Append("-- regenerate it from a real tree with:\r\n");
        text.Append("--\r\n");
        text.Append("--   python tools/run-suite.py --refresh-fixture --from <checkout>\r\n");
        text.Append("--\r\n");
        text.Append($"-- {models.Count} aliases, vendored as-is; nothing here is hand-edited.\r\n");
        text.Append("Open77AdminConfig = {\r\n");
        text.Append("  props = {\r\n");
        text.Append("    models = {\r\n");
        foreach (var model in models)
        {
            text.Append($"      \"{model}\",\r\n");
        }
        text.Append("    },\r\n");
        text.Append("  },\r\n");
        text.Append("}\r\n");
        return Encoding.UTF8.GetBytes(text.ToString());
    }

    /// <summary>
    /// The first line that differs, with both sides, rather than a byte offset: the
    /// likely causes are one hand-edited alias, a count line left behind or a file
    /// that lost its CRLF endings, and each of those reads off the line.
    ///
    /// Line endings are reported as their own case and not as "line 1 differs".
    /// They are a real cause here -- `.gitattributes` pins `* -text`, so an editor
    /// or a script that normalises them rewrites every byte of the file -- and a
    /// comparison that split on CRLF would describe that as a whole-file
    /// difference, which helps nobody.
    /// </summary>
    private static string Describe(string path, byte[] actual, byte[] expected)
    {
        var got = Lines(actual);
        var want = Lines(expected);
        var sameContent = got.Length == want.Length
            && got.Zip(want).All(pair => pair.First.TrimEnd('\r') == pair.Second.TrimEnd('\r'));

        if (sameContent)
        {
            return $"{path} has {LineEnding(actual)} line endings where the generated form has CRLF, "
                + "so every line of the file is a different byte (`.gitattributes` pins `* -text`)";
        }

        for (var i = 0; i < Math.Max(got.Length, want.Length); i++)
        {
            var left = i < got.Length ? got[i].TrimEnd('\r') : "(no more lines)";
            var right = i < want.Length ? want[i].TrimEnd('\r') : "(no more lines)";
            if (!string.Equals(left, right, StringComparison.Ordinal))
            {
                return $"{path} is not the generated form: line {i + 1} is '{Small(left)}' but should be '{Small(right)}' "
                    + "(the alias count, the order and the line endings are all part of the generated form)";
            }
        }

        return $"{path} is not the generated form";
    }

    private static string Small(string line) =>
        line.Length <= 120 ? line : line[..117] + "...";

    private static string LineEnding(byte[] bytes)
    {
        var text = Encoding.UTF8.GetString(bytes);
        var crlf = text.Contains("\r\n", StringComparison.Ordinal);
        var bareLf = text.Replace("\r\n", string.Empty, StringComparison.Ordinal).Contains('\n');
        return crlf && bareLf ? "mixed" : crlf ? "CRLF" : bareLf ? "LF" : "no";
    }

    private static string DescribeLive(string checkout, List<string> live, List<string> snapshot)
    {
        var added = live.Except(snapshot, StringComparer.Ordinal).ToList();
        var removed = snapshot.Except(live, StringComparer.Ordinal).ToList();
        var parts = new List<string>();
        if (added.Count > 0) parts.Add($"{added.Count} alias(es) upstream that the snapshot lacks: {Preview(added)}");
        if (removed.Count > 0) parts.Add($"{removed.Count} alias(es) the snapshot has that upstream does not: {Preview(removed)}");
        if (parts.Count == 0) parts.Add("the same aliases in a different order");

        return $"{checkout} does not match the snapshot ({live.Count} aliases live, {snapshot.Count} vendored): "
            + string.Join("; ", parts);
    }

    private static string Preview(List<string> names) =>
        string.Join(", ", names.Take(5)) + (names.Count > 5 ? $", ... (+{names.Count - 5})" : string.Empty);

    /// <summary>Split on LF and keep any CR, so a CRLF/LF difference is visible as content-plus-ending rather than as one enormous line.</summary>
    private static string[] Lines(byte[] bytes) =>
        Encoding.UTF8.GetString(bytes).Split('\n', StringSplitOptions.None);

    private static string Relative(string repo, string path) =>
        Path.GetRelativePath(repo, path).Replace(Path.DirectorySeparatorChar, '/');

    /// <summary>A quoted Lua string literal on a patch line: `"electronics.tv.16x9"`.</summary>
    private static readonly Regex LiteralPattern = new("\"([^\"]+)\"", RegexOptions.Compiled);

    // ---------------------------------------------------------------------
    // Plumbing
    // ---------------------------------------------------------------------

    private static Lua NewLua() => new(true) { Encoding = Encoding.UTF8 };

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
                "  dotnet run --project tools/suite-runner -- --refresh-fixture --from <checkout>");
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
        writer.WriteLine("Runs the four open77_media Lua suites with no Lua interpreter installed,");
        writer.WriteLine("and maintains the vendored prop-model snapshot they are checked against.");
        writer.WriteLine();
        writer.WriteLine("usage:");
        writer.WriteLine("  dotnet run --project tools/suite-runner [-- --repo <path>] [--from <checkout>]");
        writer.WriteLine("  dotnet run --project tools/suite-runner -- --refresh-fixture --from <checkout>");
        writer.WriteLine("  dotnet run --project tools/suite-runner -- [--repo <path>] --check-fixture [--from <checkout>]");
        writer.WriteLine();
        writer.WriteLine("  --repo <path>     the checkout to run against (default: the nearest");
        writer.WriteLine("                    ancestor of the working directory holding open77_media)");
        writer.WriteLine("  --from <checkout> use the live open77_admin prop-model list from an");
        writer.WriteLine("                    open77-base checkout instead of the vendored snapshot");
        writer.WriteLine("  --refresh-fixture rewrite the snapshot from --from (needs one)");
        writer.WriteLine("  --check-fixture   fail if the snapshot is stale: not in generated form,");
        writer.WriteLine("                    listing an alias twice, missing an alias this repository's");
        writer.WriteLine("                    own open77_admin hunk adds, or (with --from) differing");
        writer.WriteLine("                    from the live list");
    }

    private sealed record Suite(string Name, string[] Preload, string File, bool NeedsRepoRoot);

    private sealed class SuiteRunnerException(string message) : Exception(message);
}
