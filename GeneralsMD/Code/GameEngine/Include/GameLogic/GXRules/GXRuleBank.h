// GeneralsX @feature Claude 30/10/2026 AI RULE BANK, the engine side (gx-handoff ai/rulebank/PLAN_rulebank.md 2.2.2-2.2.7, rb-core plan
// Task 4). One GXRuleBank per skirmish AI player (AISkirmishPlayer owns it): the three AI_PLAYER_GX_RULE* actions configure it, ONE call in
// AISkirmishPlayer::update() runs it and returns at once while no rule is enabled, AISkirmishPlayer::xfer saves it in its own block. The
// decisions are GXRuleCore.h's (pure, unit-tested); this layer adapts them to the engine: rule instances, the context a rule reads the
// game through, the dispatch timer, the [GXRULE] lines (test-instruments builds only) and the stream adapter over Xfer.
// A rule never reads another player's objects or side except through GXRuleSense (scripts/test/gx-rulebank-lint.py enforces it).
#pragma once

#include <vector>
#include "Common/AsciiString.h"
#include "Common/GameType.h"
#include "GameLogic/GXRules/GXRuleCore.h"
#include "GameLogic/GXRules/GXTierFilter.h"

class Player;
class Object;
class Dict;
class TeamsInfo;
class Xfer;
class ScriptList;
struct Coord3D;
class GXRuleBank;
class GXRuleSense;
class ObjectTypes;
class PolygonTrigger;

/// what a rule sees of the game: its own player and nothing else, plus the bank's tables and the sense layer
class GXRuleCtx
{
public:
	GXRuleCtx(GXRuleBank &bank, int rule) : m_bank(bank), m_rule(rule) {}
	Player *player() const;
	int playerIndex() const;
	unsigned frame() const;
	int param(int key) const;							///< the value set, else the registry default
	AsciiString ref(int key) const;						///< the name set (resolve it at use), else empty
	int money() const;									///< own cash
	/// own live objects in ObjectID order (never pointer order, never the team-list order a load can change)
	void ownObjects(std::vector<Object *> *out) const;
	const GXRuleSense &sense() const;					///< the only way to other players' objects (fair for Hard/Sentient)
	gxrb::UnitClaims &claims() const;
	gxrb::MoneyReserves &reserves() const;
	/// an order of the rule: move an OWN unit (CMD_FROM_AI). Refuses anything not owned by this player.
	Bool orderMoveTo(Object *own, const Coord3D &to) const;
	void triggered() const;								///< the rule's precondition occurred (ev=activity triggered=)
	void fired() const;									///< the rule acted (ev=activity fired=)
	/// a rule-specific [GXRULE] event (test-instruments builds only; nothing at all in a release build): ev=<ev> <kv>
	void log(const char *ev, const char *kv) const;
	int rule() const { return m_rule; }
#if defined(GX_TEST_INSTRUMENTS)
	GXRuleBank &bankForTest() const { return m_bank; }	///< the sense fixtures only (GXRuleSense.cpp); never a rule's
#endif
private:
	GXRuleBank &m_bank;
	int m_rule;
};

/// one rule's code. Every member has the stock answer, so a rule overrides only what it does. State lives in the rule object,
/// reset by its constructor and saved by xferState (bump stateVersion when its fields change - never the skirmish version).
class GXRuleImpl
{
public:
	virtual ~GXRuleImpl() {}
	virtual void tick(GXRuleCtx &ctx) { (void)ctx; }						///< at the registry cadence, staggered by player
	virtual Bool hasMaintain() const { return FALSE; }
	virtual void maintain(GXRuleCtx &ctx) { (void)ctx; }					///< every frame while enabled, when hasMaintain()
	virtual Bool hasOpenOperation() const { return FALSE; }				///< holds something only its code can undo (2.2.4)
	virtual void onReconfigure(GXRuleCtx &ctx) { (void)ctx; }				///< live rules: close open operations first
	virtual void onDisable(GXRuleCtx &ctx) { (void)ctx; }					///< live rules: release everything first
	virtual unsigned short stateVersion() const { return 1; }
	virtual void xferState(gxrb::ByteIO &io, unsigned short savedVersion) { (void)io; (void)savedVersion; }
	virtual int valueHook(GXRuleCtx &ctx, int hook, const char *context, int stock) { (void)ctx; (void)hook; (void)context; return stock; }
	virtual Bool joinsPermitHook(int hook) const { (void)hook; return FALSE; }	///< asked by permitHook(hook) only when TRUE
	virtual Bool permitHook(GXRuleCtx &ctx, int hook, const char *context) { (void)ctx; (void)hook; (void)context; return TRUE; }
};

class GXRuleBank : private gxrb::LiveHooks, private gxrb::StateHooks
{
public:
	explicit GXRuleBank(Player *player);
	virtual ~GXRuleBank();

	/// AISkirmishPlayer::update(): returns at once when no rule is enabled; else every enabled rule in id order: maintain() each
	/// frame, tick() when due
	void update();

	// the three scb actions (ScriptActions.cpp)
	void action(int rule, int key, int value);
	void actionTemplate(int rule, int key, const AsciiString &templateName);
	void actionTeam(int rule, int key, const AsciiString &teamName);

	/// a VALUE hook at a stock decision point: the stock value unless an enabled rule claims HOOK:context (exclusive). Declared, wired
	/// by the first rule that needs it.
	int valueHook(int hook, const char *context, int stock);
	/// a PERMISSION hook ("may I spend / sell / build"): the AND of every enabled rule's answer
	Bool permitHook(int hook, const char *context);

	/// SENSE.ENEMY_FACTION (GeneralsX 01/11/2026 rb-sense-1), the AI_PLAYER_GX_SENSE condition: TRUE iff this AI fairly knows `enemy`'s
	/// faction and it is `faction` - the BASE side (USA, China, GLA: PlayerTemplate BaseSide) once known from the lobby or from 3 fair
	/// sightings, or the lobby's own side (America, ChinaTankGeneral, ...) when the lobby showed it. Never the resolved side of a Random
	/// slot. A question changes nothing (Codex sense-r3 P1): whether the bank looks is decided once per game from the scripts - if any
	/// loaded script list holds AI_PLAYER_GX_SENSE, every bank runs the sighting pass (every half second, staggered by player, until
	/// every tracked player is known or mixed); a game whose scripts never ask (every shipped scb) costs nothing.
	Bool senseEnemyFaction(const Player *enemy, const char *faction);
	/// SENSE.FOG_CONDITIONS (GeneralsX 02/11/2026 rb-sense-2), the AI_PLAYER_GX_SEEN_COUNT condition: how many of `who`'s units and
	/// buildings (owner and template AS SEEN) of `types` this AI's own units and buildings see NOW (GXRuleSense::seenOf, computed at this ask); -1 for no
	/// player or this AI itself. Reads only; no state, no snapshot (Codex rbs2-r2 P1).
	Int seenCount(const Player *who, const ObjectTypes &types);
	/// the AI_PLAYER_GX_SEEN_VALUE condition: the build cost of `who`'s ARMED units and buildings (as seen; neither dozers nor harvesters)
	/// this AI's own eyes see NOW inside `area` (pointInTrigger on the position, as Object::isInside); -1 for no player, itself or no area
	Int seenValueIn(const Player *who, const PolygonTrigger *area);
#if defined(GX_TEST_INSTRUMENTS)
	/// judge evidence (rb-sense-2): one ev=seen_cond line each time a SEEN condition's answer for one (kind, player, what, op, number)
	/// changes - the replay and lockstep arms compare them frame by frame
	void seenLogAnswer(const char *kind, const Player *who, const std::string &what, int op, int want, int got, Bool answer);
#endif
#if defined(GX_TEST_INSTRUMENTS)
	/// fixtures only (GXRuleSense.cpp's faction cases): run the sighting pass now, as its half-second cadence would
	void senseSampleNowForTest();
	/// fixtures only: what this bank holds about a player (nullptr when it has no entry)
	const gxrb::FactionSense *senseStateForTest(const Player *p) const;
#endif

	gxrb::UnitClaims &claims() { return m_claims; }
	gxrb::MoneyReserves &reserves() { return m_reserves; }

	void xfer(Xfer *xfer);
	void reset();				///< a load of a save made before the bank existed: nothing configured
	/// ev=cost and ev=activity per rule that ran (test-instruments builds; once per game: the destructor or the exit flush)
	void flushStats();

	// for GXRuleCtx
	Player *player() const { return m_player; }
	const gxrb::ConfigCore &config() const { return m_cfg; }
	const GXRuleSense &sense() const { return *m_sense; }
	void countTriggered(int rule);
	void countFired(int rule);
	void logEvent(int rule, const char *ev, const char *kv) const;

private:
	struct Instance
	{
		int rule;
		GXRuleImpl *impl;
		unsigned triggered, fired;
		unsigned long long calls, us;	///< test-instruments builds: dispatches and their time (us, this thread's CPU or wall)
		int depth;						///< > 0 while a dispatch of this rule runs: a nested one is not timed twice
		int testSkipped;				///< test-instruments builds: ticks the late_start red arm has swallowed (always 0 otherwise)
	};

	Instance *instance(int rule);		///< creates it on first use (its constructor resets its state)
	Instance *findInstance(int rule);
	const Instance *findInstance(int rule) const;
	void dropInstance(int rule);
	void afterAction(const std::vector<gxrb::Event> &ev, const std::vector<std::string> &before);
	std::vector<std::string> configSnapshot() const;
	unsigned tier() const;
	void testSelfHooks();				///< test-instruments builds: GX_RULE_PROBE_SLOW=hook's permission-hook site
	void senseLobby();					///< once per game and again after a load: each lobby slot's ORIGINAL template (TheGameInfo)
	void senseSample(unsigned now, Bool force);	///< the sighting pass (force: fixtures)
	void senseClear();					///< nothing known, nothing asked (a new game's state, before a load restores the block)
	void senseStart();					///< the first update of a game (and after a load): the lobby half, and whether the scripts ask at all
	Bool senseHasState() const;			///< the SENSE block is worth saving: asked, or something sighted

	// gxrb::LiveHooks
	virtual void beforeLiveChange(int rule, bool disabling) override;
	// gxrb::StateHooks
	virtual bool known(int rule) const override;
	virtual unsigned short stateVersion(int rule) const override;
	virtual bool hasOpenOperation(int rule) const override;
	virtual std::vector<int> rulesWithState() const override;
	virtual void xferState(int rule, unsigned short savedVersion, gxrb::ByteIO &io) override;

	Player *m_player;
	gxrb::ConfigCore m_cfg;
	gxrb::UnitClaims m_claims;
	gxrb::MoneyReserves m_reserves;
	std::vector<Instance *> m_inst;		///< sorted by rule id; heap nodes so a dispatch's Instance stays put if another is created
	GXRuleSense *m_sense;
	Bool m_flushed;
	Bool m_saving;						///< inside xfer() in a saving mode (test-instruments save fixtures read it)
	std::vector<gxrb::FactionSense> m_faction;	///< SENSE.ENEMY_FACTION, by player index (MAX_PLAYER_COUNT entries)
	Bool m_senseWanted;					///< a script asked about a player still unknown: the sighting pass runs
	Bool m_senseLobbyRead;				///< m_faction holds the lobby half (not saved: re-read after a load)
	Bool m_senseScanned;				///< senseStart ran this game (reset by a load: the scan runs again on the loaded scripts)
	Bool m_senseBlockLoaded;			///< this load already read a SENSE block (a second one refuses the load)
	unsigned long long m_senseCalls, m_senseUs, m_senseObjs;	///< test-instruments builds: the pass's cost (ev=cost n=SENSE)
	unsigned long long m_seenAsks = 0, m_seenUs = 0;		///< test-instruments builds: the SEEN conditions' asks and their time (ev=seen_cost)
	std::vector<unsigned> m_senseKnownAt;	///< test-instruments builds: the frame each player became known by sight (late_flip, logs)
	std::vector<std::string> m_seenLast;	///< test-instruments builds: "<key>=<0|1>" of every SEEN condition answer logged (ev=seen_cond)
	std::vector<std::string> m_senseTrue;	///< test-instruments builds: "<player>:<faction>" already logged TRUE once (ev=sense_cond)
	friend class GXRuleCtx;
	friend class GXRuleDispatchScope;
};

/// THE tier function (plan 2.8): every gate that asks a player's level asks this. HARD for a skirmish computer player at Hard
/// difficulty, else NONE, until the level table lands; GX_RULE_TIER_TEST=<tier>:<slot> in test-instruments builds. The slot is the
/// lobby slot, read from the player's original name "player<N>" (its name KEY survives the skirmish rename).
unsigned gxRuleTierOf(const Player *player);

/// The tier filter at Player::initFromDict's copy sites (Player.cpp): the player's qualified script copy, filtered in place for its tier
/// (groups first, then each kept group's scripts, then the top-level scripts). With no marked name anywhere it returns at once.
ScriptList *gxTierFilterScripts(ScriptList *scripts, const Player *player, const AsciiString &qualifier);	///< returns the list to use (may be a new shell); qualifier = what duplicateAndQualify appended
/// the side's skirmish team names in copy order -> the plan the team loop follows (identity when no team name is marked)
std::vector<gxrb::PlanRow> gxTierPlanTeams(const std::vector<std::string> &names, const Player *player);
/// the skirmish team a plan row copies: its src (the mark's, for a replacement). Test-instruments builds only, GX_TEST_TIER_MUTANT=team_at
/// copies the row's stock position instead - the broken copy N3's team-contents check must catch (Codex core-r3-lensB P1)
int gxTierTeamCopySource(const gxrb::PlanRow &row);
/// test-instruments builds: ev=tier_team for a team the filter REPLACED - the team AS STORED (after addTeam) and the stock twin's dict, by
/// their teamMaxInstances, so a gate proves the replacement's CONTENTS were taken, not only its name (Codex core-r2/r3-lensB)
void gxRuleLogTeamPick(const Player *player, TeamsInfo *stored, const Dict *stock, const std::string &name);
/// once per player per game start (test-instruments builds): ev=tier when GX_RULE_TIER_TEST set this player's tier
void gxRuleLogTier(const Player *player);

/// test-instruments builds: ev=refused why=not_skirmish_ai for a GX rule action aimed at a player without a bank (human, campaign AI)
void gxRuleLogNoBank(const Player *player, int rule);

/// test-instruments builds: ev=cost / ev=activity of every bank still alive (a process that _exits with the game running)
void gxRuleBankFlushAll();
