// GeneralsX: AI RULE BANK registry (gx-handoff ai/rulebank/PLAN_rulebank.md 2.2.1, CONTRACT v1 2.6). THE
// SOURCE OF TRUTH: scripts/ai/rulebank/gen-rule-ids.py generates rule_ids.json from these rows, and GXRuleSpec.h / GXRuleBank.cpp expand
// GX_RULE_TABLE in place (a macro, never an #include inside an initializer: scripts/build/simid-closure.py refuses that). The rows sit in
// ONE macro: every row line ends with " \", NO comment inside it (a // comment would swallow the line splice), and the blank line after
// the last row ends the macro - so a new row is one more line ending in " \" before that blank line. Row forms (one per line):
//   GX_RULE(id, Class, "NAME", tiers, vision, cadence_s, live)    id: explicit, append-only, never reused (ids live in scb data and saves);
//                                                                NAME = RULES.tsv rule_id; Class names the glue (gxRuleMake_<Class>);
//                                                                tiers: GX_TIER_HARD | GX_TIER_SENTIENT | GX_TIER_BRUTAL (a real rule is
//                                                                HARD only if it is an in-flight pin, plan 2.8); vision 1 = reads the whole
//                                                                map, so BRUTAL only; cadence_s 0 = no scheduled tick; live 1 = accepts
//                                                                configuration after its first dispatch (needs tested onReconfigure/onDisable)
//   GX_RULE_PARAM(rule id, key, "name", default, lo, hi, "unit") key >= 1, explicit, immutable, append-only within the rule; decimal integers only
//   GX_RULE_REF(rule id, key, "name", GX_REF_TEMPLATE|GX_REF_TEAM) key >= 1, its own key space; saved by name, resolved at use
//   GX_RULE_CLAIM(rule id, "HOOK:context")                       an EXCLUSIVE value-hook claim (plan 2.2.5); two enabled rules never share one
//   GX_CAPABILITY("NAME")                                        what this binary can do (rule_ids.json _capabilities)
// A new rule: its rows here, its files, one CMake line - see GXRules/README.md.
#pragma once

#define GX_RULE_TABLE(GX_RULE, GX_RULE_PARAM, GX_RULE_REF, GX_RULE_CLAIM, GX_CAPABILITY) \
	GX_CAPABILITY("TIER.FILTER") \
	GX_CAPABILITY("SENSE.ENEMY_FACTION") \
	GX_CAPABILITY("SENSE.FOG_CONDITIONS") \
	GX_RULE(1, Probe, "PROBE", GX_TIER_HARD | GX_TIER_SENTIENT | GX_TIER_BRUTAL, 0, 2, 0) \
	GX_RULE_PARAM(1, 1, "period_s", 10, 1, 600, "s") \
	GX_RULE_REF(1, 1, "unit_template", GX_REF_TEMPLATE) \
	GX_RULE_REF(1, 2, "unit_team", GX_REF_TEAM) \

