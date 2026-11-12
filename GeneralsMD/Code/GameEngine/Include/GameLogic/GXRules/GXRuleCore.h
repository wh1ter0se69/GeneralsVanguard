/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 GeneralsX contributors
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// AI RULE BANK core (2.2.2, 2.2.4, 2.2.5, 2.2.8; rb-core
// plan D3/D4). Everything the bank decides that does not need the engine, as plain C++17 over a Registry: one AI player's configuration
// (the three scb actions' clamp / unknown / tier / window / conflict rules), the unit-claim and money-reserve tables, the scheduler's
// due-test, gxRuleHash, and the bank's save block over an abstract byte stream. No engine type, no global state, no I/O, no RNG;
// scripts/test/gx-rulebank-test.cpp tests it directly and gx-rulebank-mutants.sh proves the tests bite.
#pragma once

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>
#include "GXRuleSpec.h"

namespace gxrb
{

// ---- events: what the engine prints as [GXRULE] lines in test-instruments builds ----
struct Event
{
	int rule;			///< the rule the event is about (0 = none)
	const char *ev;		///< config | clamp | unknown | refused | conflict
	const char *why;	///< refused: tier | retail_save | active | vision; unknown: rule | key | ref_type | state; conflict: lower_id | active
	int key;			///< the param/ref key, -1 when none
	int asked;			///< clamp: the value asked
	int used;			///< clamp: the value used
	int other;			///< conflict: the other rule
};
inline Event mkEvent(int rule, const char *ev, const char *why = "", int key = -1, int asked = 0, int used = 0, int other = 0)
{
	Event e = { rule, ev, why, key, asked, used, other };
	return e;
}

enum ActResult { ACT_OK = 0, ACT_CLAMPED = 1, ACT_UNKNOWN = 2, ACT_REFUSED = 3 };

/// told before a DISPATCHED live rule's configuration changes, so it can close what it holds first (plan 2.2.2); disabling = it is
/// being switched off (onDisable), else reconfigured (onReconfigure)
struct LiveHooks
{
	virtual ~LiveHooks() {}
	virtual void beforeLiveChange(int rule, bool disabling) = 0;
};

struct RefValue { int key; int type; std::string name; };

/// one rule's configuration in one player
struct RuleConfig
{
	int rule;
	bool requested;		///< the scripts asked for it on (key 0 = 1, last word wins)
	bool accepted;		///< it is on: requested and won every claim it shares (recomputed while windows are open, D3)
	bool dispatched;	///< it has run at least once (tick, maintain or hook): its configuration window is closed
	std::vector<std::pair<int, int> > params;	///< (key, value) explicitly set, sorted by key
	std::vector<RefValue> refs;					///< explicitly set, sorted by key
};

// ---- the unit-claim table (plan 2.2.5): which rule has borrowed which unit, until when. Sorted by object id ----
struct UnitClaim { int rule; unsigned objectId; unsigned untilFrame; };
class UnitClaims
{
public:
	/// false when another rule holds a live claim on the unit; the same rule re-claiming moves its until
	bool claim(int rule, unsigned objectId, unsigned untilFrame, unsigned now)
	{
		size_t i = lower(objectId);
		if (i < m_v.size() && m_v[i].objectId == objectId)
		{
			if (m_v[i].rule != rule && m_v[i].untilFrame >= now)
				return false;
			m_v[i].rule = rule;
			m_v[i].untilFrame = untilFrame;
			return true;
		}
		UnitClaim c = { rule, objectId, untilFrame };
		m_v.insert(m_v.begin() + (long)i, c);
		return true;
	}
	/// the rule holding a live claim on the unit, or 0
	int owner(unsigned objectId, unsigned now) const
	{
		size_t i = lower(objectId);
		return (i < m_v.size() && m_v[i].objectId == objectId && m_v[i].untilFrame >= now) ? m_v[i].rule : 0;
	}
	void release(int rule, unsigned objectId)
	{
		size_t i = lower(objectId);
		if (i < m_v.size() && m_v[i].objectId == objectId && m_v[i].rule == rule)
			m_v.erase(m_v.begin() + (long)i);
	}
	void releaseRule(int rule) { for (size_t i = m_v.size(); i-- > 0;) if (m_v[i].rule == rule) m_v.erase(m_v.begin() + (long)i); }
	void expire(unsigned now) { for (size_t i = m_v.size(); i-- > 0;) if (m_v[i].untilFrame < now) m_v.erase(m_v.begin() + (long)i); }
	int countOf(int rule) const { int n = 0; for (size_t i = 0; i < m_v.size(); ++i) n += (m_v[i].rule == rule) ? 1 : 0; return n; }
	const std::vector<UnitClaim> &all() const { return m_v; }
	void restore(const std::vector<UnitClaim> &v) { m_v = v; }
	void clear() { m_v.clear(); }
private:
	size_t lower(unsigned objectId) const
	{
		size_t i = 0;
		while (i < m_v.size() && m_v[i].objectId < objectId)
			++i;
		return i;
	}
	std::vector<UnitClaim> m_v;
};

// ---- the money-reserve table (plan 2.2.5): one entry per rule, the cash it is holding back, until when. Sorted by rule ----
struct MoneyReserve { int rule; int amount; unsigned untilFrame; };
class MoneyReserves
{
public:
	void reserve(int rule, int amount, unsigned untilFrame)
	{
		size_t i = 0;
		while (i < m_v.size() && m_v[i].rule < rule)
			++i;
		if (amount <= 0)
		{
			if (i < m_v.size() && m_v[i].rule == rule)
				m_v.erase(m_v.begin() + (long)i);
			return;
		}
		if (i < m_v.size() && m_v[i].rule == rule)
		{
			m_v[i].amount = amount;
			m_v[i].untilFrame = untilFrame;
			return;
		}
		MoneyReserve r = { rule, amount, untilFrame };
		m_v.insert(m_v.begin() + (long)i, r);
	}
	/// the cash held back right now (what a stock money check will subtract, once the first rule wires it)
	int total(unsigned now) const { int t = 0; for (size_t i = 0; i < m_v.size(); ++i) if (m_v[i].untilFrame >= now) t += m_v[i].amount; return t; }
	void releaseRule(int rule) { for (size_t i = m_v.size(); i-- > 0;) if (m_v[i].rule == rule) m_v.erase(m_v.begin() + (long)i); }
	void expire(unsigned now) { for (size_t i = m_v.size(); i-- > 0;) if (m_v[i].untilFrame < now) m_v.erase(m_v.begin() + (long)i); }
	int countOf(int rule) const { int n = 0; for (size_t i = 0; i < m_v.size(); ++i) n += (m_v[i].rule == rule) ? 1 : 0; return n; }
	const std::vector<MoneyReserve> &all() const { return m_v; }
	void restore(const std::vector<MoneyReserve> &v) { m_v = v; }
	void clear() { m_v.clear(); }
private:
	std::vector<MoneyReserve> m_v;
};

// ---- the scheduler's test and the tie-break hash ----
/// a rule with this cadence (frames; 0 = never) ticks on this frame for this player: staggered by player index, the existing idiom
inline bool ruleDue(unsigned cadenceFrames, unsigned frame, int playerIndex)
{
	return cadenceFrames > 0u && ((frame + (unsigned)playerIndex) % cadenceFrames) == 0u;
}
/// One AI player's rule configuration. retailSave: a RETAIL_COMPATIBLE_XFER_SAVE build has nowhere to save rule state, so it refuses
/// every enable (plan 2.2.4) - no rule ever runs and nothing can be open at a save.
class ConfigCore
{
public:
	ConfigCore(const Registry &reg, bool retailSave) : m_reg(&reg), m_retail(retailSave), m_claims(nullptr), m_reserves(nullptr) {}
	/// the bank's two tables: a DISPATCHED live rule that is reconfigured or switched off is told first (LiveHooks: it closes its
	/// open operations), then everything it still holds in them is released here (plan 2.2.2)
	void attachTables(UnitClaims *claims, MoneyReserves *reserves) { m_claims = claims; m_reserves = reserves; }

	const Registry &registry() const { return *m_reg; }

	/// AI_PLAYER_GX_RULE(player, rule, key, value): key 0 = enable 0/1, else a param key. playerTier is gxRuleTierOf's answer.
	ActResult setInt(int rule, int key, int value, unsigned playerTier, std::vector<Event> *out, LiveHooks *live = nullptr)
	{
		const RuleRow *spec = findRule(*m_reg, rule);
		const char *why = gate(spec, rule, playerTier);
		if (spec == nullptr)
		{
			push(out, mkEvent(rule, "unknown", "rule", key));
			return ACT_UNKNOWN;
		}
		if (why != nullptr)
		{
			push(out, mkEvent(rule, "refused", why, key));
			return ACT_REFUSED;
		}
		if (key == 0)
		{
			int used = value < 0 ? 0 : (value > 1 ? 1 : value);
			if (used != value)
				push(out, mkEvent(rule, "clamp", "", 0, value, used));
			RuleConfig &c = entry(rule);
			const bool was = c.requested;
			c.requested = (used == 1);
			if (was != c.requested)
				resolve(out, live, rule);
			return used != value ? ACT_CLAMPED : ACT_OK;
		}
		const ParamRow *p = findParam(*m_reg, rule, key);
		if (p == nullptr)
		{
			push(out, mkEvent(rule, "unknown", "key", key));
			return ACT_UNKNOWN;
		}
		const int used = value < p->lo ? p->lo : (value > p->hi ? p->hi : value);
		if (used != value)
			push(out, mkEvent(rule, "clamp", "", key, value, used));
		RuleConfig &c = entry(rule);
		if (c.accepted && c.dispatched)
			liveChange(rule, false, live);
		setParam(c, key, used);
		return used != value ? ACT_CLAMPED : ACT_OK;
	}

	/// AI_PLAYER_GX_RULE_TEMPLATE / _TEAM: type is the slot type the ACTION carries; a mismatch is unknown (why=ref_type)
	ActResult setRef(int rule, int key, const std::string &name, int type, unsigned playerTier, std::vector<Event> *out,
		LiveHooks *live = nullptr)
	{
		const RuleRow *spec = findRule(*m_reg, rule);
		const char *why = gate(spec, rule, playerTier);
		if (spec == nullptr)
		{
			push(out, mkEvent(rule, "unknown", "rule", key));
			return ACT_UNKNOWN;
		}
		if (why != nullptr)
		{
			push(out, mkEvent(rule, "refused", why, key));
			return ACT_REFUSED;
		}
		const RefRow *r = findRef(*m_reg, rule, key);
		if (r == nullptr || r->type != type)
		{
			push(out, mkEvent(rule, "unknown", r == nullptr ? "key" : "ref_type", key));
			return ACT_UNKNOWN;
		}
		RuleConfig &c = entry(rule);
		if (c.accepted && c.dispatched)
			liveChange(rule, false, live);
		setRefValue(c, key, type, name);
		return ACT_OK;
	}

	bool anyEnabled() const
	{
		for (size_t i = 0; i < m_cfg.size(); ++i)
			if (m_cfg[i].accepted)
				return true;
		return false;
	}
	bool enabled(int rule) const { const RuleConfig *c = find(rule); return c != nullptr && c->accepted; }
	bool dispatched(int rule) const { const RuleConfig *c = find(rule); return c != nullptr && c->dispatched; }
	/// the bank calls this before EVERY dispatch of a rule (tick, maintain, hook): it closes the configuration window
	void markDispatched(int rule) { RuleConfig *c = findMut(rule); if (c != nullptr) c->dispatched = true; }
	/// a param's value: the one set, else the registry default (0 for a key the registry does not have)
	int param(int rule, int key) const
	{
		const RuleConfig *c = find(rule);
		if (c != nullptr)
			for (size_t i = 0; i < c->params.size(); ++i)
				if (c->params[i].first == key)
					return c->params[i].second;
		const ParamRow *p = findParam(*m_reg, rule, key);
		return p != nullptr ? p->def : 0;
	}
	const std::string *ref(int rule, int key) const
	{
		const RuleConfig *c = find(rule);
		if (c != nullptr)
			for (size_t i = 0; i < c->refs.size(); ++i)
				if (c->refs[i].key == key)
					return &c->refs[i].name;
		return nullptr;
	}
	/// the enabled rules, in id order (the scheduler's order)
	std::vector<int> enabledInIdOrder() const
	{
		std::vector<int> out;
		for (size_t i = 0; i < m_cfg.size(); ++i)
			if (m_cfg[i].accepted)
				out.push_back(m_cfg[i].rule);
		return out;
	}
	/// the enabled rule that claims HOOK:context, or 0 (exclusive, so at most one)
	int claimOwner(int hook, const char *context) const
	{
		if (hook < 0 || hook >= HOOK_COUNT || context == nullptr)
			return 0;
		std::string claim = std::string(kHookNames[hook]) + ":" + context;
		for (size_t i = 0; i < m_cfg.size(); ++i)
			if (m_cfg[i].accepted && ruleClaims(*m_reg, m_cfg[i].rule, claim.c_str()))
				return m_cfg[i].rule;
		return 0;
	}
	// ---- dispatch selection: every dispatch path goes through one of these, and each closes the window of what it returns ----
	struct Dispatch { int rule; bool maintain; };	///< maintain: call maintain(), else tick()
	/// this frame's dispatches in id order (a rule's maintain() before its tick()), each rule marked dispatched as it is chosen.
	/// hasMaintain(rule) -> bool is the rule's own answer; framesPerSecond scales the registry cadence (seconds).
	template <class HasMaintain>
	std::vector<Dispatch> takeFrame(unsigned frame, int playerIndex, unsigned framesPerSecond, HasMaintain hasMaintain)
	{
		std::vector<Dispatch> out;
		for (size_t i = 0; i < m_cfg.size(); ++i)
		{
			if (!m_cfg[i].accepted)
				continue;
			const int rule = m_cfg[i].rule;
			const RuleRow *spec = findRule(*m_reg, rule);
			if (spec == nullptr)
				continue;
			if (hasMaintain(rule))
			{
				m_cfg[i].dispatched = true;
				Dispatch d = { rule, true };
				out.push_back(d);
			}
			if (ruleDue((unsigned)spec->cadenceS * framesPerSecond, frame, playerIndex))
			{
				m_cfg[i].dispatched = true;
				Dispatch d = { rule, false };
				out.push_back(d);
			}
		}
		return out;
	}
	/// a VALUE hook: the one enabled rule that claims HOOK:context (exclusive), marked dispatched; 0 = none (the stock value stands)
	int takeValueHook(int hook, const char *context)
	{
		const int owner = claimOwner(hook, context);
		if (owner != 0)
			markDispatched(owner);
		return owner;
	}
	/// a PERMISSION hook: the enabled rules that join it (joins(rule, hook) -> bool), in id order, each marked dispatched
	template <class Joins>
	std::vector<int> takePermitHook(int hook, Joins joins)
	{
		std::vector<int> out;
		for (size_t i = 0; i < m_cfg.size(); ++i)
			if (m_cfg[i].accepted && joins(m_cfg[i].rule, hook))
			{
				m_cfg[i].dispatched = true;
				out.push_back(m_cfg[i].rule);
			}
		return out;
	}
	/// the AND of every joined rule's answer: EVERY rule is asked (no short circuit), so which rules a check dispatches never depends
	/// on another rule's answer
	template <class Ask>
	static bool permitAll(const std::vector<int> &rules, Ask ask)
	{
		bool ok = true;
		for (size_t i = 0; i < rules.size(); ++i)
		{
			const bool a = ask(rules[i]);
			ok = ok && a;
		}
		return ok;
	}

	const std::vector<RuleConfig> &all() const { return m_cfg; }
	/// the save block's loader: the configuration it read, verbatim (accepted sets are restored, never recomputed)
	void restore(const std::vector<RuleConfig> &cfg) { m_cfg = cfg; }
	void clear() { m_cfg.clear(); }

private:
	/// nullptr = may configure; else the refusal (the rule exists)
	const char *gate(const RuleRow *spec, int rule, unsigned playerTier) const
	{
		if (spec == nullptr)
			return nullptr;
		if (m_retail)
			return "retail_save";
		if ((spec->tiers & playerTier) == 0u)
			return spec->vision ? "vision" : "tier";
		const RuleConfig *c = find(rule);
		if (c != nullptr && c->dispatched && !spec->live)
			return "active";
		return nullptr;
	}

	/// D3: the rules that are on = a function of the REQUESTED set, never of arrival order. Fixed first: on, dispatched and not live
	/// (they cannot be switched off now). Then every other requested rule in id order, accepted unless it shares a claim with one
	/// accepted before it. Events for every rule whose state changed, and for `asked` when it stays off.
	void resolve(std::vector<Event> *out, LiveHooks *live, int asked)
	{
		std::vector<int> acc;
		for (size_t i = 0; i < m_cfg.size(); ++i)
		{
			const RuleRow *s = findRule(*m_reg, m_cfg[i].rule);
			if (m_cfg[i].accepted && m_cfg[i].dispatched && s != nullptr && !s->live)
				acc.push_back(m_cfg[i].rule);
		}
		std::vector<std::pair<int, int> > lost;		// (rule, the accepted rule it lost to)
		for (size_t i = 0; i < m_cfg.size(); ++i)	// m_cfg is sorted by rule id
		{
			const RuleConfig &c = m_cfg[i];
			if (!c.requested || contains(acc, c.rule))
				continue;
			int against = 0;
			for (size_t j = 0; j < acc.size() && against == 0; ++j)
				if (shareClaim(*m_reg, c.rule, acc[j]))
					against = acc[j];
			if (against == 0)
				acc.push_back(c.rule);
			else
				lost.push_back(std::make_pair(c.rule, against));
		}
		for (size_t i = 0; i < m_cfg.size(); ++i)
		{
			RuleConfig &c = m_cfg[i];
			const bool now = contains(acc, c.rule);
			if (c.accepted && !now && c.dispatched)
				liveChange(c.rule, true, live);
			const bool changed = c.accepted != now;
			c.accepted = now;
			if (!now && c.requested && (changed || c.rule == asked))
			{
				int against = 0;
				for (size_t j = 0; j < lost.size(); ++j)
					if (lost[j].first == c.rule)
						against = lost[j].second;
				const RuleConfig *w = find(against);
				push(out, mkEvent(c.rule, "conflict", (w != nullptr && w->dispatched) ? "active" : "lower_id", -1, 0, 0, against));
			}
		}
	}
	void liveChange(int rule, bool disabling, LiveHooks *live)
	{
		if (live != nullptr)
			live->beforeLiveChange(rule, disabling);	// the rule closes its own operations first ...
		if (m_claims != nullptr)
			m_claims->releaseRule(rule);				// ... then nothing of it stays in the bank's tables
		if (m_reserves != nullptr)
			m_reserves->releaseRule(rule);
	}
	static bool contains(const std::vector<int> &v, int x)
	{
		for (size_t i = 0; i < v.size(); ++i)
			if (v[i] == x)
				return true;
		return false;
	}
	static void push(std::vector<Event> *out, const Event &e) { if (out != nullptr) out->push_back(e); }
	static void setParam(RuleConfig &c, int key, int value)
	{
		size_t i = 0;
		while (i < c.params.size() && c.params[i].first < key)
			++i;
		if (i < c.params.size() && c.params[i].first == key)
			c.params[i].second = value;
		else
			c.params.insert(c.params.begin() + (long)i, std::make_pair(key, value));
	}
	static void setRefValue(RuleConfig &c, int key, int type, const std::string &name)
	{
		size_t i = 0;
		while (i < c.refs.size() && c.refs[i].key < key)
			++i;
		if (i < c.refs.size() && c.refs[i].key == key)
			c.refs[i].name = name;
		else
		{
			RefValue v = { key, type, name };
			c.refs.insert(c.refs.begin() + (long)i, v);
		}
	}
	RuleConfig &entry(int rule)
	{
		size_t i = 0;
		while (i < m_cfg.size() && m_cfg[i].rule < rule)
			++i;
		if (i == m_cfg.size() || m_cfg[i].rule != rule)
		{
			RuleConfig c;
			c.rule = rule;
			c.requested = c.accepted = c.dispatched = false;
			m_cfg.insert(m_cfg.begin() + (long)i, c);
		}
		return m_cfg[i];
	}
	const RuleConfig *find(int rule) const
	{
		for (size_t i = 0; i < m_cfg.size(); ++i)
			if (m_cfg[i].rule == rule)
				return &m_cfg[i];
		return nullptr;
	}
	RuleConfig *findMut(int rule)
	{
		for (size_t i = 0; i < m_cfg.size(); ++i)
			if (m_cfg[i].rule == rule)
				return &m_cfg[i];
		return nullptr;
	}

	const Registry *m_reg;
	bool m_retail;
	std::vector<RuleConfig> m_cfg;	///< sorted by rule id; only rules that got an accepted action
	UnitClaims *m_claims;
	MoneyReserves *m_reserves;
};

// ---- the tie-break hash ----
/// a rule's only source of "randomness" (plan 2.2.8): a pure function of its arguments, no shared state, never the logic RNG
inline unsigned long long ruleHashMix(unsigned long long x)
{
	x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
	x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
	return x ^ (x >> 31);
}
inline unsigned ruleHash(unsigned frame, int playerIndex, int ruleId, unsigned salt)
{
	unsigned long long x = ruleHashMix((unsigned long long)frame + 0x9E3779B97F4A7C15ull);
	x = ruleHashMix(x ^ (unsigned long long)(unsigned)playerIndex);
	x = ruleHashMix(x ^ (unsigned long long)(unsigned)ruleId);
	x = ruleHashMix(x ^ (unsigned long long)salt);
	return (unsigned)(x ^ (x >> 32));
}

// ---- the save block (plan 2.2.4, rb-core plan D4) ----
/// the byte stream the block is written to / read from: the engine adapts Xfer, the unit test a memory buffer. On load, refuse() must
/// not return (the engine throws XFER_INVALID_VERSION: GameState::loadGame then refuses the whole save).
class ByteIO
{
public:
	virtual ~ByteIO() {}
	virtual bool saving() const = 0;
	virtual void u8(unsigned char *v) = 0;
	virtual void u16(unsigned short *v) = 0;
	virtual void i32(int *v) = 0;
	virtual void u32(unsigned *v) = 0;
	virtual void str(std::string *v) = 0;	///< one length byte, then that many bytes (Xfer::xferAsciiString's layout)
	virtual void bytes(void *p, int n) = 0;	///< n raw bytes (Xfer::xferUser)
	virtual int beginBlock() = 0;		///< save: open a size-prefixed block (returns 0); load: read the block's size
	virtual void endBlock() = 0;
	virtual void skip(int bytes) = 0;	///< load only
	virtual void refuse() = 0;			///< load only; never returns
};

/// what the bank supplies for the per-rule state blocks
class StateHooks
{
public:
	virtual ~StateHooks() {}
	virtual bool known(int rule) const = 0;						///< this binary has the rule's code
	virtual unsigned short stateVersion(int rule) const = 0;	///< the binary's state version of a known rule
	virtual bool hasOpenOperation(int rule) const = 0;			///< something only its own code can undo (borrowed unit, temp team, try)
	virtual std::vector<int> rulesWithState() const = 0;		///< id order; the rules whose instances exist
	virtual void xferState(int rule, unsigned short savedVersion, ByteIO &io) = 0;	///< save: the binary's version; load: the saved one
};

/// One rule's state block on LOAD, held to the block's saved size (XferLoad::endBlock checks nothing, so a rule
/// that read more than it saved would eat the next rule's bytes). A read that would pass the budget refuses the load at once; a rule
/// that read LESS (an older layout it knows how to read) has the rest of its block skipped.
class BudgetIO : public ByteIO
{
public:
	BudgetIO(ByteIO &io, int budget) : m_io(io), m_left(budget) {}
	virtual bool saving() const override { return false; }
	virtual void u8(unsigned char *v) override { take(1); m_io.u8(v); }
	virtual void u16(unsigned short *v) override { take(2); m_io.u16(v); }
	virtual void i32(int *v) override { take(4); m_io.i32(v); }
	virtual void u32(unsigned *v) override { take(4); m_io.u32(v); }
	/// the length byte FIRST, checked against the budget, then exactly that many bytes (the stream's own string read
	/// would consume the whole announced payload before any check could refuse it; embedded NULs are kept as bytes)
	virtual void str(std::string *v) override
	{
		unsigned char n = 0;
		u8(&n);
		take((int)n);
		std::string out((size_t)n, '\0');
		if (n > 0)
			m_io.bytes(&out[0], (int)n);
		*v = out;
	}
	virtual void bytes(void *p, int n) override { take(n); m_io.bytes(p, n); }
	virtual int beginBlock() override { m_io.refuse(); return 0; }	// a rule's own state has no nested blocks
	virtual void endBlock() override { m_io.refuse(); }
	virtual void skip(int bytes) override { take(bytes); m_io.skip(bytes); }
	virtual void refuse() override { m_io.refuse(); }
	int left() const { return m_left; }
private:
	void take(int n)
	{
		if (n < 0 || n > m_left)
			m_io.refuse();
		m_left -= n;
	}
	ByteIO &m_io;
	int m_left;
};

enum { BANK_FORMAT = 1 };
enum { CFG_REQUESTED = 1, CFG_ACCEPTED = 2, CFG_DISPATCHED = 4 };

/// the rule's "open" byte: it holds a claim, a reserve, or an operation only its code can finish
inline bool ruleOpen(int rule, const UnitClaims &claims, const MoneyReserves &res, const StateHooks &st)
{
	return claims.countOf(rule) > 0 || res.countOf(rule) > 0 || st.hasOpenOperation(rule);
}

/// The whole bank block. Load: a rule this binary does not know, or whose saved state version is newer than the binary's, is skipped
/// when its open byte is 0 (its config dropped, ev=unknown why=state) and REFUSES the load when it is 1; a table entry of an unknown
/// rule refuses at once; unknown keys and refs of a known rule are ignored (ev=unknown); an unknown bank format refuses.
inline void bankXfer(ByteIO &io, ConfigCore &cfg, UnitClaims &claims, MoneyReserves &res, StateHooks &st, std::vector<Event> *out)
{
	const Registry &g = cfg.registry();
	unsigned char format = BANK_FORMAT;
	io.u8(&format);
	if (!io.saving() && format != BANK_FORMAT)
		io.refuse();
	if (io.saving())
	{
		const std::vector<RuleConfig> &all = cfg.all();
		unsigned short n = (unsigned short)all.size();
		io.u16(&n);
		for (size_t i = 0; i < all.size(); ++i)
		{
			const RuleConfig &c = all[i];
			unsigned short rule = (unsigned short)c.rule;
			unsigned char flags = (unsigned char)((c.requested ? CFG_REQUESTED : 0) | (c.accepted ? CFG_ACCEPTED : 0) | (c.dispatched ? CFG_DISPATCHED : 0));
			io.u16(&rule);
			io.u8(&flags);
			unsigned short np = (unsigned short)c.params.size();
			io.u16(&np);
			for (size_t k = 0; k < c.params.size(); ++k)
			{
				unsigned short key = (unsigned short)c.params[k].first;
				int v = c.params[k].second;
				io.u16(&key);
				io.i32(&v);
			}
			unsigned short nr = (unsigned short)c.refs.size();
			io.u16(&nr);
			for (size_t k = 0; k < c.refs.size(); ++k)
			{
				unsigned short key = (unsigned short)c.refs[k].key;
				unsigned char type = (unsigned char)c.refs[k].type;
				std::string name = c.refs[k].name;
				io.u16(&key);
				io.u8(&type);
				io.str(&name);
			}
		}
		unsigned short nc = (unsigned short)claims.all().size();
		io.u16(&nc);
		for (size_t i = 0; i < claims.all().size(); ++i)
		{
			UnitClaim c = claims.all()[i];
			unsigned short rule = (unsigned short)c.rule;
			io.u16(&rule);
			io.u32(&c.objectId);
			io.u32(&c.untilFrame);
		}
		unsigned short nm = (unsigned short)res.all().size();
		io.u16(&nm);
		for (size_t i = 0; i < res.all().size(); ++i)
		{
			MoneyReserve r = res.all()[i];
			unsigned short rule = (unsigned short)r.rule;
			io.u16(&rule);
			io.i32(&r.amount);
			io.u32(&r.untilFrame);
		}
		std::vector<int> rs = st.rulesWithState();
		unsigned short ns = (unsigned short)rs.size();
		io.u16(&ns);
		for (size_t i = 0; i < rs.size(); ++i)
		{
			unsigned short rule = (unsigned short)rs[i];
			unsigned short ver = st.stateVersion(rs[i]);
			unsigned char open = ruleOpen(rs[i], claims, res, st) ? 1 : 0;
			io.u16(&rule);
			io.u16(&ver);
			io.u8(&open);
			io.beginBlock();
			st.xferState(rs[i], ver, io);
			io.endBlock();
		}
		return;
	}

	// ---- load ----
	std::vector<RuleConfig> loaded;
	std::vector<int> dropped;
	unsigned short n = 0;
	io.u16(&n);
	for (unsigned short i = 0; i < n; ++i)
	{
		RuleConfig c;
		unsigned short rule = 0;
		unsigned char flags = 0;
		io.u16(&rule);
		io.u8(&flags);
		c.rule = rule;
		c.requested = (flags & CFG_REQUESTED) != 0;
		c.accepted = (flags & CFG_ACCEPTED) != 0;
		c.dispatched = (flags & CFG_DISPATCHED) != 0;
		const bool knownRule = findRule(g, rule) != nullptr;
		if (!knownRule)
		{
			dropped.push_back(rule);
			if (out != nullptr) out->push_back(mkEvent(rule, "unknown", "rule"));
		}
		unsigned short np = 0;
		io.u16(&np);
		for (unsigned short k = 0; k < np; ++k)
		{
			unsigned short key = 0;
			int v = 0;
			io.u16(&key);
			io.i32(&v);
			const ParamRow *p = knownRule ? findParam(g, rule, key) : nullptr;
			if (p == nullptr)
			{
				if (knownRule && out != nullptr) out->push_back(mkEvent(rule, "unknown", "key", key));
				continue;
			}
			c.params.push_back(std::make_pair((int)key, v < p->lo ? p->lo : (v > p->hi ? p->hi : v)));
		}
		unsigned short nr = 0;
		io.u16(&nr);
		for (unsigned short k = 0; k < nr; ++k)
		{
			unsigned short key = 0;
			unsigned char type = 0;
			std::string name;
			io.u16(&key);
			io.u8(&type);
			io.str(&name);
			const RefRow *r = knownRule ? findRef(g, rule, key) : nullptr;
			if (r == nullptr || r->type != type)
			{
				if (knownRule && out != nullptr) out->push_back(mkEvent(rule, "unknown", r == nullptr ? "key" : "ref_type", key));
				continue;
			}
			RefValue rv = { (int)key, (int)type, name };
			c.refs.push_back(rv);
		}
		if (knownRule)
			loaded.push_back(c);
	}
	std::vector<UnitClaim> lc;
	unsigned short nc = 0;
	io.u16(&nc);
	for (unsigned short i = 0; i < nc; ++i)
	{
		unsigned short rule = 0;
		UnitClaim c = { 0, 0u, 0u };
		io.u16(&rule);
		io.u32(&c.objectId);
		io.u32(&c.untilFrame);
		c.rule = rule;
		if (!st.known(rule))
			io.refuse();	// a unit borrowed by code this binary does not have: only that code can give it back
		lc.push_back(c);
	}
	std::vector<MoneyReserve> lm;
	unsigned short nm = 0;
	io.u16(&nm);
	for (unsigned short i = 0; i < nm; ++i)
	{
		unsigned short rule = 0;
		MoneyReserve r = { 0, 0, 0u };
		io.u16(&rule);
		io.i32(&r.amount);
		io.u32(&r.untilFrame);
		r.rule = rule;
		if (!st.known(rule))
			io.refuse();
		lm.push_back(r);
	}
	unsigned short ns = 0;
	io.u16(&ns);
	for (unsigned short i = 0; i < ns; ++i)
	{
		unsigned short rule = 0, ver = 0;
		unsigned char open = 0;
		io.u16(&rule);
		io.u16(&ver);
		io.u8(&open);
		if (!st.known(rule) || ver > st.stateVersion(rule))
		{
			if (open != 0)
				io.refuse();	// it holds something only its own (newer) code can finish
			const int size = io.beginBlock();
			if (size < 0)
				io.refuse();
			io.skip(size);
			io.endBlock();
			dropped.push_back(rule);
			if (out != nullptr) out->push_back(mkEvent(rule, "unknown", "state"));
			continue;
		}
		const int size = io.beginBlock();
		if (size < 0)
			io.refuse();
		BudgetIO block(io, size);
		st.xferState(rule, ver, block);
		if (block.left() > 0)
			io.skip(block.left());
		io.endBlock();
	}
	std::vector<RuleConfig> kept;
	for (size_t i = 0; i < loaded.size(); ++i)
	{
		bool drop = false;
		for (size_t j = 0; j < dropped.size(); ++j)
			drop = drop || dropped[j] == loaded[i].rule;
		if (!drop)
			kept.push_back(loaded[i]);
	}
	cfg.restore(kept);
	claims.restore(lc);
	res.restore(lm);
}

// ---- tiers (plan 2.8) ----
/// The tier of a player, from what is known at game start on a fresh game, a replay and a save load alike: HARD for a skirmish
/// computer player at Hard difficulty, else NONE (Sentient and Brutal arrive with the level table). tiOverride is
/// GX_RULE_TIER_TEST's value ("<HARD|SENTIENT|BRUTAL>:<slot>", one slot) and gives that slot's skirmish computer player the tier.
/// *applied = 1 when the override set this player's tier, -1 when it was malformed.
inline unsigned tierFor(bool skirmishComputer, bool hardDifficulty, int slot, const char *tiOverride, int *applied)
{
	if (applied != nullptr)
		*applied = 0;
	unsigned tier = (skirmishComputer && hardDifficulty) ? TIER_HARD : TIER_NONE;
	if (tiOverride == nullptr || *tiOverride == 0 || !skirmishComputer)
		return tier;
	std::string s(tiOverride);
	const size_t colon = s.find(':');
	unsigned t = TIER_NONE;
	const std::string name = colon == std::string::npos ? s : s.substr(0, colon);
	if (name == "HARD") t = TIER_HARD;
	else if (name == "SENTIENT") t = TIER_SENTIENT;
	else if (name == "BRUTAL") t = TIER_BRUTAL;
	int want = -1;
	if (colon != std::string::npos && colon + 1 < s.size() && s.size() - colon - 1 <= 2)
	{
		want = 0;
		for (size_t i = colon + 1; i < s.size(); ++i)
			want = (s[i] >= '0' && s[i] <= '9') ? want * 10 + (s[i] - '0') : -1000;
	}
	if (t == TIER_NONE || want < 0)
	{
		if (applied != nullptr)
			*applied = -1;
		return tier;
	}
	if (want != slot)
		return tier;
	if (applied != nullptr)
		*applied = 1;
	return t;
}

/// GX_RULE_TIER_TEST's value - in a test-instruments build only; a release build never reads it (the env is not in any digest and
/// differs per peer, so it must never reach a shipped simulation)
inline const char *tierOverrideEnv()
{
#if defined(GX_TEST_INSTRUMENTS)
	return std::getenv("GX_RULE_TIER_TEST");
#else
	return nullptr;
#endif
}

/// "player<N>" (the lobby slot's player name, GameLogic.cpp startNewGame) -> N, else -1
inline int slotOfPlayerName(const char *name)
{
	if (name == nullptr)
		return -1;
	const char *p = "player";
	int i = 0;
	while (p[i] != 0 && name[i] == p[i])
		++i;
	if (p[i] != 0 || name[i] == 0)
		return -1;
	int n = 0;
	for (; name[i] != 0; ++i)
	{
		if (name[i] < '0' || name[i] > '9' || n > 1000)
			return -1;
		n = n * 10 + (name[i] - '0');
	}
	return n;
}

// ---- sense identity (plan 2.2.6): who a human viewer would see as an object's owner ----
enum { REL_ENEMIES = 0, REL_NEUTRAL = 1, REL_ALLIES = 2 };	///< the engine's Relationship values (GameCommon.h)
/// a disguised object shows its disguise's owner to everyone its real owner is not ALLIED with - InGameUI.cpp's rule
inline int seenOwner(int realOwner, bool disguised, int disguiseOwner, int realOwnerToViewer)
{
	return (disguised && disguiseOwner >= 0 && realOwnerToViewer != REL_ALLIES) ? disguiseOwner : realOwner;
}
/// a SEEN object adds its cost to enemyValueNear only when its (seen) template can carry a weapon and is neither a dozer nor a harvester
/// (each exclusion tested on its own here - no harvester in the data is armed, so a fixture cannot)
inline bool countsAsThreat(bool armed, bool dozer, bool harvester)
{
	return armed && !dozer && !harvester;
}
/// SENSE.FOG_CONDITIONS: a script COMPARISON of what the AI sees through its own units (a count or a value) against the
/// script number - Parameter numbering, LESS_THAN = 0, LESS_EQUAL, EQUAL, GREATER_EQUAL, GREATER, NOT_EQUAL = 5; any other operator FALSE
inline bool compareSeen(int op, long got, long want)
{
	switch (op)
	{
	case 0: return got < want;
	case 1: return got <= want;
	case 2: return got == want;
	case 3: return got >= want;
	case 4: return got > want;
	case 5: return got != want;
	default: return false;
	}
}
inline bool seenAsDisguise(bool disguised, int disguiseOwner, int realOwnerToViewer)
{
	return disguised && disguiseOwner >= 0 && realOwnerToViewer != REL_ALLIES;
}

// ---- SENSE.ENEMY_FACTION ----
// What one viewer fairly knows of another player's faction. A slot the lobby shows (its ORIGINAL template is not Random) is known at once,
// general included. A Random slot is unknown until the viewer has fairly SEEN SENSE_SIGHT_TEMPLATES different templates of one base
// faction owned (as seen) by that player and none of another: a sighting reads the template, never the owner's real side, and gives the
// faction, never the general. A captured or hijacked foreign unit keeps its template under its new owner, so a mixed picture stays
// unknown for good. Once known it stays known.
enum { SENSE_STATE_ID = 0, SENSE_STATE_VERSION = 1, SENSE_SIGHT_TEMPLATES = 3 };	///< id 0: the bank's own block in the per-rule state list (rule ids start at 1)
struct FactionSense
{
	bool tracked;					///< a Random lobby slot: the sighting pass looks for it (from the lobby, never saved)
	std::string known;				///< the BASE side once known ("" = unknown)
	std::string general;			///< the lobby's own side (e.g. ChinaTankGeneral); a sighting never sets it
	std::string seenBase;			///< the one base side sighted so far ("" = none)
	bool mixed;						///< a second base side was sighted: never known by sight
	std::vector<std::string> tmpl;	///< distinct sighted templates of seenBase, at most SENSE_SIGHT_TEMPLATES
	FactionSense() : tracked(false), mixed(false) {}
};
/// one object the viewer fairly sees, owned (as seen) by the tracked player: its template's base side and its template name
struct Sighting
{
	std::string base;
	std::string tmpl;
};
/// a slot the lobby shows: known now, general included
inline void senseFromLobby(FactionSense &s, const std::string &baseSide, const std::string &side)
{
	s.tracked = false;
	s.known = baseSide.empty() ? side : baseSide;
	s.general = side;
}
/// a Random slot: unknown until fairly seen
inline void senseTrack(FactionSense &s)
{
	s.tracked = true;
}
/// the pass still has something to find for this player
inline bool senseOpen(const FactionSense &s)
{
	return s.tracked && s.known.empty() && !s.mixed;
}
/// whether a bank runs the sighting pass at all: decided ONCE per game from the scripts (scriptsAsk = some loaded script list holds an
/// AI_PLAYER_GX_SENSE condition - the same on every peer and in a replay), never by a question being evaluated: a client-local condition
/// before it in an AND would make only some peers look. Never in a RETAIL_COMPATIBLE_XFER_SAVE build: its saves carry
/// no bank block, so a faction learned by sight would be lost by a save/load - there a Random slot stays unknown.
inline bool senseShouldSample(bool scriptsAsk, bool retailSave)
{
	return scriptsAsk && !retailSave;
}
/// a LOADED sighting state stands only on a player the lobby showed as Random (tracked): anything else is a crafted save (:
/// "known by sight" on the neutral player or a lobby-known slot would answer TRUE) and refuses the load
inline bool senseStateFitsLobby(const std::vector<FactionSense> &all)
{
	for (size_t i = 0; i < all.size(); ++i)
		if (!all[i].tracked && (!all[i].seenBase.empty() || all[i].mixed || !all[i].tmpl.empty()))
			return false;
	return true;
}
/// a LOADED sighting state names only what the pass can have seen: every template it holds exists and is of its seen
/// side (baseOf(name) = the template's BASE side, "" for none or no such template - so the seen side is a base side, never a general's;
/// senseXfer already refuses a seen side with no template)
template <class BaseOf>
inline bool senseStateFitsCatalog(const std::vector<FactionSense> &all, BaseOf baseOf)
{
	for (size_t i = 0; i < all.size(); ++i)
		for (size_t k = 0; k < all[i].tmpl.size(); ++k)
			if (baseOf(all[i].tmpl[k]) != all[i].seenBase)
				return false;
	return true;
}
/// one sighting pass of one tracked player (every object the viewer saw this pass, any order). Decided at the END of the pass, so the
/// order inside a pass never matters. TRUE when this pass made the faction known.
inline bool senseApplyPass(FactionSense &s, const std::vector<Sighting> &pass)
{
	if (!s.tracked || !s.known.empty() || s.mixed)
		return false;
	for (size_t i = 0; i < pass.size(); ++i)
	{
		const Sighting &g = pass[i];
		if (g.base.empty() || g.tmpl.empty())
			continue;
		if (s.seenBase.empty())
			s.seenBase = g.base;
		if (g.base != s.seenBase)
		{
			s.mixed = true;
			continue;
		}
		bool have = false;
		for (size_t k = 0; k < s.tmpl.size(); ++k)
			have = have || s.tmpl[k] == g.tmpl;
		if (!have && s.tmpl.size() < (size_t)SENSE_SIGHT_TEMPLATES)
			s.tmpl.push_back(g.tmpl);
	}
	if (!s.mixed && s.tmpl.size() >= (size_t)SENSE_SIGHT_TEMPLATES)
	{
		s.known = s.seenBase;
		return true;
	}
	return false;
}
/// the condition's answer: known, and the faction asked is the known base side or (lobby only) the general's own side
inline bool senseAnswer(const FactionSense &s, const char *faction)
{
	if (faction == nullptr || *faction == 0 || s.known.empty())
		return false;
	return s.known == faction || (!s.general.empty() && s.general == faction);
}
/// the SENSE state block (inside the bank block, id SENSE_STATE_ID): u8 wanted, u8 n, then per player with a sighting state: u8 index,
/// str known, str seenBase, u8 mixed, u8 ntmpl, ntmpl x str. The lobby half is never saved: it is re-read from the loaded game info. A
/// load REPLACES every sighting state; an index past the table, a duplicate, a flag that is not 0/1, too many templates or a known faction
/// that does not follow from its sightings refuses the load.
inline void senseXfer(ByteIO &io, std::vector<FactionSense> &all, bool *wanted)
{
	unsigned char w = *wanted ? 1 : 0;
	io.u8(&w);
	if (io.saving())
	{
		unsigned char n = 0;
		for (size_t i = 0; i < all.size() && i < 256u; ++i)
			n += (!all[i].seenBase.empty() || all[i].mixed || !all[i].tmpl.empty()) ? 1 : 0;
		io.u8(&n);
		for (size_t i = 0; i < all.size() && i < 256u; ++i)
		{
			FactionSense &s = all[i];
			if (s.seenBase.empty() && !s.mixed && s.tmpl.empty())
				continue;
			unsigned char idx = (unsigned char)i, mixed = s.mixed ? 1 : 0, nt = (unsigned char)s.tmpl.size();
			io.u8(&idx);
			io.str(&s.known);
			io.str(&s.seenBase);
			io.u8(&mixed);
			io.u8(&nt);
			for (size_t k = 0; k < s.tmpl.size(); ++k)
				io.str(&s.tmpl[k]);
		}
		return;
	}
	if (w > 1)
		io.refuse();
	*wanted = w != 0;
	for (size_t i = 0; i < all.size(); ++i)
	{
		all[i].known.clear();
		all[i].general.clear();
		all[i].seenBase.clear();
		all[i].mixed = false;
		all[i].tmpl.clear();
		all[i].tracked = false;
	}
	unsigned char n = 0;
	io.u8(&n);
	std::vector<bool> seen(all.size(), false);
	for (unsigned char e = 0; e < n; ++e)
	{
		unsigned char idx = 0, mixed = 0, nt = 0;
		std::string known, base;
		io.u8(&idx);
		io.str(&known);
		io.str(&base);
		io.u8(&mixed);
		io.u8(&nt);
		if (idx >= all.size() || seen[idx] || mixed > 1 || nt > (unsigned char)SENSE_SIGHT_TEMPLATES)
			io.refuse();
		// a NUL inside a name: the engine's C strings end there, so "a\0x" would be "a" again
		if (known.find('\0') != std::string::npos || base.find('\0') != std::string::npos)
			io.refuse();
		seen[idx] = true;
		FactionSense &s = all[idx];
		for (unsigned char k = 0; k < nt; ++k)
		{
			std::string t;
			io.str(&t);
			for (size_t j = 0; j < s.tmpl.size(); ++j)
				if (s.tmpl[j] == t)
					io.refuse();	// one template twice is not two sightings
			if (t.empty() || t.find('\0') != std::string::npos)
				io.refuse();
			s.tmpl.push_back(t);
		}
		// what the pass can leave behind, nothing else: known = its seen base with all its templates, never mixed; unknown and not
		// mixed = fewer templates than it takes (a full set unmixed would have been known)
		if (!known.empty() && (known != base || mixed != 0 || nt < (unsigned char)SENSE_SIGHT_TEMPLATES))
			io.refuse();
		if (known.empty() && mixed == 0 && nt >= (unsigned char)SENSE_SIGHT_TEMPLATES)
			io.refuse();
		if (base.empty() && (nt != 0 || mixed != 0))
			io.refuse();	// a template or a second side is only ever recorded against a first seen side
		if (!base.empty() && nt == 0)
			io.refuse();	// ...and a first seen side always with its template (else it would block learning for good)
		s.known = known;
		s.seenBase = base;
		s.mixed = mixed != 0;
	}
}

/// the bank's loader of its SENSE block: ONE per bank block (a crafted second one would clear what the first restored
/// and leave the lobby half unread), then senseXfer
inline void senseLoadBlock(ByteIO &io, std::vector<FactionSense> &all, bool *wanted, bool *loadedOnce)
{
	if (*loadedOnce)
		io.refuse();
	*loadedOnce = true;
	senseXfer(io, all, wanted);
}

}
