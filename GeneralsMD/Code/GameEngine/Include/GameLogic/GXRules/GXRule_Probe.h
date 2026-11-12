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

// AI RULE BANK rule 1, PROBE (2.4 N2): the bank's test rule.
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
