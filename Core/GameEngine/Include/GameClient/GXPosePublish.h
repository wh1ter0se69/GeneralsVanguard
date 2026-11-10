// GeneralsX: render-split step 4 (S7): the render
// interpolation snapshot is published in the sync phase (R1) instead of being refreshed by the draw.
//
// A drawable's snapshot (Drawable::m_interpPrev/Cur, the turret angles, m_interpFrame) is the pose at the last two logic
// frames. It used to be refreshed lazily, on the first draw-time read of each frame, and WHEN it was read is part of the
// meaning: a frame continues the blend only if the snapshot was read on the frame before, which is what makes a drawable
// that was not drawn for a while (culled, shrouded, in a transport) snap instead of sliding from where it was last seen.
// So the snapshot keeps a CONSUMED bit: the frame of m_interpCur is in the low 31 bits of m_interpFrame, bit 31 says a
// draw-time read used it (Drawable.h is digested whole, so no member can be added; a frame never reaches 2^31).
//
// refresh() is the one rule, today's: a new frame takes the snapshot, continuing from the old one only if that one was
// CONSUMED at the frame before (and moved at most 200 units); the same frame snaps when the pose moved without a tick (a
// script in frozen time). Called at every read and consumed at once, it is exactly the old draw-time refresh. Called by
// the R1 walk for every drawable, it builds the same snapshot before the draw, and the draw only reads it - falling back
// to refresh() itself for a drawable the walk did not cover (needsFallback).
//
// RENDER-ONLY: no simulation code includes it. No engine includes, so scripts/test/audit/bench/pose_publish.cpp checks
// this exact code against a verbatim copy of the old refresh.
#pragma once

#include <cstring>

#ifndef GX_POSE_PUBLISH_TEST_ARM
#define GX_POSE_PUBLISH_TEST_ARM 0	// the bench's fail arms; always 0 in the game
#endif

namespace GXPosePublish
{
constexpr unsigned Never = ~0u;				// InvalidShroudClearFrame: no snapshot yet
constexpr unsigned Consumed = 0x80000000u;

// The snapshot is of this logic frame.
inline bool current(unsigned f, unsigned frame) { return f != Never && (f & ~Consumed) == frame; }

// A draw-time read used the snapshot of this logic frame (the old m_interpFrame == frame).
inline bool consumedAt(unsigned f, unsigned frame)
{
#if GX_POSE_PUBLISH_TEST_ARM == 1
	return current(f, frame);	// fail arm: publish ignores consumption
#else
	return f != Never && f == (frame | Consumed);
#endif
}

inline void consume(unsigned& f)
{
	if (f != Never)
		f |= Consumed;
}

// pose: the pose base now (the logic transform, or a superweapon missile's visual flight). readTurrets(float (&)[T][2])
// fills the turrets' angle and pitch now.
template <class M, int T, class ReadTurrets>
void refresh(M& prev, M& cur, unsigned& f, float (&tPrev)[T][2], float (&tCur)[T][2], const M& pose, ReadTurrets readTurrets,
	unsigned frame)
{
	if (!current(f, frame))
	{
		bool continues = consumedAt(f, frame - 1);
		if (continues)
		{
			const float dx = pose.Get_X_Translation() - cur.Get_X_Translation();
			const float dy = pose.Get_Y_Translation() - cur.Get_Y_Translation();
			const float dz = pose.Get_Z_Translation() - cur.Get_Z_Translation();
			continues = (dx * dx + dy * dy + dz * dz <= 200.0f * 200.0f);
		}
		prev = continues ? cur : pose;
		cur = pose;
		f = frame;
		float turrets[T][2];
		readTurrets(turrets);
		memcpy(tPrev, continues ? tCur : turrets, sizeof(turrets));
		memcpy(tCur, turrets, sizeof(turrets));
	}
	else if (memcmp(&cur, &pose, sizeof(M)) != 0)
	{
		prev = pose;
		cur = pose;
#if GX_POSE_PUBLISH_TEST_ARM != 4	// fail arm 4: the turrets' previous angles are left alone
		memcpy(tPrev, tCur, sizeof(tPrev));
#endif
	}
}

// R1: walk now? Once per logic update (serial: GXRenderSplit::tickSerial, bumped by every GameLogic::update, frozen time
// included) while the draw can read the snapshot (active: interpolating, or a diagnostic that reads it while pinned).
inline bool walkDue(bool on, bool active, unsigned long serial, unsigned frame, unsigned long& lastSerial, unsigned& lastFrame)
{
	if (!on || !active)
		return false;
#if GX_POSE_PUBLISH_TEST_ARM == 3
	(void)serial;
	(void)lastSerial;
	if (frame == lastFrame)	// fail arm: a frozen-time logic update is missed
		return false;
	lastFrame = frame;
	return true;
#else
	(void)frame;
	(void)lastFrame;
	if (serial == lastSerial)
		return false;
	lastSerial = serial;
	return true;
#endif
}

// The walk covers drawables with an object, except where the pose base it would read is not this frame's yet - the draw
// keeps refreshing those as before:
//   visualState: a superweapon missile (Drawable::m_missileVisual): its pose base is the visual flight, which updateDrawable
//     evaluates per logic frame AFTER R1;
//   visualUnchecked: updateDrawable has not classified it yet (m_missileVisualChecked), so it may still become one, AND it
//     was drawn on the frame before - only possible across a script freeze, which skips updateDrawable but not the draw.
//     A drawable created by the last tick is unchecked too, but has no frame before: it snaps either way, so it is covered.
inline bool walkCovers(bool hasObject, bool visualState, bool visualUnchecked, unsigned f, unsigned frame)
{
#if GX_POSE_PUBLISH_TEST_ARM == 5
	(void)visualState;	// fail arm: missiles are published from last frame's visual pose
#else
	if (visualState)
		return false;
#endif
#if GX_POSE_PUBLISH_TEST_ARM == 6
	(void)visualUnchecked;	// fail arm: an unclassified drawable drawn across a freeze is published from its logic pose
	(void)f;
	(void)frame;
#else
	if (visualUnchecked && consumedAt(f, frame - 1))
		return false;
#endif
	return hasObject;
}

// A draw-time read runs refresh() itself (today's path) when publishing is off, for a superweapon missile, or when the walk
// did not publish this frame's snapshot (not covered, created since, interpolation switched on since).
inline bool needsFallback(bool on, bool visualState, unsigned f, unsigned frame)
{
#if GX_POSE_PUBLISH_TEST_ARM == 2
	(void)f;
	(void)frame;
	return !on || visualState;	// fail arm: a stale snapshot is read as it is
#else
	return !on || visualState || !current(f, frame);
#endif
}
}

// The sync phase's walk (Drawable.cpp), called from GameClient::update after the logic phase is updated: publishes every
// covered drawable's snapshot once per logic update. Off by default; GX_POSEPUB=1 turns it on. GX_POSEPUB_STATS=1 prints
// [GXPOSEPUB]; GX_POSEPUB_CHECK=1 (which turns the walk on) compares every draw-time read with today's rule (=2: its
// negative control).
void gxPublishDrawnPoses();
