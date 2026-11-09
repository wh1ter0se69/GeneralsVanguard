# GXRules - the AI rule bank

GeneralsX @feature Claude 30/10/2026 (C · rb-core). Plan: gx-handoff `ai/rulebank/PLAN_rulebank.md` (2.2 components, 2.6 CONTRACT v1,
2.8 tiers, 4.1 determinism). Every engine AI rule plugs in here the same way, so no rule ever again adds a skirmish save version, a
script action or an edit to `AISkirmishPlayer::update()`.

## What is where

| file | what | engine types? |
|---|---|---|
| `GXRuleTable.inl` | the registry: one row per rule, param, ref, claim, capability. THE source of truth | no |
| `GXRuleSpec.h` | the rows as constexpr tables + `static_assert`s | no |
| `GXRuleCore.h` | one player's configuration, the claim/reserve tables, the scheduler test, `ruleHash`, the save block, `tierFor` | no |
| `GXTierFilter.h` | the `GXT.<tiers>.<name>` marks and the per-parent plan | no |
| `GXRule_<Name>.h` | a rule's pure decider (plain numbers in, decision out) | no |
| `GXRuleBank.h` / `Source/.../AI/GXRules/GXRuleBank.cpp` | the engine side: instances, `GXRuleCtx`, dispatch + timer, `[GXRULE]`, Xfer adapter, `gxRuleTierOf`, the tier filter glue | yes |
| `GXRuleSense.h` / `GXRuleSense.cpp` | the ONLY way to other players' objects | yes |
| `Source/.../AI/GXRules/GXRule_<Name>.cpp` | a rule's glue: a `GXRuleImpl`, and `GXRuleImpl *gxRuleMake_<Class>()` | yes |

Stock code calls the bank in exactly these places: `AISkirmishPlayer::update()` (one call), `AISkirmishPlayer::xfer()` (the
`GX_SKIRMISH_XFER_RULEBANK` block), the three actions in `ScriptActions.cpp`, and the tier filter in `Player::initFromDict`. A value or
permission hook at a stock decision point is wired by the FIRST rule that needs it - one line calling `gxRuleBank()->valueHook(...)` /
`permitHook(...)` that returns the stock answer when no enabled rule claims the hook. Which rule a dispatch runs (and the closing of its
configuration window) is decided in `GXRuleCore.h` (`takeFrame`, `takeValueHook`, `takePermitHook`, `permitAll`), unit-tested; the glue only
runs what it chose.

## Adding a rule

1. **Registry rows** (`GXRuleTable.inl`), one per line INSIDE the one `GX_RULE_TABLE(...)` macro: every row ends with ` \`, there is no
   comment inside the macro (a `//` would swallow the line splice), and the blank line after the last row closes it - so a new row is one
   more ` \`-terminated line before that blank line. (One macro the consumers expand in place, never an `#include` inside an initializer:
   `scripts/build/simid-closure.py` refuses that.)
   `GX_RULE(<next free id>, <Class>, "<RULES.tsv rule_id>", <tiers>, <vision 0|1>, <cadence_s>, <live 0|1>)`, then its
   `GX_RULE_PARAM(<id>, <key>, "<name>", <default>, <lo>, <hi>, "<unit>")`, `GX_RULE_REF(<id>, <key>, "<name>", GX_REF_TEMPLATE|GX_REF_TEAM)`
   and `GX_RULE_CLAIM(<id>, "HOOK:context")` rows. Ids and keys are explicit, append-only, never reused or reordered (they live in scb
   data and in saves). Plain decimal integers only (no leading zero: the compiler reads `010` as 8; seconds, cash, world units, counts,
   per-mille); one plain string literal per string. Tiers: `GX_TIER_SENTIENT | GX_TIER_BRUTAL` for a
   new rule; HARD only for a registered in-flight pin (plan 2.8); a VISION rule (reads the whole map) is `GX_TIER_BRUTAL` alone - the
   build refuses anything else.
2. **Pure decider** `Include/GameLogic/GXRules/GXRule_<Name>.h`: no engine type, no state, no I/O. Unit test
   `scripts/test/gx-rule-<name>-test.cpp` + `gx-rule-<name>-mutants.sh` (every mutant caught).
3. **Glue** `Source/GameLogic/AI/GXRules/GXRule_<Name>.cpp`: a `GXRuleImpl` (override only what the rule does: `tick`, `maintain`,
   `valueHook`, `permitHook` + `joinsPermitHook`, `hasOpenOperation`, `stateVersion` + `xferState`, and for a `live` rule
   `onReconfigure` / `onDisable`), and `GXRuleImpl *gxRuleMake_<Class>() { return new GXRule_<Name>; }`. The glue reads the game ONLY
   through `GXRuleCtx` (own objects in ObjectID order, money, params, refs, `sense()`, the tables, `orderMoveTo`, `triggered()` /
   `fired()`, `log()`). `gx-rulebank-lint.py` refuses any engine global (`The...`), any `friend_` accessor and partition/iterator
   type, every stock function whose return type hands out another object/player/team (read from the engine headers at lint time),
   every name the random-number headers declare (read the same way), clocks, `getenv` outside a branch that provably needs test
   instruments, unordered containers, macros and token pastes (digraphs included), and includes beyond the standard library / stock
   engine headers / the bank's headers / `GXRule_*.h`; an exception goes in `scripts/test/gx-rulebank-lint.allow` with its reason. The
   lint is a BACKSTOP, not a sandbox (a cast or a pointer walk is still C++). Its second line is what the compiler emits:
   `scripts/test/gx-rulebank-symbols.py` compiles each rule as a RELEASE unit and refuses any undefined symbol not in
   `scripts/test/gx-rulebank-symbols.allow` (one regex + reason per line - a new rule adds exactly the engine calls it needs) and any
   counter-read instruction. Review against the checklist below stays the guard for what neither can see (a fully inline accessor).
4. **One CMake line** in `GeneralsMD/Code/GameEngine/CMakeLists.txt`, after `GXRule_Probe.cpp`.
5. **Regenerate** `scripts/ai/rulebank/rule_ids.json`: `scripts/ai/rulebank/gen-rule-ids.py` (the gate runs `--check`).
6. **Gate**: `scripts/test/gx-rulebank-gate.sh` (unit tests in three builds, mutants, the lint, the generator) + the rule's own tests.
7. **Fixtures** for each activation path (6 seeds), per-frame CRC identity vs its NO_OP control, the four-platform replay identity
   (rb-xplat's procedure), and the amended save/load 7b(2) (plan 2.4).

## Determinism checklist (plan 4.1) - every rule, checked in review

* No randomness: never `GameLogicRandomValue` / `GameClientRandomValue` (one extra draw desyncs from stock at once); a tie-break uses
  `gxrb::ruleHash(frame, playerIndex, ruleId, salt)`.
* Arithmetic only of the kinds the simulation already uses: no new transcendental calls, no float-to-int of a value that can be out of
  range or NaN (memory x64-ub-float-int-needs-o0-under-rosetta).
* Iterate in ObjectID order (`ctx.ownObjects`), never by pointer, never `std::unordered_*`.
* No wall clock, environment, file, client or render state in a decision; `getenv` and clocks only inside `#if defined(GX_TEST_INSTRUMENTS)`.
* All state saved (`xferState`) and reset in the constructor; a change to saved fields bumps the rule's OWN `stateVersion()`, never the
  skirmish xfer version. A load of a newer version of a rule that holds nothing (`open` = 0) skips it; one that holds a claim, a
  reserve or an open operation REFUSES the load (`GXRuleCore.h bankXfer`) - so `hasOpenOperation()` must be TRUE for anything only the
  rule's own code can undo (a borrowed unit, a temporary team, a changed recruitable flag, a try in progress).
* Cadence only from the scheduler; configuration only inside the configuration window (until the rule's first dispatch of any kind),
  unless the rule is `live` and its `onReconfigure` / `onDisable` release everything first (unit-tested).
* Other players only through `GXRuleSense` (`scripts/test/gx-rulebank-lint.py` enforces it): fair tiers see what a human in that seat
  sees - partition iteration only (hidden cargo never counts), fog, stealth, a disguise shows its disguise unless the real owner is
  allied; BRUTAL drops only the fog filter.
* `scripts/build/simid-closure.py` must print CLOSED. Rule code is digested: sourceID moves even with the rule off, so code ships dark
  first (plan 4.4).

## Configuration (the three actions, CONTRACT v1 2.6.1)

`AI_PLAYER_GX_RULE(SIDE, INT rule, INT key, INT value)` (key 0 = enable 0/1), `AI_PLAYER_GX_RULE_TEMPLATE(SIDE, INT rule, INT key,
TEXT_STRING)`, `AI_PLAYER_GX_RULE_TEAM(SIDE, INT rule, INT key, TEAM)` (arrives player-qualified). Out of box -> clamped (`ev=clamp`);
unknown rule / key / wrong ref type -> ignored (`ev=unknown`); a tier the rule does not allow -> `ev=refused why=tier|vision`; after the
first dispatch -> `ev=refused why=active` (unless `live`); a `RETAIL_COMPATIBLE_XFER_SAVE` build refuses every enable
(`why=retail_save`). Conflicts (two enabled rules claiming one `HOOK:context`): the on-set is a function of the REQUESTED set, not of
arrival order - requested rules in id order, each accepted unless it shares a claim with one accepted before it; a dispatched non-live
rule is never displaced (`ev=conflict why=lower_id|active with=<id>`).

## Tier marks (plan 2.8; grammar agreed with rb-data 30/10)

`GXT.<T>.<name>` on a script, a whole script group or a team; `<T>` exactly one of `SENTIENT`, `BRUTAL`, `SENTIENT+BRUTAL`; the tier
token runs to the next `.`, the rest is the stock name byte for byte. Anything else after `GXT.` - HARD, `BRUTAL+SENTIENT`, lowercase,
an empty name, a mark inside a mark - is malformed and dropped for every player. A kept mark replaces the stock entry of its name in
the same parent (top-level script list, one group, the group list, one owner's teams) at the stock entry's position and takes its name;
with no stock twin it stays where it is, renamed; two kept marks of one name in one parent: the first wins. A player with no tier
(HARD, Easy/Normal AI, human) keeps no mark: its lists are exactly stock. Scripts inside a marked group are not marked; references
always use plain names.

## The instrument (test-instruments builds only, plan 2.2.7)

`[GXRULE] f=<frame> p=<player index> r=<rule id> n=<NAME> ev=<event> k=v ...` - `ev=config on=<0|1> v=<key:value,...> refs=<key:name,...>`
after every accepted change (spaces in names print as `~`), `ev=clamp|unknown|refused|conflict`, rule events (`ev=probe ...`),
`ev=tier slot= tier=` (GX_RULE_TIER_TEST), `ev=tier_filter what=scripts|teams tier= kept= replaced= dropped= malformed= dups=`, and per
rule at game end `ev=cost calls= us= clock=thread|wall` (`wall` = no thread clock, Windows today: never CPU evidence) and
`ev=activity triggered= fired=`. One `[GXHOOK] ... hook=rule_<NAME>` timer per rule around every dispatch (2 ms/frame bound).
`ev=tier_team name= used_max= stock_max=` for a team the filter replaced, read from the team AS STORED after `addTeam`.

Red-arm knobs (test-instruments builds only; each exists so a gate can be seen to FAIL): `GX_RULE_PROBE_RED=1` (PROBE issues one move
order), `GX_RULE_PROBE_SLOW=maintain|hook`, `GX_TEST_TIER_MUTANT=keep_for_hard` (the filter keeps marks for HARD), `=team_at` (a
replaced team copies its stock twin's dict) and `=keep_at` (a replaced group - or top-level script: one shared path - keeps its stock twin), `GX_TEST_RULE_MUTANT=fps_half|late_start|template_noop|
team_noop|team_as_template` (the scheduler at half rate, or missing a rule's first 7 ticks; the template / team action dropped, or forwarded as the wrong slot type), `GX_TEST_SENSE_MUTANT=
no_owner_guard|brutal_no_stealth` (the sense layer counts around a foreign anchor / Brutal sees undetected stealth),
`GX_RULE_TEST_SAVE_EXTRA=closed|claim|newer` (a planted save block), `GX_RULE_TEST_COUNTERS=<N>` (N > 256 counters and flags planted, then checked after a load), `GX_RULE_TEST_LOAD_CLEAR=1` (a load that drops params and refs).
`ev=bank fps=` once per bank (the engine's logic rate), `ev=tier_group name= used_delay= stock_delay=` for a group the filter replaced, `ev=team_action k= name=` for every TEAM action.
