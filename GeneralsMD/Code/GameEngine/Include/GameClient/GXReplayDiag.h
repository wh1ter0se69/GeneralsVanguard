// FILE: GXReplayDiag.h ///////////////////////////////////////////////////////////////////////////
//
// GeneralsX: Writes the replay diagnostics block (GXReplayDiagFormat.h) while a match records.
//
// Here in GameClient/, on the client side of the SimID digest: nothing in this module is read by the simulation, and
// resources/gitinfo/simdigest_exclusions.txt names the two calls the Recorder makes into it, with that reason.

#pragma once

#include <stdint.h>

/// RecorderClass::startRecording, once the header is on disk: the settings snapshot, so a crash in the map load still has it.
void gxReplayDiagRecordingStarted( void );

/// RecorderClass::stopRecording, before it closes the file: the last partial window, and a second snapshot if a setting changed
/// during the match. Every way out of a match comes through there, quit and restart included; they never write MSG_CLEAR_GAME_DATA.
void gxReplayDiagRecordingEnding( void );

/// GameClient::update, once per main-loop iteration. One clock read and a handful of loads; a -headless peer runs it too (its rows
/// are flagged headless).
void gxReplayDiagSample( void );

/// W3DDisplay::draw, after a successful End_Render: a frame that really reached the screen. One increment.
void gxReplayDiagFrameDrawn( void );

/// GameClient::update, around Display::draw: how long this frame's draw took. It is what says whether render paid; the row's
/// other time is only what is left over, and blaming that on the renderer would be a guess.
void gxReplayDiagDrawSpan( long long ns );

/// What only the device layer can say (GPU, present mode). A pointer, so anything that links GameEngine without
/// GameEngineDevice still links. `put` takes a key and a value, and sanitizes the value.
typedef void (*GXReplayDiagPutFn)( const char *key, const char *value );
typedef void (*GXReplayDiagDeviceFn)( GXReplayDiagPutFn put );
void gxReplayDiagSetDeviceFacts( GXReplayDiagDeviceFn fn );

// ------------------------------------------------------------------------------------------------
// GeneralsX: TELEMETRY: a per-second PERF row (fps, frame-time
// percentiles, TPS, stall, hold, network, own CPU) on the same comment-record path, batched once per window. Bytes and arithmetic:
// GXReplayDiagFormat.h. Nothing here is read by the simulation, sent on the wire or a CRC input.
// ------------------------------------------------------------------------------------------------

/// Options.ini GXReplayTelemetry = full | perf | off, read once when a recording starts (anything else: perf). OFF MEANS OFF: no
/// settings snapshot, no timeline, no telemetry - the replay is the retail one plus its header identity (lead ruling 05/11). Phase 1
/// records PERF only, so full writes what perf writes until VIEW and INPUT exist.
enum GxTelLevel { GX_TEL_OFF = 0, GX_TEL_PERF = 1, GX_TEL_FULL = 2 };
GxTelLevel gxReplayTelemetryLevel( void );

/// The game loop is about to stop (true) or runs again (false) in a solo game: Win32GameEngine's minimised loop, SDL3GameEngine's
/// inactive iOS return. Closes the open row flagged suspended, so the reader prints "suspended N s" and never a 12 s hitch; a real
/// hitch stays a measured frame. Idempotent; a no-op while nothing records.
void gxReplayTelSuspend( bool suspended );

/// GeneralsX: The player is away (true) or back (false) while the loop keeps running: iOS inactive or
/// in the background, a Windows window that is minimised or lost focus, an SDL desktop window that lost focus. Every PERF row that
/// overlaps it is flagged unfocused (U), so a match that ran on unwatched - fps 0 at 60 TPS - says so. Idempotent; a flag only (never
/// closes a row, never touches what the simulation does); harmless while nothing records - a recording that starts away begins flagged.
void gxReplayTelAway( bool away );

/// What the device layer's 1 Hz sampler thread measured (phase 2). Counters already normalised to [startNs, endNs] (gxPerfMonoNs
/// clock); u16 fields 0xFFFF and u8 0xFF when not readable, the byte counts UINT64_MAX.
struct GxTelOsSnapshot
{
	uint32_t seq;
	long long startNs, endNs;
	long long beginNs;		///< when the sampler BEGAN the pass that startNs stamps: a counter of that sample may be this old
	uint16_t cpuProcMs, cpuRenderMs, cpuAudioMs, cpuPcoreMs, sysCpuX10, gpuProcMs;
	uint64_t rssBytes, sysAvailBytes;	///< WIDE: narrowed to 16 MiB units only when the row is packed
	uint8_t sysGpuPct;
};
/// Non-blocking: false when the sampler holds its lock or has nothing yet. The row then says UNKNOWN and sampler-late; the main thread
/// never waits on the sampler.
typedef bool (*GXReplayTelOsFn)( GxTelOsSnapshot *out );

/// Whether a snapshot may fill the row closing at nowNs that began at rowStartNs: not used before, ended at most 1.5 s ago (spec 2.4),
/// and of this row's time - it began at most 2.5 s before the row (a missed sample or two is still this run's data). A base taken before
/// a suspension or a window restart reaches back further and is refused, so the gap's load is never charged to the first row after it
/// notBeforeNs: when the loop last resumed from a suspension (0: never) - a snapshot that began before it spans
/// the stopped seconds however short they were, so it is refused too (a 2 s stop inside the 2.5 s bound diluted
/// the resumed row's CPU to a third). A snapshot with a counter read after nowNs is a later row's. Otherwise the row says UNKNOWN +
/// sampler late.
inline bool gxTelOsUsable( const GxTelOsSnapshot &os, bool haveSeq, uint32_t lastSeq, long long nowNs, long long rowStartNs, long long notBeforeNs )
{
	return ( !haveSeq || os.seq != lastSeq ) && os.endNs > os.startNs && nowNs - os.endNs <= 1500000000LL
#ifndef GX_TEL_RED_FUTURE	// fail arm: a sample read after the row closed (the main thread preempted before fillRow) fills it
		&& os.endNs <= nowNs	// its seconds are a later row's
#endif
		&& os.startNs >= rowStartNs - 2500000000LL
#ifndef GX_TEL_RED_STAMP_BARRIER	// fail arm: the base sample's stamp, not its pass's start, is held against the barrier
		&& os.beginNs >= notBeforeNs;	// a pass begun before a resume read some counter before it, however late it was stamped (r9)
#else
		&& os.startNs >= notBeforeNs;
#endif
}
void gxReplayTelSetOsReader( GXReplayTelOsFn fn );

/// GeneralsX: Phase 2: the device layer's sampler thread, started and idled by the recording. start 1
/// (gxReplayDiagRecordingStarted, level not off): creates the thread on first use, then wakes it. start 0 (gxReplayDiagRecordingEnding):
/// a flag store, returns at once - the thread is detached and never joined, so a recording never waits on an OS or driver query.
typedef void (*GXReplayTelControlFn)( int start );
void gxReplayTelSetSamplerControl( GXReplayTelControlFn fn );
