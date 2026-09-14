using Open77.Server.Core.Loot;
using Open77.Server.Core.Props;
using Open77.Server.Core.Routing;
using Open77.Server.Scripting.Hosting;

namespace Open77.Server.Tests.Resources;

/// <summary>
/// The placement feature — nudging and turning a spawned television — run
/// through the server's own resource host: the real Lua, the real prop registry,
/// the real command dispatch, the real `Open77.props.setTransform` binding.
/// </summary>
/// <remarks>
/// Why this exists as an integration test rather than only as arithmetic.
/// `shared/placement.lua` is pure and pinned by its own suite, but purity is
/// exactly what that suite cannot see: it proves the direction table is
/// self-consistent, not that anything that direction reaches ever moves an
/// object. Between the arithmetic and the cabinet there are four seams, and each
/// one fails silently in a different way —
///
///   * the command may never be registered (a name typo answers "unknown command")
///   * `Open77.props.setTransform` may not exist on this runtime (the throw lands
///     in the command wrapper and prints as a usage line, which reads like a
///     mistake the operator made)
///   * the binding may refuse the prop because the resource does not own it, or
///     because the patch is malformed (a refusal, not an error)
///   * the resource may report success and update its own record while the
///     registry holds the old transform, so the menu and the world disagree
///
/// So this asserts on the registry itself — the object the replication layer
/// reads — and on the resource's own report of where the set is, at the same
/// time, after each request. The negative case at the end is the control: a
/// refused direction must leave the prop exactly where it was, which is what
/// makes the positive assertions mean something.
/// </remarks>
[TestClass]
public sealed class MediaPlacementIntegrationTests
{
    private static string FindRepositoryRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null)
        {
            if (Directory.Exists(Path.Combine(dir.FullName, "resources", "system", "open77_media")))
            {
                return dir.FullName;
            }
            dir = dir.Parent;
        }
        throw new DirectoryNotFoundException(
            $"could not locate the repository root from {AppContext.BaseDirectory}");
    }

    private sealed class Harness : IDisposable
    {
        public ServerResourceHost Host { get; }
        public PropAuthorityService Props { get; }
        public List<string> Log { get; } = [];

        public Harness()
        {
            var buckets = new RoutingBucketService();
            buckets.RegisterPlayer(7);
            Props = new PropAuthorityService(buckets);
            Host = new ServerResourceHost(
                Path.Combine(FindRepositoryRoot(), "resources"),
                (level, resource, message) => Log.Add($"{level}|{resource}|{message}"),
                buckets,
                props: Props)
            {
                PlayerNameResolver = static player => player == 7 ? "Operator" : null,
                // Where the operator is standing. `media.spawn` places the set in
                // front of them, so this is the position every expectation below
                // is measured from.
                PlayerPositionResolver = static player => player == 7
                    ? new LootPlayerPosition(100, 200, 5, 0) : null,
            };
            Host.Discover();
            Host.Start("open77_media");
            Host.Tick(1_000);
        }

        private double _clock = 1_000.0;

        /// <summary>A command the console would accept, run to completion.</summary>
        public void Run(params string[] tokens)
        {
            var dispatch = Host.ExecuteCommand(tokens, 7, allowRestricted: true);
            Assert.AreEqual(ResourceCommandStatus.Queued, dispatch.Status,
                $"{string.Join(' ', tokens)} was not queued ({dispatch.Status}: {dispatch.Message})");
            Host.Tick(_clock += 1_000.0);
        }

        public ServerProp TheTelevision()
        {
            var props = Props.Snapshot();
            Assert.AreEqual(1, props.Count,
                "expected exactly one prop in the registry; the resource said: " + string.Join(" | ", Log));
            return props[0];
        }

        public bool Logged(string fragment) =>
            Log.Any(line => line.Contains(fragment, StringComparison.Ordinal));

        public void Dispose() => Host.Dispose();
    }

    /// <summary>
    /// The television this test drives is the second record in the catalogue, so
    /// the test does not depend on which record sorts first.
    /// </summary>
    private const string Record = "tv.16x9";

    [TestMethod]
    public void NudgeAndTurnMoveThePropTheServerActuallyHolds()
    {
        using var harness = new Harness();

        harness.Run("media.spawn", Record);
        var spawned = harness.TheTelevision();
        Assert.IsTrue(harness.Logged($"television 1 created here ({Record}"),
            "the spawn command did not report a created set: " + string.Join(" | ", harness.Log));

        // A set is placed in front of the caller, not on top of them -- the
        // placement is `facingPlacement`'s, and the assertions below are all
        // deltas from wherever that landed.
        var start = (spawned.X, spawned.Y, spawned.Z, spawned.Yaw);
        Assert.AreNotEqual(0.0F, start.X, "the set was placed at the origin: the caller position never reached Lua");

        // ---- UP: z only, and the prop is patched rather than respawned -------
        harness.Run("media.move", "1", "up", "0.5");
        var raised = harness.TheTelevision();
        Assert.AreEqual(spawned.Id, raised.Id,
            "the move replaced the prop; every client would see it disappear and reappear");
        Assert.AreEqual(start.X, raised.X, 0.01F, "moving up changed x");
        Assert.AreEqual(start.Y, raised.Y, 0.01F, "moving up changed y");
        Assert.AreEqual(start.Z + 0.5F, raised.Z, 0.01F, "moving up did not raise the set");
        Assert.AreEqual(start.Yaw, raised.Yaw, 0.01F, "moving up changed the heading");

        // ---- LEFT: the set's own left, at its own heading --------------------
        var yawRadians = start.Yaw * Math.PI / 180.0;
        harness.Run("media.move", "1", "left", "2");
        var shifted = harness.TheTelevision();
        Assert.AreEqual(spawned.Id, shifted.Id);
        Assert.AreEqual(start.X - (float)(Math.Cos(yawRadians) * 2.0), shifted.X, 0.01F,
            "left is not the set's own left");
        Assert.AreEqual(start.Y - (float)(Math.Sin(yawRadians) * 2.0), shifted.Y, 0.01F,
            "left is not the set's own left");
        Assert.AreEqual(start.Z + 0.5F, shifted.Z, 0.01F, "a sideways nudge changed height");

        // ---- TURN: heading only, on the spot ---------------------------------
        harness.Run("media.rotate", "1", "left", "90");
        var turned = harness.TheTelevision();
        Assert.AreEqual(spawned.Id, turned.Id);
        var expectedYaw = (start.Yaw + 90.0F) % 360.0F;
        Assert.AreEqual(expectedYaw, turned.Yaw, 0.01F, "a left turn did not add to the heading");
        Assert.AreEqual(shifted.X, turned.X, 0.01F, "turning moved the set");
        Assert.AreEqual(shifted.Y, turned.Y, 0.01F, "turning moved the set");
        Assert.AreEqual(shifted.Z, turned.Z, 0.01F, "turning moved the set");

        // ---- FORWARD is the set's own forward, after the turn ----------------
        // This is the property the whole module exists for: a button labelled
        // FORWARD that walks the set the way it faces, not the way the world
        // happens to be aligned. At the new heading it is a different world
        // vector from the one it was before the turn.
        var turnedRadians = expectedYaw * Math.PI / 180.0;
        harness.Run("media.move", "1", "forward", "3");
        var walked = harness.TheTelevision();
        Assert.AreEqual(turned.X - (float)(Math.Sin(turnedRadians) * 3.0), walked.X, 0.01F,
            "forward is not the set's own forward");
        Assert.AreEqual(turned.Y + (float)(Math.Cos(turnedRadians) * 3.0), walked.Y, 0.01F,
            "forward is not the set's own forward");

        // ---- the resource's own report agrees with the registry --------------
        // The failure this catches is the quiet one: a command that patches the
        // prop and forgets its own record leaves the menu and `media.list`
        // pointing at where the set used to be, and the next nudge is applied to
        // a stale position.
        Assert.IsTrue(harness.Logged("television 1 moved left to"), "the move was not reported");
        Assert.IsTrue(harness.Logged("television 1 turned left to yaw"), "the turn was not reported");
        var report = harness.Log.Last(line => line.Contains("television 1 moved forward to", StringComparison.Ordinal));
        Assert.IsTrue(report.Contains(
                $"{walked.X:F2},{walked.Y:F2},{walked.Z:F2}", StringComparison.Ordinal),
            $"the resource reports a different position from the registry: {report} vs " +
            $"{walked.X:F2},{walked.Y:F2},{walked.Z:F2}");
    }

    [TestMethod]
    public void ARefusedNudgeLeavesTheRailTheSetIsOn()
    {
        using var harness = new Harness();

        harness.Run("media.spawn", Record);
        var before = harness.TheTelevision();

        // An invented direction, and a distance that is not a distance. Both are
        // refused by name in the pure module; what is asserted here is that the
        // refusal reaches the caller as a refusal and that the prop registry --
        // the thing every client replicates -- is untouched.
        harness.Run("media.move", "1", "upward", "2");
        Assert.IsTrue(harness.Logged("move refused: unknown_direction"),
            "an invented direction was not refused by name: " + string.Join(" | ", harness.Log));
        harness.Run("media.move", "1", "up", "-5");
        Assert.IsTrue(harness.Logged("move refused: distance_must_be_positive"),
            "a negative distance was not refused: " + string.Join(" | ", harness.Log));

        // A set that does not exist, which is the other silent path: an id that
        // is not in the registry has to answer, not no-op.
        harness.Run("media.move", "9", "up", "1");
        Assert.IsTrue(harness.Logged("no such television"),
            "a move naming an unknown set did not say so: " + string.Join(" | ", harness.Log));

        var after = harness.TheTelevision();
        Assert.AreEqual(before.Id, after.Id);
        Assert.AreEqual(before.X, after.X, 0.001F);
        Assert.AreEqual(before.Y, after.Y, 0.001F);
        Assert.AreEqual(before.Z, after.Z, 0.001F);
        Assert.AreEqual(before.Yaw, after.Yaw, 0.001F);
    }

    /// <summary>
    /// Two sets, one move, one removal: the prop that changes is the one named,
    /// and the set that was not named is left exactly as it was.
    /// </summary>
    /// <remarks>
    /// Between `byProp` and the media table there is a mapping by prop id, and a
    /// move or a removal that crossed it would act on the wrong cabinet. That is
    /// invisible in game whenever two sets are near each other, which is exactly
    /// when an operator is lining them up.
    /// </remarks>
    [TestMethod]
    public void MovingOrRemovingOneSetLeavesItsNeighbourAlone()
    {
        using var harness = new Harness();

        harness.Run("media.spawn", Record);
        harness.Run("media.spawn", Record);
        var before = harness.Props.Snapshot().ToDictionary(prop => prop.Id);
        Assert.AreEqual(2, before.Count, "two spawns did not produce two props");

        harness.Run("media.move", "1", "up", "1");
        var afterMove = harness.Props.Snapshot().ToDictionary(prop => prop.Id);
        Assert.AreEqual(2, afterMove.Count, "moving one set changed how many props exist");
        var movedIds = afterMove.Keys.Where(id => Math.Abs(afterMove[id].Z - before[id].Z) > 0.001F).ToArray();
        Assert.AreEqual(1, movedIds.Length, "exactly one prop should have risen in z");
        var movedId = movedIds[0];
        foreach (var (id, prop) in afterMove)
        {
            if (id == movedId) continue;
            Assert.AreEqual(before[id].X, prop.X, 0.001F, "the other set moved in x");
            Assert.AreEqual(before[id].Y, prop.Y, 0.001F, "the other set moved in y");
            Assert.AreEqual(before[id].Z, prop.Z, 0.001F, "the other set moved in z");
        }

        harness.Run("media.remove", "1");
        var remaining = harness.Props.Snapshot();
        Assert.AreEqual(1, remaining.Count, "removing one television removed the wrong number of props");
        Assert.AreNotEqual(movedId, remaining[0].Id, "removing television 1 removed television 2's prop");
        Assert.AreEqual(before[remaining[0].Id].X, remaining[0].X, 0.001F,
            "the surviving set moved when its neighbour was removed");
    }
}
