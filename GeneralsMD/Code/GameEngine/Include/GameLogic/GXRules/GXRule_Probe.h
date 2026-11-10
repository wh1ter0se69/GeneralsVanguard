// GeneralsX: AI RULE BANK rule 1, PROBE (2.4 N2): the bank's test rule.
// It issues nothing and changes nothing; it exercises every path a real rule uses (a param, a ref, the scheduler, the sense layer, a
// saved state block) and logs what it sees, so "PROBE on" vs its NO_OP control proves the bank itself is free (per-frame CRC identical).
// Its pure decision: when does a tick print its [GXRULE] ev=probe line. Pure: no engine type; scripts/test/gx-rulebank-test.cpp tests it.
#pragma once

namespace gxprobe
{

/// ticks = this tick's 1-based count, periodS = the period_s param, cadenceS = the registry cadence: log on the first tick and then
/// once every period, rounded UP to whole ticks (a period no longer than the cadence logs every tick: every = 1)
inline bool shouldLog(unsigned ticks, int periodS, int cadenceS)
{
	if (ticks == 0u || cadenceS <= 0)
		return false;
	const unsigned every = (unsigned)((periodS + cadenceS - 1) / cadenceS);
	return ((ticks - 1u) % every) == 0u;
}

}
