# Rule bank, DATA half: the ruleset compiler

A **ruleset** (JSON, `gx-ruleset/1`) names rules from `registry.json` with their integer parameters. The compiler turns it into a
loose `Data/Scripts/SkirmishScripts.scb` on top of the recipe's shipped output, plus a manifest:

    python3 scripts/tooling/make-skirmish-scripts.py --ruleset rules.json --out <dir>/SkirmishScripts.scb [--tier-gate on|off] [--rule-ids rule_ids.json]

```json
{"schema": "gx-ruleset/1", "base": "<integration sha>", "tier_gate": "on",
 "rules": [{"id": "OPEN.SECOND_SUPPLY_T", "on": true, "generals": ["SkirmishAmerica"], "tiers": ["SENTIENT", "BRUTAL"],
            "params": {"second_supply_t": 60}}]}
```

* An absent rule is off; an absent param is the registry default; `generals` defaults to the rule's scope, `tiers` to its allowed
  tiers. A ruleset that targets HARD is refused: Hard keeps the registered defaults. Params are integers inside their box.
* An empty ruleset gives the recipe's own bytes exactly (`BASE_SHA256`; equal to `OUTPUT_SHA256` until a ruleset is promoted -
  then `rulebank/shipped_ruleset.json` is what `--out`, `--root` and every packager compile on top, pinned by `OUTPUT_SHA256`). Entries apply in registry order, so a reordered file gives the
  same bytes. A rule may appear twice only with disjoint tiers.
* **DATA rules** are modules `data/<RULE_ID>.py`: `RULE_ID`, `CLAIMS` (the scripts, groups, teams and new priority sets they edit or
  add), `apply(scb, scope, params)` and `activation(obs, me) -> {"triggered", "fired", "detail"}`. The loader gives each module `rb`
  (`scbx.py`) and `mss` (the recipe).
* **ENGINE rules** become one `GX Rule <id>` group per in-scope general: a Hard-only one-shot script calling
  `AI_PLAYER_GX_RULE(<This Player>, id, 0, 1)`, then each explicitly given param by key and each ref (ids and keys from the bank's
  `rule_ids.json`). No bank on the tree -> ENGINE entries are refused.

## PARKED rules fail closed (rb-parked-block)
A registry rule tagged `PARKED` (built or not, judged not to ship) is refused when a ruleset names it, `on` or not; the refusal names the
rule and quotes why it was parked (`parked.json`, rule id -> the note's PARKED sentence; the selftest pins its keys to the registry tags).
The only way past is an explicit reopen in the same ruleset, one entry per rule, with a reason:

    {"schema": "gx-ruleset/1", "rules": [{"id": "COUNTER.GLA_VS_CHINA", "on": true}],
     "reopen": [{"id": "COUNTER.GLA_VS_CHINA", "reason": "half the Quads, rb-tune's go 02/11"}]}

A reopen of a live rule, an unknown rule, a rule the `rules` do not list, a duplicate, or one without a reason (8+ characters) is refused;
no environment variable and no command-line flag opens a parked rule. The manifest records `reopened` (absent when there is none), and
a reopen changes no output byte. Parked modules stay in `data/` and compile for tests and smoke packs that reopen them.

## Reading what the AI's own units see: SENSE.FOG_CONDITIONS (rb-sense-2, emitter rb-sense-3)
A DATA rule that declares `SENSE.FOG_CONDITIONS` may gate on what this AI's OWN units and buildings see NOW (fog, stealth and disguise as a human sees them;
a superweapon that shows itself to every enemy counts). `rb` builds the leaves:

    rb.seen_count(scb, rb.GE, 3, 'GLAVehicleTechnical')             # a template (a typo counts 0 for ever: nothing here can check it)
    rb.seen_count(scb, rb.GE, 1, 'Aircraft')                        # a stock object-type list, read only (Tanks, Infantry, Artillery, Super Weapons, ...)
    rb.seen_count(scb, rb.GE, 5, 'GX Seen CHINA Humvees')           # a list the rule builds:
    rb.add_script(scb, owner, rb.seen_list_script(scb, 'GX Seen List CHINA', 'GX Seen CHINA Humvees', [...]))   # at the TOP LEVEL of the general's list
    rb.seen_value(scb, rb.GE, 800, '[Skirmish]MyInnerPerimeter')    # the list price of armed units seen inside MY perimeter

Every leaf goes in a script with `delay=rb.SEEN_MIN_DELAY` or more (each ask walks the partition). "Seen so far" is the rule's own latch: a one-shot setting a
flag on the first TRUE; the sensor keeps no memory, so a rule must not read "seen now" as "the enemy has". Refused: any seat but `<This Player>` asking about
`<This Player's Enemy>`, a script delay under the minimum, any other area, a comparison outside COMPARISON, a negative threshold, a list no script of this
general builds, a rule list with no `GX Seen ` prefix, any other write to a list, a list with other contents in another general (lists are global).

## What the compiler refuses (guards.py; every refusal names the module, the script and why)
* an edit outside the entry's generals' lists and teams, or outside the script lists and ScriptTeams;
* a deleted or renamed stock entry, a changed difficulty or subroutine bit, a change to a script that is not a Hard script, a new script
  that is not Hard-only, an edit to a name that occurs twice in its parent;
* any change to a script that reaches a money action (directly, through any SCRIPT parameter, a group holding one, or a team whose
  hook pays - a stock Hacker team gives $750 on create, so its production condition is a gift too - transitively), any change to a
  stock team that runs one, or a new or changed script or team field that names one: Hard's cash gifts stay frozen;
* a further definition of an existing attack-priority set (names are global), any change to - or new reference to - a stock script that
  defines one (its conditions decide when the shared table exists), or one new set name in two generals;
* any change to, reference to (DISABLE included) or hook naming a stock script that writes what a frozen gift or table definer waits
  on, transitively (`guards.frozen_inputs`: switching off "Time Escalation" would stall the `_ESCALATION >= 25` expansion gift);
* a new script or group name equal to another script or group of its list (the engine's lookups share one namespace: CALL_SUBROUTINE
  finds a group first, ENABLE_SCRIPT switches both), and any two counter / flag / script / team names of a compile - every general and
  every tier, new against stock and new against new - that alias once a start index is appended;
* an ADDED condition that reads anything but this player's own state (allow-list; every SIDE argument `<This Player>`): another
  player's objects or side, `SKIRMISH_PLAYER_FACTION`, `NAMED_DISCOVERED`, `TEAM_DISCOVERED`, `SKIRMISH_PLAYER_HAS_DISCOVERED_PLAYER`,
  the area/value conditions, `SUPPLY_SOURCE_SAFE` - they read through fog. `SKIRMISH_TECH_BUILDING_WITHIN_DISTANCE` caches its first
  result, so it is allowed only of MY perimeter and only where that first evaluation is the first frame (first condition of the first
  OR block of an active, undelayed, non-subroutine script in an active non-subroutine group, which nothing else names);
* an added, removed or changed leaf naming `_ESCALATION` (stock adds enemy tech to it): new time gates use `_GX_CLOCK` (+1 per 10 s,
  added by the compiler where read);
* a stock flag, counter, timer or team state an added condition reads that can carry another player's state (traced transitively,
  including team hooks behind a tainted production condition, subroutine groups and enemy-sighted hooks, and rechecked after every
  module) - except the stock gate itself, copied unchanged with that script's own actions (the manifest's `flag_audit` lists every read);
* a team field that gains an enemy-sighted or all-clear hook (they fire on stealthed enemies a human cannot see; stock uses none);
* a launch hook that is not one of the exact shapes the shipped rules use (`guards.team_hooks`, an allow-list, stricter than the engine by design; use `rb.launch_scripts` + `rb.launch_fields`, or add the shape with its red arm): judged on a new team, a
  team a module moved to another owner, a hook name it set, or a name whose script now leads to anything else. No group, CALL_SUBROUTINE, false action, repeating generic hook, inactive or delayed script, missing target, or `teamOnDestroyedScript` / `teamOnIdleScript` / `teamOnUnitDestroyedScript`, even where the engine
  would run them (ENABLE_SCRIPT then CALL_SUBROUTINE). Generic hooks run on a team that has not formed, so a one-shot one is spent on an empty or partial team and later units never get the order: it needs `TEAM_HAS_UNITS` or a twin on `teamOnCreateScript`, which no other script may name. A team launched by TEAM_HUNT holds no Battle Bus: an empty one has no ground weapon and stays home (COUNTER.GLA_VS_CHINA, smoke18);
* a changed stock script gated on an enemy read whose OR blocks no longer each hold all the enemy-reading conditions of one stock block
  (regrouping `cash AND _ESCALATION` into `cash OR _ESCALATION` loosens the stock gate); the same for a copied production condition;
* an added write of a STOCK flag or counter (a rule writes only its own new state - a frozen script may wait on a stock one), and any
  added `OBJECTLIST_ADDOBJECTTYPE` / `OBJECTLIST_REMOVEOBJECTTYPE` (object-type lists are global: stock priority setters expand list names);
* a NUL inside any name, name-table entry (team keys included), string parameter or ENGINE ref (the engine's C string ends there, so
  `_ESCALATION\0` is `_ESCALATION`), and an appended name-table entry whose name or id the table already holds;
* an added action that is not on the reviewed allow-list `guards.ACTIONS_OK` (orders to my own units, buildings, scripts, state and NEW
  priority tables, each engine side read; `ENGINE_READS` are on it and listed in the manifest) - a deny-list of fog readers never closes;
* a new reference - CALL / ENABLE / sequential script / a team field - to stock logic whose running can depend on another player (an
  enemy-reading or tainted condition, a HIDDEN action, or a call / group leading to one; `guards.enemy_reach`): only a DISABLE may name
  it. A copied stock gate may only stand in for its own team's production condition; a new hook on a stock team whose production
  condition is enemy-gated is refused (its hooks are that gate's effects; its unit counts may change);
* a group of a priority definer named at all; a stock group's CLAIM covers its member scripts in the overlap check;
* a copied stock gate standing in for any condition it does not keep (each team it serves is checked against its own old condition,
  on the finished file too - an earlier module's forward reference to a later module's copy included);
* two entries for one player whose claims meet once a claimed group counts the members it has at the END of the compile;
* a chunk version the shipped file does not use for that chunk (the engine picks the payload layout by it), a call key whose low byte
  is not 3;
* a NEW attack-priority table written behind a player-dependent condition, or by a script anything switches, calls or hooks: names
  are global, so every player of the general - friend or foe - shares the table; only `_GX_CLOCK` thresholds, `CONDITION_TRUE` and a
  stock definer's own startup trigger may time it;
* more attack-priority tables than the engine's pool holds (255 besides the default, global over every general and tier), or a list
  heavy enough in counters / flags (per player) that a retail map's 8 players fill the 255-slot pool. The manifest's `pool_budget`
  says how many players of the heaviest general fit: stock 14, with the compiler's clock 13. **Promotion** (the shipping gate) refuses
  anything under the 14-slot lobby (MAX_SLOTS) - so a tiered default that gives GLA the clock ships only once the engine raises
  MAX_COUNTERS / MAX_FLAGS (an rb-core ask);
* a chunk where the shipped file never puts its kind (a group inside a group: the engine skips it whole);
* a new reference (any leaf or team field) to a reserved name - `GX Clock`, `GX Rule <id>`, `GXT.` - which the compiler generates after
  every module: no rule ticks the clock or switches an ENGINE line;
* a changed ScriptList chunk name or version (the engine reads only ScriptList chunks);
* a new attack-priority set defined differently - setters OR the defining script's conditions and timing - by two tiers of one compile;
* an added action that makes money, picks its target by enemy value or proximity through fog (guards.HIDDEN: the value-ranked
  attacks, `SKIRMISH_FIRE_SPECIAL_POWER_AT_MOST_COST`, the nearest-object command / move / garrison / capture orders), names another
  SIDE or writes `_GX_CLOCK`; any change to a stock
  script that writes enemy-derived state; a new effect or new timing on a stock script gated by another player's state;
* an added leaf whose parameter signature differs from the stock one, that carries cached fields, or whose counter / flag / script /
  team name aliases another once a start index is appended; a rewritten name-table entry;
* a changed stock script RUN behind an enemy gate (a team hook behind a tainted production condition, a call from a gated script) that
  gains an action or new timing - only a team may be pointed at the rule's own new table;
* a module touching what its CLAIMS do not name, or naming a stock script (switch, call, sequential orders, team hook) its CLAIMS do
  not name, or switching a new script its CLAIMS do not name; two entries for one
  player whose CLAIMS overlap (except two MATCHUP keys that differ, below); registry conflicts; a missing dependency or capability; a param outside its box; a module that crashes
  (the team view `teams_ro()` is read-only: write through `teams()` + `put_teams()`).

**Matchup keys (rb-data-13, the lead's ruling: one rule per matchup, each switchable alone).** A module may declare `MATCHUP = '<USA |
China | GLA>'`: its variants run only against that enemy base side, read from ONE latch per general that the rules share (`rb.matchup`:
group `GX Matchup`, one one-shot script per base side; the first side this AI fairly knows for its enemy sets `gxrb_mu_<side>` and
`gxrb_mu_decided`, so at most one key is ever set). Two rules may claim the same stock team only when both declare a key and the keys
differ - they then share only stock teams, the latch and the off gates (`guards.mu_shareable`). A stock team switched by a key waits on its
own off gate `GX MU Off <team>` (`rb.matchup_off`: its stock gate + `FLAG(gxrb_mu_<k>) == 0` for every key, in name order, composed in
place; variants follow the stock team in name order), so the order the rules apply in does not change the bytes. The rules' own groups
go where the apply order puts them, so the WHOLE file's bytes vary with the registry's order (Codex rbd13-r2 P2, ruled accepted): that
order is fixed by the registry (a ruleset's own order never matters) and the keys are exclusive, so no game reads a difference. Refused: the same key
twice, a rule with no key on a claimed team, a key the rule does not gate on (every script and team it adds waits on `gxrb_mu_<K>` ON), a
stock team changed beyond its off gate or a stock script changed by a MATCHUP rule, a matchup flag written by anything but the canonical
latch, a matchup flag read by a rule without a key (`guards.matchup_module`, `guards.matchup_final`). `data_claims.json` carries each
rule's `matchup` key for the tuner.

**A general-level read (rb-data-15).** The sensor names an enemy GENERAL only when the lobby showed it (a Random enemy is never known by
its general). A MATCHUP rule that wants one (`rb.general_latch`) holds its own one-shot scripts, gated on its key: in the frame the key is
set, per general one script sets the rule's own flag when the sensor says that general, and a last one sets the rule's own `done` either
way, so it decides once, for the enemy the key decided on. The flags are the rule's own (no other rule reads them) and cost flag-pool
slots per player (`pool_budget`).

**Keep the stock team, never copy it (rb-data-17, the lead's ruling on rb-data-16 s5).** A variant with the stock team's own slots on the
rule's clock is NOT stock: it fielded 40 vs 57 Scorpions by 600 s (the own clock reaches each wave later than stock's escalation). So a
rule that leaves some enemies to stock - a general it does not counter (GLA_UNIT_BY_ENEMY vs Nuke / Tank), or "until my building
stands" (GLA_VS_USA's Palace, CHINA_VS_CHINA's Propaganda Center) - keeps the STOCK team on through its off gate: `rb.matchup_keep`
appends, after the OFF blocks, a KEEP block per stock block [stock block, FLAG(gxrb_mu_<K>) == 1, the rule's own leaves], in canonical
order (so rules compose in any order); `matchup_off` and `mu_stock_gate` read only the OFF blocks. A module builds a variant only when its
slots differ from the stock team's (`rb.same_slots`); an equal one becomes a keep block with that variant's leaves. `guards.keep_blocks`
accepts exactly that form - a stock-block prefix, one key ON of a rule that claims the team, at least one own leaf (FLAG == 0 / 1 of a
flag only that rule's claimed scripts write, or PLAYER_HAS_OBJECT_COMPARISON of <This Player>), right after the OFF blocks, none twice,
and excluding every variant of that stock team by one leaf each (`guards.excludes`) - and refuses everything else.

**A team's provenance is recorded, never inferred (the lead's ruling on Codex rbd17-r4).** `rb.variant_team` clones its donor and records donor ->
variant in a build-time table (`rb.BUILT`, per tier; a copy of a copy resolves to its stock team); `rb.add_new_team` declares a team that stands in
for no stock team. `guards.team_provenance` refuses any other new team, and the keep-block overlap scan reads that table alone: a name or a place in
the file proves nothing. These guards check our own reviewed modules, so a recorded donor is enough; what no table reads is intent (a true copy of T'
whose slots were meant to replace T). Two rules of the SAME key stay refused on one general even when only the latch
is shared (the lead's one-rule-per-key ruling): a ruleset gives that general to one of them.

**A rule that builds units must show they left home and did the job (the lead's bar, rb-data-18).** A count of units built proves neither: smoke14's earlier Burton was built and stayed near home all game. Read how far they get. Two
traps it found: a generic hook runs on a team that has not formed (`guards.team_hooks`), and an empty Battle Bus has no weapon (`PRIMARY None`), so TEAM_HUNT leaves it idle (COUNTER.GLA_VS_CHINA). Before tuning, also check the team can win
`AIPlayer::selectTeamToBuild`, and that a window can still open after the key arrives.

**What a DATA rule may KEEP (PLAN_rulebank 1.3, 4.3).** The line is "never ADD an enemy input", not "remove stock Hard's": a copied stock
gate keeps its enemy input (`_ESCALATION`, the wave gates), an own-state threshold beside it may change, and an added stock action whose
engine side searches the map without a fog filter (`AI_PLAYER_BUILD_SUPPLY_CENTER`'s pile choice, `TEAM_HUNT` - guards.ENGINE_READS)
is the stock AI's own omniscience. Making that fair is rb-sentient's job (the Sentient tier, without touching Hard); the manifest lists
every such action a rule adds in `engine_hidden_reads` so that audit sees each one. The line between that list and the refused
`HIDDEN` one: an action whose OWN code picks a specific object or place by enemy value or by proximity among objects a human may not
see is HIDDEN (never added; a FIRE rule retimes the stock strike scripts' own thresholds instead of adding strikes); an order to my own
units or buildings whose engine side (unit AI, placement) is fog-blind is ENGINE_READS. Naming the enemy only as a TARGET - its start
area (`[Skirmish]EnemyInnerPerimeter`), an approach path - is allowed by the plan (1.3: NO_ENEMY_OBJECTS) and listed there too.

## Tier marks (`--tier-gate on`, the default; the last step, never a module's)
Hard, Brutal and Sentient share one script list. Every stock script and team stays byte-identical; a changed one gets a copy
`GXT.<T>.<name>` right after it, a new script / group / team is named `GXT.<T>.<name>` where the rule put it, `T` = `SENTIENT`,
`BRUTAL` or `SENTIENT+BRUTAL`. The engine's tier filter gives a player of tier T the marks that hold T, in the stock twin's place under
the plain name, and drops the rest. Marks need `TIER.FILTER` in the target's `rule_ids.json` capabilities. `--tier-gate off` edits in
place for proxy runs on a binary without the filter; its manifest says so, and `--promote-default` refuses it.

## The manifest (`<out>.manifest.json`)
Ruleset sha256, the base and retail sha256, the compiler's own sha256, `tier_gate`, every entry with its resolved params, CLAIMS and
module sha256, the ENGINE lines, the marks, `normal_also` (changed stock scripts that also run at Normal/Easy - exact under `on`),
`flag_audit`, `engine_hidden_reads` (added actions whose engine side consults hidden state - for the Sentient fog audit), and `output`: sha256, bytes, `fnv1a32` (= the engine's `[SIMID] loose ... crc=`) and `script_count` (= `[SKIRMISH_DIAG]
parsed scriptCount=`, the number of ScriptLists, 14).

## Tests
`python3 -I scripts/tooling/rulebank/selftest.py` (needs a retail SkirmishScripts.scb, as the recipe does): every refusal has a
planted-module fixture in `testdata/modules/`; `tiermark.filter_for()` models the engine's filter and proves, per general,
filter(HARD) == the shipped file and filter(T) == T's own compile. `scripts/test/skirmish-scripts-recipe-test.sh` runs it too.
