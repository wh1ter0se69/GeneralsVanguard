// GeneralsX: AI RULE BANK: the registry as tables. Pure:
// no engine type, no state, no I/O - scripts/test/gx-rulebank-test.cpp builds it with plain c++17 and feeds it fake registries.
// GXRuleTable.inl is the source of truth; this header turns its rows into constexpr arrays and refuses to compile a registry that
// breaks a rule of the bank (ids unique, keys unique per rule, defaults inside their box, VISION rules Brutal only, claims well formed).
#pragma once

namespace gxrb
{

// ---- tiers (plan 2.8): a player's level as the bank sees it. Easy/Normal AIs, humans and campaign AIs are TIER_NONE ----
enum : unsigned { TIER_NONE = 0u, TIER_HARD = 1u, TIER_SENTIENT = 2u, TIER_BRUTAL = 4u, TIER_ALL = 7u };
enum RefType { REF_TEMPLATE = 1, REF_TEAM = 2 };

// ---- hooks (plan 2.2.5): value hooks are EXCLUSIVE per (player, HOOK:context), permission hooks compose by AND ----
enum HookId { HOOK_GATHER, HOOK_PLACE, HOOK_BUILDORDER, HOOK_TEAMPICK, HOOK_TARGET, HOOK_SKILL, HOOK_UPGRADE, HOOK_SELL,
	HOOK_MICRO, HOOK_TRANSPORT, HOOK_COUNT };
constexpr const char *kHookNames[HOOK_COUNT] = { "GATHER", "PLACE", "BUILDORDER", "TEAMPICK", "TARGET", "SKILL", "UPGRADE", "SELL",
	"MICRO", "TRANSPORT" };

struct RuleRow { int id; const char *cls; const char *name; unsigned tiers; bool vision; int cadenceS; bool live; };
struct ParamRow { int rule; int key; const char *name; int def; int lo; int hi; const char *unit; };
struct RefRow { int rule; int key; const char *name; int type; };
struct ClaimRow { int rule; const char *claim; };
struct CapRow { const char *name; };

/// one registry: the real one (gxRegistry() below) or a test's. Every array ends with a sentinel row (id/rule 0) not counted in n*.
struct Registry
{
	const RuleRow *rules; int nRules;
	const ParamRow *params; int nParams;
	const RefRow *refs; int nRefs;
	const ClaimRow *claims; int nClaims;
	const CapRow *caps; int nCaps;
};

// ---- constexpr helpers ----
constexpr bool cstrEq(const char *a, const char *b)
{
	if (a == nullptr || b == nullptr)
		return a == b;
	while (*a != 0 && *a == *b)
	{
		++a;
		++b;
	}
	return *a == *b;
}
constexpr int cstrLen(const char *a)
{
	int n = 0;
	while (a != nullptr && a[n] != 0)
		++n;
	return n;
}
/// "HOOK:context" with a known HOOK and a non-empty context without spaces
constexpr bool claimWellFormed(const char *c)
{
	if (c == nullptr)
		return false;
	int colon = -1;
	for (int i = 0; c[i] != 0; ++i)
	{
		if (c[i] == ' ')
			return false;
		if (c[i] == ':' && colon < 0)
			colon = i;
	}
	if (colon <= 0 || c[colon + 1] == 0)
		return false;
	for (int h = 0; h < HOOK_COUNT; ++h)
	{
		const char *n = kHookNames[h];
		int i = 0;
		while (i < colon && n[i] != 0 && n[i] == c[i])
			++i;
		if (i == colon && n[i] == 0)
			return true;
	}
	return false;
}
constexpr int countRows(const RuleRow *r) { int n = 0; while (r[n].id != 0) ++n; return n; }
constexpr int countRows(const ParamRow *r) { int n = 0; while (r[n].rule != 0) ++n; return n; }
constexpr int countRows(const RefRow *r) { int n = 0; while (r[n].rule != 0) ++n; return n; }
constexpr int countRows(const ClaimRow *r) { int n = 0; while (r[n].rule != 0) ++n; return n; }
constexpr int countRows(const CapRow *r) { int n = 0; while (r[n].name != nullptr) ++n; return n; }

constexpr bool ruleExists(const Registry &g, int id)
{
	for (int i = 0; i < g.nRules; ++i)
		if (g.rules[i].id == id)
			return true;
	return false;
}

/// every rule of the bank a registry must keep; a failing check names itself through its own function, for the static_assert below
constexpr bool rulesValid(const Registry &g)
{
	for (int i = 0; i < g.nRules; ++i)
	{
		const RuleRow &r = g.rules[i];
		if (r.id <= 0 || r.id > 65535 || r.tiers == 0u || (r.tiers & ~TIER_ALL) != 0u || r.cadenceS < 0 || cstrLen(r.name) == 0)
			return false;
		if (r.vision && r.tiers != TIER_BRUTAL)
			return false;	// a rule that reads the whole map is Brutal's alone (plan 4.3)
		for (int j = 0; j < i; ++j)
			if (g.rules[j].id == r.id || cstrEq(g.rules[j].name, r.name))
				return false;
	}
	return true;
}
constexpr bool paramsValid(const Registry &g)
{
	for (int i = 0; i < g.nParams; ++i)
	{
		const ParamRow &p = g.params[i];
		if (!ruleExists(g, p.rule) || p.key <= 0 || p.key > 65535 || p.lo > p.def || p.def > p.hi || cstrLen(p.name) == 0)
			return false;
		for (int j = 0; j < i; ++j)
			if (g.params[j].rule == p.rule && (g.params[j].key == p.key || cstrEq(g.params[j].name, p.name)))
				return false;
	}
	return true;
}
constexpr bool refsValid(const Registry &g)
{
	for (int i = 0; i < g.nRefs; ++i)
	{
		const RefRow &r = g.refs[i];
		if (!ruleExists(g, r.rule) || r.key <= 0 || r.key > 65535 || (r.type != REF_TEMPLATE && r.type != REF_TEAM) || cstrLen(r.name) == 0)
			return false;
		for (int j = 0; j < i; ++j)
			if (g.refs[j].rule == r.rule && (g.refs[j].key == r.key || cstrEq(g.refs[j].name, r.name)))
				return false;
	}
	return true;
}
constexpr bool claimsValid(const Registry &g)
{
	for (int i = 0; i < g.nClaims; ++i)
	{
		if (!ruleExists(g, g.claims[i].rule) || !claimWellFormed(g.claims[i].claim))
			return false;
		for (int j = 0; j < i; ++j)
			if (g.claims[j].rule == g.claims[i].rule && cstrEq(g.claims[j].claim, g.claims[i].claim))
				return false;
	}
	return true;
}
constexpr bool registryValid(const Registry &g)
{
	return rulesValid(g) && paramsValid(g) && refsValid(g) && claimsValid(g);
}

// ---- lookups (linear: a registry is small, and these run at configuration time, never per frame) ----
inline const RuleRow *findRule(const Registry &g, int id)
{
	for (int i = 0; i < g.nRules; ++i)
		if (g.rules[i].id == id)
			return &g.rules[i];
	return nullptr;
}
inline const ParamRow *findParam(const Registry &g, int rule, int key)
{
	for (int i = 0; i < g.nParams; ++i)
		if (g.params[i].rule == rule && g.params[i].key == key)
			return &g.params[i];
	return nullptr;
}
inline const RefRow *findRef(const Registry &g, int rule, int key)
{
	for (int i = 0; i < g.nRefs; ++i)
		if (g.refs[i].rule == rule && g.refs[i].key == key)
			return &g.refs[i];
	return nullptr;
}
/// do rules a and b claim one HOOK:context in common
inline bool shareClaim(const Registry &g, int a, int b)
{
	for (int i = 0; i < g.nClaims; ++i)
	{
		if (g.claims[i].rule != a)
			continue;
		for (int j = 0; j < g.nClaims; ++j)
			if (g.claims[j].rule == b && cstrEq(g.claims[i].claim, g.claims[j].claim))
				return true;
	}
	return false;
}
inline bool ruleClaims(const Registry &g, int rule, const char *claim)
{
	for (int i = 0; i < g.nClaims; ++i)
		if (g.claims[i].rule == rule && cstrEq(g.claims[i].claim, claim))
			return true;
	return false;
}

// ---- the real registry, from GXRuleTable.inl ----
#define GX_TIER_HARD gxrb::TIER_HARD
#define GX_TIER_SENTIENT gxrb::TIER_SENTIENT
#define GX_TIER_BRUTAL gxrb::TIER_BRUTAL
#define GX_REF_TEMPLATE gxrb::REF_TEMPLATE
#define GX_REF_TEAM gxrb::REF_TEAM

// GX_RULE_TABLE expanded IN PLACE per array (scripts/build/simid-closure.py refuses an #include inside an initializer)
#include "GXRuleTable.inl"
#define GX_RB_SKIP1(a)
#define GX_RB_SKIP2(a, b)
#define GX_RB_SKIP4(a, b, c, d)
#define GX_RB_SKIP7(a, b, c, d, e, f, g)
#define GX_RB_ROW_RULE(id, cls, name, tiers, vision, cadence, live) { id, #cls, name, (unsigned)(tiers), (vision) != 0, cadence, (live) != 0 },
#define GX_RB_ROW_PARAM(rule, key, name, def, lo, hi, unit) { rule, key, name, def, lo, hi, unit },
#define GX_RB_ROW_REF(rule, key, name, type) { rule, key, name, (int)(type) },
#define GX_RB_ROW_CLAIM(rule, claim) { rule, claim },
#define GX_RB_ROW_CAP(name) { name },

namespace real
{
constexpr RuleRow kRules[] = { GX_RULE_TABLE(GX_RB_ROW_RULE, GX_RB_SKIP7, GX_RB_SKIP4, GX_RB_SKIP2, GX_RB_SKIP1) { 0, nullptr, nullptr, 0u, false, 0, false } };
constexpr ParamRow kParams[] = { GX_RULE_TABLE(GX_RB_SKIP7, GX_RB_ROW_PARAM, GX_RB_SKIP4, GX_RB_SKIP2, GX_RB_SKIP1) { 0, 0, nullptr, 0, 0, 0, nullptr } };
constexpr RefRow kRefs[] = { GX_RULE_TABLE(GX_RB_SKIP7, GX_RB_SKIP7, GX_RB_ROW_REF, GX_RB_SKIP2, GX_RB_SKIP1) { 0, 0, nullptr, 0 } };
constexpr ClaimRow kClaims[] = { GX_RULE_TABLE(GX_RB_SKIP7, GX_RB_SKIP7, GX_RB_SKIP4, GX_RB_ROW_CLAIM, GX_RB_SKIP1) { 0, nullptr } };
constexpr CapRow kCaps[] = { GX_RULE_TABLE(GX_RB_SKIP7, GX_RB_SKIP7, GX_RB_SKIP4, GX_RB_SKIP2, GX_RB_ROW_CAP) { nullptr } };
constexpr Registry kRegistry = { kRules, countRows(kRules), kParams, countRows(kParams), kRefs, countRows(kRefs),
	kClaims, countRows(kClaims), kCaps, countRows(kCaps) };
static_assert(rulesValid(kRegistry), "GXRuleTable.inl: a rule id is 0, too big or repeated, a name is empty or repeated, tiers are empty, or a VISION rule is not BRUTAL-only");
static_assert(paramsValid(kRegistry), "GXRuleTable.inl: a param names no rule, its key is < 1 or repeated within its rule, or its default is outside [lo, hi]");
static_assert(refsValid(kRegistry), "GXRuleTable.inl: a ref names no rule, its key is < 1 or repeated within its rule, or its type is not TEMPLATE/TEAM");
static_assert(claimsValid(kRegistry), "GXRuleTable.inl: a claim names no rule, is repeated, or is not HOOK:context with a known HOOK");
}
#undef GX_RB_ROW_RULE
#undef GX_RB_ROW_PARAM
#undef GX_RB_ROW_REF
#undef GX_RB_ROW_CLAIM
#undef GX_RB_ROW_CAP

inline const Registry &gxRegistry() { return real::kRegistry; }

}
