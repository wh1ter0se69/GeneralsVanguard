// GeneralsX: IN-MATCH VOTE KICK - the rules, and nothing else.
//
// wh1ter0se, 05/10/2026: "Kick needs to be initiated by one play; one play can initiate a vote to kick once every 5 minutes; and a
// small banner appears with ... f1 to accept, f2 to reject; EVERYONE should vote YES for a kick to happen, one vetos the thing,
// and make it likes cs with a countdown of 30 seconds before its failed to vote, and make it small enough and can be hidden".
//
// LOCKSTEP. This is simulation code: GameLogic calls it only from the synchronised orders MSG_GX_VOTEKICK_START / _CAST (executed
// at the same frame on every peer and in a replay) and from its per-frame tick right after that frame's orders. Every input is
// therefore identical everywhere - the frame, the sender's Player index (the network layer's, never anything the order says),
// the order's arguments, and `living`, the mask of living human players GameLogic reads from simulation state (player type and
// Player::isPlayerActive). No clock, no network state, no UI state. Pure and engine-free so scripts/test/vote-kick-test.cpp can
// hold every rule without a match.
//
// THE RULES (each one written down for the owner, fixes/PLAN_vote-kick.md):
//  - Starter and electors are living humans. The target is a living human other than the starter, and never votes.
//  - Electorate = living humans minus the target, and it must hold at least `minElectors` (2) at all times: in a 1v1 a kick would
//    be a win for the starter, so the start is refused, and a vote whose electorate falls under two fails.
//  - The start is the starter's YES. Unanimous: the vote passes the moment every elector has said YES - a ballot, or an elector
//    leaving, can complete it - and the first NO vetoes it. One ballot per elector per vote, naming the vote's id.
//  - durationFrames after the start the tick times it out; ballots executing on the last frame still count.
//  - One vote at a time. A starter may start again cooldownFrames after his previous ACCEPTED start, whatever its outcome; a
//    refused start spends nothing.
#pragma once

#include <stdint.h>

// GeneralsX: 32 -> 64 with MAX_PLAYER_COUNT 40 (a Player index must fit the masks).
enum { GXVK_MAX_PLAYERS = 64 };	///< mask width; GameLogic asserts MAX_PLAYER_COUNT fits
typedef uint64_t GXVoteKickMask;	///< one bit per Player index

enum GXVoteKickVerdict
{
	GXVK_NONE = 0,				///< nothing changed (tick), or nothing to do
	GXVK_STARTED,
	GXVK_COUNTED,				///< a YES counted, the vote runs on
	GXVK_PASSED,				///< every elector said YES: remove the target now
	GXVK_VETOED,
	GXVK_TIMED_OUT,
	GXVK_TARGET_GONE,			///< the target left or was defeated first
	GXVK_TOO_FEW,				///< the electorate fell under minElectors
	GXVK_REFUSED_BUSY,			///< refusals change nothing
	GXVK_REFUSED_COOLDOWN,
	GXVK_REFUSED_STARTER,
	GXVK_REFUSED_TARGET,
	GXVK_REFUSED_TOO_FEW,
	GXVK_REFUSED_NO_VOTE,
	GXVK_REFUSED_WRONG_VOTE,
	GXVK_REFUSED_NOT_VOTER,
	GXVK_REFUSED_ALREADY
};

struct GXVoteKickRules
{
	uint32_t durationFrames;	///< 30 s of logic frames
	uint32_t cooldownFrames;	///< 5 min of logic frames
	uint32_t minElectors;		///< 2
};

struct GXVoteKickState
{
	uint32_t id;				///< the latest vote's serial, 1-based; 0 = none yet. A ballot names it.
	int32_t target;				///< Player index; -1 = no vote running
	int32_t starter;
	uint32_t startFrame;
	uint32_t endFrame;			///< startFrame + durationFrames
	GXVoteKickMask yes;			///< electors who said YES, by Player index bit
	uint32_t nextStart[GXVK_MAX_PLAYERS];	///< first frame each Player may start again (0 = any)
	// the latest vote's outcome, for the client to show; never read by the rules
	uint32_t lastId;
	int32_t lastTarget;
	int32_t lastStarter;
	int32_t lastVerdict;
	uint32_t lastFrame;
};

inline GXVoteKickMask gxVoteKickBit( int32_t p ) { return ( p >= 0 && p < GXVK_MAX_PLAYERS ) ? ( (GXVoteKickMask)1 << p ) : 0u; }

inline uint32_t gxVoteKickCount( GXVoteKickMask m )
{
	uint32_t n = 0;
	for( ; m != 0; m &= m - 1 )
		++n;
	return n;
}

inline void gxVoteKickReset( GXVoteKickState *s )
{
	s->id = 0;
	s->target = -1;
	s->starter = -1;
	s->startFrame = 0;
	s->endFrame = 0;
	s->yes = 0;
	for( int i = 0; i < GXVK_MAX_PLAYERS; ++i )
		s->nextStart[i] = 0;
	s->lastId = 0;
	s->lastTarget = -1;
	s->lastStarter = -1;
	s->lastVerdict = GXVK_NONE;
	s->lastFrame = 0;
}

/// Who must say YES while a vote runs: the living humans except the target.
inline GXVoteKickMask gxVoteKickElectorate( const GXVoteKickState *s, GXVoteKickMask living )
{
	return ( s->target < 0 ) ? 0u : ( living & ~gxVoteKickBit( s->target ) );
}

/// Is a vote running that still waits for @p player's ballot? (The client's banner and F1/F2 ask this.)
inline bool gxVoteKickAwaits( const GXVoteKickState *s, int32_t player, GXVoteKickMask living )
{
	const GXVoteKickMask b = gxVoteKickBit( player );
	return b != 0 && ( gxVoteKickElectorate( s, living ) & b ) != 0 && ( s->yes & b ) == 0;
}

/// Frames until @p player may start a vote again (0 = now).
inline uint32_t gxVoteKickCooldownLeft( const GXVoteKickState *s, int32_t player, uint32_t frame )
{
	if( gxVoteKickBit( player ) == 0 )
		return 0;
	const uint32_t next = s->nextStart[player];
	return ( frame < next ) ? ( next - frame ) : 0u;
}

inline GXVoteKickVerdict gxVoteKickEnd( GXVoteKickState *s, uint32_t frame, GXVoteKickVerdict verdict )
{
	s->lastId = s->id;
	s->lastTarget = s->target;
	s->lastStarter = s->starter;
	s->lastVerdict = verdict;
	s->lastFrame = frame;
	s->target = -1;
	s->starter = -1;
	s->yes = 0;
	return verdict;
}

/// The running vote's standing against the current electorate: PASSED / TARGET_GONE / TOO_FEW end it, NONE leaves it running.
inline GXVoteKickVerdict gxVoteKickJudge( GXVoteKickState *s, const GXVoteKickRules &r, uint32_t frame, GXVoteKickMask living )
{
	if( ( living & gxVoteKickBit( s->target ) ) == 0 )
		return gxVoteKickEnd( s, frame, GXVK_TARGET_GONE );
	const GXVoteKickMask electorate = gxVoteKickElectorate( s, living );
	if( gxVoteKickCount( electorate ) < r.minElectors )
		return gxVoteKickEnd( s, frame, GXVK_TOO_FEW );
	if( ( s->yes & electorate ) == electorate )
		return gxVoteKickEnd( s, frame, GXVK_PASSED );
	return GXVK_NONE;
}

inline GXVoteKickVerdict gxVoteKickStart( GXVoteKickState *s, const GXVoteKickRules &r, uint32_t frame, int32_t starter,
	int32_t target, GXVoteKickMask living )
{
	if( s->target >= 0 )
		return GXVK_REFUSED_BUSY;
	const uint32_t sb = gxVoteKickBit( starter ), tb = gxVoteKickBit( target );
	if( ( living & sb ) == 0 )
		return GXVK_REFUSED_STARTER;
	if( ( living & tb ) == 0 || target == starter )
		return GXVK_REFUSED_TARGET;
	if( frame < s->nextStart[starter] )
		return GXVK_REFUSED_COOLDOWN;
	if( gxVoteKickCount( living & ~tb ) < r.minElectors )
		return GXVK_REFUSED_TOO_FEW;

	++s->id;
	s->target = target;
	s->starter = starter;
	s->startFrame = frame;
	s->endFrame = frame + r.durationFrames;
	s->yes = sb;
	s->nextStart[starter] = frame + r.cooldownFrames;
	const GXVoteKickVerdict v = gxVoteKickJudge( s, r, frame, living );
	return ( v == GXVK_NONE ) ? GXVK_STARTED : v;
}

inline GXVoteKickVerdict gxVoteKickCast( GXVoteKickState *s, const GXVoteKickRules &r, uint32_t frame, int32_t voter,
	uint32_t voteId, bool yes, GXVoteKickMask living )
{
	if( s->target < 0 )
		return GXVK_REFUSED_NO_VOTE;
	if( voteId != s->id )
		return GXVK_REFUSED_WRONG_VOTE;
	const uint32_t vb = gxVoteKickBit( voter );
	if( vb == 0 || ( gxVoteKickElectorate( s, living ) & vb ) == 0 )
		return GXVK_REFUSED_NOT_VOTER;
	if( ( s->yes & vb ) != 0 )
		return GXVK_REFUSED_ALREADY;
	if( !yes )
		return gxVoteKickEnd( s, frame, GXVK_VETOED );
	s->yes |= vb;
	const GXVoteKickVerdict v = gxVoteKickJudge( s, r, frame, living );
	return ( v == GXVK_NONE ) ? GXVK_COUNTED : v;
}

/// Once per logic frame, after that frame's orders: the electorate may have changed (a player left or was defeated), and time runs.
inline GXVoteKickVerdict gxVoteKickTick( GXVoteKickState *s, const GXVoteKickRules &r, uint32_t frame, GXVoteKickMask living )
{
	if( s->target < 0 )
		return GXVK_NONE;
	const GXVoteKickVerdict v = gxVoteKickJudge( s, r, frame, living );
	if( v != GXVK_NONE )
		return v;
	if( frame >= s->endFrame )
		return gxVoteKickEnd( s, frame, GXVK_TIMED_OUT );
	return GXVK_NONE;
}

inline const char *gxVoteKickVerdictName( int32_t v )
{
	switch( v )
	{
		case GXVK_NONE: return "none";
		case GXVK_STARTED: return "started";
		case GXVK_COUNTED: return "counted";
		case GXVK_PASSED: return "passed";
		case GXVK_VETOED: return "vetoed";
		case GXVK_TIMED_OUT: return "timed-out";
		case GXVK_TARGET_GONE: return "target-gone";
		case GXVK_TOO_FEW: return "too-few";
		case GXVK_REFUSED_BUSY: return "refused-busy";
		case GXVK_REFUSED_COOLDOWN: return "refused-cooldown";
		case GXVK_REFUSED_STARTER: return "refused-starter";
		case GXVK_REFUSED_TARGET: return "refused-target";
		case GXVK_REFUSED_TOO_FEW: return "refused-too-few";
		case GXVK_REFUSED_NO_VOTE: return "refused-no-vote";
		case GXVK_REFUSED_WRONG_VOTE: return "refused-wrong-vote";
		case GXVK_REFUSED_NOT_VOTER: return "refused-not-voter";
		case GXVK_REFUSED_ALREADY: return "refused-already";
	}
	return "?";
}
