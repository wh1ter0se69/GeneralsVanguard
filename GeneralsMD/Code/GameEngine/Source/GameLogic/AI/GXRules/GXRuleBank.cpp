// GeneralsX @feature Claude 30/10/2026 AI RULE BANK, the engine side - see GXRuleBank.h. Everything this file decides is GXRuleCore.h's
// or GXTierFilter.h's (pure, unit-tested by scripts/test/gx-rulebank-test.cpp); this file adapts. Determinism (PLAN_rulebank.md 4.1): no
// random draw, rules in id order, own objects in ObjectID order, no wall clock / env / file in a decision - the clocks, the env knobs and
// every [GXRULE] line exist in test-instruments builds only.
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "Common/NameKeyGenerator.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/ThingFactory.h"
#include "Common/Dict.h"
#include "Common/WellKnownKeys.h"
#include "Common/Xfer.h"
#include "GameLogic/AI.h"
#include "GameLogic/AIPlayer.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/ObjectTypes.h"
#include "GameLogic/PolygonTrigger.h"
#include "GameLogic/Scripts.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/SidesList.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/GXHookTimer.h"
#include "GameLogic/GXRules/GXRuleBank.h"
#include "GameLogic/GXRules/GXRuleSense.h"
#include "GameNetwork/GameInfo.h"

// ---- the registry's rules: their factories (and, in test-instruments builds, one hook timer each), GX_RULE_TABLE expanded in place ----
#define GX_RB_DECL_MAKE(id, cls, name, tiers, vision, cadence, live) GXRuleImpl *gxRuleMake_##cls();
GX_RULE_TABLE(GX_RB_DECL_MAKE, GX_RB_SKIP7, GX_RB_SKIP4, GX_RB_SKIP2, GX_RB_SKIP1)
#undef GX_RB_DECL_MAKE

static GXRuleImpl *gxRuleMake(int id)
{
#define GX_RB_CASE_MAKE(rid, cls, name, tiers, vision, cadence, live) case rid: return gxRuleMake_##cls();
	switch (id)
	{
		GX_RULE_TABLE(GX_RB_CASE_MAKE, GX_RB_SKIP7, GX_RB_SKIP4, GX_RB_SKIP2, GX_RB_SKIP1)
		default:
			return nullptr;
	}
#undef GX_RB_CASE_MAKE
}

#if defined(GX_TEST_INSTRUMENTS)
// ONE timer per rule around EVERY dispatch of it - ticks, maintain() and every hook (PLAN 2.2.7): a placement rule does its work in a
// hook, so a tick-only timer would call it cheap. GXHookTimer sums a frame's calls and prints [GXHOOK] ... hook=rule_<NAME> over at 2 ms.
#define GX_RB_HOOK_STATS(id, cls, name, tiers, vision, cadence, live) GX_HOOK_STATS(gxRuleHook_##cls, "rule_" name);
GX_RULE_TABLE(GX_RB_HOOK_STATS, GX_RB_SKIP7, GX_RB_SKIP4, GX_RB_SKIP2, GX_RB_SKIP1)
#undef GX_RB_HOOK_STATS
static GXHookStats *gxRuleHookStats(int id)
{
#define GX_RB_CASE_HOOK(rid, cls, name, tiers, vision, cadence, live) case rid: return &gxRuleHook_##cls;
	switch (id)
	{
		GX_RULE_TABLE(GX_RB_CASE_HOOK, GX_RB_SKIP7, GX_RB_SKIP4, GX_RB_SKIP2, GX_RB_SKIP1)
		default:
			return nullptr;
	}
#undef GX_RB_CASE_HOOK
}
#endif

// ---- [GXRULE] lines (test-instruments builds only) ----
#if defined(GX_TEST_INSTRUMENTS)
static const char *gxRuleName(int rule)
{
	const gxrb::RuleRow *r = gxrb::findRule(gxrb::gxRegistry(), rule);
	return r != nullptr ? r->name : "?";
}
/// a value with no spaces (CONTRACT 2.6.4): spaces become '~'
static std::string gxRuleWord(const std::string &s)
{
	std::string out = s.empty() ? std::string("-") : s;
	for (size_t i = 0; i < out.size(); ++i)
		if (out[i] == ' ' || out[i] == '\t')
			out[i] = '~';
	return out;
}
static void gxRuleLine(int player, int rule, const char *ev, const std::string &kv)
{
	fprintf(stderr, "[GXRULE] f=%u p=%d r=%d n=%s ev=%s%s%s\n", TheGameLogic ? (unsigned)TheGameLogic->getFrame() : 0u, player, rule,
		rule > 0 ? gxRuleName(rule) : "-", ev, kv.empty() ? "" : " ", kv.c_str());
	fflush(stderr);
}
static Bool gxRuleEnvIs(const char *name, const char *value)
{
	const char *v = getenv(name);
	return v != nullptr && strcmp(v, value) == 0;
}
/// SENSE.ENEMY_FACTION's lines: rule 0 is the bank's own sensor (n=SENSE), every event named sense_*
static void gxSenseLine(int player, const char *ev, const std::string &kv)
{
	fprintf(stderr, "[GXRULE] f=%u p=%d r=0 n=SENSE ev=%s%s%s\n", TheGameLogic ? (unsigned)TheGameLogic->getFrame() : 0u, player, ev,
		kv.empty() ? "" : " ", kv.c_str());
	fflush(stderr);
}

// the bank's dispatch clock: this thread's CPU where the platform has one (clock=thread), else wall time (clock=wall: Windows today -
// never CPU evidence, PLAN 3.7)
static long long gxRuleClockUs(Bool *thread)
{
#ifndef _WIN32
	struct timespec t;
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) == 0)
	{
		*thread = TRUE;
		return t.tv_sec * 1000000LL + t.tv_nsec / 1000;
	}
#endif
	*thread = FALSE;
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static Bool s_gxRuleThreadClock = TRUE;

void (*g_gxHookFlushExtra)() = nullptr;			///< GXHookTimer.h: run by GX_HOOK_FLUSH_ALL() before "[GXHOOK] exit"
static std::vector<GXRuleBank *> s_gxRuleBanks;		///< live banks, for the exit flush
static void gxRuleBankFlushHook() { gxRuleBankFlushAll(); }
#endif

/// times one dispatch of one rule; a dispatch nested inside another of the SAME rule is not timed again (it would count twice)
class GXRuleDispatchScope
{
public:
#if defined(GX_TEST_INSTRUMENTS)
	explicit GXRuleDispatchScope(GXRuleBank::Instance &in) : m_in(in), m_timer(nullptr), m_t0(0)
	{
		if (m_in.depth++ != 0)
			return;
		GXHookStats *s = gxRuleHookStats(m_in.rule);
		if (s != nullptr)
			m_timer = new GXHookTimer(*s);
		m_t0 = gxRuleClockUs(&s_gxRuleThreadClock);
	}
	~GXRuleDispatchScope()
	{
		if (--m_in.depth != 0)
			return;
		Bool thread = TRUE;
		const long long t1 = gxRuleClockUs(&thread);
		m_in.us += (unsigned long long)(t1 > m_t0 ? t1 - m_t0 : 0);
		++m_in.calls;
		delete m_timer;
	}
private:
	GXRuleBank::Instance &m_in;
	GXHookTimer *m_timer;
	long long m_t0;
#else
	explicit GXRuleDispatchScope(GXRuleBank::Instance &in) { (void)in; }
#endif
};

// ---- the save stream over Xfer ----
namespace
{
class GXXferIO : public gxrb::ByteIO
{
public:
	explicit GXXferIO(Xfer *x) : m_x(x) {}
	virtual bool saving() const override { return m_x->getXferMode() != XFER_LOAD; }	// CRC modes hash what a save writes
	virtual void u8(unsigned char *v) override { UnsignedByte b = *v; m_x->xferUnsignedByte(&b); *v = b; }
	virtual void u16(unsigned short *v) override { UnsignedShort s = *v; m_x->xferUnsignedShort(&s); *v = s; }
	virtual void i32(int *v) override { Int i = *v; m_x->xferInt(&i); *v = i; }
	virtual void u32(unsigned *v) override { UnsignedInt u = *v; m_x->xferUnsignedInt(&u); *v = u; }
	virtual void str(std::string *v) override
	{
		AsciiString a(v->c_str());
		m_x->xferAsciiString(&a);
		*v = a.str();
	}
	virtual void bytes(void *p, int n) override { m_x->xferUser(p, n); }
	virtual int beginBlock() override { return m_x->beginBlock(); }
	virtual void endBlock() override { m_x->endBlock(); }
	virtual void skip(int bytes) override { m_x->skip(bytes); }
	virtual void refuse() override { throw XFER_INVALID_VERSION; }	// GameState::loadGame refuses the whole save (SC_INVALID_DATA)
private:
	Xfer *m_x;
};
}

// ================================================================================================================================
// GXRuleCtx
// ================================================================================================================================
Player *GXRuleCtx::player() const { return m_bank.player(); }
int GXRuleCtx::playerIndex() const { return m_bank.player()->getPlayerIndex(); }
unsigned GXRuleCtx::frame() const { return (unsigned)TheGameLogic->getFrame(); }
int GXRuleCtx::param(int key) const { return m_bank.config().param(m_rule, key); }
AsciiString GXRuleCtx::ref(int key) const
{
	const std::string *s = m_bank.config().ref(m_rule, key);
	return s != nullptr ? AsciiString(s->c_str()) : AsciiString::TheEmptyString;
}
int GXRuleCtx::money() const { return (int)m_bank.player()->getMoney()->countMoney(); }
namespace
{
void gxRuleCollectOwn(Object *o, void *userData)
{
	if (o != nullptr && !o->isDestroyed())
		static_cast<std::vector<Object *> *>(userData)->push_back(o);
}
bool gxRuleById(const Object *a, const Object *b) { return a->getID() < b->getID(); }
}
void GXRuleCtx::ownObjects(std::vector<Object *> *out) const
{
	out->clear();
	m_bank.player()->iterateObjects(gxRuleCollectOwn, out);
	std::sort(out->begin(), out->end(), gxRuleById);
}
const GXRuleSense &GXRuleCtx::sense() const { return m_bank.sense(); }
gxrb::UnitClaims &GXRuleCtx::claims() const { return m_bank.claims(); }
gxrb::MoneyReserves &GXRuleCtx::reserves() const { return m_bank.reserves(); }
Bool GXRuleCtx::orderMoveTo(Object *own, const Coord3D &to) const
{
	if (own == nullptr || own->getControllingPlayer() != m_bank.player() || own->getAI() == nullptr)
		return FALSE;
	own->getAI()->aiMoveToPosition(&to, CMD_FROM_AI);
	return TRUE;
}
void GXRuleCtx::triggered() const { m_bank.countTriggered(m_rule); }
void GXRuleCtx::fired() const { m_bank.countFired(m_rule); }
void GXRuleCtx::log(const char *ev, const char *kv) const { m_bank.logEvent(m_rule, ev, kv); }

// ================================================================================================================================
// GXRuleBank
// ================================================================================================================================
GXRuleBank::GXRuleBank(Player *player) :
	m_player(player),
	m_cfg(gxrb::gxRegistry(), RETAIL_COMPATIBLE_XFER_SAVE != 0),
	m_sense(new GXRuleSense(player, gxRuleTierOf)),
	m_flushed(FALSE),
	m_saving(FALSE),
	m_senseWanted(FALSE),
	m_senseLobbyRead(FALSE),
	m_senseScanned(FALSE),
	m_senseBlockLoaded(FALSE),
	m_senseCalls(0),
	m_senseUs(0),
	m_senseObjs(0)
{
	m_cfg.attachTables(&m_claims, &m_reserves);
	senseClear();
#if defined(GX_TEST_INSTRUMENTS)
	s_gxRuleBanks.push_back(this);
	g_gxHookFlushExtra = gxRuleBankFlushHook;
	// the ENGINE's logic rate, straight from the constant the simulation runs at (not the bank's own dispatch argument): a gate derives
	// every rule's expected frame stride and phase from it and the registry cadence (Codex core-r8 P1/P2)
	char kv[48];
	snprintf(kv, sizeof(kv), "fps=%d", (int)LOGICFRAMES_PER_SECOND);
	gxRuleLine(player != nullptr ? player->getPlayerIndex() : -1, 0, "bank", kv);
#endif
}

GXRuleBank::~GXRuleBank()
{
	flushStats();
#if defined(GX_TEST_INSTRUMENTS)
	s_gxRuleBanks.erase(std::remove(s_gxRuleBanks.begin(), s_gxRuleBanks.end(), this), s_gxRuleBanks.end());
#endif
	for (size_t i = 0; i < m_inst.size(); ++i)
	{
		delete m_inst[i]->impl;
		delete m_inst[i];
	}
	m_inst.clear();
	delete m_sense;
}

unsigned GXRuleBank::tier() const { return gxRuleTierOf(m_player); }

GXRuleBank::Instance *GXRuleBank::findInstance(int rule)
{
	for (size_t i = 0; i < m_inst.size(); ++i)
		if (m_inst[i]->rule == rule)
			return m_inst[i];
	return nullptr;
}
const GXRuleBank::Instance *GXRuleBank::findInstance(int rule) const
{
	for (size_t i = 0; i < m_inst.size(); ++i)
		if (m_inst[i]->rule == rule)
			return m_inst[i];
	return nullptr;
}
GXRuleBank::Instance *GXRuleBank::instance(int rule)
{
	Instance *in = findInstance(rule);
	if (in != nullptr)
		return in;
	GXRuleImpl *impl = gxRuleMake(rule);
	if (impl == nullptr)
		return nullptr;
	in = new Instance;
	in->rule = rule;
	in->impl = impl;
	in->triggered = in->fired = 0;
	in->calls = in->us = 0;
	in->depth = 0;
	in->testSkipped = 0;
	size_t at = 0;
	while (at < m_inst.size() && m_inst[at]->rule < rule)
		++at;
	m_inst.insert(m_inst.begin() + (long)at, in);
	return in;
}
void GXRuleBank::dropInstance(int rule)
{
	for (size_t i = 0; i < m_inst.size(); ++i)
		if (m_inst[i]->rule == rule)
		{
			delete m_inst[i]->impl;
			delete m_inst[i];
			m_inst.erase(m_inst.begin() + (long)i);
			return;
		}
}

#if defined(GX_TEST_INSTRUMENTS)
// GX_RULE_TEST_COUNTERS=<N> (257..1000; test-instruments builds only, the lead's MAX_COUNTERS / MAX_FLAGS condition 30/10): once per process,
// at the first bank update on or after frame 300. A process that finds no "gxrbc_0" PLANTS N counters gxrbc_<i> = 7i + 1 and N flags
// gxrbf_<i> = (i even) (ev=test_counters); one that finds it - the loaded half of a save/load - CHECKS every one by name and value
// (ev=test_counters_check). rb-saveload-judge.py's loads-counters kind reads both.
static void gxTestCountersOnce()
{
	static Bool done = FALSE;
	const char *v = getenv("GX_RULE_TEST_COUNTERS");
	if (done || v == nullptr || TheScriptEngine == nullptr || TheGameLogic == nullptr || TheGameLogic->getFrame() < 300u)
		return;
	done = TRUE;
	const int n = atoi(v);
	if (n < 257 || n > 1000)
	{
		gxRuleLine(-1, 0, "test_counters", std::string("why=bad_n value=") + gxRuleWord(v));
		return;
	}
	char name[32];
	Int probe = 0;
	const Bool loaded = TheScriptEngine->gxTestCounter(AsciiString("gxrbc_0"), &probe);
	int cok = 0, fok = 0;
	for (int i = 0; i < n; ++i)
	{
		snprintf(name, sizeof(name), "gxrbc_%d", i);
		if (loaded)
		{
			Int c = -1;
			cok += (TheScriptEngine->gxTestCounter(AsciiString(name), &c) && c == 7 * i + 1) ? 1 : 0;
		}
		else
			cok += TheScriptEngine->gxTestSetCounter(AsciiString(name), 7 * i + 1) > 0 ? 1 : 0;
		snprintf(name, sizeof(name), "gxrbf_%d", i);
		if (loaded)
		{
			Bool f = FALSE;
			fok += (TheScriptEngine->gxTestFlag(AsciiString(name), &f) && f == ((i % 2) == 0)) ? 1 : 0;
		}
		else
			fok += TheScriptEngine->gxTestSetFlag(AsciiString(name), (i % 2) == 0) > 0 ? 1 : 0;
	}
	Int nf = 0;
	const Int nc = TheScriptEngine->gxTestNumCounters(&nf);
	char kv[160];
	snprintf(kv, sizeof(kv), "%s=%d counters_ok=%d flags_ok=%d used=%d flags_used=%d max=%d", loaded ? "checked" : "planted", n, cok, fok,
		(int)nc, (int)nf, (int)ScriptEngine::MAX_COUNTERS);
	gxRuleLine(-1, 0, loaded ? "test_counters_check" : "test_counters", kv);
}
#endif

void GXRuleBank::update()
{
#if defined(GX_TEST_INSTRUMENTS)
	gxTestCountersOnce();
#endif
	if (!m_senseScanned)
		senseStart();		// SENSE.ENEMY_FACTION (rb-sense-1): once per game - do the scripts ask at all
	if (m_senseWanted)
		senseSample((unsigned)TheGameLogic->getFrame(), FALSE);
	if (!m_cfg.anyEnabled())
		return;		// the stock path: one test per player per frame
	const unsigned now = (unsigned)TheGameLogic->getFrame();
	m_claims.expire(now);
	m_reserves.expire(now);
	const int pi = m_player->getPlayerIndex();
	// GXRuleCore.h chooses (and marks dispatched) this frame's maintain()s and tick()s; this only runs them
	unsigned fps = (unsigned)LOGICFRAMES_PER_SECOND;
#if defined(GX_TEST_INSTRUMENTS)
	static const Bool fpsHalf = gxRuleEnvIs("GX_TEST_RULE_MUTANT", "fps_half");
	if (fpsHalf)
		fps /= 2u;		// a BROKEN scheduler rate (the frequency argument halved): the tier gate's stride check must catch it
#endif
	const std::vector<gxrb::ConfigCore::Dispatch> due = m_cfg.takeFrame(now, pi, fps,
		[this](int rule) { Instance *in = instance(rule); return in != nullptr && in->impl->hasMaintain() != FALSE; });
	for (size_t i = 0; i < due.size(); ++i)
	{
		Instance *in = instance(due[i].rule);
		if (in == nullptr)
			continue;
#if defined(GX_TEST_INSTRUMENTS)
		static const Bool lateStart = gxRuleEnvIs("GX_TEST_RULE_MUTANT", "late_start");
		if (lateStart && !due[i].maintain && in->testSkipped < 7)
		{
			++in->testSkipped;	// a BROKEN scheduler that misses a rule's first 7 ticks: the tier gate's first-dispatch check must catch it
			continue;
		}
#endif
		GXRuleDispatchScope scope(*in);
		GXRuleCtx ctx(*this, due[i].rule);
		if (due[i].maintain)
			in->impl->maintain(ctx);
		else
			in->impl->tick(ctx);
	}
#if defined(GX_TEST_INSTRUMENTS)
	testSelfHooks();
#endif
}

int GXRuleBank::valueHook(int hook, const char *context, int stock)
{
	const int owner = m_cfg.takeValueHook(hook, context);	// marks it dispatched
	if (owner == 0)
		return stock;
	Instance *in = instance(owner);
	if (in == nullptr)
		return stock;
	GXRuleDispatchScope scope(*in);
	GXRuleCtx ctx(*this, owner);
	return in->impl->valueHook(ctx, hook, context, stock);
}

Bool GXRuleBank::permitHook(int hook, const char *context)
{
	if (!m_cfg.anyEnabled())
		return TRUE;
	const std::vector<int> joined = m_cfg.takePermitHook(hook,
		[this](int rule, int h) { Instance *in = instance(rule); return in != nullptr && in->impl->joinsPermitHook(h) != FALSE; });
	return gxrb::ConfigCore::permitAll(joined, [this, hook, context](int rule) {
		Instance *in = instance(rule);
		GXRuleDispatchScope scope(*in);
		GXRuleCtx ctx(*this, rule);
		return in->impl->permitHook(ctx, hook, context) != FALSE;
	}) ? TRUE : FALSE;
}

// ---- SENSE.ENEMY_FACTION (GeneralsX 01/11/2026 rb-sense-1; gx-handoff ai/rulebank/core/NEXT_rb-sense-1.md) ----
// Determinism: the lobby half is each slot's ORIGINAL template (GameSlot::saveOriginalSetup, recorded before the random draw on every
// peer, in a replay's header and in a skirmish save), the sighting half is the logic's own partition, shroud and stealth state, sampled
// at frames fixed by the logic frame and the player index. Nothing reads the local player, the client's shroud or the UI.
void GXRuleBank::senseClear()
{
	m_faction.assign((size_t)MAX_PLAYER_COUNT, gxrb::FactionSense());
	m_senseWanted = FALSE;
	m_senseLobbyRead = FALSE;
	m_senseScanned = FALSE;
	m_senseBlockLoaded = FALSE;
#if defined(GX_TEST_INSTRUMENTS)
	m_senseKnownAt.assign((size_t)MAX_PLAYER_COUNT, 0u);
	m_senseTrue.clear();
	m_seenLast.clear();
#endif
}

Bool GXRuleBank::senseHasState() const
{
	if (m_senseWanted)
		return TRUE;
	for (size_t i = 0; i < m_faction.size(); ++i)
		if (!m_faction[i].seenBase.empty() || m_faction[i].mixed || !m_faction[i].tmpl.empty())
			return TRUE;
	return FALSE;
}

namespace
{
Bool gxSenseScriptAsks(Script *sc)
{
	for (OrCondition *o = sc != nullptr ? sc->getOrCondition() : nullptr; o != nullptr; o = o->getNextOrCondition())
		for (Condition *c = o->getFirstAndCondition(); c != nullptr; c = c->getNext())
			if (c->getConditionType() == Condition::AI_PLAYER_GX_SENSE)
				return TRUE;
	return FALSE;
}
/// does any loaded script list (every side's: the players' qualified copies and the map's own) hold an AI_PLAYER_GX_SENSE condition -
/// a property of the loaded data, the same on every peer, in a replay and after a load
Bool gxSenseScriptsAsk()
{
	if (TheSidesList == nullptr)
		return FALSE;
	for (Int i = 0; i < TheSidesList->getNumSides(); ++i)
	{
		SidesInfo *si = TheSidesList->getSideInfo(i);
		ScriptList *sl = si != nullptr ? si->getScriptList() : nullptr;
		if (sl == nullptr)
			continue;
		for (Script *sc = sl->getScript(); sc != nullptr; sc = sc->getNext())
			if (gxSenseScriptAsks(sc))
				return TRUE;
		for (ScriptGroup *g = sl->getScriptGroup(); g != nullptr; g = g->getNext())
			for (Script *sc = g->getScript(); sc != nullptr; sc = sc->getNext())
				if (gxSenseScriptAsks(sc))
					return TRUE;
	}
	return FALSE;
}
}

void GXRuleBank::senseStart()
{
	m_senseScanned = TRUE;
	senseLobby();
	const Bool ask = gxSenseScriptsAsk();
	m_senseWanted = gxrb::senseShouldSample(ask != FALSE, RETAIL_COMPATIBLE_XFER_SAVE != 0) ? TRUE : FALSE;
#if defined(GX_TEST_INSTRUMENTS)
	if (ask)
	{
		char kv[64];
		snprintf(kv, sizeof(kv), "scripts_ask=1 sample=%d", m_senseWanted ? 1 : 0);
		gxSenseLine(m_player->getPlayerIndex(), "sense_start", kv);
	}
#endif
}

void GXRuleBank::senseLobby()
{
	if (m_senseLobbyRead)
		return;
	m_senseLobbyRead = TRUE;
	if (TheGameInfo == nullptr || ThePlayerList == nullptr || ThePlayerTemplateStore == nullptr || TheNameKeyGenerator == nullptr)
		return;		// no lobby (a campaign): every player stays unknown
	for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)
	{
		const Player *p = ThePlayerList->getNthPlayer(i);
		if (p == nullptr || p == m_player || p->getPlayerIndex() < 0 || (size_t)p->getPlayerIndex() >= m_faction.size())
			continue;
		const int slot = gxrb::slotOfPlayerName(TheNameKeyGenerator->keyToName(p->getPlayerNameKey()).str());
		if (slot < 0 || slot >= MAX_SLOTS)
			continue;	// not a lobby player (neutral, civilian, a map's own sides): never known
		const GameSlot *gs = TheGameInfo->getConstSlot(slot);
		if (gs == nullptr || !gs->isOccupied())
			continue;
		gxrb::FactionSense &f = m_faction[(size_t)p->getPlayerIndex()];
		const Int orig = gs->hasSavedOriginalSetup() ? gs->getOriginalPlayerTemplate() : (Int)PLAYERTEMPLATE_RANDOM;
		const PlayerTemplate *pt = orig >= 0 ? ThePlayerTemplateStore->getNthPlayerTemplate(orig) : nullptr;
		if (pt != nullptr && pt->isPlayableSide())
			gxrb::senseFromLobby(f, pt->getBaseSide().str(), pt->getSide().str());
		else if (orig == PLAYERTEMPLATE_RANDOM)
			gxrb::senseTrack(f);	// what the lobby shows a non-ally: "Random" - unknown until fairly seen
		// an observer slot (PLAYERTEMPLATE_OBSERVER) is never known
#if defined(GX_TEST_INSTRUMENTS)
		char kv[160];
		snprintf(kv, sizeof(kv), "enemy=%d slot=%d orig=%s known=%s general=%s", (int)p->getPlayerIndex(), slot,
			orig == PLAYERTEMPLATE_RANDOM ? "random" : orig == PLAYERTEMPLATE_OBSERVER ? "observer" : (pt != nullptr ? pt->getName().str() : "?"),
			gxRuleWord(f.known).c_str(), gxRuleWord(f.general).c_str());
		gxSenseLine(m_player->getPlayerIndex(), "sense_lobby", kv);
		// the TRUTH of a Random slot, for the judge only (never a decision): its resolved template, read after the draw
		if (orig == PLAYERTEMPLATE_RANDOM)
		{
			const PlayerTemplate *now = ThePlayerTemplateStore->getNthPlayerTemplate(gs->getPlayerTemplate());
			snprintf(kv, sizeof(kv), "enemy=%d base=%s side=%s", (int)p->getPlayerIndex(), now ? gxRuleWord(now->getBaseSide().str()).c_str() : "-",
				now ? gxRuleWord(now->getSide().str()).c_str() : "-");
			gxSenseLine(m_player->getPlayerIndex(), "sense_truth", kv);
		}
#endif
	}
}

void GXRuleBank::senseSample(unsigned now, Bool force)
{
	if (RETAIL_COMPATIBLE_XFER_SAVE != 0)
		return;		// its saves carry no bank block: what it saw would not survive a save/load (senseShouldSample), so it never looks
	const unsigned period = (unsigned)LOGICFRAMES_PER_SECOND / 2u > 0u ? (unsigned)LOGICFRAMES_PER_SECOND / 2u : 1u;
	const unsigned pi = (unsigned)m_player->getPlayerIndex();
	if (!force && ((now + pi) % period) != 0u)
		return;
	senseLobby();
	// GeneralsX @feature Claude 06/11/2026 a PlayerMaskType (64-bit), not 32 bits: MAX_PLAYER_COUNT is 40.
	PlayerMaskType track = 0u;
	for (size_t i = 0; i < m_faction.size() && i < (size_t)MAX_PLAYER_COUNT; ++i)
		if (gxrb::senseOpen(m_faction[i]))
			track |= gxPlayerBit((Int)i);
	if (track == 0u)
		return;		// every tracked player is known or mixed: nothing left to look for
#if defined(GX_TEST_INSTRUMENTS)
	Bool thread = TRUE;
	const long long t0 = gxRuleClockUs(&thread);
#endif
	std::vector<std::vector<gxrb::Sighting> > seen(m_faction.size());
	const Int objs = m_sense->collectSightings(track, &seen);
	for (size_t i = 0; i < m_faction.size() && i < (size_t)MAX_PLAYER_COUNT; ++i)
	{
		if ((track & gxPlayerBit((Int)i)) == 0u)
			continue;
#if defined(GX_TEST_INSTRUMENTS)
		const size_t tmplBefore = m_faction[i].tmpl.size();
#endif
		const Bool known = gxrb::senseApplyPass(m_faction[i], seen[i]) ? TRUE : FALSE;
#if defined(GX_TEST_INSTRUMENTS)
		const gxrb::FactionSense &f = m_faction[i];
		// ev=sense_tmpl (rb-sense-2, judge evidence only): each template the moment it joins the sighting list, with its nearest own eye
		// NOW (distance/vision range@eye) - so a template seen early and gone by the flip still shows the eye that saw it
		for (size_t k = tmplBefore; k < f.tmpl.size(); ++k)
		{
			char kt[384];
			const std::string ev = m_sense->eyeEvidenceForTest(ThePlayerList->getNthPlayer((Int)i), std::vector<std::string>(1, f.tmpl[k]));
			snprintf(kt, sizeof(kt), "enemy=%d base=%s n=%d eyes=%s%s", (int)i, gxRuleWord(f.seenBase).c_str(), (int)(k + 1), gxRuleWord(ev).c_str(),
				force ? " test=1" : "");
			gxSenseLine((int)pi, "sense_tmpl", kt);
		}
		if (known || f.mixed)
		{
			std::string t;
			for (size_t k = 0; k < f.tmpl.size(); ++k)
				t += (k ? "," : "") + f.tmpl[k];
			char kv[768];
			// eyes= (rb-sense-2, judge evidence only): each sighted template's nearest own eye, distance/vision range@its template
			const std::string ev = m_sense->eyeEvidenceForTest(ThePlayerList->getNthPlayer((Int)i), f.tmpl);
			snprintf(kv, sizeof(kv), "enemy=%d faction=%s via=sight tmpl=%s eyes=%s%s", (int)i, gxRuleWord(known ? f.known : f.seenBase).c_str(),
				gxRuleWord(t).c_str(), gxRuleWord(ev).c_str(), force ? " test=1" : "");
			gxSenseLine((int)pi, known ? "sense_known" : "sense_mixed", kv);
			m_senseKnownAt[i] = now;
		}
#else
		(void)known;
#endif
	}
#if defined(GX_TEST_INSTRUMENTS)
	const long long t1 = gxRuleClockUs(&thread);
	++m_senseCalls;
	m_senseUs += (unsigned long long)(t1 > t0 ? t1 - t0 : 0);
	m_senseObjs += (unsigned long long)(objs > 0 ? objs : 0);
#else
	(void)objs;
#endif
}

Bool GXRuleBank::senseEnemyFaction(const Player *enemy, const char *faction)
{
	senseLobby();
	if (enemy == nullptr || enemy == m_player || faction == nullptr || enemy->getPlayerIndex() < 0
		|| (size_t)enemy->getPlayerIndex() >= m_faction.size())
		return FALSE;
	const size_t ei = (size_t)enemy->getPlayerIndex();
	gxrb::FactionSense &f = m_faction[ei];
	Bool answer = gxrb::senseAnswer(f, faction) ? TRUE : FALSE;
#if defined(GX_TEST_INSTRUMENTS)
	// GX_TEST_SENSE_MUTANT=get_side: the LEAKY read the design forbids - the resolved side (a Random slot's draw) answers at once. The
	// Random-enemy fixture and S1's judge must FAIL on it (plan 1.3's fair-play rule, the lead's red arm)
	static const Bool getSide = gxRuleEnvIs("GX_TEST_SENSE_MUTANT", "get_side");
	if (getSide)
	{
		const PlayerTemplate *pt = enemy->getPlayerTemplate();
		answer = pt != nullptr && (pt->getSide() == faction || pt->getBaseSide() == faction) ? TRUE : FALSE;
	}
	// GX_TEST_SENSE_MUTANT=late_flip: a sighting's answer withheld 31 frames - the replay CRC arm's red (a flip at another frame differs)
	static const Bool lateFlip = gxRuleEnvIs("GX_TEST_SENSE_MUTANT", "late_flip");
	if (lateFlip && answer && f.tracked && (unsigned)TheGameLogic->getFrame() < m_senseKnownAt[ei] + 31u)
		answer = FALSE;
	if (answer)
	{
		const std::string key = std::to_string((int)ei) + ":" + faction;
		if (std::find(m_senseTrue.begin(), m_senseTrue.end(), key) == m_senseTrue.end())
		{
			m_senseTrue.push_back(key);
			char kv[160];
			snprintf(kv, sizeof(kv), "enemy=%d faction=%s result=1 known=%s via=%s%s", (int)ei, gxRuleWord(faction).c_str(),
				gxRuleWord(f.known).c_str(), f.known.empty() ? "-" : (f.tracked ? "sight" : "lobby"), getSide ? " mutant=get_side" : "");
			gxSenseLine(m_player->getPlayerIndex(), "sense_cond", kv);
		}
	}
#endif
	return answer;
}

// ---- SENSE.FOG_CONDITIONS (GeneralsX 02/11/2026 rb-sense-2) ----
Int GXRuleBank::seenCount(const Player *who, const ObjectTypes &types)
{
	if (who == nullptr || who == m_player || m_sense == nullptr)
		return -1;
#if defined(GX_TEST_INSTRUMENTS)
	Bool thread = TRUE;
	const long long t0 = gxRuleClockUs(&thread);
#endif
	const std::vector<GXRuleSense::Seen> seen = m_sense->seenOf(who->getPlayerIndex());
	Int n = 0;
	for (size_t i = 0; i < seen.size(); ++i)
		if (types.isInSet(seen[i].tmpl))
			++n;
#if defined(GX_TEST_INSTRUMENTS)
	const long long t1 = gxRuleClockUs(&thread);
	++m_seenAsks;
	m_seenUs += (unsigned long long)(t1 > t0 ? t1 - t0 : 0);
#endif
	return n;
}

Int GXRuleBank::seenValueIn(const Player *who, const PolygonTrigger *area)
{
	if (who == nullptr || who == m_player || area == nullptr || m_sense == nullptr)
		return -1;
#if defined(GX_TEST_INSTRUMENTS)
	Bool thread = TRUE;
	const long long t0 = gxRuleClockUs(&thread);
#endif
	const std::vector<GXRuleSense::Seen> seen = m_sense->seenOf(who->getPlayerIndex());
	Int v = 0;
	for (size_t i = 0; i < seen.size(); ++i)
	{
		if (seen[i].value <= 0)
			continue;
		ICoord3D ip;		// Object::setTriggerAreaFlagsForChangeInPosition's integer point: trigger areas compare on x, y only
		ip.x = REAL_TO_INT(seen[i].pos.x);
		ip.y = REAL_TO_INT(seen[i].pos.y);
		ip.z = 0;
		if (area->pointInTrigger(ip))
			v += seen[i].value;
	}
#if defined(GX_TEST_INSTRUMENTS)
	const long long t1 = gxRuleClockUs(&thread);
	++m_seenAsks;
	m_seenUs += (unsigned long long)(t1 > t0 ? t1 - t0 : 0);
#endif
	return v;
}

#if defined(GX_TEST_INSTRUMENTS)
void GXRuleBank::seenLogAnswer(const char *kind, const Player *who, const std::string &what, int op, int want, int got, Bool answer)
{
	const std::string key = std::string(kind) + ":" + std::to_string(who != nullptr ? (int)who->getPlayerIndex() : -1) + ":" + what + ":"
		+ std::to_string(op) + ":" + std::to_string(want);
	const std::string now = key + "=" + (answer ? "1" : "0");
	size_t i = 0;
	while (i < m_seenLast.size() && m_seenLast[i].compare(0, key.size() + 1, key + "=") != 0)
		++i;
	if (i < m_seenLast.size())
	{
		if (m_seenLast[i] == now)
			return;
		m_seenLast[i] = now;
	}
	else
	{
		m_seenLast.push_back(now);
		if (!answer)
			return;		// the first answer is logged only when TRUE: FALSE is every condition's start
	}
	char kv[320];
	snprintf(kv, sizeof(kv), "kind=%s player=%d what=%s op=%d want=%d got=%d result=%d", kind, who != nullptr ? (int)who->getPlayerIndex() : -1,
		gxRuleWord(what).c_str(), op, want, got, answer ? 1 : 0);
	gxSenseLine(m_player->getPlayerIndex(), "seen_cond", kv);
}

void GXRuleBank::senseSampleNowForTest()
{
	senseSample((unsigned)TheGameLogic->getFrame(), TRUE);
}
const gxrb::FactionSense *GXRuleBank::senseStateForTest(const Player *p) const
{
	return (p != nullptr && p->getPlayerIndex() >= 0 && (size_t)p->getPlayerIndex() < m_faction.size()) ? &m_faction[(size_t)p->getPlayerIndex()] : nullptr;
}
#endif

// ---- the three actions ----
std::vector<std::string> GXRuleBank::configSnapshot() const
{
	std::vector<std::string> out;
#if defined(GX_TEST_INSTRUMENTS)
	const gxrb::Registry &g = gxrb::gxRegistry();
	const std::vector<gxrb::RuleConfig> &all = m_cfg.all();
	for (size_t i = 0; i < all.size(); ++i)
	{
		const gxrb::RuleConfig &c = all[i];
		std::string s = "on=" + std::string(c.accepted ? "1" : "0") + " v=";
		bool first = true;
		for (int k = 0; k < g.nParams; ++k)
			if (g.params[k].rule == c.rule)
			{
				char buf[48];
				snprintf(buf, sizeof(buf), "%s%d:%d", first ? "" : ",", g.params[k].key, m_cfg.param(c.rule, g.params[k].key));
				s += buf;
				first = false;
			}
		if (first)
			s += "-";
		s += " refs=";
		for (size_t k = 0; k < c.refs.size(); ++k)
		{
			char buf[16];
			snprintf(buf, sizeof(buf), "%s%d:", k ? "," : "", c.refs[k].key);
			s += buf + gxRuleWord(c.refs[k].name);
		}
		if (c.refs.empty())
			s += "-";
		char head[16];
		snprintf(head, sizeof(head), "%d|", c.rule);
		out.push_back(head + s);
	}
#endif
	return out;
}

void GXRuleBank::afterAction(const std::vector<gxrb::Event> &ev, const std::vector<std::string> &before)
{
#if defined(GX_TEST_INSTRUMENTS)
	const int p = m_player->getPlayerIndex();
	for (size_t i = 0; i < ev.size(); ++i)
	{
		const gxrb::Event &e = ev[i];
		char kv[160];
		if (strcmp(e.ev, "clamp") == 0)
			snprintf(kv, sizeof(kv), "k=%d asked=%d used=%d", e.key, e.asked, e.used);
		else if (strcmp(e.ev, "conflict") == 0)
			snprintf(kv, sizeof(kv), "why=%s with=%d", e.why, e.other);
		else if (e.key >= 0)
			snprintf(kv, sizeof(kv), "why=%s k=%d", e.why, e.key);
		else
			snprintf(kv, sizeof(kv), "why=%s", e.why);
		gxRuleLine(p, e.rule, e.ev, kv);
	}
	// ev=config for every rule whose configuration changed (the acted rule, and any a conflict switched off)
	const std::vector<std::string> after = configSnapshot();
	for (size_t i = 0; i < after.size(); ++i)
	{
		if (std::find(before.begin(), before.end(), after[i]) != before.end())
			continue;
		const size_t bar = after[i].find('|');
		gxRuleLine(p, atoi(after[i].c_str()), "config", after[i].substr(bar + 1));
	}
#else
	(void)ev;
	(void)before;
#endif
}

void GXRuleBank::action(int rule, int key, int value)
{
#if defined(GX_TEST_INSTRUMENTS)
	std::vector<gxrb::Event> ev;
	const std::vector<std::string> before = configSnapshot();
	m_cfg.setInt(rule, key, value, tier(), &ev, this);
	afterAction(ev, before);
#else
	m_cfg.setInt(rule, key, value, tier(), nullptr, this);
#endif
}

void GXRuleBank::actionTemplate(int rule, int key, const AsciiString &templateName)
{
#if defined(GX_TEST_INSTRUMENTS)
	std::vector<gxrb::Event> ev;
	const std::vector<std::string> before = configSnapshot();
	if (templateName.getLength() > 255)
		ev.push_back(gxrb::mkEvent(rule, "unknown", "too_long", key));	// a save writes names with a one-byte length
	else if (gxRuleEnvIs("GX_TEST_RULE_MUTANT", "template_noop"))
		return;		// a BROKEN handler that drops the action: N2's config/probe check must catch it (Codex core-r3-lensB P1)
	else
		m_cfg.setRef(rule, key, templateName.str(), gxrb::REF_TEMPLATE, tier(), &ev, this);
	afterAction(ev, before);
#else
	if (templateName.getLength() <= 255)
		m_cfg.setRef(rule, key, templateName.str(), gxrb::REF_TEMPLATE, tier(), nullptr, this);
#endif
}

void GXRuleBank::actionTeam(int rule, int key, const AsciiString &teamName)
{
#if defined(GX_TEST_INSTRUMENTS)
	if (gxRuleEnvIs("GX_TEST_RULE_MUTANT", "team_noop"))
		return;		// a BROKEN handler that drops the action: N2's team_action check must catch it (Codex core-r4-lensB P2)
	{
		// the TEAM action reached the bank, with the name as the script engine qualified it for this player
		char kv[300];
		snprintf(kv, sizeof(kv), "k=%d name=%s", key, gxRuleWord(std::string(teamName.str())).c_str());
		gxRuleLine(m_player->getPlayerIndex(), rule, "team_action", kv);
	}
	std::vector<gxrb::Event> ev;
	const std::vector<std::string> before = configSnapshot();
	if (teamName.getLength() > 255)
		ev.push_back(gxrb::mkEvent(rule, "unknown", "too_long", key));
	else
		m_cfg.setRef(rule, key, teamName.str(), gxRuleEnvIs("GX_TEST_RULE_MUTANT", "team_as_template") ? gxrb::REF_TEMPLATE : gxrb::REF_TEAM,
			tier(), &ev, this);	// team_as_template: a BROKEN handler forwarding the wrong slot type (Codex core-r5-lensB P2)
	afterAction(ev, before);
#else
	if (teamName.getLength() <= 255)
		m_cfg.setRef(rule, key, teamName.str(), gxrb::REF_TEAM, tier(), nullptr, this);
#endif
}

// gxrb::LiveHooks: a DISPATCHED live rule is about to be reconfigured or switched off - it closes what it holds first; ConfigCore then
// drops whatever it still has in the two tables (attachTables)
void GXRuleBank::beforeLiveChange(int rule, bool disabling)
{
	Instance *in = findInstance(rule);
	if (in != nullptr)
	{
		GXRuleDispatchScope scope(*in);
		GXRuleCtx ctx(*this, rule);
		if (disabling)
			in->impl->onDisable(ctx);
		else
			in->impl->onReconfigure(ctx);
	}
	// GXRuleCore.h then releases whatever the rule still holds in the two tables
}

// ---- counters + rule events ----
void GXRuleBank::countTriggered(int rule) { Instance *in = findInstance(rule); if (in != nullptr) ++in->triggered; }
void GXRuleBank::countFired(int rule) { Instance *in = findInstance(rule); if (in != nullptr) ++in->fired; }
void GXRuleBank::logEvent(int rule, const char *ev, const char *kv) const
{
#if defined(GX_TEST_INSTRUMENTS)
	gxRuleLine(m_player->getPlayerIndex(), rule, ev, kv != nullptr ? kv : "");
#else
	(void)rule;
	(void)ev;
	(void)kv;
#endif
}

void GXRuleBank::flushStats()
{
	if (m_flushed)
		return;
	m_flushed = TRUE;
#if defined(GX_TEST_INSTRUMENTS)
	for (size_t i = 0; i < m_inst.size(); ++i)
	{
		const Instance &in = *m_inst[i];
		if (in.calls == 0 && in.triggered == 0 && in.fired == 0)
			continue;
		char kv[128];
		snprintf(kv, sizeof(kv), "calls=%llu us=%llu clock=%s", in.calls, in.us, s_gxRuleThreadClock ? "thread" : "wall");
		gxRuleLine(m_player->getPlayerIndex(), in.rule, "cost", kv);
		snprintf(kv, sizeof(kv), "triggered=%u fired=%u", in.triggered, in.fired);
		gxRuleLine(m_player->getPlayerIndex(), in.rule, "activity", kv);
	}
	if (m_senseWanted || m_senseCalls > 0)
	{
		char kv[160];
		snprintf(kv, sizeof(kv), "calls=%llu us=%llu objs=%llu clock=%s", m_senseCalls, m_senseUs, m_senseObjs,
			s_gxRuleThreadClock ? "thread" : "wall");
		gxSenseLine(m_player->getPlayerIndex(), "sense_cost", kv);
	}
	if (m_seenAsks > 0 && m_sense != nullptr)
	{
		char kv[192];
		snprintf(kv, sizeof(kv), "asks=%llu us=%llu builds=%llu objs=%llu clock=%s", m_seenAsks, m_seenUs, m_sense->m_seenBuildsForTest,
			m_sense->m_seenObjsForTest, s_gxRuleThreadClock ? "thread" : "wall");
		gxSenseLine(m_player->getPlayerIndex(), "seen_cost", kv);
	}
#endif
}

void gxRuleLogNoBank(const Player *player, int rule)
{
#if defined(GX_TEST_INSTRUMENTS)
	gxRuleLine(player != nullptr ? player->getPlayerIndex() : -1, rule, "refused", "why=not_skirmish_ai");
#else
	(void)player;
	(void)rule;
#endif
}

void gxRuleBankFlushAll()
{
#if defined(GX_TEST_INSTRUMENTS)
	for (size_t i = 0; i < s_gxRuleBanks.size(); ++i)
		s_gxRuleBanks[i]->flushStats();
#endif
}

// ---- test-instruments self-test sites ----
void GXRuleBank::testSelfHooks()
{
#if defined(GX_TEST_INSTRUMENTS)
	// GX_RULE_PROBE_SLOW=hook: one permission-hook call per frame from HERE (after the scheduler, never nested in a tick), so a rule that
	// is slow only in a hook shows on its timer - the red arm of "one timer around every dispatch"
	static const Bool slowHook = gxRuleEnvIs("GX_RULE_PROBE_SLOW", "hook");
	if (slowHook)
		(void)permitHook(gxrb::HOOK_SELL, "probe");
#endif
}

// ---- save ----
bool GXRuleBank::known(int rule) const
{
	if (rule == gxrb::SENSE_STATE_ID)
		return true;	// the bank's own SENSE block (rb-sense-1); an older binary does not know it and skips it (closed)
	return gxrb::findRule(gxrb::gxRegistry(), rule) != nullptr;	// a planted rule 9999 (GX_RULE_TEST_SAVE_EXTRA) is never known: a load meets it as a later binary's rule
}
unsigned short GXRuleBank::stateVersion(int rule) const
{
#if defined(GX_TEST_INSTRUMENTS)
	if (rule == 9999)
		return 1;
	if (rule == 1 && m_saving && gxRuleEnvIs("GX_RULE_TEST_SAVE_EXTRA", "newer"))
		return 99;		// a later binary's PROBE
#endif
	if (rule == gxrb::SENSE_STATE_ID)
		return (unsigned short)gxrb::SENSE_STATE_VERSION;
	const Instance *in = findInstance(rule);
	if (in != nullptr)
		return in->impl->stateVersion();
	GXRuleImpl *tmp = gxRuleMake(rule);	// side-effect free: a rule's constructor only resets its own state
	const unsigned short v = tmp != nullptr ? tmp->stateVersion() : 0;
	delete tmp;
	return v;
}
bool GXRuleBank::hasOpenOperation(int rule) const
{
#if defined(GX_TEST_INSTRUMENTS)
	if (rule == 9999)
		return FALSE;
#endif
	if (rule == gxrb::SENSE_STATE_ID)
		return FALSE;	// nothing only its code can undo: an older binary may skip it
	const Instance *in = findInstance(rule);
	return in != nullptr && in->impl->hasOpenOperation();
}
std::vector<int> GXRuleBank::rulesWithState() const
{
	std::vector<int> out;
	if (senseHasState())
		out.push_back(gxrb::SENSE_STATE_ID);	// id 0 first: the list is in id order
	for (size_t i = 0; i < m_inst.size(); ++i)
		out.push_back(m_inst[i]->rule);
#if defined(GX_TEST_INSTRUMENTS)
	if (m_saving && (gxRuleEnvIs("GX_RULE_TEST_SAVE_EXTRA", "closed") || gxRuleEnvIs("GX_RULE_TEST_SAVE_EXTRA", "claim")))
		out.push_back(9999);
#endif
	return out;
}
void GXRuleBank::xferState(int rule, unsigned short savedVersion, gxrb::ByteIO &io)
{
#if defined(GX_TEST_INSTRUMENTS)
	if (rule == 9999)
	{
		int n = 3;
		io.i32(&n);
		for (int i = 0; i < n; ++i)
		{
			int v = 1000 + i;
			io.i32(&v);
		}
		return;
	}
#endif
	if (rule == gxrb::SENSE_STATE_ID)
	{
		(void)savedVersion;		// version 1 is the only one; a newer one is skipped by bankXfer
		bool wanted = m_senseWanted != FALSE;
#if defined(GX_TEST_INSTRUMENTS)
		if (!io.saving() && gxRuleEnvIs("GX_TEST_SENSE_MUTANT", "load_drop"))
		{
			// a BROKEN load that skips the SENSE block: rb-sense's save/load red arm must catch it
			std::vector<gxrb::FactionSense> dropped(m_faction.size());
			bool w = false;
			gxrb::senseXfer(io, dropped, &w);
			gxRuleLine(m_player->getPlayerIndex(), 0, "test_load_drop", "sense=dropped");
			return;
		}
#endif
		if (io.saving())
		{
			gxrb::senseXfer(io, m_faction, &wanted);
			return;
		}
		bool once = m_senseBlockLoaded != FALSE;
		gxrb::senseLoadBlock(io, m_faction, &wanted, &once);	// one SENSE block per load (Codex sense-r4 P1)
		m_senseBlockLoaded = TRUE;
		m_senseWanted = wanted ? TRUE : FALSE;	// the format keeps it; the first update's senseStart decides again from the loaded scripts
		m_senseLobbyRead = FALSE;
		senseLobby();	// the LOADED game info (CHUNK_GameStateMap is read before CHUNK_Players)
		// what the block may hold: sightings only on a Random slot (Codex sense-r3 P1), and only templates the pass can record - in the data,
		// a unit or building (GXRuleSense::sightable, Codex sense-r6 P1) - each of its entry's seen BASE side (Codex sense-r4 P1)
		const GXRuleSense &sense = *m_sense;
		auto baseOf = [&sense](const std::string &name)
		{
			const ThingTemplate *t = TheThingFactory != nullptr ? TheThingFactory->findTemplate(AsciiString(name.c_str()), FALSE) : nullptr;
			return t != nullptr ? sense.sightBaseOf(t) : std::string();	// only what the pass can record (Codex sense-r6 P1)
		};
		if (!gxrb::senseStateFitsLobby(m_faction) || !gxrb::senseStateFitsCatalog(m_faction, baseOf))
			io.refuse();
		return;
	}
	Instance *in = instance(rule);
	if (in != nullptr)
		in->impl->xferState(io, savedVersion);
}

void GXRuleBank::xfer(Xfer *xfer)
{
	GXXferIO io(xfer);
	m_saving = io.saving();
	if (!m_saving)
		senseClear();	// a load REPLACES the sensor: a save without a SENSE block asked nothing and saw nothing
#if defined(GX_TEST_INSTRUMENTS)
	std::vector<gxrb::Event> ev;
	// GX_RULE_TEST_SAVE_EXTRA (the in-engine save fixtures): closed = a rule 9999 this binary does not know, closed, plus an unknown key
	// of PROBE - the load must SKIP both; claim = the same rule holding a claimed unit - REFUSED; newer = PROBE at state version 99,
	// holding a claim - REFUSED. Only the save is planted; the load runs the real code path.
	const char *extra = m_saving ? getenv("GX_RULE_TEST_SAVE_EXTRA") : nullptr;
	if (extra != nullptr && !gxRuleEnvIs("GX_RULE_TEST_SAVE_EXTRA", "closed") && !gxRuleEnvIs("GX_RULE_TEST_SAVE_EXTRA", "claim")
		&& !gxRuleEnvIs("GX_RULE_TEST_SAVE_EXTRA", "newer"))
	{
		gxRuleLine(m_player->getPlayerIndex(), 0, "test_save_extra", std::string("why=unknown_value value=") + gxRuleWord(extra) + " planted=nothing");
		extra = nullptr;	// Codex core-r1-lensB P2: an unknown value plants nothing (never a silently different save)
	}
	if (extra != nullptr && m_cfg.enabled(1))
	{
		gxrb::ConfigCore cfg = m_cfg;
		std::vector<gxrb::RuleConfig> all = cfg.all();
		gxrb::UnitClaims claims = m_claims;
		for (size_t i = 0; i < all.size(); ++i)
			if (all[i].rule == 1)
				all[i].params.push_back(std::make_pair(999, 7));
		if (!gxRuleEnvIs("GX_RULE_TEST_SAVE_EXTRA", "newer"))
		{
			gxrb::RuleConfig x;
			x.rule = 9999;
			x.requested = x.accepted = x.dispatched = true;
			all.push_back(x);
		}
		cfg.restore(all);
		if (gxRuleEnvIs("GX_RULE_TEST_SAVE_EXTRA", "claim"))
			claims.claim(9999, 1u, 0xFFFFFFFFu, 0u);
		if (gxRuleEnvIs("GX_RULE_TEST_SAVE_EXTRA", "newer"))
			claims.claim(1, 1u, 0xFFFFFFFFu, 0u);
		{	// what this save carries, so the run can show the plant is real: its configured rules, its claims, its state blocks
			std::string kv = std::string("kind=") + extra + " config=";
			for (size_t i = 0; i < cfg.all().size(); ++i)
				kv += (i ? "," : "") + std::to_string(cfg.all()[i].rule) + (cfg.all()[i].params.empty() ? "" : ":k" + std::to_string(cfg.all()[i].params.back().first));
			kv += " claims=";
			for (size_t i = 0; i < claims.all().size(); ++i)
				kv += (i ? "," : "") + std::to_string(claims.all()[i].rule) + ":" + std::to_string(claims.all()[i].objectId);
			const std::vector<int> st = rulesWithState();
			kv += " state=";
			for (size_t i = 0; i < st.size(); ++i)
				kv += (i ? "," : "") + std::to_string(st[i]) + ":v" + std::to_string(stateVersion(st[i]));
			gxRuleLine(m_player->getPlayerIndex(), 0, "test_save_extra", kv);
		}
		gxrb::bankXfer(io, cfg, claims, m_reserves, *this, &ev);
		m_saving = FALSE;
		return;
	}
	gxrb::bankXfer(io, m_cfg, m_claims, m_reserves, *this, &ev);
	for (size_t i = 0; i < ev.size(); ++i)
	{
		char kv[96];
		if (ev[i].key >= 0)
			snprintf(kv, sizeof(kv), "why=%s k=%d load=1", ev[i].why, ev[i].key);
		else
			snprintf(kv, sizeof(kv), "why=%s load=1", ev[i].why);
		gxRuleLine(m_player->getPlayerIndex(), ev[i].rule, ev[i].ev, kv);
	}
	if (!m_saving && gxRuleEnvIs("GX_RULE_TEST_LOAD_CLEAR", "1"))
	{
		// a BROKEN load that keeps each rule's on/off and its state but drops its explicit params and refs (back to the registry
		// defaults): the save/load judge's restored-configuration check must catch it (Codex core-r5-lensB P1)
		std::vector<gxrb::RuleConfig> all = m_cfg.all();
		for (size_t i = 0; i < all.size(); ++i)
		{
			all[i].params.clear();
			all[i].refs.clear();
		}
		m_cfg.restore(all);
		gxRuleLine(m_player->getPlayerIndex(), 0, "test_load_clear", "params=default refs=none");
	}
#else
	gxrb::bankXfer(io, m_cfg, m_claims, m_reserves, *this, nullptr);
#endif
	if (!m_saving)
	{
		// a rule whose configuration the load dropped (unknown here, or a newer state) keeps no instance
		const std::vector<gxrb::RuleConfig> &all = m_cfg.all();
		for (size_t i = m_inst.size(); i-- > 0;)
		{
			bool keep = false;
			for (size_t k = 0; k < all.size() && !keep; ++k)
				keep = all[k].rule == m_inst[i]->rule;
			if (!keep)
				dropInstance(m_inst[i]->rule);
		}
#if defined(GX_TEST_INSTRUMENTS)
		// what the load restored (rb-sense-1's save/load judge): the sighting half from the block, the lobby half re-read from the
		// LOADED game info - read here only for this line; a release build reads it at the first question, which is the same value
		senseLobby();
		for (size_t i = 0; i < m_faction.size(); ++i)
		{
			const gxrb::FactionSense &f = m_faction[i];
			if (f.known.empty() && f.seenBase.empty() && !f.mixed && !f.tracked)
				continue;
			char kv[200];
			snprintf(kv, sizeof(kv), "enemy=%d known=%s via=%s seen=%s ntmpl=%d mixed=%d tracked=%d wanted=%d load=1", (int)i,
				gxRuleWord(f.known).c_str(), f.known.empty() ? "-" : (f.tracked ? "sight" : "lobby"), gxRuleWord(f.seenBase).c_str(),
				(int)f.tmpl.size(), f.mixed ? 1 : 0, f.tracked ? 1 : 0, m_senseWanted ? 1 : 0);
			gxSenseLine(m_player->getPlayerIndex(), "sense_state", kv);
		}
#endif
	}
	m_saving = FALSE;
}

void GXRuleBank::reset()
{
	senseClear();
	m_cfg.clear();
	m_claims.clear();
	m_reserves.clear();
	for (size_t i = 0; i < m_inst.size(); ++i)
	{
		delete m_inst[i]->impl;
		delete m_inst[i];
	}
	m_inst.clear();
}

// ================================================================================================================================
// tiers
// ================================================================================================================================
unsigned gxRuleTierOf(const Player *p)
{
	if (p == nullptr)
		return gxrb::TIER_NONE;
	AIPlayer *ai = p->getAIPlayer();
	const Bool skirmishComputer = p->getPlayerType() == PLAYER_COMPUTER && ai != nullptr && ai->isSkirmishAI();
	const Bool hard = ai != nullptr && ai->getAIDifficulty() == DIFFICULTY_HARD;
	const AsciiString name = TheNameKeyGenerator->keyToName(p->getPlayerNameKey());
	int applied = 0;
	return gxrb::tierFor(skirmishComputer, hard, gxrb::slotOfPlayerName(name.str()), gxrb::tierOverrideEnv(), &applied);
}

int gxTierTeamCopySource(const gxrb::PlanRow &row)
{
#if defined(GX_TEST_INSTRUMENTS)
	if (row.src != row.at && gxRuleEnvIs("GX_TEST_TIER_MUTANT", "team_at"))
		return row.at;
#endif
	return row.src;
}

void gxRuleLogTeamPick(const Player *p, TeamsInfo *stored, const Dict *stock, const std::string &name)
{
#if defined(GX_TEST_INSTRUMENTS)
	const Dict *used = stored != nullptr ? stored->getDict() : nullptr;
	Bool e1 = FALSE, e2 = FALSE;
	const Int u = used != nullptr ? used->getInt(TheKey_teamMaxInstances, &e1) : -1;
	const Int t = stock != nullptr ? stock->getInt(TheKey_teamMaxInstances, &e2) : -1;
	char kv[256];
	snprintf(kv, sizeof(kv), "name=%s used_max=%d stock_max=%d%s", gxRuleWord(name).c_str(), e1 ? (int)u : -1, e2 ? (int)t : -1,
		gxRuleEnvIs("GX_TEST_TIER_MUTANT", "team_at") ? " mutant=team_at" : "");
	gxRuleLine(p != nullptr ? p->getPlayerIndex() : -1, 0, "tier_team", kv);
#else
	(void)p;
	(void)stored;
	(void)stock;
	(void)name;
#endif
}

void gxRuleLogTier(const Player *p)
{
#if defined(GX_TEST_INSTRUMENTS)
	if (p == nullptr || gxrb::tierOverrideEnv() == nullptr)
		return;
	AIPlayer *ai = p->getAIPlayer();
	const Bool skirmishComputer = p->getPlayerType() == PLAYER_COMPUTER && ai != nullptr && ai->isSkirmishAI();
	const Bool hard = ai != nullptr && ai->getAIDifficulty() == DIFFICULTY_HARD;
	const AsciiString name = TheNameKeyGenerator->keyToName(p->getPlayerNameKey());
	const int slot = gxrb::slotOfPlayerName(name.str());
	int applied = 0;
	const unsigned t = gxrb::tierFor(skirmishComputer, hard, slot, gxrb::tierOverrideEnv(), &applied);
	if (applied == 0)
		return;
	char kv[96];
	snprintf(kv, sizeof(kv), "slot=%d tier=%s%s", slot, t == gxrb::TIER_SENTIENT ? "SENTIENT" : t == gxrb::TIER_BRUTAL ? "BRUTAL" : t == gxrb::TIER_HARD ? "HARD" : "NONE",
		applied < 0 ? " why=malformed" : "");
	gxRuleLine(p->getPlayerIndex(), 0, "tier", kv);
#else
	(void)p;
#endif
}

// ---- the tier filter (GXTierFilter.h has the rules; this applies a plan to the engine's lists) ----
namespace
{
struct GXFilterStats
{
	gxrb::PlanStats s;
	int containers;
	void add(const gxrb::PlanStats &p)
	{
		s.kept += p.kept;
		s.replaced += p.replaced;
		s.dropped += p.dropped;
		s.malformed += p.malformed;
		s.dups += p.dups;
		++containers;
	}
};
/// the entry a plan row keeps: its src (the mark's, for a replacement). Test-instruments builds only, GX_TEST_TIER_MUTANT=keep_at keeps
/// the stock twin at the row's position instead - the broken copy N3's group-contents check must catch
int gxTierKeepSource(const gxrb::PlanRow &row)
{
#if defined(GX_TEST_INSTRUMENTS)
	if (row.src != row.at && gxRuleEnvIs("GX_TEST_TIER_MUTANT", "keep_at"))
		return row.at;
#endif
	return row.src;
}
/// ONE implementation for both lists a player's copy holds at top level - its groups and its top-level scripts (Codex core-r6/r7 P2:
/// no general's list in the shipped scb has a top-level script, so only the group list can exercise this code, and it does: N3M's
/// whole-group replacement and the keep_at red arm). Each kept row's entry, renamed, goes to `add` in plan order with its stock twin
/// (the row's `at`); `keep` marks what stays, the caller frees the rest.
template <class Node, class Add>
void gxKeepInPlanOrder(const std::vector<Node *> &nodes, const std::vector<gxrb::PlanRow> &plan, std::vector<bool> *keep, Add add)
{
	for (size_t r = 0; r < plan.size(); ++r)
	{
		const size_t from = (size_t)gxTierKeepSource(plan[r]);
		Node *x = nodes[from];
		x->setName(AsciiString(plan[r].name.c_str()));
		add(x, plan[r], nodes[(size_t)plan[r].at]);
		(*keep)[from] = true;
	}
}
#if defined(GX_TEST_INSTRUMENTS)
/// a group's content, as one number a check can compare: the sum of its scripts' evaluation delays
int gxGroupDelaySum(ScriptGroup *g)
{
	int sum = 0;
	for (Script *s = g->getScript(); s != nullptr; s = s->getNext())
		sum += s->getDelayEvalSeconds();
	return sum;
}
#endif
/// a group's scripts, in place: the entry at each row's `at` takes the content of its src (Script::updateFrom) and its name; every
/// script no row keeps is unlinked and freed (ScriptGroup::deleteScript)
void gxFilterGroupScripts(ScriptGroup *g, unsigned tier, const std::string &q, GXFilterStats *st)
{
	std::vector<Script *> nodes;
	std::vector<std::string> names;
	for (Script *s = g->getScript(); s != nullptr; s = s->getNext())
	{
		nodes.push_back(s);
		names.push_back(s->getName().str());
	}
	if (!gxrb::anyMarked(names))
		return;
	gxrb::PlanStats ps;
	const std::vector<gxrb::PlanRow> plan = gxrb::tierPlanQualified(names, q, tier, &ps);
	st->add(ps);
	std::vector<bool> keep(nodes.size(), false);
	for (size_t r = 0; r < plan.size(); ++r)
	{
		Script *at = nodes[(size_t)plan[r].at];
		if (plan[r].src != plan[r].at)
			at->updateFrom(nodes[(size_t)plan[r].src]);
		at->setName(AsciiString(plan[r].name.c_str()));
		keep[(size_t)plan[r].at] = true;
	}
	for (size_t i = 0; i < nodes.size(); ++i)
		if (!keep[i])
			g->deleteScript(nodes[i]);
}
}

ScriptList *gxTierFilterScripts(ScriptList *in, const Player *p, const AsciiString &qualifier)
{
	const std::string q(qualifier.str());
	if (in == nullptr)
		return in;
	std::vector<ScriptGroup *> groups;
	std::vector<std::string> gnames;
	std::vector<Script *> scripts;
	std::vector<std::string> snames;
	Bool any = FALSE;
	for (ScriptGroup *g = in->getScriptGroup(); g != nullptr; g = g->getNext())
	{
		groups.push_back(g);
		gnames.push_back(g->getName().str());
		for (Script *s = g->getScript(); s != nullptr && !any; s = s->getNext())
			any = strncmp(s->getName().str(), "GXT.", 4) == 0;
	}
	for (Script *s = in->getScript(); s != nullptr; s = s->getNext())
	{
		scripts.push_back(s);
		snames.push_back(s->getName().str());
	}
	if (!any && !gxrb::anyMarked(gnames) && !gxrb::anyMarked(snames))
		return in;		// a stock list: untouched, the stock path
	unsigned tier = gxRuleTierOf(p);
	GXFilterStats st = { { 0, 0, 0, 0, 0 }, 0 };
#if defined(GX_TEST_INSTRUMENTS)
	// GX_TEST_TIER_MUTANT=keep_for_hard: a BROKEN filter that keeps the marks of the first marked container for a HARD player - N3's
	// red arm (that a filter keeping a marked script for Hard changes the simulation)
	Bool mutant = tier == gxrb::TIER_HARD && gxRuleEnvIs("GX_TEST_TIER_MUTANT", "keep_for_hard");
#endif
	// 1. the group list: rebuilt in plan order in a new shell (ScriptList has no unlink); dropped groups freed, unlinked first so a
	//    group's destructor does not free the groups after it
	gxrb::PlanStats ps;
	unsigned gtier = tier;
#if defined(GX_TEST_INSTRUMENTS)
	if (mutant && gxrb::anyMarked(gnames))
	{
		gtier = gxrb::TIER_SENTIENT | gxrb::TIER_BRUTAL;
		mutant = FALSE;
	}
#endif
	const std::vector<gxrb::PlanRow> gplan = gxrb::tierPlanQualified(gnames, q, gtier, &ps);
	if (gxrb::anyMarked(gnames))
		st.add(ps);
	std::vector<gxrb::PlanRow> splan;
	unsigned stier = tier;
#if defined(GX_TEST_INSTRUMENTS)
	if (mutant && gxrb::anyMarked(snames))
	{
		stier = gxrb::TIER_SENTIENT | gxrb::TIER_BRUTAL;
		mutant = FALSE;
	}
#endif
	splan = gxrb::tierPlanQualified(snames, q, stier, &ps);
	if (gxrb::anyMarked(snames))
		st.add(ps);
	for (size_t i = 0; i < groups.size(); ++i)
		groups[i]->setNextGroup(nullptr);
	for (size_t i = 0; i < scripts.size(); ++i)
		scripts[i]->setNextScript(nullptr);
	in->discard();		// frees the shell only (its heads are cleared first)
	ScriptList *out = newInstance(ScriptList);
	std::vector<bool> gkeep(groups.size(), false);
	int n = 0;
	gxKeepInPlanOrder(groups, gplan, &gkeep, [&](ScriptGroup *g, const gxrb::PlanRow &row, ScriptGroup *twin)
	{
#if defined(GX_TEST_INSTRUMENTS)
		if (row.src != row.at)	// a REPLACED group: what the kept one holds vs its stock twin (Codex core-r6-lensB P2)
		{
			char kv[200];
			snprintf(kv, sizeof(kv), "name=%s used_delay=%d stock_delay=%d%s", gxRuleWord(row.name).c_str(), gxGroupDelaySum(g),
				gxGroupDelaySum(twin), gxRuleEnvIs("GX_TEST_TIER_MUTANT", "keep_at") ? " mutant=keep_at" : "");
			gxRuleLine(p != nullptr ? p->getPlayerIndex() : -1, 0, "tier_group", kv);
		}
#else
		(void)row;
		(void)twin;
#endif
		out->addGroup(g, n++);
	});
	for (size_t i = 0; i < groups.size(); ++i)
		if (!gkeep[i])
			deleteInstance(groups[i]);
	std::vector<bool> skeep(scripts.size(), false);
	n = 0;
	gxKeepInPlanOrder(scripts, splan, &skeep, [&](Script *s, const gxrb::PlanRow &row, Script *twin)
	{
		(void)row;
		(void)twin;
		out->addScript(s, n++);
	});
	for (size_t i = 0; i < scripts.size(); ++i)
		if (!skeep[i])
			deleteInstance(scripts[i]);
	// 2. inside every kept group, its own marks
	for (ScriptGroup *g = out->getScriptGroup(); g != nullptr; g = g->getNext())
	{
		unsigned t = tier;
#if defined(GX_TEST_INSTRUMENTS)
		if (mutant)
		{
			std::vector<std::string> nm;
			for (Script *s = g->getScript(); s != nullptr; s = s->getNext())
				nm.push_back(s->getName().str());
			if (gxrb::anyMarked(nm))
			{
				t = gxrb::TIER_SENTIENT | gxrb::TIER_BRUTAL;
				mutant = FALSE;
			}
		}
#endif
		gxFilterGroupScripts(g, t, q, &st);
	}
#if defined(GX_TEST_INSTRUMENTS)
	char kv[160];
	snprintf(kv, sizeof(kv), "what=scripts tier=%u kept=%d replaced=%d dropped=%d malformed=%d dups=%d parents=%d%s", tier, st.s.kept,
		st.s.replaced, st.s.dropped, st.s.malformed, st.s.dups, st.containers,
		gxRuleEnvIs("GX_TEST_TIER_MUTANT", "keep_for_hard") && tier == gxrb::TIER_HARD ? " mutant=keep_for_hard" : "");
	gxRuleLine(p != nullptr ? p->getPlayerIndex() : -1, 0, "tier_filter", kv);
#endif
	return out;
}

std::vector<gxrb::PlanRow> gxTierPlanTeams(const std::vector<std::string> &names, const Player *p)
{
	if (!gxrb::anyMarked(names))
	{
		std::vector<gxrb::PlanRow> id;
		for (size_t i = 0; i < names.size(); ++i)
		{
			gxrb::PlanRow r = { (int)i, names[i], (int)i };
			id.push_back(r);
		}
		return id;
	}
	gxrb::PlanStats ps;
	const unsigned tier = gxRuleTierOf(p);
	std::vector<gxrb::PlanRow> plan = gxrb::tierPlan(names, tier, &ps);
#if defined(GX_TEST_INSTRUMENTS)
	char kv[160];
	snprintf(kv, sizeof(kv), "what=teams tier=%u kept=%d replaced=%d dropped=%d malformed=%d dups=%d", tier, ps.kept, ps.replaced, ps.dropped,
		ps.malformed, ps.dups);
	gxRuleLine(p != nullptr ? p->getPlayerIndex() : -1, 0, "tier_filter", kv);
#endif
	return plan;
}
