# Security policy

## Reporting a vulnerability

Report security problems **privately**, through GitHub:
[Security → Report a vulnerability](https://github.com/wh1ter0se69/GeneralsX-source/security/advisories/new).
Only you and the maintainer can see it.

Please do not open a public issue, pull request or chat post about it first. A crash or takeover that anyone can
trigger in someone else's game can be used before a fix ships.

A useful report says:

* the GeneralsX version (the version line on the main menu) and the platform;
* what an attacker sends or provides (a packet, a replay, a map, a file) and what happens;
* the smallest steps or file that reproduce it.

## What is in scope

The code in this repository, in particular anything that reads data another person controls:

* network messages between players and from the relay server;
* replay, save, map and INI files that players share;
* signature checks: the signed relay list and sign-in identity (`GXEd25519`, `GXSha512`, `GXIdentityAssertion`);
* how the game client stores and sends sign-in tokens.

The relay server, the account services and the GeneralsX Launcher are not in this repository. If you find a problem
with them, report it through the same form.

## What is out of scope

* Cheats that only change your own game (for example editing your own files). Report desyncs and gameplay bugs as a
  normal issue.
* Problems in Electronic Arts' original game data.
* Load tests or denial-of-service against the public servers. Do not test against other players' games or
  accounts; use your own.

## What happens next

The maintainer reads every report. There is no fixed response time and no bug bounty. A fix ships in a normal
release and its advisory credits you, unless you ask not to be named. Only the latest release receives fixes.
