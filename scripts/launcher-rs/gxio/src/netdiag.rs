//! The network test and the diagnostics facts.
//!
//! THIS MODULE ANSWERS ONE QUESTION - "why can't I join my friend?" - and every check earns its
//! place by answering it. The decision surface is lifted from
//! `scripts/build/windows/loader/NetworkProbe.cs`, which argues it at length and has a test suite
//! behind it; what changed is the language and the transport available, not the reasoning. Where
//! this file departs from that one, the departure is written down at the point it happens.
//!
//! NO HOST IS COMPILED IN, and that is the requirement that shapes the whole file. "Adding a relay
//! must not require updating clients" means the relay list is DATA: it arrives in a signed
//! document, and the caller hands the verified result in. A relay list that could not be obtained
//! produces the row "no relay list", never a guessed address. Grep this file for a dot in a
//! hostname and you will find only `example` inside tests.
//!
//! IT OWNS NO PATHS. Where the game is, where the user data is, which document is cached - all of
//! that is `gxio::game` and `gxio::release`, and this module calls them. A second spelling of
//! "where is the install" is exactly the drift that made the Windows loader report a working
//! install as missing (GameInstall.cs, 12/08/2026), so there is one answer and this is not it.
//!
//! WHY IT IS PURE std. `gxio` declares no dependencies and this module adds none, so every verdict
//! in it is unit-testable on a box with no network, no game and no launcher. That has one
//! structural consequence: **it cannot verify a signature**, because `gxcore` - the trust anchor -
//! is not reachable from here. So it never treats a document as trustworthy on its own. The caller
//! passes a closure that verifies and parses (see `gxlauncher/src/cmd_netdiag.rs`), exactly as
//! NetworkProbe.cs takes the updater's verdict as a delegate rather than re-deciding it. A document
//! this module holds is a `&str` and nothing more.
//!
//! WHAT IS DELIBERATELY NOT PROBED, and this is a privacy decision rather than an oversight. The
//! copy line exists to be pasted into a chat window, so everything it can carry is published by
//! construction:
//!
//!   * THE PLAYER'S PUBLIC IP. Never fetched, never shown. A relay round trip proves reachability
//!     without the client ever being told what the far end saw.
//!   * THE INSTALL PATH, THE USER NAME, THE MACHINE NAME. The paths are on the DIAGNOSTICS page,
//!     for the person who owns the machine. They have no business in a line meant for a public
//!     thread, and `copy_line_names_nothing_about_the_person` is the test that keeps them out.
//!   * DOWNLOAD SPEED. It diagnoses nothing here and invites "my internet is fine, so it's your
//!     game" - precisely the wrong conclusion when the fault is a stale build.
//!
//! ICMP AND TCP ARE NOT EVIDENCE ABOUT UDP. A machine behind a router that permits web traffic and
//! drops outbound UDP pings everything and joins nothing, so the relay check speaks the relay's own
//! protocol over its own transport, and every row states which transport it actually used. The
//! reverse also holds, which is why the download-host row is named "TCP 443" rather than "HTTPS":
//! this module opens a socket, it does not speak TLS, and a row that claimed otherwise would be
//! lying about what it measured.

use std::io;
use std::net::{IpAddr, SocketAddr, TcpStream, ToSocketAddrs, UdpSocket};
use std::path::{Path, PathBuf};
use std::sync::mpsc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

// =================================================================================================
// Timeouts. EVERY probe has one, and they are stated together so the worst case is arithmetic
// rather than a hope. A launcher that hangs on a dead relay is worse than one that says it could
// not reach it.
//
// A hung resolver is the specific hazard, and std has no timeout for it: `ToSocketAddrs` blocks
// until the platform resolver returns. `resolve` below therefore runs it on a helper thread and
// races the answer against a channel timeout, abandoning the loser. Nothing in this file calls a
// resolver on a thread whose progress it depends on.
// =================================================================================================

pub const DNS_TIMEOUT: Duration = Duration::from_secs(5);
pub const TCP_TIMEOUT: Duration = Duration::from_secs(8);

/// Per attempt, not per relay. See `RELAY_ATTEMPTS`.
pub const UDP_TIMEOUT: Duration = Duration::from_millis(1500);

/// TWO datagrams per relay and no more. One cannot distinguish "blocked" from "one packet was
/// lost", which on a bad wifi link is a coin flip and would make this page cry wolf. Three would be
/// politeness spent on nothing, and this is a live service with real matches on it.
pub const RELAY_ATTEMPTS: usize = 2;

/// At most three relays even when the list is longer. "0 of 3 answered" is strong evidence of a
/// local block while "1 of 3 failed" is evidence about that relay, and three is where that
/// distinction is already made. Beyond it each extra relay buys a decimal place and costs somebody
/// else's socket.
pub const MAX_RELAYS_PROBED: usize = 3;

/// The whole run cannot outlive this. It is longer than the longest single probe on purpose - the
/// relay leg is a lookup plus two attempts - and it exists so that a probe which ignores its own
/// timeout still cannot leave a row on "checking…" forever.
pub const RUN_CEILING: Duration = Duration::from_secs(14);

/// `RELAY_LOBBY_PORT`, as `gxio::release` already names it. An empty port field on the wire means
/// this. A port number is not an address: it names no host, and adding a relay does not change it.
pub const DEFAULT_RELAY_PORT: u16 = crate::release::RELAY_LOBBY_PORT;

/// THE BANDS ARE THE GAME'S OWN, COPIED RATHER THAN CHOSEN. `kLatencyMsGood` = 80 and
/// `kLatencyMsFair` = 180 colour the in-game ping readout (InGameUI.cpp `drawNetworkLatency`), and a
/// launcher that called 90 ms "poor" while the HUD painted it green would be teaching the player
/// that one of the two is lying. The quantities are not identical - the HUD measures a peer round
/// trip through the packet router, this measures the leg to the relay - but the relay leg is the
/// dominant part of the former, and a player reading two numbers in milliseconds about the same
/// match will compare them whatever the labels say.
pub const LATENCY_GOOD_MS: u128 = 80;
pub const LATENCY_FAIR_MS: u128 = 180;

/// The relay answers `GXUPD <ordinal>` with one datagram framed `GXMANI <payload>|<sig>`. The wire
/// bound the relay enforces is 476 bytes; this buffer is deliberately larger so that a relay which
/// one day sends more is READ rather than truncated into a document that fails to verify and reads
/// as a forgery.
const RECV_BUF: usize = 2048;

// =================================================================================================
// Rows
// =================================================================================================

/// A check's outcome.
///
/// `Pending` is a real state, not a placeholder: a probe that has not answered yet must never
/// render as a pass. And a probe that COULD NOT answer must never render as a fail - "could not
/// check" is `Warn`, and keeping those apart is the whole lesson of this page.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Status {
    Pending,
    Pass,
    Warn,
    Fail,
}

impl Status {
    pub fn as_str(self) -> &'static str {
        match self {
            Status::Pending => "pending",
            Status::Pass => "pass",
            Status::Warn => "warn",
            Status::Fail => "fail",
        }
    }
}

/// One row.
///
/// `value` is the sentence a person reads. `short` is the token that goes in the copy line, and it
/// is carried separately rather than derived because the two have opposite jobs: the sentence
/// explains, the token has to survive being pasted into a chat message alongside seven others.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Row {
    pub key: &'static str,
    pub name: &'static str,
    pub status: Status,
    pub value: String,
    pub short: String,
}

impl Row {
    fn new(
        key: &'static str,
        name: &'static str,
        status: Status,
        value: impl Into<String>,
        short: impl Into<String>,
    ) -> Row {
        Row { key, name, status, value: value.into(), short: short.into() }
    }
}

/// The rows, in the order they are shown, and the order a person should fix them in: a stale build
/// and missing data make every join fail regardless of the network, so they come before
/// reachability, and reachability comes before latency.
pub const ROW_ORDER: &[(&str, &str)] = &[
    ("version", "Game version"),
    ("data", "Required game data"),
    ("debug", "Debug mode"),
    ("dns_dl", "Download host DNS"),
    ("tcp_dl", "Download host, TCP 443"),
    ("dns_relay", "Relay DNS"),
    ("relay", "Relay round trip, UDP"),
    ("agree", "Relay build agreement"),
];

fn pending_rows() -> Vec<Row> {
    ROW_ORDER.iter().map(|(k, n)| Row::new(k, n, Status::Pending, "checking…", "")).collect()
}

/// Everything the screen needs, as of right now.
///
/// THE WHOLE SNAPSHOT IS REPUBLISHED ON EVERY CHANGE rather than a per-row delta. It is a few
/// hundred bytes, and a consumer that replaces its state wholesale cannot get out of step with one
/// that appends - which is the bug NetworkProbe.cs warns about when it says callers must key on
/// `Key` rather than append.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Snapshot {
    pub rows: Vec<Row>,
    pub verdict: String,
    pub copy_line: String,
    pub done: bool,
}

impl Snapshot {
    fn of(rows: &[Row], done: bool, stamp_secs: u64) -> Snapshot {
        Snapshot {
            rows: rows.to_vec(),
            verdict: overall_verdict(rows),
            copy_line: copy_line(rows, stamp_secs),
            done,
        }
    }
}

fn set_row(rows: &mut [Row], row: Row) {
    if let Some(slot) = rows.iter_mut().find(|r| r.key == row.key) {
        *slot = row;
    }
}

// =================================================================================================
// The facts a run needs
// =================================================================================================

/// Where a relay address came from. It is carried with the address because the two have different
/// standing and the row says which one it used: a relay named by a signed document is one the
/// project published, and a relay named by the player's own Options.ini is one they - or an older
/// bootstrap - configured. Neither is "compiled in".
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RelaySource {
    /// From the signed manifest, verified by gxcore before it reached this module.
    Signed,
    /// From the game's own Options.ini `RelayAddress`. Unsigned, local, and exactly the address the
    /// engine itself will use - which is why it is worth probing when there is no document.
    GameConfig,
    /// From `GX_RELAY_LIST`. A test hook, and the reason no host has to be written into a source
    /// file to exercise a real round trip.
    Env,
}

impl RelaySource {
    pub fn as_str(self) -> &'static str {
        match self {
            RelaySource::Signed => "the signed relay list",
            RelaySource::GameConfig => "your game's own Options.ini",
            RelaySource::Env => "GX_RELAY_LIST",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RelayTarget {
    pub host: String,
    pub port: u16,
    pub region: String,
    pub source: RelaySource,
}

/// The facts this module needs out of a manifest document, whichever transport delivered it.
///
/// IT IS NOT A TRUSTED TYPE BY ITSELF. It is produced by the caller's verify-and-parse closure, and
/// that closure is the only route to one. Building one from an unverified document would be a
/// signature-shaped cover over no check at all - the exact thing relay.js refuses to do for the
/// same reason.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ManifestFacts {
    pub ordinal: u64,
    pub minimum: u64,
    /// The release commit, recovered from the document's signed `sha` field by
    /// `release::commit_hex_from_sha_field`. Empty when it could not be, in which case the row
    /// simply does not name the published build.
    pub release_commit: String,
    /// The `base` field's host. THIS is where the download host comes from: it is data in the
    /// signed document, not a constant in this file.
    pub base_host: String,
    pub relays: Vec<RelayTarget>,
}

impl ManifestFacts {
    pub fn any(&self) -> bool {
        self.ordinal > 0 || !self.relays.is_empty()
    }
}

/// What a relay said when asked.
///
/// FOUR OUTCOMES, NOT TWO, because they have four different fixes and three of them are routinely
/// mistaken for a signature failure. `NothingToServe` is the relay's own `GXMANI -`: an ANSWER
/// meaning "I have nothing for you", which must never reach a player as "the signature is wrong" -
/// that is the one message that makes somebody suspect their install is tampered with when it is
/// fine.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RelayDoc {
    Verified(ManifestFacts),
    /// `GXMANI -`. The relay is deliberately serving no manifest.
    NothingToServe,
    /// Bytes arrived that are not a manifest at all, or are one whose signature did not verify.
    Unreadable,
}

/// Everything a run needs, resolved by the caller.
#[derive(Debug, Clone, Default)]
pub struct ProbeInputs {
    /// Already chosen and already labelled by the caller. Empty is a NORMAL state.
    pub relays: Vec<RelayTarget>,
    /// The signed document this machine already had, if any. Never a verdict on its own - it is the
    /// manifest as of the last update, so it is a starting point and an address book.
    pub cached: Option<ManifestFacts>,
    /// The engine commit from BUILD.txt. Empty when this is not a packaged install.
    pub installed_commit: String,
    /// From gxinstalled.txt, parsed by `gxcore::InstallState`. 0 means "this install has no
    /// record", which is unmeasured rather than behind.
    pub installed_ordinal: u64,
    /// `None` means Options.ini could not be read at all, which is "cannot check" and never "off".
    pub debug_menu: Option<bool>,
    /// Where the mandatory overlay archives live, from `gxio::game::discover`.
    pub patch_dir: Option<PathBuf>,
    /// Set when the game's Options.ini says `RelayAddress = none`. It is an explicit opt-out with a
    /// real consequence, so it gets said out loud rather than looking like "no list".
    pub relay_opted_out: bool,
}

// =================================================================================================
// CHECK 1 - are you on the current build?
//
// THE MOST COMMON CAUSE OF A REFUSED JOIN, and the one that reads least like itself. sourceID is a
// digest of the simulation source and peers refuse each other over it, so a player one release
// behind is rejected by everybody with a message about the network. They then open this page. That
// is why it opens with this row.
//
// DEPARTURE FROM NetworkProbe.cs. That file delegates the verdict to GeneralsXUpdate.exe, which
// fetches and verifies. There is no such process to ask here, so this compares ORDINALS out of a
// document the caller has already verified - and when there is no document it says "could not
// verify" rather than "up to date". That phrasing is a promise nothing will download, and an
// unanswered check cannot make it.
// =================================================================================================

pub fn version_verdict(
    installed_commit: &str,
    installed_ordinal: u64,
    published: Option<&ManifestFacts>,
) -> Row {
    const KEY: &str = "version";
    const NAME: &str = "Game version";

    let mine =
        if installed_commit.is_empty() { "unknown".to_string() } else { installed_commit.to_string() };

    let Some(p) = published.filter(|p| p.any()) else {
        return Row::new(
            KEY,
            NAME,
            Status::Warn,
            format!(
                "could not verify - you are on {mine}, and nothing on this machine names the current \
                 release. Press Play and it will update if needed."
            ),
            format!("build={mine} UNVERIFIED"),
        );
    };

    let theirs = p.release_commit.clone();

    if installed_ordinal == 0 {
        // An install with no record. It is not "behind" - it is unmeasured, and calling it behind
        // would send somebody to re-download an install that may be perfectly current.
        return Row::new(
            KEY,
            NAME,
            Status::Warn,
            format!(
                "this install keeps no version record, so it cannot be compared{}. Press Play once \
                 and it will write one.",
                if theirs.is_empty() {
                    String::new()
                } else {
                    format!(" against the published {theirs}")
                }
            ),
            format!("build={mine} NORECORD"),
        );
    }

    // The manifest's own floor. `minimum` is the ordinal below which a peer should be told to
    // update, so an install under it is not merely behind - it is declared incompatible by the
    // release itself. That is a stronger statement than "there are files to download".
    if p.minimum > 0 && installed_ordinal < p.minimum {
        return Row::new(
            KEY,
            NAME,
            Status::Fail,
            format!(
                "you are on {mine}, and this release declares builds that old incompatible. Press \
                 Play to update - nobody can join you until you do."
            ),
            format!("build={mine} BELOW-MINIMUM"),
        );
    }

    if installed_ordinal < p.ordinal {
        let what = if theirs.is_empty() || theirs == installed_commit {
            format!("you are on {mine} and a newer release has been published")
        } else {
            format!("you are on {mine}, current is {theirs}")
        };
        return Row::new(
            KEY,
            NAME,
            Status::Fail,
            format!(
                "{what}. Press Play to update - anyone on the current build will refuse to join you \
                 until you do."
            ),
            format!("build={mine} UPDATE->{}", if theirs.is_empty() { "?" } else { &theirs }),
        );
    }

    Row::new(
        KEY,
        NAME,
        Status::Pass,
        format!("{mine} - current. Your build is not why a join is failing."),
        format!("build={mine} current"),
    )
}

// =================================================================================================
// CHECK 2 - do you have the required game data?
//
// Every archive named by the payload's required list moves dataID, and dataID is compared on join.
// An install missing one is refused by every peer with DATA_DIFFERS, which reads as a mismatch
// caused by the OTHER machine. This also catches an antivirus quarantine and a half-finished
// update, neither of which check 1 can see: the updater is satisfied by its own state file, and the
// state file records what it downloaded, not what survived on disk.
//
// THE LIST AND THE CHECK BOTH BELONG TO `gxio::game`. Reading required.txt a second time here would
// be a second idea of what "present" means - and `game::missing_archives` already encodes the one
// that matters, that a zero-length file counts as absent.
// =================================================================================================

pub fn data_verdict(patch_dir: Option<&Path>) -> Row {
    const KEY: &str = "data";
    const NAME: &str = "Required game data";

    let Some(dir) = patch_dir else {
        return Row::new(
            KEY,
            NAME,
            Status::Warn,
            "cannot check - no game install was found on this machine, so there is nothing to look in.",
            "data=NOINSTALL",
        );
    };

    let required = crate::game::read_required(dir);
    if required.is_empty() {
        // An install made before the list shipped, or one somebody has emptied. Both are "cannot
        // check". Guessing at the set would put this launcher's idea of it ahead of the payload's,
        // which is the drift reading it at runtime exists to avoid.
        return Row::new(
            KEY,
            NAME,
            Status::Warn,
            "cannot check - this install has no required.txt. Press Play once to refresh it, then run this again.",
            "data=UNKNOWN",
        );
    }

    let missing = crate::game::missing_archives(dir, &required);
    // The always-on HD texture pack is mandatory to SHIP but does not move dataID (overlay-archives.txt's dated exception,
    // D · batch-0964 13/10/2026): missing only that, joins still work - a warning, never the DATA_DIFFERS failure (Codex r3b P3).
    if !missing.is_empty()
        && missing.iter().all(|m| crate::game::ALWAYS_ON_ARCHIVES.iter().any(|a| a.eq_ignore_ascii_case(m)))
    {
        return Row::new(
            KEY,
            NAME,
            Status::Warn,
            format!(
                "missing {} - HD art only: joins still work, the game just looks plainer. Press Play, which re-downloads it.",
                name_a_few(&missing)
            ),
            format!("data={}/{} HD-MISSING", required.len() - missing.len(), required.len()),
        );
    }
    if missing.is_empty() {
        return Row::new(
            KEY,
            NAME,
            Status::Pass,
            format!("all {} required archives present.", required.len()),
            format!("data={}/{}", required.len(), required.len()),
        );
    }

    Row::new(
        KEY,
        NAME,
        Status::Fail,
        format!(
            "missing {}. Every join will be refused with DATA_DIFFERS until this is repaired - press \
             Play, which re-downloads it.",
            name_a_few(&missing)
        ),
        format!("data={}/{} MISSING", required.len() - missing.len(), required.len()),
    )
}

/// The most names to spell out before summarising the rest.
///
/// A WHOLLY EMPTY OVERLAY FOLDER NAMES FOURTEEN ARCHIVES, and a sentence carrying fourteen
/// filenames is both unreadable and long enough to push the rest of the page off a fixed-height
/// window - measured 20/08/2026, where the worst realistic row left 12px of slack. The fix belongs
/// here rather than in CSS: three names already tell a person what kind of thing is missing, and
/// the count tells them the scale. The exact list is a job for the log, not for a sentence.
const NAMES_SHOWN: usize = 3;

fn name_a_few(names: &[String]) -> String {
    if names.len() <= NAMES_SHOWN {
        return names.join(", ");
    }
    format!("{}, and {} more", names[..NAMES_SHOWN].join(", "), names.len() - NAMES_SHOWN)
}

// =================================================================================================
// CHECK 3 - debug mode.
//
// No network, no cost, and it answers the page's question outright. A debug client runs a different
// simulation and is refused on engineID, so `DebugMenu = yes` means online and LAN are both
// unavailable - and the engine's refusal says nothing about debug mode. Somebody who turned it on a
// month ago to look at something has no reason to connect the two.
// =================================================================================================

pub fn debug_verdict(debug_menu: Option<bool>) -> Row {
    const KEY: &str = "debug";
    const NAME: &str = "Debug mode";
    match debug_menu {
        Some(true) => Row::new(
            KEY,
            NAME,
            Status::Fail,
            "on - online and LAN are unavailable while it is. Turn it off on the DIAGNOSTICS page.",
            "debug=ON",
        ),
        Some(false) => Row::new(KEY, NAME, Status::Pass, "off, as it must be to join.", "debug=off"),
        // NOT "off". An unreadable Options.ini is "cannot check", and rendering that as a pass is
        // exactly the mistake this page is written to be incapable of.
        None => Row::new(
            KEY,
            NAME,
            Status::Warn,
            "cannot check - the game's Options.ini could not be read.",
            "debug=UNKNOWN",
        ),
    }
}

// =================================================================================================
// CHECKS 4 and 5 - the download host.
//
// DNS IS ITS OWN ROW rather than folded into reachability, because the two have different fixes. A
// name that does not resolve is a resolver, a captive portal or a hosts-file edit; a name that
// resolves and does not answer is a firewall or an outage. Collapsing them would send half the
// people who read this page to the wrong place.
//
// THE RESOLVED ADDRESS IS SHOWN. It is the far end's, never the player's, and a wrong answer here -
// a portal's 10.x, an ad-blocker's 0.0.0.0 - is a diagnosis a person can act on and cannot get any
// other way.
// =================================================================================================

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ResolveErr {
    Timeout,
    Empty,
    Failed(String),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Resolved {
    pub ip: String,
    pub count: usize,
    pub ms: u128,
}

pub fn dns_row(
    key: &'static str,
    name: &'static str,
    host: &str,
    outcome: &Result<Resolved, ResolveErr>,
) -> Row {
    // AN IP LITERAL IS NOT A LOOKUP. "198.51.100.7 -> 198.51.100.7, 0 ms" read as a DNS answer that
    // happened to match (owner, 05/10/2026: "why is my relay dns IPs the same?"). Say what it is.
    if let Ok(ip) = host.parse::<std::net::IpAddr>() {
        let name = match key {
            "dns_relay" => "Relay address",
            "dns_dl" => "Download host address",
            _ => name,
        };
        return Row::new(
            key,
            name,
            Status::Pass,
            format!("{ip} (an IP - no DNS lookup needed)."),
            format!("{key}={ip}(literal)"),
        );
    }
    match outcome {
        Ok(r) => {
            let extra = if r.count > 1 { format!(" (+{} more)", r.count - 1) } else { String::new() };
            Row::new(
                key,
                name,
                Status::Pass,
                format!("{host} -> {}{extra}, {} ms.", r.ip, r.ms),
                format!("{key}={}", r.ip),
            )
        }
        Err(ResolveErr::Timeout) => Row::new(
            key,
            name,
            Status::Fail,
            format!(
                "{host} did not resolve within {} s. A resolver, a captive portal or a hosts-file \
                 entry is in the way.",
                DNS_TIMEOUT.as_secs()
            ),
            format!("{key}=TIMEOUT"),
        ),
        Err(ResolveErr::Empty) => Row::new(
            key,
            name,
            Status::Fail,
            format!("{host} resolved to nothing. Something is answering DNS for it with an empty record."),
            format!("{key}=EMPTY"),
        ),
        Err(ResolveErr::Failed(why)) => Row::new(
            key,
            name,
            Status::Fail,
            format!("{host} did not resolve ({why})."),
            format!("{key}=FAIL"),
        ),
    }
}

/// The download-host reachability row.
///
/// NAMED FOR WHAT IT MEASURED. This opens a TCP connection to port 443 and closes it; it does not
/// negotiate TLS and it does not fetch the manifest, so it is not evidence that an update would
/// succeed - only that packets reach the host. Calling it "HTTPS" would be a claim about a layer
/// this module never touches, and a green row that means less than it says is worse than an honest
/// narrower one.
pub fn tcp_row(host: &str, outcome: &Result<(String, u128), String>) -> Row {
    const KEY: &str = "tcp_dl";
    const NAME: &str = "Download host, TCP 443";
    match outcome {
        Ok((ip, ms)) => Row::new(
            KEY,
            NAME,
            Status::Pass,
            format!(
                "{host} accepted a connection on 443 in {ms} ms, at {ip}. Transport only - this test \
                 does not speak TLS and does not fetch anything."
            ),
            format!("tcp443={ms}ms"),
        ),
        Err(why) => Row::new(
            KEY,
            NAME,
            Status::Fail,
            format!(
                "{host} could not be reached on TCP 443 ({why}). Updates and the relay list both come \
                 from here; you can still play on the build you have, with people on the same one."
            ),
            "tcp443=UNREACHABLE",
        ),
    }
}

// =================================================================================================
// CHECK 6 - the relay, over UDP, in the relay's own protocol.
//
// WHY GXUPD. It is the one request a relay answers WITHOUT a room registration (relay.js, UPD_TAG:
// "a client checking for an update has not joined anything"), it is exactly what every client
// already sends on every launch, and it is one small datagram each way. A synthetic ping would be
// traffic this service has never been asked to carry, and a malformed datagram sent to a live server
// with real matches on it is not a diagnostic, it is a nuisance.
//
// THE REPLY IS ALSO THE ANSWER TO CHECK 7, at no extra cost - it is the signed manifest, so the
// caller can verify it and compare what the relay serves against what this machine has.
//
// A CONNECTED UDP SOCKET TURNS AN ICMP PORT-UNREACHABLE INTO AN ERROR ON THE NEXT RECEIVE. That is
// normally a hazard - it is how one dead peer loses a live peer's reply - but here it is the single
// most informative outcome available: it proves the datagram REACHED the host and that nothing is
// listening on the port, which is a completely different fault from silence and has a completely
// different fix. Windows reports ConnectionReset and the BSD/Linux stacks report ConnectionRefused;
// both are the same event through two stacks, so both are matched. A page that special-cased one
// would print the informative sentence on one platform and a generic failure on the others.
// =================================================================================================

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RelayReply {
    pub answered: bool,
    pub ms: u128,
    /// The raw datagram, verbatim. NOT trusted and NOT parsed here - the caller verifies it.
    pub wire: String,
    /// Why not, when it did not answer. Empty when it did.
    pub refused: String,
    pub resolved: Result<Resolved, ResolveErr>,
}

/// Send one `GXUPD <ordinal>` and wait for the reply, twice at most.
///
/// The ordinal is what the relay logs against its behind-counter. 0 is the documented value for "a
/// client with nothing installed, which is honestly behind", and is what an install whose record
/// could not be read genuinely is.
pub fn probe_relay(target: &RelayTarget, ordinal: u64) -> RelayReply {
    // THE NAME IS RESOLVED HERE, ON A TIMER, BEFORE ANY SOCKET EXISTS. The C# original learned this
    // the hard way: `UdpClient.Connect(string, int)` resolves synchronously on the calling thread,
    // so a wedged resolver parked the probe forever and the relay row - the one that matters most -
    // sat on "checking…" permanently. `resolve` below cannot do that.
    let resolved = resolve(&target.host, target.port, DNS_TIMEOUT);
    let addrs = match &resolved {
        Ok((addrs, _)) => addrs.clone(),
        Err(e) => {
            return RelayReply {
                answered: false,
                ms: 0,
                wire: String::new(),
                refused: format!("{} did not resolve", target.host),
                resolved: Err(e.clone()),
            }
        }
    };

    let info = match &resolved {
        Ok((a, ms)) => Ok(one(a, *ms)),
        Err(e) => Err(e.clone()),
    };

    // IPv4 by preference - the relay's port is the same on both and every deployed relay is
    // reachable over v4 - but a v6-only answer is used rather than refused. The socket's family has
    // to match the address it is handed, so it is chosen FROM the address rather than assumed.
    let addr = addrs.iter().find(|a| matches!(a.ip(), IpAddr::V4(_))).copied().unwrap_or(addrs[0]);

    let payload = format!("GXUPD {ordinal}");

    for _ in 0..RELAY_ATTEMPTS {
        let bind: SocketAddr = if addr.is_ipv4() {
            SocketAddr::from(([0u8, 0, 0, 0], 0))
        } else {
            SocketAddr::from(([0u16; 8], 0))
        };

        let sock = match UdpSocket::bind(bind) {
            Ok(s) => s,
            Err(e) => {
                return RelayReply {
                    answered: false,
                    ms: 0,
                    wire: String::new(),
                    refused: reason(&e),
                    resolved: info,
                }
            }
        };
        // Connect binds the destination so the OS drops anything from another source - a reply this
        // page accepted from elsewhere would be a measurement of a stranger.
        if let Err(e) = sock.connect(addr) {
            return RelayReply {
                answered: false,
                ms: 0,
                wire: String::new(),
                refused: reason(&e),
                resolved: info,
            };
        }
        if sock.set_read_timeout(Some(UDP_TIMEOUT)).is_err() {
            return RelayReply {
                answered: false,
                ms: 0,
                wire: String::new(),
                refused: "this machine refused to put a timeout on the socket".into(),
                resolved: info,
            };
        }

        let started = Instant::now();
        if let Err(e) = sock.send(payload.as_bytes()) {
            if is_unreachable(&e) {
                return RelayReply {
                    answered: false,
                    ms: started.elapsed().as_millis(),
                    wire: String::new(),
                    refused: format!("the host answered, but nothing is listening on UDP {}", target.port),
                    resolved: info,
                };
            }
            return RelayReply {
                answered: false,
                ms: 0,
                wire: String::new(),
                refused: reason(&e),
                resolved: info,
            };
        }

        let mut buf = [0u8; RECV_BUF];
        match sock.recv(&mut buf) {
            Ok(n) => {
                let text = String::from_utf8_lossy(&buf[..n]).trim_matches('\0').trim().to_string();
                return RelayReply {
                    answered: true,
                    ms: started.elapsed().as_millis(),
                    wire: text,
                    refused: String::new(),
                    resolved: info,
                };
            }
            Err(e) if is_unreachable(&e) => {
                // Definitive, and it ends the loop: retrying an answer this specific is pointless.
                return RelayReply {
                    answered: false,
                    ms: started.elapsed().as_millis(),
                    wire: String::new(),
                    refused: format!("the host answered, but nothing is listening on UDP {}", target.port),
                    resolved: info,
                };
            }
            // Silence: lost, or dropped by something in the path. Try once more.
            Err(e) if is_timeout(&e) => continue,
            Err(e) => {
                return RelayReply {
                    answered: false,
                    ms: 0,
                    wire: String::new(),
                    refused: reason(&e),
                    resolved: info,
                }
            }
        }
    }

    RelayReply {
        answered: false,
        ms: 0,
        wire: String::new(),
        refused: "no reply".into(),
        resolved: info,
    }
}

pub fn relay_verdict(results: &[(RelayTarget, RelayReply)], opted_out: bool) -> Row {
    const KEY: &str = "relay";
    const NAME: &str = "Relay round trip, UDP";

    // THE OPT-OUT OUTRANKS A SUCCESSFUL PROBE, and getting that order wrong is a bug this page had
    // for an afternoon: with a signed relay list on disk the probe answered in 63 ms and the row
    // went green, on a machine whose ENGINE had been told to use no relay at all. The launcher's
    // reachability is not the question - `OptionPreferences::getRelayAddress` returns empty for
    // `RelayAddress = none` and for GENERALSX_NO_RELAY, so the GAME will not contact a relay
    // whatever this test can reach. Reporting green there sends somebody to look at their router.
    //
    // The measurement is still worth carrying, as a parenthetical: "the setting is off AND the
    // network is fine" is a different conversation from "the setting is off and so is your UDP".
    if opted_out {
        let reachable = results
            .iter()
            .filter(|(_, r)| r.answered)
            .min_by_key(|(_, r)| r.ms)
            .map(|(_, r)| format!(" A relay is reachable from here ({} ms), so this is the setting and not your network.", r.ms))
            .unwrap_or_default();
        return Row::new(
            KEY,
            NAME,
            Status::Fail,
            format!(
                "your game is set to use no relay at all - `RelayAddress = none` in Options.ini, or \
                 GENERALSX_NO_RELAY in the environment. Online play is off until that is \
                 removed.{reachable}"
            ),
            "relay=OPTED-OUT",
        );
    }

    if results.is_empty() {
        return Row::new(
            KEY,
            NAME,
            Status::Warn,
            "no relay to test - this machine has no relay list, so nothing was attempted. The list \
             arrives with the first update; press Play once.",
            "relay=NOLIST",
        );
    }

    let answered: Vec<&(RelayTarget, RelayReply)> = results.iter().filter(|(_, r)| r.answered).collect();

    if answered.is_empty() {
        // The refusal reason, when one relay produced a specific one, is far more useful than the
        // count - "nothing is listening" and "no reply at all" have different fixes.
        let named =
            results.iter().map(|(_, r)| r.refused.as_str()).find(|r| !r.is_empty() && *r != "no reply");
        let why = match named {
            Some(w) => w.to_string(),
            None => format!(
                "no reply from any of {} within {:.1} s, {RELAY_ATTEMPTS} attempts each",
                results.len(),
                UDP_TIMEOUT.as_secs_f64()
            ),
        };
        return Row::new(
            KEY,
            NAME,
            Status::Fail,
            format!(
                "{why}. The game needs OUTBOUND UDP, which web browsing does not - a router or \
                 firewall blocking it will let everything else work and no match start."
            ),
            format!("relay=BLOCKED 0/{}", results.len()),
        );
    }

    let best = answered.iter().min_by_key(|(_, r)| r.ms).expect("answered is not empty");
    let ms = best.1.ms;
    let region = if best.0.region.is_empty() { "relay".to_string() } else { best.0.region.clone() };

    // NEITHER BAND IS A FAIL. High latency under lockstep is a slow match, not a refused join, and
    // this page reserves red for things that stop you playing at all.
    let status = if ms <= LATENCY_GOOD_MS { Status::Pass } else { Status::Warn };
    let note = if ms <= LATENCY_GOOD_MS {
        "."
    } else if ms <= LATENCY_FAIR_MS {
        " - playable, but every player in the match waits on it, because a lockstep frame cannot run \
         until the slowest input arrives."
    } else {
        " - high. Every player in your match feels this one, not just you."
    };
    let from = if best.0.source == RelaySource::Signed {
        String::new()
    } else {
        format!(" Address from {}.", best.0.source.as_str())
    };

    Row::new(
        KEY,
        NAME,
        status,
        format!("{ms} ms to {region} over UDP, {} of {} answered{note}{from}", answered.len(), results.len()),
        format!("relay={ms}ms/{region} {}/{}", answered.len(), results.len()),
    )
}

// =================================================================================================
// CHECK 7 - does the relay agree with this machine about which build is current?
//
// FREE, because check 6's reply IS the relay's copy of the signed document. And it detects a fault
// class that has actually happened: on 09/08/2026 the CDN took a new manifest and the relays kept
// serving the old one for two hours, so every client that reads its manifest over UDP was pointed at
// an artifact that had been deleted. Those clients abandoned the update in silence while the publish
// reported success.
//
// WARN AND NOT FAIL for a split: the fault is on the service side, not this machine's, and the
// player can do nothing about it except report it - which is exactly what the copy line is for.
// =================================================================================================

pub fn agreement_verdict(local: Option<&ManifestFacts>, relay: Option<&RelayDoc>) -> Row {
    const KEY: &str = "agree";
    const NAME: &str = "Relay build agreement";

    let Some(doc) = relay else {
        return Row::new(KEY, NAME, Status::Warn, "not checked, the relay never answered.", "agree=UNTESTED");
    };

    let r = match doc {
        RelayDoc::NothingToServe => {
            return Row::new(
                KEY,
                NAME,
                Status::Warn,
                "the relay answered and is deliberately serving no release document. Update checks \
                 over UDP will find nothing there - which is a service setting, not a fault on this \
                 machine.",
                "agree=RELAY-EMPTY",
            )
        }
        RelayDoc::Unreadable => {
            // NOT phrased as tampering. The overwhelmingly likelier causes are a truncated file on
            // the relay and a signer newer than this build, and telling a player their manifest was
            // forged is how somebody ends up reinstalling a machine that was fine.
            return Row::new(
                KEY,
                NAME,
                Status::Warn,
                "the relay answered with something this launcher could not read as a signed release \
                 document. Copy the line below and report it - it is a service-side problem, not \
                 yours.",
                "agree=RELAY-UNVERIFIED",
            );
        }
        RelayDoc::Verified(r) => r,
    };

    let Some(l) = local.filter(|l| l.any()) else {
        return Row::new(
            KEY,
            NAME,
            Status::Warn,
            format!(
                "not compared - this machine has no stored document to compare against. The relay is \
                 serving release {}.",
                r.ordinal
            ),
            "agree=NOLOCAL",
        );
    };

    if r.ordinal == l.ordinal {
        return Row::new(
            KEY,
            NAME,
            Status::Pass,
            "the relay and this machine's stored document name the same release.",
            "agree=ok",
        );
    }

    let which = if r.ordinal > l.ordinal { "newer" } else { "older" };
    Row::new(
        KEY,
        NAME,
        Status::Warn,
        format!(
            "the relay is serving a {which} release ({}) than the document stored here ({}). If it is \
             newer, press Play. If it is older, that is a server-side problem - copy the line below \
             and report it.",
            r.ordinal, l.ordinal
        ),
        format!("agree=SPLIT relay={} local={}", r.ordinal, l.ordinal),
    )
}

// =================================================================================================
// The verdict, and the copy line
// =================================================================================================

fn status_of(rows: &[Row], key: &str) -> Status {
    rows.iter().find(|r| r.key == key).map(|r| r.status).unwrap_or(Status::Pending)
}

/// One sentence naming the FIRST thing worth fixing, in the order a person should fix it.
///
/// A page of eight dots is a list; this is the answer. The order is causal rather than cosmetic: a
/// stale build and missing data make every join fail regardless of the network, so they come before
/// reachability, and reachability comes before latency.
///
/// A DEFINITE FAILURE IS ANNOUNCED THE MOMENT IT LANDS, even while other rows are still running.
/// "Still checking…" above a red row is a page telling somebody to wait for news it already has, and
/// the ordering below is stable, so an answer given early is not one that changes later. The wait is
/// reported only when nothing has actually gone wrong yet.
pub fn overall_verdict(rows: &[Row]) -> String {
    // OFFLINE FIRST, because on a machine with no connection every network row fails and reporting
    // them one at a time would read as four separate faults. It is also not an error: "no
    // connection" is a complete, correct diagnostic result and this page says so plainly rather than
    // putting up a dialog.
    let net: Vec<&Row> =
        rows.iter().filter(|r| matches!(r.key, "dns_dl" | "tcp_dl" | "dns_relay" | "relay")).collect();
    if !net.is_empty() && net.iter().all(|r| r.status == Status::Fail) {
        return "No connection. Nothing could be reached from this machine, so nothing about your \
                setup was actually tested - the three checks above the line still ran, and they read \
                from disk."
            .to_string();
    }

    if status_of(rows, "version") == Status::Fail {
        return "You are on an older build. That is the most common reason a join is refused, and it \
                does not look like one - press Play to update, then try again."
            .to_string();
    }
    if status_of(rows, "data") == Status::Fail {
        return "Game data is incomplete. Every join will be refused with DATA_DIFFERS until it is \
                repaired - press Play, which re-downloads what is missing."
            .to_string();
    }
    if status_of(rows, "debug") == Status::Fail {
        return "Debug mode is on, so online and LAN are both disabled. Turn it off on the \
                DIAGNOSTICS page."
            .to_string();
    }
    if status_of(rows, "relay") == Status::Fail {
        // THE OPT-OUT IS NOT A FIREWALL, and this branch has to come first. Measured 20/08/2026:
        // with `GENERALSX_NO_RELAY` set, the row correctly reported the setting while the headline
        // above it told the reader to go and check their router - two sentences on one screen
        // disagreeing about the cause, and the wrong one in the larger type.
        if rows.iter().any(|r| r.key == "relay" && r.short == "relay=OPTED-OUT") {
            return "Your game is set to use no relay at all. That is a setting, not a network \
                    fault - remove `RelayAddress = none` from Options.ini, or unset \
                    GENERALSX_NO_RELAY, and online play comes back."
                .to_string();
        }
        return "This machine cannot reach a relay over UDP. The game needs outbound UDP, which web \
                browsing does not - check a firewall, a VPN, or a router that blocks it."
            .to_string();
    }
    if status_of(rows, "dns_dl") == Status::Fail || status_of(rows, "dns_relay") == Status::Fail {
        return "A name did not resolve. Nothing else can work until DNS does - a captive portal, a \
                VPN or an ad-blocker is the usual cause."
            .to_string();
    }
    if status_of(rows, "tcp_dl") == Status::Fail {
        return "Updates cannot be fetched: the download host is unreachable. You can still play on \
                the build you have, with people on the same one."
            .to_string();
    }

    // Nothing has failed. Only NOW does an unfinished run get to be the headline - and it must be,
    // because a pending row could still turn red and neither of the two lines below may be said
    // about a run that has not finished.
    if rows.iter().any(|r| r.status == Status::Pending) {
        return "Still checking… nothing has failed so far.".to_string();
    }
    if rows.iter().any(|r| r.status == Status::Warn) {
        return "Nothing here will stop a join, but something is worth reporting. Copy the line below \
                when you ask for help."
            .to_string();
    }
    "Everything this launcher can check is healthy. If a join still fails, the problem is on the \
     other machine or between you - send them this page too."
        .to_string()
}

/// ONE LINE, PASTEABLE, AND ENOUGH TO DIAGNOSE WITHOUT A FOLLOW-UP QUESTION.
///
/// The test it has to pass is not "does it contain the results" but "can somebody in a chat window
/// answer without asking anything else". So the tokens go in a FIXED order, independent of the order
/// results arrived in, and a reader learns the shape once.
///
/// IT CONTAINS NOTHING ABOUT THE PERSON. No public IP, no local address, no path, no user name, no
/// machine name. That is checked by a test rather than left to inspection, because this line is
/// written to be published.
pub fn copy_line(rows: &[Row], stamp_secs: u64) -> String {
    let mut parts = vec![format!("GeneralsX net test {}", utc_stamp(stamp_secs))];
    for (key, _) in ROW_ORDER {
        if let Some(r) = rows.iter().find(|r| r.key == *key) {
            if !r.short.is_empty() {
                parts.push(r.short.clone());
            }
        }
    }
    // FAIL > INCOMPLETE > WARN > OK, and the middle of that order is the part worth stating. A
    // definite failure outranks everything: it is the actionable fact and more results cannot
    // un-fail it. But INCOMPLETE outranks WARN and OK, because both of those are claims about the
    // WHOLE run - "nothing here will stop a join" cannot honestly be said while a probe that might
    // yet fail is still outstanding. Ranking WARN above INCOMPLETE would let a run that stalled on
    // the relay check get pasted into a chat window looking merely untidy, and the reader would
    // never learn that the most important row never answered.
    let worst = if rows.iter().any(|r| r.status == Status::Fail) {
        "FAIL"
    } else if rows.iter().any(|r| r.status == Status::Pending) {
        "INCOMPLETE"
    } else if rows.iter().any(|r| r.status == Status::Warn) {
        "WARN"
    } else {
        "OK"
    };
    parts.push(format!("verdict={worst}"));
    parts.join(" | ")
}

/// `YYYY-MM-DD HH:MMZ` from a Unix timestamp, with no dependency.
///
/// Howard Hinnant's civil-from-days, which is exact for every date this will ever see. A launcher
/// that printed a LOCAL time would produce a copy line whose timestamp means something different to
/// every reader of the thread it lands in, so this is UTC and says so in the string.
pub fn utc_stamp(secs: u64) -> String {
    let days = (secs / 86_400) as i64;
    let rem = secs % 86_400;

    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = if m <= 2 { y + 1 } else { y };

    format!("{y:04}-{m:02}-{d:02} {:02}:{:02}Z", rem / 3600, (rem % 3600) / 60)
}

pub fn now_secs() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

// =================================================================================================
// Running them
// =================================================================================================

enum Msg {
    Dns(Result<Resolved, ResolveErr>),
    Tcp(Result<(String, u128), String>),
    RelayResolved(Result<Resolved, ResolveErr>),
    RelayDone(usize, Box<RelayReply>),
}

/// Run every check, republishing the whole snapshot the moment a row settles.
///
/// CONCURRENT, AND EACH ROW APPEARS WHEN IT SETTLES rather than in a fixed sequence. Awaiting them
/// in order would hold a fast answer behind a slow one - a five-second DNS timeout would keep a
/// hundred-millisecond relay result off the screen - and a page that fills in out of order is
/// exactly what tells the reader the checks really are running at once.
///
/// THIS FUNCTION BLOCKS ITS OWN THREAD, deliberately: the caller runs it on a thread of its own so
/// the UI thread is never inside it. A synchronous probe on the UI thread freezes the window and
/// reads as a hang, which is the failure this whole design is arranged around.
///
/// `parse_signed` is the caller's verify-and-parse. It is the ONLY route from a datagram to a
/// `ManifestFacts`, which is what keeps the trust anchor in gxcore where it belongs.
pub fn run<F, P>(input: ProbeInputs, parse_signed: F, mut progress: P)
where
    F: Fn(&str) -> RelayDoc,
    P: FnMut(&Snapshot),
{
    let deadline = Instant::now() + RUN_CEILING;
    let stamp = now_secs();
    let mut rows = pending_rows();
    progress(&Snapshot::of(&rows, false, stamp));

    // The three that read only local disk. Instant, and deliberately first: on a machine with no
    // connection at all they are the entire result, and two of them are the most likely causes of
    // the refused join that brought the player here.
    set_row(&mut rows, version_verdict(&input.installed_commit, input.installed_ordinal, input.cached.as_ref()));
    set_row(&mut rows, data_verdict(input.patch_dir.as_deref()));
    set_row(&mut rows, debug_verdict(input.debug_menu));
    progress(&Snapshot::of(&rows, false, stamp));

    let (tx, rx) = mpsc::channel::<Msg>();
    let mut outstanding = 0usize;

    // THE DOWNLOAD HOST IS DATA. It comes out of the signed document's `base` field; with no
    // document there is no host, and the two rows say so instead of naming one.
    let dl_host = input.cached.as_ref().map(|c| c.base_host.clone()).filter(|h| !h.is_empty());
    match &dl_host {
        Some(host) => {
            let (h1, t1) = (host.clone(), tx.clone());
            std::thread::spawn(move || {
                let r = resolve(&h1, 443, DNS_TIMEOUT).map(|(a, ms)| one(&a, ms));
                let _ = t1.send(Msg::Dns(r));
            });
            let (h2, t2) = (host.clone(), tx.clone());
            std::thread::spawn(move || {
                let _ = t2.send(Msg::Tcp(tcp_reach(&h2, 443)));
            });
            outstanding += 2;
        }
        None => {
            for (key, name, short) in [
                ("dns_dl", "Download host DNS", "dns_dl=NOHOST"),
                ("tcp_dl", "Download host, TCP 443", "tcp443=NOHOST"),
            ] {
                set_row(
                    &mut rows,
                    Row::new(
                        key,
                        name,
                        Status::Warn,
                        "not checked - this machine holds no signed release document, so it does not \
                         know which host to ask. Press Play once and it will fetch one.",
                        short,
                    ),
                );
            }
            progress(&Snapshot::of(&rows, false, stamp));
        }
    }

    let chosen: Vec<RelayTarget> = input.relays.iter().take(MAX_RELAYS_PROBED).cloned().collect();
    if chosen.is_empty() {
        set_row(&mut rows, Row::new("dns_relay", "Relay DNS", Status::Warn, "no relay to look up.", "dns_relay=NOLIST"));
        set_row(&mut rows, relay_verdict(&[], input.relay_opted_out));
        set_row(&mut rows, agreement_verdict(input.cached.as_ref(), None));
        progress(&Snapshot::of(&rows, false, stamp));
    } else {
        for (i, target) in chosen.iter().enumerate() {
            let t = tx.clone();
            let target = target.clone();
            let ordinal = input.installed_ordinal;
            // Relay 0 also produces the DNS row, taken from the probe's OWN lookup rather than a
            // second one: a separate resolve would charge the page for a lookup it is not
            // measuring, and the two could disagree in front of the reader.
            let first = i == 0;
            std::thread::spawn(move || {
                let reply = probe_relay(&target, ordinal);
                if first {
                    let _ = t.send(Msg::RelayResolved(reply.resolved.clone()));
                }
                let _ = t.send(Msg::RelayDone(i, Box::new(reply)));
            });
        }
        outstanding += chosen.len() + 1; // + the DNS message from relay 0
    }

    drop(tx);

    let mut replies: Vec<Option<RelayReply>> = vec![None; chosen.len()];
    while outstanding > 0 {
        let left = deadline.saturating_duration_since(Instant::now());
        if left.is_zero() {
            break;
        }
        let Ok(msg) = rx.recv_timeout(left) else { break };
        outstanding -= 1;
        match msg {
            Msg::Dns(r) => {
                let host = dl_host.clone().unwrap_or_default();
                set_row(&mut rows, dns_row("dns_dl", "Download host DNS", &host, &r));
            }
            Msg::Tcp(r) => {
                let host = dl_host.clone().unwrap_or_default();
                set_row(&mut rows, tcp_row(&host, &r));
            }
            Msg::RelayResolved(r) => {
                let host = chosen.first().map(|t| t.host.clone()).unwrap_or_default();
                set_row(&mut rows, dns_row("dns_relay", "Relay DNS", &host, &r));
            }
            Msg::RelayDone(i, reply) => {
                if let Some(slot) = replies.get_mut(i) {
                    *slot = Some(*reply);
                }
                // Republished on every arrival, so a first answer shows up immediately instead of
                // waiting on the slowest of three.
                set_row(&mut rows, relay_verdict(&pairs(&chosen, &replies), input.relay_opted_out));
            }
        }
        progress(&Snapshot::of(&rows, false, stamp));
    }

    // Anything still pending ran out of time. It is a WARN and it names itself: "did not finish" and
    // "failed" are different facts, and only one of them is about the network.
    for row in rows.iter_mut().filter(|r| r.status == Status::Pending) {
        row.status = Status::Warn;
        row.value = format!("did not finish within {} s.", RUN_CEILING.as_secs());
        row.short = format!("{}=TIMEOUT", row.key);
    }

    if !chosen.is_empty() {
        let done = pairs(&chosen, &replies);
        set_row(&mut rows, relay_verdict(&done, input.relay_opted_out));

        // The fastest answer's document, verified by the caller. A relay that answered with
        // something that does not verify is reported as exactly that, never as agreement.
        let best = done.iter().filter(|(_, r)| r.answered).min_by_key(|(_, r)| r.ms);
        let doc = best.map(|(_, r)| parse_signed(&r.wire));
        set_row(&mut rows, agreement_verdict(input.cached.as_ref(), doc.as_ref()));
    }

    progress(&Snapshot::of(&rows, true, stamp));
}

fn pairs(chosen: &[RelayTarget], replies: &[Option<RelayReply>]) -> Vec<(RelayTarget, RelayReply)> {
    chosen
        .iter()
        .cloned()
        .zip(replies.iter().cloned())
        .filter_map(|(t, r)| r.map(|r| (t, r)))
        .collect()
}

// =================================================================================================
// Plumbing
// =================================================================================================

fn one(addrs: &[SocketAddr], ms: u128) -> Resolved {
    Resolved { ip: addrs[0].ip().to_string(), count: addrs.len(), ms }
}

/// Resolve a host, racing the platform resolver against a timer.
///
/// WHY NOT JUST CALL `to_socket_addrs`. It has no timeout and is not interruptible: a wedged
/// resolver, a captive portal, or a VPN that has just gone down parks the calling thread for as long
/// as the OS feels like, and a row stuck on "checking…" is the exact failure this page must not
/// have. The loser is ABANDONED rather than awaited - it holds one resolver handle until it finishes
/// and then goes away, which is a bounded cost paid only when something is already wrong.
///
/// A COMPLETED LOOKUP IS NEVER CACHED. "What does this name resolve to right now" is the question,
/// and a captive portal that has just taken over the resolver is precisely the case worth catching.
pub fn resolve(host: &str, port: u16, limit: Duration) -> Result<(Vec<SocketAddr>, u128), ResolveErr> {
    let (tx, rx) = mpsc::channel();
    let owned = host.to_string();
    let started = Instant::now();
    std::thread::spawn(move || {
        let r = (owned.as_str(), port).to_socket_addrs().map(|it| it.collect::<Vec<SocketAddr>>());
        // The receiver may already have given up. That is the normal abandoned-loser path, not an
        // error, so the result is dropped rather than unwrapped.
        let _ = tx.send(r.map_err(|e| reason(&e)));
    });

    match rx.recv_timeout(limit) {
        Ok(Ok(v)) if v.is_empty() => Err(ResolveErr::Empty),
        Ok(Ok(v)) => Ok((v, started.elapsed().as_millis())),
        Ok(Err(why)) => Err(ResolveErr::Failed(why)),
        Err(_) => Err(ResolveErr::Timeout),
    }
}

/// Open a TCP connection and close it. Transport reachability only - see `tcp_row`.
pub fn tcp_reach(host: &str, port: u16) -> Result<(String, u128), String> {
    let started = Instant::now();
    let (addrs, _) = resolve(host, port, DNS_TIMEOUT).map_err(|e| match e {
        ResolveErr::Timeout => "the name did not resolve in time".to_string(),
        ResolveErr::Empty => "the name resolved to nothing".to_string(),
        ResolveErr::Failed(w) => w,
    })?;
    let addr = addrs.iter().find(|a| matches!(a.ip(), IpAddr::V4(_))).copied().unwrap_or(addrs[0]);
    let left = TCP_TIMEOUT.saturating_sub(started.elapsed());
    if left.is_zero() {
        return Err("timed out".to_string());
    }
    match TcpStream::connect_timeout(&addr, left) {
        Ok(_) => Ok((addr.ip().to_string(), started.elapsed().as_millis())),
        Err(e) => Err(reason(&e)),
    }
}

fn is_timeout(e: &io::Error) -> bool {
    matches!(e.kind(), io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut)
}

/// Windows reports `ConnectionReset` and the BSD/Linux stacks report `ConnectionRefused` for the
/// same event: an ICMP port-unreachable delivered back to a connected UDP socket.
fn is_unreachable(e: &io::Error) -> bool {
    matches!(e.kind(), io::ErrorKind::ConnectionRefused | io::ErrorKind::ConnectionReset)
}

/// A short cause for a row, WITHOUT the platform's own message text.
///
/// System messages name paths, proxy hosts and sometimes the local address - all of which this page
/// has undertaken not to publish, and the copy line is pasted in public. A person reading "the
/// connection was refused" can act; a person reading a raw errno sentence cannot.
fn reason(e: &io::Error) -> String {
    match e.kind() {
        io::ErrorKind::TimedOut | io::ErrorKind::WouldBlock => "timed out",
        io::ErrorKind::ConnectionRefused => "the connection was refused",
        io::ErrorKind::ConnectionReset => "the connection was reset",
        io::ErrorKind::PermissionDenied => "permission was denied",
        io::ErrorKind::AddrNotAvailable => "the address is not available",
        io::ErrorKind::NotFound => "the name did not resolve",
        _ => "it failed",
    }
    .to_string()
}

// =================================================================================================
// Reading the game's own settings
//
// TWO KEYS AND NOTHING ELSE, both read-only. Options.ini belongs to the game and is shared with
// retail Zero Hour and with the Generals Online client; this module opens it, looks at two lines and
// closes it. Writing either belongs to whoever owns the settings screens.
// =================================================================================================

/// What the game's own Options.ini says about a relay.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RelayConfigured {
    /// Options.ini could not be read.
    NoFile,
    /// No `RelayAddress` line, or a blank one. The engine falls back to its own compiled-in
    /// bootstrap, which this launcher deliberately does not name.
    NotSet,
    /// `RelayAddress = none`, or `GENERALSX_NO_RELAY` in the environment. A deliberate opt-out with
    /// a real consequence.
    Disabled,
    Host(String),
}

/// Whether an Options.ini is there to be read at all.
///
/// `gxio::options::load_from` answers "no file" and "empty file" with the same empty `Options`,
/// which is right for a screen showing settings and wrong here: this module has to tell "the key
/// says off" from "there was nothing to look in", because only the first is a measurement.
fn options_readable(path: Option<&Path>) -> bool {
    path.map(|p| std::fs::metadata(p).is_ok()).unwrap_or(false)
}

/// Read `RelayAddress` with the engine's own semantics (`OptionPreferences::getRelayAddress`).
///
/// A HOSTNAME, NOT AN ADDRESS, and never one this file supplies. The engine treats an absent or
/// blank key as "use the compiled-in bootstrap"; this launcher does NOT reproduce that constant,
/// because writing a host into a source file is the thing the whole design forbids. Absent here
/// means "no relay from this source", and the screen says so.
///
/// THE LOOKUP IS `gxio::options`, not a scan written here. That module already mirrors
/// `UserPreferences::load` exactly - exact case, LAST occurrence wins, an empty value never enters
/// the map - and a first-occurrence case-insensitive scan (which is what this function did in its
/// first draft) would report a value the engine is not using.
pub fn read_relay_configured(options_ini: Option<&Path>) -> RelayConfigured {
    // Checked BEFORE the file, exactly as the engine does, so a sandbox can force "no relay" over an
    // Options.ini it inherited or one a previous run left behind.
    if let Some(v) = std::env::var_os("GENERALSX_NO_RELAY") {
        let v = v.to_string_lossy().to_string();
        if !v.is_empty() && v != "0" {
            return RelayConfigured::Disabled;
        }
    }
    if !options_readable(options_ini) {
        return RelayConfigured::NoFile;
    }
    let opts = crate::options::load_from(options_ini.expect("checked just above"));
    // `get` already treats an empty value as absent, which is the engine's own rule and produces
    // the same outcome the engine reaches for a blank line: fall back to the bootstrap, which this
    // launcher will not name.
    let Some(v) = opts.get("RelayAddress").map(str::trim).filter(|v| !v.is_empty()) else {
        return RelayConfigured::NotSet;
    };
    if v.eq_ignore_ascii_case("none") {
        return RelayConfigured::Disabled;
    }
    RelayConfigured::Host(v.to_string())
}

/// `DebugMenu` from the game's Options.ini.
///
/// `None` is "could not read the file", which is NOT the same as off and must not render as a pass.
/// A file that exists without the key IS off - the engine matches only "yes" - so that case is a
/// definite `Some(false)`.
pub fn read_debug_menu(options_ini: Option<&Path>) -> Option<bool> {
    if !options_readable(options_ini) {
        return None;
    }
    let opts = crate::options::load_from(options_ini.expect("checked just above"));
    Some(opts.get(crate::options::KEY_DEBUG_MENU).is_some_and(|v| v.trim().eq_ignore_ascii_case("yes")))
}

/// Turn `RelayAddress` into something probeable, or nothing.
///
/// The engine parses this as a bare hostname and uses the default port; a `host:port` spelling is
/// accepted too, because an operator will type one sooner or later and silently probing the wrong
/// port would be a lie in a diagnostic.
pub fn relay_from_config(cfg: &RelayConfigured) -> Option<RelayTarget> {
    let RelayConfigured::Host(raw) = cfg else { return None };
    let (host, port) = match raw.rsplit_once(':') {
        Some((h, p)) if !h.is_empty() && !p.is_empty() && p.chars().all(|c| c.is_ascii_digit()) => {
            (h.to_string(), p.parse().unwrap_or(DEFAULT_RELAY_PORT))
        }
        _ => (raw.clone(), DEFAULT_RELAY_PORT),
    };
    if host.is_empty() {
        return None;
    }
    Some(RelayTarget { host, port, region: String::new(), source: RelaySource::GameConfig })
}

/// `GX_RELAY_LIST`, as `host[:port][:region];…`.
///
/// A test hook, and the reason no host appears in this file or its tests: the suite supplies its own
/// and a developer can point the page at a local relay without editing anything.
pub fn relays_from_env() -> Vec<RelayTarget> {
    let Some(raw) = std::env::var_os("GX_RELAY_LIST") else { return Vec::new() };
    parse_relay_list(&raw.to_string_lossy())
}

pub fn parse_relay_list(spec: &str) -> Vec<RelayTarget> {
    spec.split(';')
        .filter_map(|item| {
            let item = item.trim();
            if item.is_empty() {
                return None;
            }
            let mut p = item.split(':');
            let host = p.next()?.trim().to_string();
            if host.is_empty() {
                return None;
            }
            let port = p.next().and_then(|s| s.trim().parse::<u16>().ok()).unwrap_or(DEFAULT_RELAY_PORT);
            let region = p.next().unwrap_or("").trim().to_string();
            Some(RelayTarget { host, port, region, source: RelaySource::Env })
        })
        .collect()
}

/// Pull the host out of a manifest `base` field, which is a prefix like `host/` or
/// `https://host/path/`.
///
/// Returns an empty string when there is nothing host-shaped in it, which the caller renders as "no
/// download host known" rather than inventing one.
pub fn base_host(base: &str) -> String {
    let s = base.trim();
    let s = s.strip_prefix("https://").or_else(|| s.strip_prefix("http://")).unwrap_or(s);
    let s = s.split('/').next().unwrap_or("");
    // Credentials and a port would both be wrong here: this is fed to a resolver, not to a URL.
    let s = s.rsplit('@').next().unwrap_or(s);
    let s = s.split(':').next().unwrap_or("");
    if s.is_empty() || s.contains(' ') {
        return String::new();
    }
    s.to_string()
}

// =================================================================================================
// The DIAGNOSTICS facts
//
// EVERY FIELD IS EITHER MEASURED OR `None`. There is no default, no placeholder and no "probably". A
// confident wrong number in a launcher is worse than an obviously-absent one, and these are the
// numbers somebody quotes when asking why a join failed - the argument AboutPage.cs already makes
// about its own four.
// =================================================================================================

/// Free bytes on the volume holding `path`.
///
/// `None` is a normal answer and nothing on either screen depends on having it.
pub fn free_space_bytes(path: &Path) -> Option<u64> {
    #[cfg(target_os = "windows")]
    {
        use std::os::windows::ffi::OsStrExt;
        #[link(name = "kernel32")]
        extern "system" {
            fn GetDiskFreeSpaceExW(
                lp_directory_name: *const u16,
                lp_free_bytes_available_to_caller: *mut u64,
                lp_total_number_of_bytes: *mut u64,
                lp_total_number_of_free_bytes: *mut u64,
            ) -> i32;
        }
        let mut wide: Vec<u16> = path.as_os_str().encode_wide().collect();
        wide.push(0);
        let mut avail: u64 = 0;
        // FreeBytesAvailableToCaller, not TotalNumberOfFreeBytes: on a volume with a quota the two
        // differ, and the one that matters is what THIS user may actually write.
        let ok = unsafe {
            GetDiskFreeSpaceExW(wide.as_ptr(), &mut avail, std::ptr::null_mut(), std::ptr::null_mut())
        };
        if ok == 0 {
            return None;
        }
        Some(avail)
    }

    #[cfg(not(target_os = "windows"))]
    {
        // `df -Pk` rather than statvfs, because gxio declares no dependencies and hand-rolling the
        // statvfs struct layout for three platforms is a portability bug waiting to happen. `-P` is
        // POSIX's "one line per filesystem, never wrapped" and `-k` fixes the block size at 1024, so
        // the parse does not depend on the environment's BLOCKSIZE.
        let out = std::process::Command::new("/bin/df").arg("-Pk").arg(path).output().ok()?;
        if !out.status.success() {
            return None;
        }
        let text = String::from_utf8_lossy(&out.stdout);
        let line = text.lines().nth(1)?;
        let fields: Vec<&str> = line.split_whitespace().collect();
        if fields.len() < 4 {
            return None;
        }
        fields[3].parse::<u64>().ok().map(|kb| kb.saturating_mul(1024))
    }
}

/// Human bytes. One decimal above a gigabyte so the number does not jitter between runs.
pub fn human_bytes(bytes: u64) -> String {
    const GB: u64 = 1024 * 1024 * 1024;
    const MB: u64 = 1024 * 1024;
    if bytes >= GB {
        format!("{:.1} GB", bytes as f64 / GB as f64)
    } else if bytes >= MB {
        format!("{} MB", bytes / MB)
    } else {
        format!("{} KB", bytes / 1024)
    }
}

/// One file or folder the DIAGNOSTICS page may reveal in a file manager.
///
/// A SYMBOLIC NAME, NEVER A PATH FROM THE WEB LAYER. The frontend cannot name a location this
/// process will open, so no page bug and no injected string can turn "show my log" into "open
/// anything on this disk".
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Reveal {
    Log,
    CrashDumps,
    GameFolder,
    UserData,
}

impl Reveal {
    pub fn parse(name: &str) -> Option<Reveal> {
        match name {
            "log" => Some(Reveal::Log),
            "dumps" => Some(Reveal::CrashDumps),
            "game" => Some(Reveal::GameFolder),
            "userdata" => Some(Reveal::UserData),
            _ => None,
        }
    }
}

/// Which log the "Files to send" row is actually pointing at.
///
/// THE ARTBOARD SAYS "Launcher log" AND NAMES `gxlaunch.log`. That file is written by the WINDOWS C
/// updater; nothing in this Rust launcher writes a log at all today, so on a machine that has never
/// run the Windows updater there is no such file. Rather than print a path to something that does
/// not exist, this reports which log was actually found and lets the screen label it correctly - and
/// `NoLog` is a real answer that the screen states plainly.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LogKind {
    /// `gxlaunch.log`, from the Windows updater.
    Launcher,
    /// `GeneralsX.log`, written by the engine into the user-data folder.
    Engine,
    /// Neither exists. The path reported is where a launcher log would go if one were written.
    None,
}

#[derive(Debug, Clone)]
pub struct DiagFacts {
    pub platform: &'static str,
    pub arch: &'static str,

    /// From `gxio::game::discover` - one discovery, shared with the General page.
    pub install_dir: Option<PathBuf>,
    pub install_how: String,
    pub data_dir: Option<PathBuf>,
    pub data_how: String,
    pub patch_dir: Option<PathBuf>,
    pub patch_how: String,
    pub required_total: usize,
    pub required_present: usize,
    /// False means the mandatory set could not be read, which is NOT "nothing is missing".
    pub required_known: bool,

    pub user_data: Option<PathBuf>,
    pub user_data_exists: bool,

    /// From BUILD.txt. Empty when this is not a packaged install.
    pub engine_commit: String,
    pub engine_packaged: String,
    /// From gxinstalled.txt via `gxcore::InstallState`, filled in by the caller. 0 = no record.
    pub installed_ordinal: u64,

    pub log_kind: LogKind,
    pub log_path: Option<PathBuf>,
    pub log_bytes: Option<u64>,
    pub crash_dumps: Option<PathBuf>,
    /// `None` when the folder does not exist - which is the normal, healthy case.
    pub crash_dump_count: Option<usize>,

    pub free_bytes: Option<u64>,
    pub free_on: Option<PathBuf>,

    pub debug_menu: Option<bool>,
    pub relay_configured: RelayConfigured,
    pub manifest_cache: Option<PathBuf>,
    pub state_dir: Option<PathBuf>,
}

/// Measure everything the DIAGNOSTICS page shows. Never fails: "nothing is installed" and "no
/// network" are ordinary states, and a page that errored on either would have nothing to show the
/// person it exists for.
pub fn collect_facts() -> DiagFacts {
    let status = crate::game::discover();
    let user_data = crate::release::game_data_dir();
    let user_data_exists = user_data.as_deref().map(Path::is_dir).unwrap_or(false);
    // The Options.ini path comes from `gxio::options`, which is the one place it is spelled.
    let options_ini = crate::options::options_path();

    let (required_total, required_present, required_known) = match status.patch_dir.as_deref() {
        Some(dir) => {
            let req = crate::game::read_required(dir);
            if req.is_empty() {
                (0, 0, false)
            } else {
                let missing = crate::game::missing_archives(dir, &req).len();
                (req.len(), req.len() - missing, true)
            }
        }
        None => (0, 0, false),
    };

    let stamp = crate::release::engine_build_stamp().unwrap_or_default();

    // The log. `gxlaunch.log` first because that is what the artboard names and what the Windows
    // updater writes; the engine's own `GeneralsX.log` second, because on macOS and Linux it is the
    // only log that exists and it is the one worth sending.
    let mut launcher_candidates: Vec<PathBuf> = Vec::new();
    if let Some(p) = crate::release::cache_path("gxlaunch.log") {
        launcher_candidates.push(p);
    }
    launcher_candidates.extend(crate::release::install_dirs().into_iter().map(|d| d.join("gxlaunch.log")));
    let launcher_log = launcher_candidates.iter().find(|p| p.is_file()).cloned();
    let engine_log = user_data.as_ref().map(|d| d.join("GeneralsX.log")).filter(|p| p.is_file());

    let (log_kind, log_path) = match (launcher_log, engine_log) {
        (Some(p), _) => (LogKind::Launcher, Some(p)),
        (None, Some(p)) => (LogKind::Engine, Some(p)),
        (None, None) => (LogKind::None, launcher_candidates.first().cloned()),
    };
    let log_bytes = if log_kind == LogKind::None {
        None
    } else {
        log_path.as_ref().and_then(|p| std::fs::metadata(p).ok()).map(|m| m.len())
    };

    // CrashDumps, where `MiniDumper::InitializeDumpDirectory` puts them: the engine's user-data
    // folder plus "CrashDumps". NOT the artboard's "dumps", which matches nothing on disk.
    let crash_dumps = user_data.as_ref().map(|d| d.join("CrashDumps"));
    let crash_dump_count = crash_dumps.as_ref().and_then(|d| {
        std::fs::read_dir(d)
            .ok()
            .map(|it| it.filter_map(|e| e.ok()).filter(|e| e.path().is_file()).count())
    });

    // The volume the game is on, because that is the one an update writes to. With no install, the
    // launcher's own state folder is the honest second choice - it is where a download would land.
    let free_on = status.install_dir.clone().or_else(crate::state::state_dir);
    let free_bytes = free_on.as_deref().and_then(free_space_bytes);

    DiagFacts {
        platform: crate::release::platform_token(),
        arch: std::env::consts::ARCH,
        install_dir: status.install_dir,
        install_how: status.install_how,
        data_dir: status.data_dir,
        data_how: status.data_how,
        patch_dir: status.patch_dir,
        patch_how: status.patch_how,
        required_total,
        required_present,
        required_known,
        user_data_exists,
        debug_menu: read_debug_menu(options_ini.as_deref()),
        relay_configured: read_relay_configured(options_ini.as_deref()),
        manifest_cache: crate::release::read_cached(
            crate::release::MANIFEST_CACHE,
            crate::release::MANIFEST_MAX_BYTES,
        )
        .map(|(p, _)| p),
        state_dir: crate::state::state_dir(),
        engine_commit: stamp.commit,
        engine_packaged: stamp.packaged,
        installed_ordinal: 0,
        log_kind,
        log_path,
        log_bytes,
        crash_dumps,
        crash_dump_count,
        free_bytes,
        free_on,
        user_data,
    }
}

// =================================================================================================
// Tests
// =================================================================================================

#[cfg(test)]
mod tests {
    use super::*;

    /// Diagnostics names the engine's packaged build through the SAME per-platform rule as every
    /// other screen (`release::engine_build_stamp`): never a BUILD.txt on macOS, only the discovered
    /// engine folder's on Linux. It used to read every install directory on every platform.
    #[test]
    fn diagnostics_reads_build_txt_through_the_per_platform_rule() {
        let src = include_str!("netdiag.rs");
        let at = src.find("pub fn collect_facts() -> DiagFacts {").expect("collect_facts is gone");
        let body = &src[at..at + src[at..].find("\n}\n").expect("collect_facts closes in column 0")];
        assert!(
            body.contains("crate::release::engine_build_stamp()") && !body.contains("find_build_stamp"),
            "Diagnostics reads BUILD.txt around the per-platform rule"
        );
    }
    use std::sync::atomic::{AtomicUsize, Ordering};

    fn scratch(tag: &str) -> PathBuf {
        static N: AtomicUsize = AtomicUsize::new(0);
        let d = std::env::temp_dir().join(format!(
            "gxnetdiag-{}-{}-{}",
            std::process::id(),
            tag,
            N.fetch_add(1, Ordering::SeqCst)
        ));
        std::fs::create_dir_all(&d).expect("a temp directory");
        d
    }

    fn facts(ordinal: u64, minimum: u64, commit: &str) -> ManifestFacts {
        ManifestFacts { ordinal, minimum, release_commit: commit.into(), ..Default::default() }
    }

    fn target(host: &str, region: &str) -> RelayTarget {
        RelayTarget {
            host: host.into(),
            port: DEFAULT_RELAY_PORT,
            region: region.into(),
            source: RelaySource::Signed,
        }
    }

    fn answered(ms: u128) -> RelayReply {
        RelayReply {
            answered: true,
            ms,
            wire: String::new(),
            refused: String::new(),
            resolved: Err(ResolveErr::Timeout),
        }
    }

    fn silent(refused: &str) -> RelayReply {
        RelayReply {
            answered: false,
            ms: 0,
            wire: String::new(),
            refused: refused.into(),
            resolved: Err(ResolveErr::Timeout),
        }
    }

    // --- check 1 ---------------------------------------------------------------------------------

    #[test]
    fn no_document_is_unverified_never_up_to_date() {
        // "Up to date" is a promise that nothing will download, and an unanswered check cannot make
        // it. This is the exact phrasing bug UpdateStatus.cs records.
        let r = version_verdict("abc1234", 100, None);
        assert_eq!(r.status, Status::Warn);
        assert!(r.value.contains("could not verify"));
        assert!(!r.value.contains("current."));
    }

    #[test]
    fn an_install_with_no_record_is_unmeasured_not_behind() {
        let r = version_verdict("abc1234", 0, Some(&facts(200, 0, "def5678")));
        assert_eq!(r.status, Status::Warn, "no record is not evidence of being behind");
        assert!(r.short.contains("NORECORD"));
    }

    #[test]
    fn below_the_minimum_outranks_merely_behind() {
        let r = version_verdict("abc1234", 100, Some(&facts(300, 200, "def5678")));
        assert_eq!(r.status, Status::Fail);
        assert!(r.short.contains("BELOW-MINIMUM"), "the release declares it incompatible, which is stronger");
    }

    #[test]
    fn behind_names_both_commits_so_two_players_can_compare() {
        let r = version_verdict("abc1234", 100, Some(&facts(300, 0, "def5678")));
        assert_eq!(r.status, Status::Fail);
        assert!(r.value.contains("abc1234") && r.value.contains("def5678"));
    }

    #[test]
    fn current_passes_and_says_it_is_not_the_cause() {
        let r = version_verdict("abc1234", 300, Some(&facts(300, 0, "abc1234")));
        assert_eq!(r.status, Status::Pass);
        assert!(r.value.contains("not why a join is failing"));
    }

    #[test]
    fn an_install_ahead_of_the_document_is_not_reported_as_behind() {
        // A tester on a local build is ahead of the published ordinal. Calling that "out of date"
        // would send them to re-download a release older than what they are running.
        assert_eq!(version_verdict("abc1234", 500, Some(&facts(300, 0, "def5678"))).status, Status::Pass);
    }

    // --- check 2 ---------------------------------------------------------------------------------

    #[test]
    fn no_required_list_is_cannot_check_not_a_pass() {
        let r = data_verdict(Some(&scratch("nolist")));
        assert_eq!(r.status, Status::Warn);
        assert!(r.short.contains("UNKNOWN"));
    }

    #[test]
    fn no_install_at_all_is_cannot_check() {
        let r = data_verdict(None);
        assert_eq!(r.status, Status::Warn);
        assert!(r.short.contains("NOINSTALL"));
    }

    #[test]
    fn a_zero_length_archive_counts_as_missing() {
        // An interrupted copy and an antivirus that emptied a file both leave a name an existence
        // check is happy with, and the engine mounts an empty archive as nothing at all.
        let d = scratch("zerolen");
        std::fs::write(d.join("required.txt"), "one.big\ntwo.big\n").unwrap();
        std::fs::write(d.join("one.big"), b"content").unwrap();
        std::fs::write(d.join("two.big"), b"").unwrap();
        let r = data_verdict(Some(&d));
        assert_eq!(r.status, Status::Fail);
        assert!(r.value.contains("two.big"));
        assert!(r.short.contains("1/2"));
    }

    #[test]
    fn a_long_missing_list_is_summarised_rather_than_spelled_out() {
        // An empty overlay folder names every required archive, and a sentence carrying fourteen
        // filenames pushes the rest of a fixed-height page off the screen.
        let d = scratch("manymissing");
        let names: Vec<String> = (0..14).map(|i| format!("archive{i}.big")).collect();
        std::fs::write(d.join("required.txt"), names.join("\n")).unwrap();
        let r = data_verdict(Some(&d));
        assert_eq!(r.status, Status::Fail);
        assert!(r.value.contains("and 11 more"), "{}", r.value);
        assert!(r.value.matches(".big").count() <= NAMES_SHOWN, "{}", r.value);
        // The COUNT still has to be exact - it is what tells the reader the scale.
        assert!(r.short.contains("0/14"), "{}", r.short);
    }

    #[test]
    fn a_missing_hd_texture_pack_alone_warns_and_never_predicts_data_differs() {
        // Codex batch-0964 r3b P3: !!GXPropsHD.big is on required.txt (shipped to everyone) but does not move dataID
        let d = scratch("hdonly");
        std::fs::write(d.join("required.txt"), "one.big\n!!GXPropsHD.big\n").unwrap();
        std::fs::write(d.join("one.big"), b"x").unwrap();
        let r = data_verdict(Some(&d));
        assert_eq!(r.status, Status::Warn, "{}", r.value);
        assert!(!r.value.contains("DATA_DIFFERS"), "{}", r.value);
        assert!(r.short.contains("HD-MISSING"), "{}", r.short);
        // a real data archive missing beside it is still the failure it always was
        std::fs::remove_file(d.join("one.big")).unwrap();
        let r = data_verdict(Some(&d));
        assert_eq!(r.status, Status::Fail);
        assert!(r.value.contains("DATA_DIFFERS") && r.value.contains("one.big"), "{}", r.value);
    }

    #[test]
    fn a_missing_hd_model_pack_alone_warns_and_never_predicts_data_differs() {
        // E · look-packs-0965: !!GXModelsHD.big is the same dated exception (art only, no dataID)
        let d = scratch("hdmodels");
        std::fs::write(d.join("required.txt"), "one.big\n!!GXPropsHD.big\n!!GXModelsHD.big\n").unwrap();
        std::fs::write(d.join("one.big"), b"x").unwrap();
        std::fs::write(d.join("!!GXPropsHD.big"), b"x").unwrap();
        let r = data_verdict(Some(&d));
        assert_eq!(r.status, Status::Warn, "{}", r.value);
        assert!(!r.value.contains("DATA_DIFFERS") && r.value.contains("!!GXModelsHD.big"), "{}", r.value);
        assert!(r.short.contains("HD-MISSING"), "{}", r.short);
    }

    #[test]
    fn a_complete_install_passes() {
        let d = scratch("complete");
        std::fs::write(d.join("required.txt"), "one.big\n").unwrap();
        std::fs::write(d.join("one.big"), b"x").unwrap();
        assert_eq!(data_verdict(Some(&d)).status, Status::Pass);
    }

    // --- check 3 ---------------------------------------------------------------------------------

    #[test]
    fn an_unreadable_options_ini_is_a_warning_not_a_pass() {
        assert_eq!(debug_verdict(None).status, Status::Warn);
        assert_eq!(debug_verdict(Some(false)).status, Status::Pass);
        assert_eq!(debug_verdict(Some(true)).status, Status::Fail);
    }

    #[test]
    fn a_file_without_the_key_is_definitely_off() {
        // The engine matches only "yes", so an Options.ini with no DebugMenu line IS off. That is a
        // measurement, unlike an unreadable file.
        let ini = scratch("debugkey").join("Options.ini");
        std::fs::write(&ini, "Resolution = 1920 1080\n").unwrap();
        assert_eq!(read_debug_menu(Some(&ini)), Some(false));
        std::fs::write(&ini, "DebugMenu = yes\n").unwrap();
        assert_eq!(read_debug_menu(Some(&ini)), Some(true));
        std::fs::write(&ini, "DebugMenu = no\n").unwrap();
        assert_eq!(read_debug_menu(Some(&ini)), Some(false));
        assert_eq!(read_debug_menu(Some(&scratch("nofile").join("Options.ini"))), None, "no file is unknown");
        assert_eq!(read_debug_menu(None), None);
    }

    #[test]
    fn the_engines_own_lookup_rules_are_used_not_a_loose_scan() {
        // Exact case and LAST occurrence wins, because that is what `UserPreferences::load` builds
        // and what the engine will therefore act on. A first-occurrence case-insensitive scan - the
        // first draft of this reader - reports a value the game is not using.
        let ini = scratch("lookup").join("Options.ini");
        std::fs::write(&ini, "DebugMenu = yes\nDebugMenu = no\n").unwrap();
        assert_eq!(read_debug_menu(Some(&ini)), Some(false), "the last line is the one the engine sees");
        std::fs::write(&ini, "debugmenu = yes\n").unwrap();
        assert_eq!(read_debug_menu(Some(&ini)), Some(false), "the engine's map would never find that line");
    }

    // --- checks 4/5 ------------------------------------------------------------------------------

    #[test]
    fn an_ip_literal_host_is_not_called_a_dns_lookup() {
        let ok = Ok(Resolved { ip: "198.51.100.7".to_string(), count: 1, ms: 0 });
        let r = dns_row("dns_relay", "Relay DNS", "198.51.100.7", &ok);
        assert_eq!(r.name, "Relay address");
        assert!(r.value.contains("no DNS lookup needed") && !r.value.contains("->"), "{}", r.value);
        assert_eq!(r.short, "dns_relay=198.51.100.7(literal)");
        // a hostname keeps the lookup wording
        let h = dns_row("dns_relay", "Relay DNS", "relay.example", &ok);
        assert_eq!(h.name, "Relay DNS");
        assert!(h.value.contains("relay.example ->"));
    }

    #[test]
    fn the_dns_row_shows_the_far_ends_address_and_counts_the_rest() {
        let ok = Ok(Resolved { ip: "203.0.113.7".into(), count: 3, ms: 12 });
        let r = dns_row("dns_dl", "Download host DNS", "host.example", &ok);
        assert_eq!(r.status, Status::Pass);
        assert!(r.value.contains("203.0.113.7") && r.value.contains("+2 more"));
    }

    #[test]
    fn a_timed_out_lookup_names_the_usual_causes() {
        let r = dns_row("dns_dl", "Download host DNS", "host.example", &Err(ResolveErr::Timeout));
        assert_eq!(r.status, Status::Fail);
        assert!(r.value.contains("captive portal"));
    }

    #[test]
    fn the_tcp_row_never_claims_to_have_spoken_tls() {
        let r = tcp_row("host.example", &Ok(("203.0.113.7".into(), 41)));
        assert!(r.value.contains("does not speak TLS"));
        assert!(!r.name.contains("HTTPS"), "a row is named for what it measured");
    }

    // --- check 6 ---------------------------------------------------------------------------------

    #[test]
    fn no_relay_list_is_a_warning_and_names_the_way_out() {
        let r = relay_verdict(&[], false);
        assert_eq!(r.status, Status::Warn, "having no list is not the same as being blocked");
        assert!(r.short.contains("NOLIST"));
    }

    #[test]
    fn an_explicit_opt_out_is_reported_as_itself() {
        // `RelayAddress = none` looks exactly like a broken list unless it is named.
        let r = relay_verdict(&[], true);
        assert_eq!(r.status, Status::Fail);
        assert!(r.value.contains("RelayAddress = none"));
    }

    #[test]
    fn an_opt_out_beats_a_relay_that_answered() {
        // MEASURED 20/08/2026 AND WRONG: with a signed relay list on disk the probe answered in
        // 63 ms and this row went green, on a machine whose engine was configured to use no relay.
        // The launcher's reachability is not the question - the game will not contact one either
        // way - and a green row there sends somebody to look at their router.
        let r = relay_verdict(&[(target("a.example", "eu"), answered(63))], true);
        assert_eq!(r.status, Status::Fail, "a reachable relay does not undo the setting");
        assert_eq!(r.short, "relay=OPTED-OUT");
        // The measurement survives as a parenthetical, because "the setting is off and the network
        // is fine" is a different conversation from "both are broken".
        assert!(r.value.contains("63 ms"), "{}", r.value);
    }

    #[test]
    fn every_relay_silent_is_a_block_and_the_row_still_renders() {
        // The path this page exists for. It must not be the one that panics - which is exactly what
        // the C# original did here before its own test caught it.
        let results =
            vec![(target("a.example", "eu"), silent("no reply")), (target("b.example", "us"), silent("no reply"))];
        let r = relay_verdict(&results, false);
        assert_eq!(r.status, Status::Fail);
        assert!(r.value.contains("OUTBOUND UDP"));
        assert!(r.short.contains("0/2"));
    }

    #[test]
    fn a_specific_refusal_beats_the_count() {
        let results = vec![
            (target("a.example", "eu"), silent("no reply")),
            (target("b.example", "us"), silent("the host answered, but nothing is listening on UDP 8086")),
        ];
        assert!(relay_verdict(&results, false).value.contains("nothing is listening"));
    }

    #[test]
    fn the_bands_are_the_engines_own_and_neither_is_a_fail() {
        assert_eq!(relay_verdict(&[(target("a.example", "eu"), answered(40))], false).status, Status::Pass);
        assert_eq!(relay_verdict(&[(target("a.example", "eu"), answered(120))], false).status, Status::Warn);
        assert_eq!(
            relay_verdict(&[(target("a.example", "eu"), answered(400))], false).status,
            Status::Warn,
            "high latency is a slow match, not a refused join"
        );
    }

    #[test]
    fn the_fastest_answering_relay_is_the_one_reported() {
        let results =
            vec![(target("a.example", "us"), answered(300)), (target("b.example", "eu"), answered(20))];
        let r = relay_verdict(&results, false);
        assert!(r.value.contains("20 ms to eu"), "{}", r.value);
        assert!(r.short.contains("2/2"));
    }

    #[test]
    fn an_unsigned_address_says_where_it_came_from() {
        let mut t = target("a.example", "");
        t.source = RelaySource::GameConfig;
        let r = relay_verdict(&[(t, answered(30))], false);
        assert!(
            r.value.contains("Options.ini"),
            "the reader must be able to tell a published relay from a configured one"
        );
    }

    // --- check 7 ---------------------------------------------------------------------------------

    #[test]
    fn agreement_is_untested_when_nothing_answered() {
        assert_eq!(agreement_verdict(Some(&facts(1, 0, "")), None).short, "agree=UNTESTED");
    }

    #[test]
    fn nothing_to_serve_is_never_reported_as_a_signature_problem() {
        // `GXMANI -` is an ANSWER. Reporting it as a bad signature is the one message that makes
        // somebody suspect their install is tampered with when it is fine.
        let r = agreement_verdict(Some(&facts(1, 0, "")), Some(&RelayDoc::NothingToServe));
        assert_eq!(r.status, Status::Warn);
        assert!(r.short.contains("RELAY-EMPTY"));
        assert!(!r.value.contains("signature"));
    }

    #[test]
    fn an_unverifiable_reply_is_distinct_from_an_empty_one_and_does_not_cry_forgery() {
        let r = agreement_verdict(Some(&facts(1, 0, "")), Some(&RelayDoc::Unreadable));
        assert_eq!(r.status, Status::Warn);
        assert!(r.short.contains("RELAY-UNVERIFIED"));
        for alarming in ["forged", "tamper", "attack"] {
            assert!(!r.value.to_lowercase().contains(alarming), "{}", r.value);
        }
    }

    #[test]
    fn a_split_is_a_warning_and_says_which_way_round() {
        let newer = agreement_verdict(Some(&facts(100, 0, "")), Some(&RelayDoc::Verified(facts(200, 0, ""))));
        assert_eq!(newer.status, Status::Warn, "a service-side split is not this machine's fault");
        assert!(newer.value.contains("newer"));
        let older = agreement_verdict(Some(&facts(200, 0, "")), Some(&RelayDoc::Verified(facts(100, 0, ""))));
        assert!(older.value.contains("older"));
    }

    #[test]
    fn matching_ordinals_pass() {
        let r = agreement_verdict(Some(&facts(7, 0, "")), Some(&RelayDoc::Verified(facts(7, 0, ""))));
        assert_eq!(r.status, Status::Pass);
    }

    // --- the verdict -----------------------------------------------------------------------------

    fn rows_with(pairs: &[(&str, Status)]) -> Vec<Row> {
        let mut rows = pending_rows();
        for (k, s) in pairs {
            if let Some(r) = rows.iter_mut().find(|r| r.key == *k) {
                r.status = *s;
                r.value = "x".into();
                r.short = format!("{k}=x");
            }
        }
        rows
    }

    fn all(status: Status) -> Vec<Row> {
        let pairs: Vec<(&str, Status)> = ROW_ORDER.iter().map(|(k, _)| (*k, status)).collect();
        rows_with(&pairs)
    }

    #[test]
    fn total_offline_is_reported_once_not_as_four_faults() {
        let mut rows = all(Status::Pass);
        for r in rows.iter_mut().filter(|r| matches!(r.key, "dns_dl" | "tcp_dl" | "dns_relay" | "relay")) {
            r.status = Status::Fail;
        }
        assert!(overall_verdict(&rows).starts_with("No connection."));
    }

    #[test]
    fn a_definite_failure_is_announced_before_the_run_finishes() {
        // "Still checking…" above a red row is a page telling somebody to wait for news it has.
        let rows = rows_with(&[("version", Status::Fail), ("data", Status::Pass)]);
        assert!(overall_verdict(&rows).starts_with("You are on an older build"));
    }

    #[test]
    fn an_unfinished_run_never_claims_everything_is_healthy() {
        let v = overall_verdict(&rows_with(&[("version", Status::Pass)]));
        assert!(v.starts_with("Still checking"));
        assert!(!v.contains("healthy"));
    }

    #[test]
    fn the_causal_order_puts_the_build_before_the_network() {
        let rows = rows_with(&[
            ("version", Status::Fail),
            ("data", Status::Fail),
            ("debug", Status::Fail),
            ("dns_dl", Status::Pass),
            ("tcp_dl", Status::Pass),
            ("dns_relay", Status::Pass),
            ("relay", Status::Fail),
            ("agree", Status::Pass),
        ]);
        assert!(overall_verdict(&rows).starts_with("You are on an older build"));
    }

    #[test]
    fn the_headline_never_blames_a_router_for_a_setting() {
        // Measured and wrong: the row said "the relay is reachable, this is your setting" while the
        // headline above it said "check a firewall". The larger type has to be the correct one.
        let mut rows = all(Status::Pass);
        let relay = rows.iter_mut().find(|r| r.key == "relay").unwrap();
        relay.status = Status::Fail;
        relay.short = "relay=OPTED-OUT".to_string();
        let v = overall_verdict(&rows);
        assert!(v.contains("setting, not a network fault"), "{v}");
        assert!(!v.contains("firewall"), "{v}");
    }

    #[test]
    fn a_genuinely_blocked_relay_still_names_the_firewall() {
        // The other side of the branch above: without the opt-out token, the advice must be the
        // network one. A guard that swallowed both cases would be worse than neither.
        let mut rows = all(Status::Pass);
        let relay = rows.iter_mut().find(|r| r.key == "relay").unwrap();
        relay.status = Status::Fail;
        relay.short = "relay=BLOCKED 0/3".to_string();
        assert!(overall_verdict(&rows).contains("firewall"));
    }

    #[test]
    fn everything_green_says_so_and_points_at_the_other_machine() {
        assert!(overall_verdict(&all(Status::Pass)).contains("other machine"));
    }

    // --- the copy line ---------------------------------------------------------------------------

    #[test]
    fn copy_line_names_nothing_about_the_person() {
        // Checked by a test rather than left to inspection, because this line is written to be
        // pasted into a public chat window.
        let line = copy_line(&all(Status::Pass), 1_787_151_908);
        for forbidden in ["/Users/", "/home/", "C:\\", "\\Users\\", "Documents", "Application Support"] {
            assert!(!line.contains(forbidden), "the copy line leaked {forbidden}: {line}");
        }
    }

    #[test]
    fn copy_line_order_is_fixed_regardless_of_arrival_order() {
        let mut rows = rows_with(&[("relay", Status::Pass), ("version", Status::Pass)]);
        rows.reverse();
        let line = copy_line(&rows, 0);
        let v = line.find("version=").expect("version token");
        let r = line.find("relay=").expect("relay token");
        assert!(v < r, "a reader learns the shape once: {line}");
    }

    #[test]
    fn incomplete_outranks_warn_in_the_verdict_token() {
        // A run that stalled on the relay check must not get pasted looking merely untidy.
        assert!(copy_line(&rows_with(&[("version", Status::Warn)]), 0).ends_with("verdict=INCOMPLETE"));
    }

    #[test]
    fn a_failure_outranks_everything() {
        let mut rows = all(Status::Pass);
        rows[0].status = Status::Fail;
        assert!(copy_line(&rows, 0).ends_with("verdict=FAIL"));
    }

    #[test]
    fn all_green_is_ok() {
        assert!(copy_line(&all(Status::Pass), 0).ends_with("verdict=OK"));
    }

    #[test]
    fn the_stamp_is_utc_and_correct() {
        // 1787151908 is the ordinal of the live manifest fixture in gxcore. The expected strings
        // here come from `date -u -r <secs>` on this host, not from arithmetic done by the same
        // head that wrote the function - the first draft of this test asserted 13:45 and was wrong
        // about the fixture, not about the code.
        assert_eq!(utc_stamp(1_787_151_908), "2026-08-19 15:05Z");
        assert_eq!(utc_stamp(0), "1970-01-01 00:00Z");
        // A leap day, which is where a hand-rolled calendar goes wrong.
        assert_eq!(utc_stamp(1_709_164_800), "2024-02-29 00:00Z");
        assert_eq!(utc_stamp(1_709_251_199), "2024-02-29 23:59Z");
    }

    // --- inputs ----------------------------------------------------------------------------------

    #[test]
    fn the_relay_list_spec_defaults_the_port_and_keeps_the_region() {
        let r = parse_relay_list("one.example;two.example:9000;three.example:8086:us");
        assert_eq!(r.len(), 3);
        assert_eq!(r[0].port, DEFAULT_RELAY_PORT);
        assert_eq!(r[1].port, 9000);
        assert_eq!(r[2].region, "us");
        assert!(r.iter().all(|t| t.source == RelaySource::Env));
    }

    #[test]
    fn an_empty_or_ragged_relay_spec_yields_nothing_rather_than_a_blank_host() {
        assert!(parse_relay_list("").is_empty());
        assert!(parse_relay_list(";;  ;").is_empty());
    }

    #[test]
    fn the_engines_relay_semantics_are_reproduced_exactly() {
        let ini = scratch("relaycfg").join("Options.ini");
        std::fs::write(&ini, "Resolution = 1920 1080\n").unwrap();
        assert_eq!(read_relay_configured(Some(&ini)), RelayConfigured::NotSet, "absent: no bootstrap is named here");
        std::fs::write(&ini, "RelayAddress = \n").unwrap();
        assert_eq!(read_relay_configured(Some(&ini)), RelayConfigured::NotSet, "blank never enters the engine's map");
        std::fs::write(&ini, "RelayAddress = NONE\n").unwrap();
        assert_eq!(read_relay_configured(Some(&ini)), RelayConfigured::Disabled, "case-insensitive, it is hand-typed");
        std::fs::write(&ini, "RelayAddress = a.example\n").unwrap();
        assert_eq!(read_relay_configured(Some(&ini)), RelayConfigured::Host("a.example".into()));
        assert_eq!(read_relay_configured(Some(&scratch("norelayfile").join("Options.ini"))), RelayConfigured::NoFile);
        assert_eq!(read_relay_configured(None), RelayConfigured::NoFile);
    }

    #[test]
    fn a_configured_relay_gets_the_default_port_and_honours_an_explicit_one() {
        let t = relay_from_config(&RelayConfigured::Host("a.example".into())).unwrap();
        assert_eq!(t.port, DEFAULT_RELAY_PORT);
        assert_eq!(t.source, RelaySource::GameConfig);
        let t = relay_from_config(&RelayConfigured::Host("a.example:9100".into())).unwrap();
        assert_eq!((t.host.as_str(), t.port), ("a.example", 9100));
        assert!(relay_from_config(&RelayConfigured::Disabled).is_none());
        assert!(relay_from_config(&RelayConfigured::NotSet).is_none());
        assert!(relay_from_config(&RelayConfigured::NoFile).is_none());
    }

    #[test]
    fn the_download_host_is_extracted_from_the_documents_base_field() {
        assert_eq!(base_host("host.example/"), "host.example");
        assert_eq!(base_host("https://host.example/some/path/"), "host.example");
        assert_eq!(base_host("host.example:8443/"), "host.example", "a port is not part of a name to resolve");
        assert_eq!(base_host(""), "", "no base means no host, and the row says so");
        assert_eq!(base_host("/"), "");
    }

    // --- the run ---------------------------------------------------------------------------------

    #[test]
    fn a_run_with_nothing_configured_finishes_and_blames_nothing() {
        // The state of a machine with no game, no document and no network. It is NORMAL, and the run
        // must complete quickly, publish a final snapshot, and leave no row on "checking…".
        let started = Instant::now();
        let mut last: Option<Snapshot> = None;
        run(ProbeInputs::default(), |_| RelayDoc::Unreadable, |s| last = Some(s.clone()));
        let snap = last.expect("run must publish at least one snapshot");
        assert!(snap.done, "the last snapshot must be the final one");
        assert!(snap.rows.iter().all(|r| r.status != Status::Pending), "no row may be left checking");
        assert!(started.elapsed() < Duration::from_secs(2), "nothing was reachable, so nothing should have waited");
        assert!(snap.copy_line.contains("verdict="));
    }

    #[test]
    fn progress_is_reported_as_rows_settle_not_only_at_the_end() {
        let mut seen = 0usize;
        run(ProbeInputs::default(), |_| RelayDoc::Unreadable, |s| {
            seen += 1;
            assert!(!s.verdict.is_empty());
            assert_eq!(s.rows.len(), ROW_ORDER.len());
        });
        assert!(seen >= 3, "saw only {seen} snapshots");
    }

    #[test]
    fn an_unreachable_relay_is_refused_rather_than_hanging() {
        // 198.51.100.0/24 is TEST-NET-2 (RFC 5737): reserved for documentation and routed nowhere,
        // so this names no operator host. The probe must give up on its own timer.
        let t = RelayTarget {
            host: "198.51.100.1".into(),
            port: DEFAULT_RELAY_PORT,
            region: "test".into(),
            source: RelaySource::Env,
        };
        let started = Instant::now();
        let reply = probe_relay(&t, 0);
        assert!(!reply.answered);
        assert!(!reply.refused.is_empty(), "a failure must name itself");
        let ceiling = UDP_TIMEOUT * (RELAY_ATTEMPTS as u32) + DNS_TIMEOUT + Duration::from_secs(2);
        assert!(started.elapsed() < ceiling, "took {:?}, past the stated ceiling", started.elapsed());
    }

    #[test]
    fn a_resolver_given_a_name_that_cannot_exist_fails_rather_than_blocking() {
        // `.invalid` is reserved by RFC 2606 and must never resolve.
        let started = Instant::now();
        assert!(resolve("nothing.invalid", 80, DNS_TIMEOUT).is_err());
        assert!(started.elapsed() < DNS_TIMEOUT + Duration::from_secs(1));
    }

    #[test]
    fn human_bytes_does_not_jitter() {
        assert_eq!(human_bytes(0), "0 KB");
        assert_eq!(human_bytes(5 * 1024 * 1024), "5 MB");
        assert_eq!(human_bytes(3 * 1024 * 1024 * 1024), "3.0 GB");
    }

    #[test]
    fn free_space_is_a_number_or_nothing_and_never_a_panic() {
        // Whatever this host is, asking about a directory that exists must not blow up.
        let _ = free_space_bytes(&std::env::temp_dir());
        assert_eq!(free_space_bytes(Path::new("/gx-no-such-volume-here")), None);
    }

    #[test]
    fn the_facts_are_collectable_on_a_machine_with_nothing_installed() {
        // Never panics, whatever is or is not on disk. This is what the DIAGNOSTICS page calls on
        // every open.
        let f = collect_facts();
        assert!(!f.platform.is_empty());
        assert!(!f.arch.is_empty());
        // required_known is the field that must never be quietly true on an unknown set.
        if f.required_total == 0 {
            assert!(!f.required_known);
        }
    }

    #[test]
    fn reveal_accepts_only_the_four_names_it_knows() {
        assert_eq!(Reveal::parse("log"), Some(Reveal::Log));
        assert_eq!(Reveal::parse("dumps"), Some(Reveal::CrashDumps));
        assert_eq!(Reveal::parse("/etc/passwd"), None, "the web layer never names a path");
        assert_eq!(Reveal::parse(".."), None);
        assert_eq!(Reveal::parse(""), None);
    }
}
