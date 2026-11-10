// GeneralsX: AI RULE BANK rule 1, PROBE (GXRule_Probe.h): issues nothing, changes nothing - it counts its ticks,
// reads the sense layer once per tick and saves its count, so "PROBE on" vs its NO_OP control (per-frame CRC identical) proves the bank
// path is free, and the save/load fixture can see its state survive a load. Test-instruments builds add the red arms and fixtures:
//   GX_RULE_PROBE_RED=1                  the first tick orders this player's lowest-id own unit to move (N2's red arm: must DIFFER)
//   GX_RULE_PROBE_SLOW=maintain|hook     a 3 ms spin in maintain() / in a permission hook (the per-rule timer's red arm: [GXHOOK] over)
//   GX_RULE_SENSE_TEST=cargo|disguise|fog   GXRuleSense fixtures, run once at the first tick (ev=sense_test ... result=PASS|FAIL)
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Common/Player.h"
#include "Common/ThingTemplate.h"
#include "GameLogic/Object.h"
#include "GameLogic/GXRules/GXRuleBank.h"
#include "GameLogic/GXRules/GXRuleSense.h"
#include "GameLogic/GXRules/GXRule_Probe.h"
#if defined(GX_TEST_INSTRUMENTS)
#include <chrono>	// the GX_RULE_PROBE_SLOW spin only
#endif

#if defined(GX_TEST_INSTRUMENTS)
void gxRuleSenseFixture(GXRuleCtx &ctx, const char *which);	// GXRuleSense.cpp
#endif

namespace
{
enum { PROBE_ID = 1, PROBE_PERIOD = 1, PROBE_TEMPLATE = 1, PROBE_TEAM = 2 };
const Real PROBE_SENSE_R = 600.0f;

class GXRule_Probe : public GXRuleImpl
{
public:
	GXRule_Probe() : m_ticks(0u), m_lastValue(0), m_redDone(FALSE) {}

	virtual void tick(GXRuleCtx &ctx) override
	{
		++m_ticks;
		ctx.triggered();
		std::vector<Object *> own;
		ctx.ownObjects(&own);
		const Object *anchor = nullptr;
		for (size_t i = 0; i < own.size() && anchor == nullptr; ++i)
			if (own[i]->isKindOf(KINDOF_STRUCTURE) && !own[i]->isEffectivelyDead())
				anchor = own[i];
		m_lastValue = anchor != nullptr ? ctx.sense().enemyValueNear(anchor, PROBE_SENSE_R) : 0;
#if defined(GX_TEST_INSTRUMENTS)
		if (m_ticks == 1u)
		{
			const char *fx = getenv("GX_RULE_SENSE_TEST");
			if (fx != nullptr && *fx != 0)
				gxRuleSenseFixture(ctx, fx);
		}
		const char *red = getenv("GX_RULE_PROBE_RED");
		if (red != nullptr && strcmp(red, "1") == 0 && !m_redDone)
		{
			for (size_t i = 0; i < own.size() && !m_redDone; ++i)
				if (!own[i]->isKindOf(KINDOF_STRUCTURE) && own[i]->getAI() != nullptr)
				{
					Coord3D to = *own[i]->getPosition();
					to.x += 100.0f;
					m_redDone = ctx.orderMoveTo(own[i], to);
					if (m_redDone)
					{
						ctx.fired();
						char kv[64];
						snprintf(kv, sizeof(kv), "unit=%u", (unsigned)own[i]->getID());
						ctx.log("probe_red_order", kv);
					}
				}
		}
#endif
		const gxrb::RuleRow *spec = gxrb::findRule(gxrb::gxRegistry(), PROBE_ID);
		if (spec != nullptr && gxprobe::shouldLog(m_ticks, ctx.param(PROBE_PERIOD), spec->cadenceS))
		{
			char kv[160];
			const AsciiString t = ctx.ref(PROBE_TEMPLATE);
			const AsciiString tm = ctx.ref(PROBE_TEAM);		// a TEAM ref only so the TEAM action has an accepting slot
			snprintf(kv, sizeof(kv), "ticks=%u value=%d tmpl=%s team=%s", m_ticks, (int)m_lastValue, t.isEmpty() ? "-" : t.str(),
				tm.isEmpty() ? "-" : tm.str());
			ctx.log("probe", kv);
		}
	}

#if defined(GX_TEST_INSTRUMENTS)
	virtual Bool hasMaintain() const override { return slowIs("maintain"); }
	virtual void maintain(GXRuleCtx &ctx) override { (void)ctx; spin(); }
	virtual Bool joinsPermitHook(int hook) const override { return hook == gxrb::HOOK_SELL && slowIs("hook"); }
	virtual Bool permitHook(GXRuleCtx &ctx, int hook, const char *context) override
	{
		(void)ctx;
		(void)hook;
		(void)context;
		spin();
		return TRUE;
	}
#endif

	virtual unsigned short stateVersion() const override { return 1; }
	virtual void xferState(gxrb::ByteIO &io, unsigned short savedVersion) override
	{
		(void)savedVersion;
		unsigned t = m_ticks;
		int v = (int)m_lastValue;
		io.u32(&t);
		io.i32(&v);
		m_ticks = t;
		m_lastValue = v;
	}

private:
#if defined(GX_TEST_INSTRUMENTS)
	static Bool slowIs(const char *what)
	{
		const char *v = getenv("GX_RULE_PROBE_SLOW");
		return v != nullptr && strcmp(v, what) == 0;
	}
	static void spin()
	{
		const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + std::chrono::microseconds(3000);
		volatile unsigned x = 0;
		while (std::chrono::steady_clock::now() < end)
			x = x + 1u;
	}
#endif
	unsigned m_ticks;
	Int m_lastValue;
	Bool m_redDone;		///< test-instruments red arm only; not saved (the red arm never crosses a save)
};
}

GXRuleImpl *gxRuleMake_Probe() { return new GXRule_Probe; }
