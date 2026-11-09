// GeneralsX @probe Claude 26/09/2026 (backlog #22, Codex) GX_NOSHROUD=1 draws the whole map on THIS client, and
// changes nothing else - see the note above gxNoShroud() in GeneralsMD's GameClient.cpp for what it is for.
//
// It used to CLEAR TheGlobalData->m_shroudOn, retail's ENABLE_CONFIGURABLE_SHROUD switch. But GameClient::update's
// per-drawable getShroudedStatus refresh - which recomputes a sim cache logic reads, PartitionData::m_everSeenByPlayer -
// is gated on that same field, so the probe had to re-open it with `|| probe`; and then on a map whose GameData says
// ShroudOn = No, where every other peer skips the refresh, the probe peer ran it. Once the probe had written the field
// there was no telling "the map turned shroud off" from "the probe did" (a map.ini GameData override copies the
// cleared value), so no gate on it could be right. The probe now writes no shared data: the refresh is retail's again,
// and every site that only DRAWS by the shroud asks gxShroudDrawn(). scripts/test/noshroud-probe-test.cpp pins both.

#pragma once

#include "Common/GameDefines.h"
#include "Common/GlobalData.h"

/// The probe. Set once, by Zero Hour's GameClient::init from the environment - never by Generals, never again.
inline Bool &gxNoShroudProbe() { static Bool s_on = FALSE; return s_on; }

/// Does this client draw by the shroud? Retail's m_shroudOn, and never under the probe. For DRAWING decisions only: a
/// shroud QUERY must run on the same schedule on every peer, so nothing may gate one on this.
/// Without the configurable shroud GlobalData has no m_shroudOn and the shroud is always drawn (Codex r5).
#if defined(RTS_DEBUG) || ENABLE_CONFIGURABLE_SHROUD
inline Bool gxShroudDrawn() { return TheGlobalData->m_shroudOn && !gxNoShroudProbe(); }
#else
inline Bool gxShroudDrawn() { return TRUE; }
#endif
