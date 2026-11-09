/* The network test's decision surface: what is probed, what the answer means, and how it is phrased.
 *
 * GeneralsX @feature Claude 17/08/2026
 *
 * THIS PAGE ANSWERS ONE QUESTION - "why can't I join my friend?" - AND EVERY CHECK EARNS ITS PLACE
 * BY ANSWERING IT. That is why it is not a speed test. A join is refused by SimIdCompare on
 * engineID, sourceID, dataID and ordinalID long before a byte of gameplay moves, so the two most
 * common causes of "the network is broken" are not network faults at all: an install one release
 * behind, and an install missing a mandatory overlay archive. Both are refused by every peer, and
 * both report as a connection problem to the person holding the mouse. They are checks 1 and 2.
 *
 * NO WinForms AND NO Win32 IN THIS FILE, deliberately, for the reason UpdateStatus.cs already gives:
 * it is the whole decision surface, and keeping it free of both is what lets it be compiled AND RUN
 * on a Mac (see ../loader-tests). A diagnostic whose logic can only be exercised by installing a
 * release on Windows is a diagnostic that ships wrong, which is exactly how the "up to date, then a
 * 60 MB download" bug reached a user.
 *
 * WHAT IS DELIBERATELY NOT PROBED, and this is a privacy decision rather than an oversight:
 *
 *   - THE PLAYER'S PUBLIC IP. Never fetched, never displayed, never in the copy line. This page
 *     exists to be screenshotted and pasted into Discord by somebody asking for help, so anything
 *     it prints is published by construction. Masking half an address is a half measure; not
 *     learning it is not. Nothing here needs it - a relay round trip proves reachability without
 *     the client ever being told what the far end saw.
 *   - THE INSTALL PATH, THE WINDOWS USER NAME, THE MACHINE NAME. Same reason. The install
 *     directory is on the GENERAL page for the person who owns the machine; it has no business in
 *     a line meant to be pasted in public.
 *   - DOWNLOAD SPEED. It is the thing every other launcher measures and it diagnoses nothing here:
 *     the payload is a one-off, the game itself is a few hundred bytes per frame, and a number in
 *     Mbps invites "my internet is fine, so it's your game" - which is precisely the wrong
 *     conclusion when the actual fault is a stale build.
 *
 * ICMP AND TCP ARE NOT EVIDENCE ABOUT UDP, so the relay check speaks the relay's own protocol over
 * its own transport. A player behind a router that permits web traffic and drops outbound UDP has a
 * machine that pings everything and can join nothing; a TCP or ICMP probe would have shown that
 * machine a full set of green rows. Every row states which transport it actually used.
 */

using System.Diagnostics;
using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Text;

namespace GeneralsX.Loader;

/// <summary>
/// A check's outcome. <see cref="Pending"/> is a real state, not a placeholder: a probe that has
/// not answered yet must never render as a pass, and a probe that could not answer must never
/// render as a fail - "could not check" is <see cref="Warn"/>, and the difference is the whole
/// lesson of UpdatePlan.Known.
/// </summary>
internal enum CheckStatus
{
    Pending,
    Pass,
    Warn,
    Fail,
}

/// <summary>
/// One row.
///
/// <see cref="Value"/> is the sentence a person reads. <see cref="Short"/> is the token that goes in
/// the copy line, and it is carried separately rather than derived because the two have opposite
/// jobs: the sentence explains, the token has to survive being pasted into a chat message alongside
/// seven others. Deriving one from the other would make the copy line either unreadable or useless.
/// </summary>
internal readonly record struct CheckResult(string Key, string Name, CheckStatus Status, string Value, string Short)
{
    public static CheckResult Pending(string key, string name) =>
        new(key, name, CheckStatus.Pending, "checking…", "");
}

/// <summary>One relay named by the signed manifest.</summary>
internal readonly record struct RelayEntry(string Host, int Port, string Region);

/// <summary>
/// The facts this page needs out of a manifest document, whichever transport delivered it.
///
/// THE SIGNATURE IS NOT VERIFIED HERE AND THIS TYPE MUST NEVER BE USED AS THOUGH IT WERE. .NET 8
/// has no Ed25519, so a check in this file would be either a second implementation of the trust
/// anchor or - far worse - a signature-shaped cover over no check at all, which is the exact
/// failure relay.js refuses to commit for the same reason. The authority on "is this install
/// current" stays GeneralsXUpdate.exe, which fetches and VERIFIES; see <see cref="VersionVerdict"/>,
/// which takes the updater's plan as its input and uses this only to NAME the published build.
/// </summary>
internal sealed record ManifestFacts(long Ordinal, long Minimum, string EngineCommit, IReadOnlyList<RelayEntry> Relays)
{
    public static readonly ManifestFacts None = new(0, 0, "", Array.Empty<RelayEntry>());

    public bool Any => Ordinal > 0 || Relays.Count > 0;
}

internal static class NetworkProbe
{
    /// <summary>
    /// The one host this program is compiled with. Everything else - relays, builds, regions - is
    /// data fetched from it, which is the requirement that makes adding a relay a publish rather
    /// than a client release. It is already compiled into gxlaunch.c as GX_BOOTSTRAP_URL and into
    /// Previews.cs, so naming it here adds no new public surface.
    /// </summary>
    public const string DownloadHost = "dl.wh1ter0se.dev";

    public const string ManifestUrl = "https://" + DownloadHost + "/gxmanifest.txt";

    /// <summary>DEFAULT_RELAY_PORT, tools/master/manifest.js:81. An empty port field means this.</summary>
    public const int DefaultRelayPort = 8086;

    /// <summary>
    /// The cache GeneralsXUpdate.exe leaves in the install directory after a run. Read FIRST, and
    /// only to learn which relays to probe: it is the manifest as of the previous run, so it is a
    /// starting point and never a verdict (UpdateStatus.CacheOnlyVerdict says why at length). Its
    /// value here is that it is on disk, so the relay probe can start immediately instead of
    /// waiting on an HTTPS round trip that may be the very thing that is broken.
    /// </summary>
    public const string ManifestCacheFile = "gxmanifest.txt";

    /// <summary>
    /// The mandatory-overlay manifest, written into GeneralsXPatch by package-windows-share.ps1
    /// from scripts/build/overlay-archives.txt. Read at runtime for the same reason optional.txt is:
    /// a list compiled into the launcher drifts from the payload the first time the set changes,
    /// and the drift is silent - the launcher would report a complete install that is missing an
    /// archive, or report a missing archive that no longer ships.
    /// </summary>
    public const string RequiredListFile = "required.txt";

    public const string PatchDirName = "GeneralsXPatch";

    // ===========================================================================================
    // Timeouts. EVERY probe has one, and they are stated together so the worst case is arithmetic
    // rather than a hope: the page cannot outlive the longest of them.
    //
    // A hung DNS lookup is the specific hazard. Dns.GetHostAddressesAsync accepts a
    // CancellationToken but is documented not to abandon the underlying resolver call on every
    // platform, so cancelling it is not enough - the answer has to be raced against a timer and
    // the loser abandoned. RaceAsync below does that; nothing in this file awaits a resolver
    // directly.
    // ===========================================================================================

    public static readonly TimeSpan DnsTimeout = TimeSpan.FromSeconds(5);
    public static readonly TimeSpan HttpTimeout = TimeSpan.FromSeconds(8);

    /// <summary>Per attempt, not per relay. See <see cref="RelayAttempts"/>.</summary>
    public static readonly TimeSpan UdpTimeout = TimeSpan.FromMilliseconds(1500);

    /// <summary>
    /// TWO datagrams per relay and no more. One is not enough to distinguish "blocked" from "one
    /// packet was lost", which on a bad wifi link is a coin flip and would make this page cry wolf.
    /// Three would be politeness spent on nothing: a second loss on a working path is already rare
    /// enough that the third attempt changes the verdict in almost no case, and this is a live
    /// service with real players on it.
    /// </summary>
    public const int RelayAttempts = 2;

    /// <summary>
    /// At most three relays are probed even when the manifest lists more. The point of asking more
    /// than one is that "0 of 3 answered" is strong evidence of a local block while "1 of 3 failed"
    /// is evidence about that relay - and three is where that distinction is already made. Beyond
    /// it, each extra relay buys a decimal place and costs somebody else's socket.
    /// </summary>
    public const int MaxRelaysProbed = 3;

    // ===========================================================================================
    // The wire format. Documented in scripts/tooling/gx-manifest.py; repeated here only as field
    // indices, never re-derived.
    //
    //   <version>|<ordinal>|<minimum>|<sha>|<relays>|<base>|<builds>|<signature>
    //
    // Extra trailing fields are the format's forward-compatibility rule ("append, and older readers
    // ignore the tail"), so reading by index is correct and a longer document is not an error.
    // ===========================================================================================

    private const int FieldOrdinal = 1;
    private const int FieldMinimum = 2;
    private const int FieldRelays = 4;
    private const int FieldBuilds = 6;
    private const int MinFields = 7;

    /// <summary>
    /// Pull the facts out of a manifest document, wire form, with or without the "GXMANI " tag a
    /// relay frames it with. Returns <see cref="ManifestFacts.None"/> for anything it cannot read -
    /// a truncated document must produce no facts rather than half of them.
    /// </summary>
    internal static ManifestFacts ParseManifest(string wire)
    {
        if (string.IsNullOrWhiteSpace(wire))
        {
            return ManifestFacts.None;
        }

        var text = wire.Trim();
        if (text.StartsWith("GXMANI ", StringComparison.Ordinal))
        {
            text = text["GXMANI ".Length..].Trim();
        }
        // "GXMANI -" is the relay's own marker for "this service is serving no manifest". It is an
        // ANSWER, not silence, and the caller needs to be able to tell those apart - so it parses to
        // no facts and the relay row still reports a successful round trip.
        if (text.Length == 0 || text == "-")
        {
            return ManifestFacts.None;
        }

        var f = text.Split('|');
        if (f.Length < MinFields)
        {
            return ManifestFacts.None;
        }

        long.TryParse(f[FieldOrdinal], NumberStyles.None, CultureInfo.InvariantCulture, out var ordinal);
        long.TryParse(f[FieldMinimum], NumberStyles.None, CultureInfo.InvariantCulture, out var minimum);

        var relays = new List<RelayEntry>();
        foreach (var item in f[FieldRelays].Split(';', StringSplitOptions.RemoveEmptyEntries))
        {
            var p = item.Split(',');
            if (p.Length < 3 || p[0].Length == 0)
            {
                continue;
            }
            var port = DefaultRelayPort;
            if (p[1].Length > 0 &&
                !int.TryParse(p[1], NumberStyles.None, CultureInfo.InvariantCulture, out port))
            {
                continue;
            }
            if (port <= 0 || port > 65535)
            {
                continue;
            }
            relays.Add(new RelayEntry(p[0], port, p[2]));
        }

        // The engine commit is carried in the win32 engine artifact's own filename and nowhere else
        // in the document - the `sha` field is a truncated base64url identifier, not a git commit,
        // and printing it would show the player a string that matches nothing they can look up.
        var commit = "";
        foreach (var item in f[FieldBuilds].Split(';', StringSplitOptions.RemoveEmptyEntries))
        {
            var p = item.Split(',');
            if (p.Length < 2)
            {
                continue;
            }
            var m = System.Text.RegularExpressions.Regex.Match(p[1], @"-win64-([0-9a-f]{7,40})(?:-|\.)");
            if (m.Success)
            {
                commit = m.Groups[1].Value;
                break;
            }
        }

        return new ManifestFacts(ordinal, minimum, commit, relays);
    }

    // ===========================================================================================
    // CHECK 1 - are you on the current build?
    //
    // THE MOST COMMON CAUSE OF A REFUSED JOIN, and the one that reads least like itself. sourceID is
    // a digest of the simulation source and peers refuse each other over it, so a player one release
    // behind is rejected by everybody with a message about the network. They then open the network
    // page. This row is why the page opens with it.
    //
    // THE VERDICT IS THE UPDATER'S, NOT OURS. GeneralsXUpdate.exe --check fetches the current
    // document, verifies its Ed25519 signature and prints the plan it would execute; Apply.CheckAsync
    // parses that. Re-deciding here from a manifest this file cannot verify would be a second
    // implementation of the trust anchor in a second language - the exact drift UpdateStatus.cs was
    // written to remove. The fetched manifest is used ONLY to name the published build in prose.
    // ===========================================================================================

    internal static CheckResult VersionVerdict(UpdatePlan plan, string installedCommit, long installedOrdinal,
        ManifestFacts published)
    {
        const string key = "version";
        const string name = "Game version";

        var mine = installedCommit.Length > 0 ? installedCommit : "unknown";
        var theirs = published.EngineCommit.Length > 0 ? published.EngineCommit : "";

        // The manifest's own floor, and nothing else in the launcher reads it. `minimum` is the
        // ordinal below which a peer should be told to update - so an install under it is not
        // merely behind, it is declared incompatible by the release itself. Escalated above the
        // plan because it is a stronger statement than "there are files to download".
        if (installedOrdinal > 0 && published.Minimum > 0 && installedOrdinal < published.Minimum)
        {
            return new CheckResult(key, name, CheckStatus.Fail,
                $"you are on {mine}, and this release declares builds that old incompatible. " +
                "Press Play to update - nobody can join you until you do.",
                $"build={mine} BELOW-MINIMUM");
        }

        if (!plan.Known)
        {
            // NEVER "up to date" FROM A FAILED CHECK. That phrasing is a promise nothing will
            // download, and an unanswered check cannot make it.
            return new CheckResult(key, name, CheckStatus.Warn,
                theirs.Length > 0 && theirs != installedCommit
                    ? $"could not verify. You are on {mine}; the download host is publishing {theirs}. Press Play."
                    : $"could not verify - you are on {mine}. Press Play and it will update if needed.",
                $"build={mine} UNVERIFIED");
        }

        if (plan.Items.Count == 0)
        {
            return new CheckResult(key, name, CheckStatus.Pass,
                $"{mine} — current. Your build is not why a join is failing.",
                $"build={mine} current");
        }

        var what = theirs.Length > 0 && theirs != installedCommit
            ? $"you are on {mine}, current is {theirs}"
            : $"you are on {mine} and {plan.Items.Count} file(s) are out of date";
        return new CheckResult(key, name, CheckStatus.Fail,
            $"{what}. {UpdateStatus.Mb(plan.TotalBytes)} to download — press Play to update. " +
            "Anyone on the current build will refuse to join you until you do.",
            $"build={mine} UPDATE->{(theirs.Length > 0 ? theirs : "?")} {UpdateStatus.Mb(plan.TotalBytes)}");
    }

    // ===========================================================================================
    // CHECK 2 - do you have the required game data?
    //
    // Every archive named by overlay-archives.txt moves dataID, and dataID is compared on join. An
    // install missing one is refused by every peer with DATA_DIFFERS, which reads as a data
    // mismatch caused by the OTHER machine. This is also the check that catches an antivirus
    // quarantine and a half-finished update, neither of which check 1 can see: the updater is
    // satisfied by its own state file, and the state file records what it downloaded, not what
    // survived on disk.
    // ===========================================================================================

    /// <summary>
    /// Read the mandatory set from GeneralsXPatch/required.txt. Bare filename per line, '#'
    /// comments and blank lines ignored; a trailing "|label" is tolerated so the file can grow the
    /// same shape as optional.txt without breaking this reader.
    /// </summary>
    internal static IReadOnlyList<string> ReadRequiredList(string patchDir)
    {
        var names = new List<string>();
        try
        {
            var path = Path.Combine(patchDir, RequiredListFile);
            if (!File.Exists(path))
            {
                return names;
            }
            foreach (var raw in File.ReadAllLines(path))
            {
                var line = raw.Split('#')[0].Trim();
                if (line.Length == 0)
                {
                    continue;
                }
                var n = line.Split('|', 2)[0].Trim();
                if (n.Length > 0)
                {
                    names.Add(n);
                }
            }
        }
        catch
        {
            // An unreadable manifest is "cannot check", which the caller renders as a warning. It is
            // never an empty required set - that would render as a confident pass.
        }
        return names;
    }

    /// <summary>
    /// Which of <paramref name="required"/> are absent or empty in <paramref name="patchDir"/>.
    ///
    /// ZERO-LENGTH COUNTS AS ABSENT, and that is not pedantry: an interrupted copy and an antivirus
    /// that has emptied a file rather than deleted it both leave a name that File.Exists is happy
    /// with, and the engine mounts an empty archive as nothing at all. gx_overlay_verify on the
    /// POSIX side already makes the same distinction.
    /// </summary>
    internal static IReadOnlyList<string> MissingArchives(string patchDir, IReadOnlyList<string> required)
    {
        var missing = new List<string>();
        foreach (var name in required)
        {
            try
            {
                var fi = new FileInfo(Path.Combine(patchDir, name));
                if (!fi.Exists || fi.Length <= 0)
                {
                    missing.Add(name);
                }
            }
            catch
            {
                missing.Add(name);
            }
        }
        return missing;
    }

    internal static CheckResult DataVerdict(string patchDir)
    {
        const string key = "data";
        const string name = "Required game data";

        var required = ReadRequiredList(patchDir);
        if (required.Count == 0)
        {
            // An install made before required.txt shipped, or one somebody has emptied. Both are
            // "cannot check". Guessing at the list would put this launcher's idea of the mandatory
            // set ahead of the payload's, which is the drift reading it at runtime exists to avoid.
            return new CheckResult(key, name, CheckStatus.Warn,
                "cannot check — this install has no " + RequiredListFile +
                ". Press Play once to refresh it, then run this again.",
                "data=UNKNOWN");
        }

        var missing = MissingArchives(patchDir, required);
        // The always-on HD texture pack ships to everyone but does not move dataID (overlay-archives.txt's dated exception,
        // D · batch-0964 13/10/2026; gxio ALWAYS_ON_ARCHIVES): missing only that, joins still work - a warning (Codex r3b P3).
        // E · look-packs-0965 17/10/2026: !!GXModelsHD.big (the HD 3D models) is the same exception
        bool onlyAlwaysOn = missing.Count > 0;
        foreach (var m in missing)
            if (!string.Equals(m, "!!GXPropsHD.big", StringComparison.OrdinalIgnoreCase)
                && !string.Equals(m, "!!GXModelsHD.big", StringComparison.OrdinalIgnoreCase))
                onlyAlwaysOn = false;
        if (onlyAlwaysOn)
        {
            return new CheckResult(key, name, CheckStatus.Warn,
                $"missing {string.Join(", ", missing)} - HD art only: joins still work, the game just looks plainer. " +
                "Press Play, which re-downloads it.",
                $"data={required.Count - missing.Count}/{required.Count} HD-MISSING");
        }
        if (missing.Count == 0)
        {
            return new CheckResult(key, name, CheckStatus.Pass,
                $"all {required.Count} required archive(s) present.",
                $"data={required.Count}/{required.Count}");
        }

        return new CheckResult(key, name, CheckStatus.Fail,
            $"missing {string.Join(", ", missing)}. Every join will be refused with DATA_DIFFERS " +
            "until this is repaired — press Play, which re-downloads it.",
            $"data={required.Count - missing.Count}/{required.Count} MISSING");
    }

    // ===========================================================================================
    // CHECK 3 - DNS.
    //
    // Its own row rather than folded into reachability, because the two have different fixes. A
    // name that does not resolve is a resolver, a captive portal or a hosts-file edit; a name that
    // resolves and does not answer is a firewall or an outage. Collapsing them would send half the
    // people who read this page to the wrong place.
    //
    // THE RESOLVED ADDRESS IS SHOWN. It is the far end's, never the player's, and a wrong answer
    // here - a portal's 10.x, an ad-blocker's 0.0.0.0 - is a diagnosis a person can act on and
    // cannot get any other way.
    // ===========================================================================================

    internal static async Task<(CheckResult Result, IPAddress[] Addresses)> ResolveAsync(
        string key, string name, string host, CancellationToken ct)
    {
        if (host.Length == 0)
        {
            return (new CheckResult(key, name, CheckStatus.Warn, "no host to resolve.", $"{key}=none"),
                Array.Empty<IPAddress>());
        }

        var sw = Stopwatch.StartNew();
        var addrs = await RaceAsync(LookupAsync(host), DnsTimeout, ct).ConfigureAwait(false);
        sw.Stop();

        if (addrs is null)
        {
            return (new CheckResult(key, name, CheckStatus.Fail,
                    $"{host} did not resolve within {DnsTimeout.TotalSeconds:0} s. " +
                    "A resolver, a captive portal or a hosts-file entry is in the way.",
                    $"{key}=TIMEOUT"),
                Array.Empty<IPAddress>());
        }
        if (addrs.Length == 0)
        {
            return (new CheckResult(key, name, CheckStatus.Fail,
                    $"{host} resolved to nothing. Something is answering DNS for it with an empty record.",
                    $"{key}=EMPTY"),
                Array.Empty<IPAddress>());
        }

        var shown = addrs[0].ToString();
        return (new CheckResult(key, name, CheckStatus.Pass,
                $"{host} → {shown}" + (addrs.Length > 1 ? $" (+{addrs.Length - 1} more)" : "") +
                $", {sw.ElapsedMilliseconds} ms.",
                $"{key}={shown}"),
            addrs);
    }

    // ===========================================================================================
    // CHECK 4 - the download host, over HTTPS.
    //
    // ONE REQUEST SERVES TWO PURPOSES and that is the reason it is a GET of the manifest rather than
    // a HEAD of the root: this page needs the published document anyway - to name the current build
    // in check 1 and to learn which relays exist for check 5 - and asking twice for the same bytes
    // would be load on a CDN and a second thing to keep in step. The latency reported is time to
    // response headers, which is the honest measure of "can this machine reach the host": the body
    // is a few hundred bytes and its transfer time says nothing.
    // ===========================================================================================

    /// <summary>The signed document is a single short line; anything larger is not it.</summary>
    private const int MaxManifestBytes = 8 * 1024;

    internal static async Task<(CheckResult Result, ManifestFacts Facts)> FetchManifestAsync(
        HttpClient http, CancellationToken ct)
    {
        const string key = "https";
        const string name = "Download host (HTTPS)";

        var sw = Stopwatch.StartNew();
        try
        {
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
            timeout.CancelAfter(HttpTimeout);

            using var resp = await http
                .GetAsync(ManifestUrl, HttpCompletionOption.ResponseHeadersRead, timeout.Token)
                .ConfigureAwait(false);
            var ttfb = sw.ElapsedMilliseconds;

            if (!resp.IsSuccessStatusCode)
            {
                return (new CheckResult(key, name, CheckStatus.Fail,
                        $"{DownloadHost} answered HTTP {(int)resp.StatusCode} in {ttfb} ms. " +
                        "The host is reachable but is not serving the release document.",
                        $"https=HTTP{(int)resp.StatusCode}"),
                    ManifestFacts.None);
            }

            var body = await resp.Content.ReadAsStringAsync(timeout.Token).ConfigureAwait(false);
            if (body.Length > MaxManifestBytes)
            {
                return (new CheckResult(key, name, CheckStatus.Warn,
                        $"{DownloadHost} answered in {ttfb} ms, but the document was not the expected shape.",
                        $"https={ttfb}ms ODD"),
                    ManifestFacts.None);
            }

            var facts = ParseManifest(body);
            var status = facts.Any ? CheckStatus.Pass : CheckStatus.Warn;
            var tail = facts.Any
                ? "."
                : " — but the document it served could not be read.";
            return (new CheckResult(key, name, status,
                    $"{DownloadHost} answered in {ttfb} ms{tail}",
                    $"https={ttfb}ms"),
                facts);
        }
        catch (Exception ex)
        {
            return (new CheckResult(key, name, CheckStatus.Fail,
                    $"{DownloadHost} could not be reached over HTTPS ({Reason(ex)}). " +
                    "Updates and the relay list both come from here.",
                    "https=UNREACHABLE"),
                ManifestFacts.None);
        }
    }

    // ===========================================================================================
    // CHECK 5 - the relay, over UDP, in the relay's own protocol.
    //
    // WHY GXUPD. It is the one request a relay answers WITHOUT a room registration (relay.js, see
    // UPD_TAG: "a client checking for an update has not joined anything"), it is exactly what every
    // client already sends on every launch, and it is one small datagram each way. A synthetic ping
    // would be traffic this service has never been asked to carry, and a malformed datagram sent to
    // a live server with real matches on it is not a diagnostic, it is a nuisance.
    //
    // THE REPLY IS ALSO THE ANSWER TO CHECK 6, at no extra cost - see AgreementVerdict.
    //
    // A CONNECTED UDP SOCKET TURNS AN ICMP PORT-UNREACHABLE INTO AN EXCEPTION ON THE NEXT RECEIVE
    // (SocketError.ConnectionReset). That is normally a hazard - it is how one dead peer loses a
    // live peer's reply - but here it is the single most informative outcome available: it proves
    // the datagram REACHED the host and that nothing is listening on the port, which is a completely
    // different fault from silence and has a completely different fix. It is caught by name and
    // reported as itself, and it ends the attempt loop because retrying a definitive answer is
    // pointless. SIO_UDP_CONNRESET is deliberately not touched: suppressing it would throw the
    // information away, and the ioctl does not exist off Windows.
    // ===========================================================================================

    internal readonly record struct RelayReply(bool Answered, long Ms, string Wire, string Refused);

    internal static async Task<RelayReply> ProbeRelayAsync(RelayEntry relay, long ordinal, CancellationToken ct)
    {
        // THE NAME IS RESOLVED HERE, ON A TIMER, BEFORE ANY SOCKET EXISTS - and that is a fix, not
        // a tidy-up. UdpClient.Connect(string, int) resolves the host SYNCHRONOUSLY on the calling
        // thread, and no timeout in this file could touch it: not UdpTimeout, which only starts at
        // the receive below, and not the run's own 45-second ceiling, because a blocked thread does
        // not observe a CancellationToken. A wedged resolver therefore parked ProbeRelayAsync
        // forever, RunAllAsync never completed, and the relay and agreement rows sat on "checking…"
        // permanently - the exact failure this page is written to be incapable of, in the row that
        // matters most. Connect(IPAddress, int) performs no lookup.
        //
        // Found by cross-model review, not by the suite: the test for an unresolvable name passes
        // in about a millisecond on a machine whose resolver answers NXDOMAIN promptly, which is
        // every machine anyone develops on.
        var addrs = await RaceAsync(LookupAsync(relay.Host), DnsTimeout, ct).ConfigureAwait(false);
        if (addrs is null || addrs.Length == 0)
        {
            return new RelayReply(false, 0, "", $"{relay.Host} did not resolve");
        }

        // IPv4 by preference - the relay's port is the same on both and every deployed relay is
        // reachable over v4 - but a v6-only answer is used rather than refused. The socket's family
        // has to match the address it is handed, so it is chosen from the address rather than
        // assumed.
        var ip = addrs.FirstOrDefault(a => a.AddressFamily == AddressFamily.InterNetwork) ?? addrs[0];

        // The ordinal the relay logs against its behind-counter. 0 is the documented value for "a
        // client with nothing installed, which is honestly behind" and is what an install whose
        // state file could not be read genuinely is.
        var stamp = ordinal > 0 ? ordinal.ToString(CultureInfo.InvariantCulture) : "0";
        var payload = Encoding.ASCII.GetBytes("GXUPD " + stamp);

        for (var attempt = 0; attempt < RelayAttempts; attempt++)
        {
            if (ct.IsCancellationRequested)
            {
                break;
            }

            using var sock = new UdpClient(ip.AddressFamily);
            var sw = Stopwatch.StartNew();
            try
            {
                // Connect binds the destination so the OS drops anything from another source - a
                // reply this page accepted from elsewhere would be a measurement of a stranger.
                sock.Connect(ip, relay.Port);
                await sock.SendAsync(payload.AsMemory(), ct).ConfigureAwait(false);

                var recv = await RaceAsync(sock.ReceiveAsync(ct).AsTask(), UdpTimeout, ct).ConfigureAwait(false);
                sw.Stop();
                if (recv is null)
                {
                    continue;   // silence: lost, or dropped by something in the path. Try once more.
                }

                var text = Encoding.ASCII.GetString(recv.Value.Buffer);
                return new RelayReply(true, sw.ElapsedMilliseconds, text, "");
            }
            // BOTH ERROR CODES ARE THE SAME EVENT seen through two stacks: an ICMP port-unreachable
            // delivered back to a connected UDP socket. Windows reports ConnectionReset, the BSD and
            // Linux stacks report ConnectionRefused, and a page that special-cased one would print
            // the informative sentence on one platform and a generic "the socket failed" on the
            // others - for identical, and unusually diagnosable, hardware behaviour.
            catch (SocketException ex) when (ex.SocketErrorCode is SocketError.ConnectionReset
                                                 or SocketError.ConnectionRefused)
            {
                sw.Stop();
                return new RelayReply(false, sw.ElapsedMilliseconds, "",
                    $"the host answered, but nothing is listening on UDP {relay.Port}");
            }
            catch (Exception ex)
            {
                return new RelayReply(false, 0, "", Reason(ex));
            }
        }

        return new RelayReply(false, 0, "", "no reply");
    }

    internal static CheckResult RelayVerdict(IReadOnlyList<(RelayEntry Relay, RelayReply Reply)> results)
    {
        const string key = "relay";
        const string name = "Relay round trip (UDP)";

        if (results.Count == 0)
        {
            return new CheckResult(key, name, CheckStatus.Warn,
                "no relay to test — the relay list could not be read, so this was not attempted.",
                "relay=UNTESTED");
        }

        var answered = results.Where(r => r.Reply.Answered).ToList();
        if (answered.Count == 0)
        {
            // The refusal reason, when one relay produced a specific one, is far more useful than
            // the count - "nothing is listening" and "no reply at all" have different fixes.
            //
            // SELECTED BEFORE FirstOrDefault, NOT AFTER. Taking the default of a tuple of structs
            // and then reading a string off it hands back a null Refused, and the NullReferenceException
            // landed on the one path this whole page exists to render: every relay silent, which is
            // exactly what a blocked firewall looks like. The row would never have appeared, and the
            // page would have sat on "checking…" for the player who most needed an answer. Caught by
            // the suite in ../loader-tests, which is the argument for this file having no WinForms in it.
            var named = results
                .Where(r => !string.IsNullOrEmpty(r.Reply.Refused) && r.Reply.Refused != "no reply")
                .Select(r => r.Reply.Refused)
                .FirstOrDefault();
            var why = named ??
                      $"no reply from any of {results.Count} within {UdpTimeout.TotalSeconds:0.0} s, " +
                      $"{RelayAttempts} attempts each";
            return new CheckResult(key, name, CheckStatus.Fail,
                $"{why}. The game needs OUTBOUND UDP, which web browsing does not — a router or " +
                "firewall blocking it will let everything else work and no match start.",
                $"relay=BLOCKED 0/{results.Count}");
        }

        var best = answered.OrderBy(r => r.Reply.Ms).First();
        var ms = best.Reply.Ms;
        var region = best.Relay.Region.Length > 0 ? best.Relay.Region : "relay";

        // THE BANDS ARE THE GAME'S OWN, COPIED RATHER THAN CHOSEN. kLatencyMsGood = 80 and
        // kLatencyMsFair = 180 in InGameUI.cpp colour the in-game ping readout, and a launcher that
        // called 90 ms "poor" while the HUD painted it green would be teaching the player that one
        // of the two is lying. The quantities are not identical - the HUD measures a peer round
        // trip through the packet router, this measures the leg to the relay - but the relay leg is
        // the dominant part of the former, and a player reading two numbers in milliseconds about
        // the same match will compare them whatever the labels say.
        //
        // NEITHER BAND IS A FAIL. High latency under lockstep is a slow match, not a refused join,
        // and this page reserves red for things that stop you playing at all.
        const long good = 80, fair = 180;
        var status = ms <= good ? CheckStatus.Pass : CheckStatus.Warn;
        var note = ms <= good
            ? "."
            : ms <= fair
                ? " — playable, but every player in the match waits on it, because a lockstep frame " +
                  "cannot run until the slowest input arrives."
                : " — high. Every player in your match feels this one, not just you.";

        return new CheckResult(key, name, status,
            $"{ms} ms to {region} over UDP, {answered.Count} of {results.Count} relay(s) answered{note}",
            $"relay={ms}ms/{region} {answered.Count}/{results.Count}");
    }

    // ===========================================================================================
    // CHECK 6 - does the relay agree with the download host about which build is current?
    //
    // FREE, because check 5's reply IS the relay's copy of the document. And it detects a fault
    // class that has actually happened: on 09/08/2026 the CDN took a new manifest and the relays
    // kept serving the old one for two hours, so every client that reads its manifest over UDP was
    // pointed at an artifact that had been deleted. Those clients abandoned the update in silence
    // while the publish reported success. A player hitting that has a launcher saying "up to date"
    // and a lobby full of people who will not accept them, and nothing else on this page would say
    // why. WARN and not FAIL: the fault is on the service side, not this machine's, and the player
    // can do nothing about it except report it - which is exactly what the copy line is for.
    // ===========================================================================================

    internal static CheckResult AgreementVerdict(ManifestFacts cdn, ManifestFacts relay, bool relayAnswered)
    {
        const string key = "agree";
        const string name = "Relay build agreement";

        if (!relayAnswered)
        {
            return new CheckResult(key, name, CheckStatus.Warn,
                "not compared — no relay answered.", "agree=UNTESTED");
        }
        if (!relay.Any)
        {
            return new CheckResult(key, name, CheckStatus.Warn,
                "the relay answered but is serving no release document. Update checks over UDP " +
                "will find nothing there.", "agree=RELAY-EMPTY");
        }
        if (!cdn.Any)
        {
            return new CheckResult(key, name, CheckStatus.Warn,
                "not compared — the download host did not serve a document to compare against.",
                "agree=UNTESTED");
        }
        if (relay.Ordinal == cdn.Ordinal)
        {
            return new CheckResult(key, name, CheckStatus.Pass,
                "the relay and the download host name the same release.", "agree=ok");
        }

        var which = relay.Ordinal < cdn.Ordinal ? "older" : "newer";
        return new CheckResult(key, name, CheckStatus.Warn,
            $"the relay is serving an {which} release than the download host. That is a server-side " +
            "problem, not yours — copy the line below and report it.",
            $"agree=SPLIT relay={relay.Ordinal} cdn={cdn.Ordinal}");
    }

    // ===========================================================================================
    // CHECK 7 - debug mode.
    //
    // No network, no cost, and it answers the page's question outright. A debug client runs a
    // different simulation and is refused on engineID, so DebugMenu=yes means online and LAN are
    // both unavailable - and the engine's refusal says nothing about debug mode. Somebody who
    // turned it on a month ago to look at something has no reason to connect the two. The
    // DIAGNOSTICS page already warns about it; this one is where a person goes when the symptom
    // has already appeared.
    // ===========================================================================================

    internal static CheckResult DebugVerdict(bool debugMenuOn) =>
        debugMenuOn
            ? new CheckResult("debug", "Debug mode", CheckStatus.Fail,
                "on — online and LAN are unavailable while it is. Turn it off on the DIAGNOSTICS " +
                "page, then press SAVE.",
                "debug=ON")
            : new CheckResult("debug", "Debug mode", CheckStatus.Pass,
                "off — not blocking anything.", "debug=off");

    // ===========================================================================================
    // The verdict, and the copy line.
    // ===========================================================================================

    /// <summary>
    /// One sentence naming the FIRST thing worth fixing, in the order a person should fix it.
    ///
    /// A page of eight dots is a list; this is the answer. The order is causal rather than
    /// cosmetic: a stale build and missing data make every join fail regardless of the network, so
    /// they come before reachability, and reachability comes before latency.
    /// </summary>
    internal static string Verdict(IReadOnlyList<CheckResult> rows)
    {
        CheckStatus Status(string key) =>
            rows.FirstOrDefault(r => r.Key == key) is { Key.Length: > 0 } r ? r.Status : CheckStatus.Pending;

        // A DEFINITE FAILURE IS ANNOUNCED THE MOMENT IT LANDS, even while other rows are still
        // running. "Still checking…" is what this said above every red row until the slowest probe
        // finished, which is a page telling somebody to wait for news it already has - and the
        // ordering below is stable, so an answer given early is not one that changes later. The
        // wait is reported only when nothing has actually gone wrong yet, at the bottom.
        //
        // OFFLINE FIRST, because on a machine with no connection every network row fails and
        // reporting them one at a time would read as five separate faults. It is also not an error:
        // "no connection" is a complete, correct diagnostic result and this page says so plainly
        // rather than putting up a dialog.
        var netKeys = new[] { "dns_dl", "https", "dns_relay", "relay" };
        var netRows = rows.Where(r => netKeys.Contains(r.Key)).ToList();
        if (netRows.Count > 0 && netRows.All(r => r.Status == CheckStatus.Fail))
        {
            return "No connection. Nothing could be reached from this machine, so nothing about " +
                   "your setup was actually tested — the two checks above the line still ran, and " +
                   "they read from disk.";
        }

        if (Status("version") == CheckStatus.Fail)
        {
            return "You are on an older build. That is the most common reason a join is refused, " +
                   "and it does not look like one — press Play to update, then try again.";
        }
        if (Status("data") == CheckStatus.Fail)
        {
            return "Game data is incomplete. Every join will be refused with DATA_DIFFERS until it " +
                   "is repaired — press Play, which re-downloads what is missing.";
        }
        if (Status("debug") == CheckStatus.Fail)
        {
            return "Debug mode is on, so online and LAN are both disabled. Turn it off on the " +
                   "DIAGNOSTICS page and press SAVE.";
        }
        if (Status("relay") == CheckStatus.Fail)
        {
            return "This machine cannot reach a relay over UDP. The game needs outbound UDP, which " +
                   "web browsing does not — check a firewall, a VPN, or a router that blocks it.";
        }
        if (Status("dns_dl") == CheckStatus.Fail || Status("dns_relay") == CheckStatus.Fail)
        {
            return "A name did not resolve. Nothing else can work until DNS does — a captive " +
                   "portal, a VPN or an ad-blocker is the usual cause.";
        }
        if (Status("https") == CheckStatus.Fail)
        {
            return "Updates cannot be fetched: the download host is unreachable. You can still play " +
                   "on the build you have, with people on the same one.";
        }
        // Nothing has failed. Only NOW does an unfinished run get to be the headline - and it must
        // be, because a pending row could still turn red and neither of the two lines below may be
        // said about a run that has not finished.
        if (rows.Any(r => r.Status == CheckStatus.Pending))
        {
            return "Still checking… nothing has failed so far.";
        }

        if (rows.Any(r => r.Status == CheckStatus.Warn))
        {
            return "Nothing here will stop a join, but something is worth reporting. Copy the line " +
                   "below when you ask for help.";
        }

        return "Everything this launcher can check is healthy. If a join still fails, the problem " +
               "is on the other machine or between you — send them this page too.";
    }

    /// <summary>
    /// ONE LINE, PASTEABLE, AND ENOUGH TO DIAGNOSE WITHOUT A FOLLOW-UP QUESTION.
    ///
    /// The test it has to pass is not "does it contain the results" but "can somebody in a chat
    /// window answer without asking anything else". So it carries the installed build AND the
    /// published one, the required-archive count as a fraction, both resolved addresses, the
    /// measured HTTPS and UDP numbers with the relay that produced them, and the verdict token -
    /// and it carries them in a fixed order, so a reader learns the shape once.
    ///
    /// IT CONTAINS NOTHING ABOUT THE PERSON. No public IP, no local address, no path, no user name,
    /// no machine name. That is checked by a test rather than left to inspection, because this line
    /// is written to be published.
    /// </summary>
    internal static string CopyLine(IReadOnlyList<CheckResult> rows, DateTimeOffset when)
    {
        var parts = new List<string>
        {
            "GeneralsX net test " + when.ToUniversalTime().ToString("yyyy-MM-dd HH:mm", CultureInfo.InvariantCulture) + "Z",
        };

        // Fixed order, independent of the order results happened to arrive in.
        foreach (var key in new[] { "version", "data", "debug", "dns_dl", "https", "dns_relay", "relay", "agree" })
        {
            var row = rows.FirstOrDefault(r => r.Key == key);
            if (row.Key is null or "" || row.Short.Length == 0)
            {
                continue;
            }
            parts.Add(row.Short);
        }

        // FAIL > INCOMPLETE > WARN > OK, and the middle of that order is the part worth stating.
        // A definite failure outranks everything: it is the actionable fact and more results
        // cannot un-fail it. But INCOMPLETE outranks WARN and OK, because both of those are claims
        // about the WHOLE run - "nothing here will stop a join" cannot honestly be said while a
        // probe that might yet fail is still outstanding. Ranking WARN above INCOMPLETE would let
        // a run that stalled on the relay check get pasted into a chat window looking merely
        // untidy, and the reader would never learn the most important row never answered.
        var worst = rows.Any(r => r.Status == CheckStatus.Fail) ? "FAIL"
            : rows.Any(r => r.Status == CheckStatus.Pending) ? "INCOMPLETE"
            : rows.Any(r => r.Status == CheckStatus.Warn) ? "WARN"
            : "OK";
        parts.Add("verdict=" + worst);

        return string.Join(" | ", parts);
    }

    // ===========================================================================================
    // Running them.
    // ===========================================================================================

    /// <summary>
    /// Everything a run needs, resolved by the caller.
    ///
    /// The two Windows-shaped facts - the updater's plan and DebugMenu - arrive as a delegate and a
    /// bool rather than being fetched here, which is what keeps this whole file free of Process,
    /// the registry and WinForms. It is also what lets the suite substitute a plan and drive every
    /// branch of <see cref="VersionVerdict"/> without a Windows box or a release installed.
    /// </summary>
    internal sealed record ProbeInputs(
        string PatchDir,
        string CachedManifestWire,
        string InstalledCommit,
        long InstalledOrdinal,
        bool DebugMenuOn,
        Func<CancellationToken, Task<UpdatePlan>> CheckUpdate);

    /// <summary>The rows, in the order they are shown. Read by the page so the two cannot drift.</summary>
    internal static readonly (string Key, string Name)[] RowOrder =
    {
        ("version",   "Game version"),
        ("data",      "Required game data"),
        ("debug",     "Debug mode"),
        ("dns_dl",    "DNS — download host"),
        ("https",     "Download host (HTTPS)"),
        ("dns_relay", "DNS — relay"),
        ("relay",     "Relay round trip (UDP)"),
        ("agree",     "Relay build agreement"),
    };

    /// <summary>
    /// Run every check, reporting each row the moment it settles.
    ///
    /// CONCURRENT, AND THE ONE REAL DEPENDENCY IS BROKEN BY THE CACHE. Which relays to probe is a
    /// fact from the manifest, and fetching the manifest is itself one of the things that may be
    /// broken - so the relay probe would otherwise be gated behind an eight-second HTTPS timeout on
    /// exactly the machine that most needs to know its UDP is blocked. The cached document from the
    /// last run supplies the relay list immediately; the fetch is only fallen back to when there is
    /// no cache at all, which is a first run.
    ///
    /// ROWS MAY BE REPORTED MORE THAN ONCE, and callers must key on <see cref="CheckResult.Key"/>
    /// rather than append. The version row is emitted as soon as the updater answers and re-emitted
    /// if the published document later arrives and can name the current build - a slow CDN must not
    /// hold back an answer that is already useful.
    /// </summary>
    internal static async Task RunAllAsync(ProbeInputs input, IProgress<CheckResult> report,
        CancellationToken ct)
    {
        // The two that read only local disk. Instant, and deliberately first: on a machine with no
        // connection at all they are the entire result, and they are also the two most likely
        // causes of the refused join that brought the player here.
        report.Report(DataVerdict(input.PatchDir));
        report.Report(DebugVerdict(input.DebugMenuOn));

        using var http = new HttpClient { Timeout = HttpTimeout };
        http.DefaultRequestHeaders.UserAgent.ParseAdd("GeneralsXLauncher/1.0");

        var httpTask = FetchManifestAsync(http, ct);
        var dnsDlTask = ResolveAsync("dns_dl", "DNS — download host", DownloadHost, ct);

        // The relay list, from the cache if there is one. Never a verdict, only an address book.
        var cached = ParseManifest(input.CachedManifestWire);

        var relayChain = RelayChainAsync(cached, httpTask, input.InstalledOrdinal, report, ct);
        var versionChain = VersionChainAsync(input, cached, httpTask, report, ct);

        // EACH ROW IS REPORTED WHEN IT SETTLES, NOT IN A FIXED SEQUENCE. Awaiting them in order
        // would hold a fast answer behind a slow one - a five-second DNS timeout would keep a
        // hundred-millisecond HTTPS result off the screen - and a page that fills in out of order
        // is exactly what tells the reader the checks really are running at once.
        await Task.WhenAll(
            EmitAsync(dnsDlTask, report),
            EmitAsync(httpTask, report),
            versionChain,
            relayChain).ConfigureAwait(false);

        var (_, cdn) = await httpTask.ConfigureAwait(false);
        var (relayFacts, relayAnswered) = await relayChain.ConfigureAwait(false);
        report.Report(AgreementVerdict(cdn, relayFacts, relayAnswered));
    }

    /// <summary>Await a probe that also yields data, and report its row the moment it has one.</summary>
    private static async Task EmitAsync<T>(Task<(CheckResult Row, T Data)> task, IProgress<CheckResult> report)
    {
        var (row, _) = await task.ConfigureAwait(false);
        report.Report(row);
    }

    private static async Task VersionChainAsync(ProbeInputs input, ManifestFacts cached,
        Task<(CheckResult, ManifestFacts)> httpTask, IProgress<CheckResult> report, CancellationToken ct)
    {
        UpdatePlan plan;
        try
        {
            plan = await input.CheckUpdate(ct).ConfigureAwait(false);
        }
        catch
        {
            plan = UpdatePlan.Unknown;
        }

        // First answer, phrased with whatever the cache already knew. Useful immediately.
        report.Report(VersionVerdict(plan, input.InstalledCommit, input.InstalledOrdinal, cached));

        var (_, cdn) = await httpTask.ConfigureAwait(false);
        if (cdn.Any)
        {
            // Second, better answer: the published document can name the current build by commit,
            // which is the string a person needs to compare against their friend's.
            report.Report(VersionVerdict(plan, input.InstalledCommit, input.InstalledOrdinal, cdn));
        }
    }

    private static async Task<(ManifestFacts Facts, bool Answered)> RelayChainAsync(ManifestFacts cached,
        Task<(CheckResult, ManifestFacts)> httpTask, long ordinal, IProgress<CheckResult> report,
        CancellationToken ct)
    {
        var relays = cached.Relays;
        if (relays.Count == 0)
        {
            // No cache: a first run, or an install whose cache was deleted. Only here is the relay
            // probe made to wait on the network.
            var (_, cdn) = await httpTask.ConfigureAwait(false);
            relays = cdn.Relays;
        }

        if (relays.Count == 0)
        {
            report.Report(new CheckResult("dns_relay", "DNS — relay", CheckStatus.Warn,
                "no relay to look up — the relay list could not be read from disk or fetched.",
                "dns_relay=UNKNOWN"));
            report.Report(RelayVerdict(Array.Empty<(RelayEntry, RelayReply)>()));
            return (ManifestFacts.None, false);
        }

        var chosen = relays.Take(MaxRelaysProbed).ToList();

        var dnsTask = ResolveAsync("dns_relay", "DNS — relay", chosen[0].Host, ct);
        // The probes do not wait on our DNS row: Connect resolves for itself, and gating them would
        // charge the relay check for a lookup it is not measuring.
        var probes = chosen.Select(async r => (r, await ProbeRelayAsync(r, ordinal, ct).ConfigureAwait(false)));
        var replies = await Task.WhenAll(probes).ConfigureAwait(false);

        var (dnsRow, _) = await dnsTask.ConfigureAwait(false);
        report.Report(dnsRow);
        report.Report(RelayVerdict(replies));

        var best = replies.Where(x => x.Item2.Answered).OrderBy(x => x.Item2.Ms).ToList();
        return best.Count > 0 ? (ParseManifest(best[0].Item2.Wire), true) : (ManifestFacts.None, false);
    }

    // ===========================================================================================
    // Plumbing.
    // ===========================================================================================

    private static readonly object DnsLock = new();
    private static readonly Dictionary<string, Task<IPAddress[]>> DnsInFlight =
        new(StringComparer.OrdinalIgnoreCase);

    /// <summary>
    /// Resolve a host, joining an earlier lookup of the same host that has not come back yet.
    ///
    /// AT MOST ONE OUTSTANDING LOOKUP PER HOST, EVER, and that bound is the point. RaceAsync
    /// abandons the loser of a timeout, and an abandoned resolver call cannot be cancelled on every
    /// platform - so pressing RUN five times against a wedged resolver used to leave five live
    /// resolver operations behind, and the file's claim that the cost was "bounded" was false over
    /// repeated runs. Joining the outstanding one instead caps it at one per host however many
    /// times the button is pressed. Raised by cross-model review.
    ///
    /// A COMPLETED LOOKUP IS NEVER REUSED. Only an in-flight one is joined; once it has finished,
    /// the next run starts a fresh query. A cached answer would defeat the row - "what does this
    /// name resolve to right now" is the question, and a captive portal that has just taken over
    /// the resolver is precisely the case worth catching.
    ///
    /// NO CancellationToken IS PASSED DOWN. The task is shared between runs, so a token from the
    /// run that started it would let that run's cancellation poison a later run that merely joined.
    /// The timeout in RaceAsync is the control, and it is per-caller.
    /// </summary>
    private static Task<IPAddress[]> LookupAsync(string host)
    {
        lock (DnsLock)
        {
            if (DnsInFlight.TryGetValue(host, out var existing) && !existing.IsCompleted)
            {
                return existing;
            }
            var fresh = Dns.GetHostAddressesAsync(host);
            DnsInFlight[host] = fresh;
            return fresh;
        }
    }

    /// <summary>
    /// Race a task against a timer and return null if the timer wins.
    ///
    /// WHY NOT JUST PASS A CancellationToken. Two of the calls here do not honour one. Cancelling
    /// Dns.GetHostAddressesAsync is documented not to abandon the resolver call on every platform,
    /// and a UdpClient receive parked in the kernel is not interruptible either. Both would leave a
    /// row on "checking…" for as long as the OS felt like, which is the exact failure this page
    /// must not have. The loser is abandoned rather than awaited; it holds one socket or one
    /// resolver handle until it finishes and then goes away, which is a bounded cost paid only when
    /// something is already wrong.
    /// </summary>
    private static async Task<T?> RaceAsync<T>(Task<T> work, TimeSpan limit, CancellationToken ct)
        where T : struct
    {
        var done = await Task.WhenAny(work, Task.Delay(limit, ct)).ConfigureAwait(false);
        if (!ReferenceEquals(done, work))
        {
            // OBSERVE THE LOSER. The abandoned task usually ends by faulting - its socket is
            // disposed out from under it the moment the caller gives up - and an unobserved
            // exception on a finalized Task is noise a future reader would spend time on.
            _ = work.ContinueWith(static t => _ = t.Exception,
                CancellationToken.None, TaskContinuationOptions.OnlyOnFaulted, TaskScheduler.Default);
            return null;
        }
        try
        {
            return await work.ConfigureAwait(false);
        }
        catch
        {
            return null;
        }
    }

    private static async Task<T[]?> RaceAsync<T>(Task<T[]> work, TimeSpan limit, CancellationToken ct)
    {
        var done = await Task.WhenAny(work, Task.Delay(limit, ct)).ConfigureAwait(false);
        if (!ReferenceEquals(done, work))
        {
            // OBSERVE THE LOSER. The abandoned task usually ends by faulting - its socket is
            // disposed out from under it the moment the caller gives up - and an unobserved
            // exception on a finalized Task is noise a future reader would spend time on.
            _ = work.ContinueWith(static t => _ = t.Exception,
                CancellationToken.None, TaskContinuationOptions.OnlyOnFaulted, TaskScheduler.Default);
            return null;
        }
        try
        {
            return await work.ConfigureAwait(false);
        }
        catch
        {
            return null;
        }
    }

    /// <summary>
    /// A short cause for a row, WITHOUT the exception's own text.
    ///
    /// Framework messages name paths, proxy hosts and sometimes the local address - all of which
    /// this page has undertaken not to publish, and the copy line is pasted in public. A person
    /// reading "the connection was refused" can act; a person reading a .NET stack sentence cannot,
    /// and the full detail is in gxlaunch.log for whoever needs it.
    /// </summary>
    private static string Reason(Exception ex) => ex switch
    {
        TaskCanceledException or OperationCanceledException => "timed out",
        HttpRequestException { HttpRequestError: HttpRequestError.NameResolutionError } => "the name did not resolve",
        HttpRequestException { HttpRequestError: HttpRequestError.SecureConnectionError } => "TLS failed",
        HttpRequestException => "the connection failed",
        SocketException { SocketErrorCode: SocketError.HostNotFound } => "the name did not resolve",
        SocketException { SocketErrorCode: SocketError.ConnectionRefused } => "the connection was refused",
        SocketException { SocketErrorCode: SocketError.NetworkUnreachable } => "the network is unreachable",
        SocketException => "the socket failed",
        _ => "it failed",
    };
}
