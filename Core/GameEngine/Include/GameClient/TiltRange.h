// GeneralsX @feature tilt-unlimited-admin 07/10/2026 The camera-tilt pitch range, pure and header-only so
// scripts/test/tilt-range-test.cpp can check it without the engine. Client-only: the view is not simulated,
// nothing here reaches the network or the digest.
#pragma once

// Player range: the Options slider's own 20..60 degrees (see LookAtXlat.cpp), and in a multiplayer match never
// flatter than the player's own default pitch.
static const float GX_TILT_PLAYER_MIN = 0.34906585f;	// 20 deg
static const float GX_TILT_PLAYER_MAX = 1.04719755f;	// 60 deg

// Admin debug range = exactly what View::setPitch itself clamps to (0.1..89.9 deg, View.cpp CLAMP_VIEW_PITCH).
// That is the renderer's limit: past 90 the look-at flips, and the RTS_DEBUG pitch key already drives the camera
// through this whole span. Mirrored here (not read from View) so the test can pin it.
static const float GX_TILT_ADMIN_MIN = 0.0017453293f;	// 0.1 deg
static const float GX_TILT_ADMIN_MAX = 1.5690510f;		// 89.9 deg

// WHEN the admin range applies (lead ruling 07/10, after):
//  * the assertion must be unexpired NOW (the loader checks expiry once; a running session must not keep it), and
//  * the local player must not be a combatant in a live multiplayer match. A copied GXLogin.txt proves no ownership
//    of the account and the client has no revocation list (moderation lives at the relay), so the fairness edge is
//    removed instead of trusted: replays, observer seats, skirmish/single player and shell keep the range; a player
//    in a live LAN/internet match is clamped even if admin. Unknown local player in a live match = combatant.
inline bool gxTiltUnlocked(bool roleIsAdmin, long long expUnixS, long long nowUnixS,
                           bool inLiveMultiplayer, bool localIsObserver)
{
	if (!roleIsAdmin || expUnixS <= nowUnixS) return false;
	return !inLiveMultiplayer || localIsObserver;
}

// `unlocked` MUST come from a signature-verified admin assertion (GXPlayerIdentity::getVerifiedRole), never from a
// setting, INI key or env var: those are editable by any player.
inline float gxTiltTarget(float pitch, float delta, bool unlocked, bool inMatch, float defaultPitch)
{
	float lo = GX_TILT_PLAYER_MIN, hi = GX_TILT_PLAYER_MAX;
	if (unlocked) { lo = GX_TILT_ADMIN_MIN; hi = GX_TILT_ADMIN_MAX; }
	else
	{
		if (inMatch) lo = defaultPitch;
		if (hi < lo) hi = lo;	// a mod default above 60 deg keeps its own floor, never snaps
	}
	float t = pitch + delta;
	return t < lo ? lo : (t > hi ? hi : t);
}
