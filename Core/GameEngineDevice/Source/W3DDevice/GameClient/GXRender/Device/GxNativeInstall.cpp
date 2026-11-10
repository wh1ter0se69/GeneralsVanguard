// GeneralsX: see GxNativeInstall.h. Built only with GX_RENDER_NATIVE=ON.
#include "W3DDevice/GameClient/GxNativeInstall.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Common/FileSystem.h"
#include "GameClient/GXGraphicsSettings.h"	// GXUpscaler (the agreed key) decides whether Streamline loads
#include "Common/file.h"
#include "WW3D2/dx8wrapper.h"
#include "GxNativeD3D8.h"
#include "GxNativeDevice.h"	// S7 A92: GxNativeFromDevice (TestSetLogicFrame)
#if defined(_WIN32)
#include "GxNativeHostWin32.h"	// GeneralsX: the Windows game's host is its Win32 window
#else
#include "GxNativeHostSDL.h"
#endif

namespace GxNative
{
bool Requested()
{
	return GXGraphics::nativeRendererRequested() != FALSE;	// Options.ini GXRenderer, GX_RENDERER over it
}

namespace
{
// Row S6's reader: the game's own path to a shader blob (W3DShaderManager::LoadAndCreateD3DShader) - openFile with the path as
// the game spells it, size, read, close - so a mod's override is seen exactly as the game would load it.
bool ReadViaGameFS(const char *path, std::vector<uint8_t> *bytes, void *)
{
	if (TheFileSystem == nullptr)
		return false;
	File *file = TheFileSystem->openFile(path, File::READ | File::BINARY);
	if (file == nullptr)
		return false;
	const Int size = file->size();
	if (size <= 0) {
		file->close();
		return false;
	}
	bytes->resize((size_t)size);
	const Int got = file->read(bytes->data(), size);
	file->close();
	return got == size;
}

IDirect3D8 *Wrap(IDirect3D8 *dxvk)
{
	using namespace GXRender::Dev;
	// The proxy copies host and session (GxNativeD3D8 ctor); the SDL window (Win32 window on Windows) exists by now (W3DDisplay::init).
	GxNativeHost host;
	const char *why = nullptr;
#if defined(_WIN32)
	const bool hostOk = FillWin32Host(host, &why);
#else
	const bool hostOk = FillSdlHost(host, &why);
#endif
	if (!hostOk) {
		fprintf(stderr, "[GXNATIVE] session=classic reason=\"%s\"\n", why != nullptr ? why : "host");
		fflush(stderr);
		return dxvk;
	}
	// GeneralsX: (PREREG A36.2): NVIDIA Streamline (DLSS) only for GXUpscaler=dlss in a
	// GX_STREAMLINE build on an NVIDIA adapter; otherwise the host is untouched and nothing of Streamline is in the process.
	// (PREREG A39): GXReflex / GXFrameGen load it too (Reflex, DLSS Frame Generation), same gates.
	GxNativeLoadStreamline(dxvk, host, GXGraphics::upscaler() == GXGraphics::UPSCALER_DLSS, GXGraphics::reflex(), GXGraphics::frameGen());
	// (U2): DLSS resolved in (a Streamline build) but Streamline did not load (not NVIDIA, DLLs missing):
	// this runs at install, before the seam sizes anything, so the whole chain goes off. Here, not in GxNativeDlss.cpp, because that file
	// is in gx_device and the standalone test exes do not link the settings.
	if (GXGraphics::upscaler() == GXGraphics::UPSCALER_DLSS && host.streamline == nullptr)
		GXGraphics::demoteUpscaler("DLSS requested, Streamline not loaded");
	GxNativeSessionHost session;
	session.readFile = &ReadViaGameFS;
	return GxNativeWrapInterface(dxvk, host, session);
}
}

bool SessionLive()
{
	return GXRender::Dev::GxNativeSessionLive();
}

void Install()
{
	if (!Requested())
		return;
	DX8Wrapper::Interface_Wrap_Hook = &Wrap;
	fprintf(stderr, "[GXNATIVE] requested: renderer=native (the [GXRENDERER] line says by what), interface hook installed\n");
	fflush(stderr);
}

void FlushFinalStats()
{
	GXRender::Dev::GxNativeFlushFinalStatsAtExit();
}

// S7 A92 TEST-ONLY (the integrator's P4 ruling (4)): GX_GPUTIME_DUMP read once; the logic frame reaches the device's own queued entry
bool GpuTimeDump()
{
	static const bool on = [] {
		const char *d = getenv("GX_GPUTIME_DUMP");
		return d != nullptr && d[0] != '\0';
	}();
	return on;
}

void TestSetLogicFrame(IDirect3DDevice8 *dev, unsigned frame)
{
	if (GXRender::Dev::GxNativeDevice *n = dev ? GXRender::Dev::GxNativeFromDevice(dev) : nullptr)
		n->testSetLogicFrame(uint32_t(frame));
}

void NoteWindowsMessage(unsigned message)
{
	GXRender::Dev::GxNativeNoteWindowsMessage(message);
}
}
