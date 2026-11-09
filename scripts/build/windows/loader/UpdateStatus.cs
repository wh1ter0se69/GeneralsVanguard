/* What Play would download, and how the launcher says it.
 *
 * GeneralsX @bugfix Claude 13/08/2026
 *
 * THE BUG THIS FILE EXISTS TO REMOVE. The launcher showed:
 *
 *     Build       e458d41d6  (2026-08-13 05:19)
 *     Available   e458d41d6  (2026-08-13 05:19)   - up to date
 *     Missing     none
 *
 * and then downloaded 60 MB when Play was pressed. Not a rendering mistake - the panel and the Play
 * path were answering the same question from different data:
 *
 *   - the panel read gxmanifest.txt, the on-disk CACHE that GeneralsXUpdate.exe writes after a run
 *     (cache_save, gxlaunch.c). After a successful run that cache EQUALS the install, so the panel
 *     says "up to date" by construction, however many releases have been published since.
 *   - Play ran the updater, which fetched the CURRENT manifest over HTTPS and decided from that.
 *
 * The payload is split into four tiers - engine / runtime / required / optional - and the launcher
 * binary itself rides the runtime tier, so a release can leave the engine commit untouched while
 * three tiers move. Both strings on screen are derived from the engine commit, which is why they
 * agreed while the install was three tiers behind.
 *
 * NOTHING HERE RE-IMPLEMENTS THE DECISION. That was the other half of the fault: the panel's rule
 * was a second implementation in a second language, and it had drifted three ways from the real one
 * (no platform filter; matching on name where the updater matches on (platform, name); never
 * reading the size field at all). The updater now PRINTS its plan - computed by the same function
 * that performs it, plan_update in gxlaunch.c - and this file only parses and phrases it.
 *
 * NO WinForms AND NO WINDOWS API IN THIS FILE, deliberately. It is the whole decision surface, and
 * keeping it free of both is what lets it be compiled and tested on a Mac (see ../loader-tests).
 * The bug shipped because the logic could only be run by installing a release on Windows.
 */

using System.Globalization;

namespace GeneralsX.Loader;

/// <summary>One artifact the updater says it would fetch.</summary>
internal readonly record struct PlanItem(string Platform, string Name, string Hash, long Size);

/// <summary>
/// The updater's answer to "what would you download right now".
///
/// <see cref="Known"/> IS NOT A DETAIL. An updater too old to print a plan, a missing updater, and
/// an updater that could not reach the network all produce no answer - and "no answer" must never
/// be phrased as "up to date", which is the shape of the original bug. Unknown is its own state and
/// the UI says so.
/// </summary>
internal sealed record UpdatePlan(bool Known, IReadOnlyList<PlanItem> Items, long TotalBytes)
{
    public static readonly UpdatePlan Unknown = new(false, Array.Empty<PlanItem>(), 0);

    /// <summary>True only when the updater positively said there is nothing to fetch.</summary>
    public bool UpToDate => Known && Items.Count == 0;

    public bool HasUpdate => Known && Items.Count > 0;
}

internal static class UpdateStatus
{
    /// <summary>
    /// The platform token this build asks the manifest for. It matches the default in gxlaunch.c
    /// (load_config writes "win32"); this program only ever ships inside the win32 runtime tier, so
    /// there is no case where the two differ. Used ONLY by the cache-only fallback below - the
    /// authoritative path never needs it, because the updater applies its own filter.
    /// </summary>
    private const string Platform = "win32";

    private const string PlanPrefix = "plan: ";
    private const string TotalPrefix = "plan-total: ";

    /// <summary>
    /// Parse `GeneralsXUpdate.exe --check` output into the plan it declared.
    ///
    /// Splitting on whitespace is safe and is an existing contract, not a new assumption: the same
    /// fields are read back by state_load with sscanf_s("%15s %127s %31s"), so an artifact name
    /// containing a space has never been loadable by the updater either.
    ///
    /// STRICT ON DISAGREEMENT. If the declared count does not match the number of plan lines, the
    /// answer is Unknown rather than a best guess - a truncated pipe must not become a confident
    /// "up to date", and a confident wrong number is worse than an honest "could not check".
    /// </summary>
    internal static UpdatePlan ParseCheckOutput(IEnumerable<string> lines)
    {
        var items = new List<PlanItem>();
        var known = false;
        long total = 0;
        var declared = 0;

        foreach (var raw in lines)
        {
            var line = raw.TrimEnd();
            if (line.StartsWith(PlanPrefix, StringComparison.Ordinal))
            {
                var p = line[PlanPrefix.Length..]
                    .Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
                if (p.Length != 4 ||
                    !long.TryParse(p[3], NumberStyles.None, CultureInfo.InvariantCulture, out var size))
                {
                    return UpdatePlan.Unknown;
                }
                items.Add(new PlanItem(p[0], p[1], p[2], size));
            }
            else if (line.StartsWith(TotalPrefix, StringComparison.Ordinal))
            {
                var p = line[TotalPrefix.Length..]
                    .Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
                if (p.Length != 2 ||
                    !int.TryParse(p[0], NumberStyles.None, CultureInfo.InvariantCulture, out declared) ||
                    !long.TryParse(p[1], NumberStyles.None, CultureInfo.InvariantCulture, out total))
                {
                    return UpdatePlan.Unknown;
                }
                known = true;
            }
        }

        if (!known || declared != items.Count)
        {
            return UpdatePlan.Unknown;
        }
        return new UpdatePlan(true, items, total);
    }

    /// <summary>
    /// A human name for an artifact, from the naming rules package-windows-share.ps1 commits to:
    /// the engine keeps a commit-shaped "GeneralsX-win64-&lt;commit&gt;.zip" and every other tier is
    /// "gx&lt;role&gt;_&lt;content8&gt;.zip".
    ///
    /// FALLS BACK TO THE FILENAME, never to a guess. A tier added later shows up as its own name and
    /// its real size, which is accurate and merely less pretty - whereas a wrong friendly label
    /// would be the same class of fault as the one this file fixes.
    /// </summary>
    internal static string TierLabel(string name)
    {
        if (name.StartsWith("gxrt_", StringComparison.OrdinalIgnoreCase)) return "runtime (launcher and DLLs)";
        if (name.StartsWith("gxreq_", StringComparison.OrdinalIgnoreCase)) return "required game data";
        if (name.StartsWith("gxmdl_", StringComparison.OrdinalIgnoreCase)) return "required game data (3D models)";
        if (name.StartsWith("gxopt_", StringComparison.OrdinalIgnoreCase)) return "optional extras";
        if (name.IndexOf("-win64-", StringComparison.OrdinalIgnoreCase) >= 0) return "engine";
        return name;
    }

    /// <summary>MB to one decimal, matching the byte counter the download already shows.</summary>
    internal static string Mb(long bytes) =>
        (bytes / 1048576.0).ToString("0.0", CultureInfo.InvariantCulture) + " MB";

    /// <summary>
    /// The two lines the panel shows about updates, given a plan. Returned rather than rendered so
    /// that the phrasing is testable without a window.
    ///
    /// "Update" replaces the old "Missing", and the difference is the entire point. "Missing" asked
    /// whether an artifact was PRESENT; this asks whether it is CURRENT, which is the question Play
    /// answers. An install can hold every artifact by name and still be three releases behind.
    /// </summary>
    internal static string DescribePlan(UpdatePlan plan)
    {
        if (!plan.Known)
        {
            return "could not check - press Play to update anyway";
        }
        if (plan.Items.Count == 0)
        {
            return "none - nothing will download";
        }

        var parts = plan.Items
            .OrderByDescending(i => i.Size)
            .Select(i => $"{TierLabel(i.Name)} {Mb(i.Size)}");
        return $"{Mb(plan.TotalBytes)} to download - " + string.Join(", ", parts);
    }

    /// <summary>
    /// THE OFFLINE FALLBACK, AND IT IS NEVER THE FINAL WORD.
    ///
    /// This is what the launcher used to do as its ONLY answer, and on the release that prompted
    /// this change it returned "up to date" while three tiers were stale - because gxmanifest.txt is
    /// the manifest as of the previous run, so it necessarily agrees with the install. It survives
    /// because a machine with no network still deserves a best guess, and because it costs no HTTP
    /// on the path that paints the first frame. It is labelled as unverified wherever it is shown.
    ///
    /// The two drifts from the updater's rule are fixed here even though this is only an estimate:
    /// leaving them would move the bug rather than remove it.
    ///   - artifacts for another platform are skipped, as do_update skips them;
    ///   - an installed item matches on (platform, name), as state_hash_of matches.
    /// </summary>
    internal static (string available, string summary) CacheOnlyVerdict(
        string manifestWire, IEnumerable<string> installedLines, string installedCommit)
    {
        var fields = manifestWire.Trim().Split('|');
        if (fields.Length < 7)
        {
            return ("unknown", "could not check");
        }

        var availDate = "";
        if (long.TryParse(fields[1], NumberStyles.None, CultureInfo.InvariantCulture, out var unix) && unix > 0)
        {
            availDate = DateTimeOffset.FromUnixTimeSeconds(unix).ToLocalTime().ToString("yyyy-MM-dd HH:mm");
        }

        // "<platform> <name>" -> hash, mirroring state_hash_of's key.
        var installed = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var raw in installedLines)
        {
            var l = raw.Trim();
            if (!l.StartsWith("item ", StringComparison.OrdinalIgnoreCase)) continue;
            var p = l.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
            if (p.Length >= 4) installed[$"{p[1]} {p[2]}"] = p[3];
        }

        var availCommit = "";
        var stale = 0;

        foreach (var item in fields[6].Split(';', StringSplitOptions.RemoveEmptyEntries))
        {
            var p = item.Split(',');
            if (p.Length != 4) continue;
            var platform = p[0];
            var name = p[1];

            if (availCommit.Length == 0)
            {
                var m = System.Text.RegularExpressions.Regex.Match(name, @"-win64-([0-9a-f]{7,40})(?:-|\.)");
                if (m.Success) availCommit = m.Groups[1].Value;
            }

            if (!platform.Equals(Platform, StringComparison.OrdinalIgnoreCase) &&
                !platform.Equals("any", StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }
            if (!installed.TryGetValue($"{platform} {name}", out var have) || have != p[2])
            {
                stale++;
            }
        }

        var available = availCommit.Length > 0 ? availCommit : "unknown";
        if (availDate.Length > 0) available += $"   ({availDate})";

        // NO "up to date" HERE, EVER. That phrase is a promise that nothing will download, and this
        // function cannot make it - it is reading a cache that agrees with the install by
        // construction. The most it can say is that the last document it saw was satisfied.
        var summary = stale == 0
            ? "no update in the last document seen - not yet re-checked"
            : $"{stale} artifact(s) stale in the last document seen - not yet re-checked";
        if (availCommit.Length > 0 && availCommit != installedCommit)
        {
            summary = $"a different build was published - not yet re-checked";
        }
        return (available, summary);
    }
}
