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

// L1 native device, game side (§8.1, §12).
//
// Options.ini GXRenderer = native (or GX_RENDERER=native, TEST-ONLY, over it: GameClient/GXRendererKey.h) asks for the native d3d8
// device; GxNative::Install() (W3DDisplay::init, right after GxCap::Install and before WW3D::Init creates the device) sets
// DX8Wrapper::Interface_Wrap_Hook. Without GX_RENDER_NATIVE the call is an inline no-op that only says so when the switch is set,
// so a build that cannot honour it never looks like one that did.
// Render-only; never writes state the simulation reads.
#pragma once

struct IDirect3DDevice8;

namespace GxNative
{
#ifdef GX_RENDER_NATIVE
// True when the process asked for the native session (GXGraphics::nativeRendererRequested). Used before SDL loads Vulkan (SDL3Main).
bool Requested();
void Install();
bool SessionLive();	///< a native session really started; Requested() alone can still end up classic (the replay diagnostics block)
// the native session's "[GXNATIVE] stats final ... end=exit" line when its device is still
// alive at process exit (a leaked child keeps it; the device prints end=teardown itself when it is destroyed). The game ends in
// _exit() (SDL3Main), which runs no atexit handler - call this before shutdownMemoryManager(): the line allocates. No-op otherwise.
void FlushFinalStats();
// Every message the Windows message loop takes, BEFORE dispatch - NVIDIA PCL
// Stats pings with its own registered message, possibly a thread message that no window procedure ever sees. No-op without Reflex.
void NoteWindowsMessage(unsigned message);
// S7 A92 TEST-ONLY (GX_GPUTIME_DUMP, read once): W3DDisplay::draw hands the native device
// the logic frame it draws through DX8Wrapper::Device_Call, so each per-frame GPU sample of the dump names its game frame. Without the
// switch there is no call at all; on another device TestSetLogicFrame does nothing.
bool GpuTimeDump();
void TestSetLogicFrame(IDirect3DDevice8 *dev, unsigned frame);
#else
inline bool Requested() { return false; }
inline bool SessionLive() { return false; }
void Install_Unavailable();	// GxNativeInstallOff.cpp: prints the classic-session line when GX_RENDERER=native
inline void Install() { Install_Unavailable(); }
inline void FlushFinalStats() {}
inline void NoteWindowsMessage(unsigned) {}
inline bool GpuTimeDump() { return false; }
inline void TestSetLogicFrame(IDirect3DDevice8 *, unsigned) {}
#endif
}
