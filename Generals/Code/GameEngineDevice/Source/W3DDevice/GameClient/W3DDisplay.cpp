/*
**	Command & Conquer Generals(tm)
**	Copyright 2025 Electronic Arts Inc.
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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// FILE: W3DDisplay.cpp ///////////////////////////////////////////////////////
//
// W3D Implementation for the Game Display which is responsible for creating
// and maintaning the entire visual display
//
// Author: Colin Day, April 2001
//
///////////////////////////////////////////////////////////////////////////////

static void drawFramerateBar();

// SYSTEM INCLUDES ////////////////////////////////////////////////////////////
#include <numeric>
#include <stdlib.h>
#ifdef __APPLE__
// TARGET_OS_IPHONE comes from here and NOWHERE ELSE. Without this include the macro is
// undefined, `defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE` is false on every platform,
// and the iOS guard below silently never fires - a gate that cannot be observed failing.
#include <TargetConditionals.h>
#endif
#include <windows.h>
// GeneralsX: io.h is Windows-specific, use unistd.h on Linux
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h> // access() for file existence checks
#include <SDL3/SDL.h> // For SDL_ShowWindow() on Linux
#endif
#include <time.h>
#include <vector>

// USER INCLUDES //////////////////////////////////////////////////////////////
#include "Common/FramePacer.h"
#include "Common/CommandLine.h"
#include "Common/FpsLimitPolicy.h"
#include "Common/GameEngine.h"
#include "Common/OptionPreferences.h"
#include "Common/ThingFactory.h"
#include "Common/GlobalData.h"
#include "Common/PerfTimer.h"
#include "Common/FileSystem.h"
#include "Common/LocalFileSystem.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/ThingTemplate.h"
#include "Common/GameLOD.h"
#include "Common/DrawModule.h"
#include "GameLogic/AIPathfind.h"

#include "GameClient/Drawable.h"
#include "GameClient/GameText.h"
#include "GameClient/GraphDraw.h"
#include "GameClient/Line2D.h"
#include "GameClient/Mouse.h"
#include "GameClient/GlobalLanguage.h"
#include "GameClient/Water.h"

#include "GameNetwork/NetworkInterface.h"
#include "Common/ModelState.h"
#include "Lib/BaseType.h"
#include "W3DDevice/Common/W3DConvert.h"
#include "W3DDevice/GameClient/W3DAssetManager.h"
#include "W3DDevice/GameClient/W3DGameClient.h"
#include "W3DDevice/GameClient/W3DFileSystem.h"
#include "W3DDevice/GameClient/W3DDynamicLight.h"
#include "W3DDevice/GameClient/W3DProfilerFrameCapture.h"
#include "W3DDevice/GameClient/HeightMap.h"
#include "W3DDevice/GameClient/WorldHeightMap.h"
#include "W3DDevice/GameClient/W3DScene.h"
#include "W3DDevice/GameClient/W3DTerrainTracks.h"
#include "W3DDevice/GameClient/W3DWater.h"
#include "W3DDevice/GameClient/W3DVideoBuffer.h"
#include "W3DDevice/GameClient/W3DShaderManager.h"
#include "W3DDevice/GameClient/W3DDebugDisplay.h"
#include "W3DDevice/GameClient/W3DProjectedShadow.h"
#include "W3DDevice/GameClient/W3DScreenshot.h"
#include "W3DDevice/GameClient/W3DShroud.h"
#include "WWMath/wwmath.h"
#include "WWLib/registry.h"
#include "WW3D2/ww3d.h"
#include "WW3D2/Renderer.h"
#include "WW3D2/predlod.h"
#include "WW3D2/part_emt.h"
#include "WW3D2/part_ldr.h"
#include "WW3D2/dx8caps.h"
#include "WW3D2/ww3dformat.h"
#include "WW3D2/agg_def.h"
#include "WW3D2/render2dsentence.h"
#include "WW3D2/sortingrenderer.h"
#include "WW3D2/textureloader.h"
#include "WW3D2/dx8webbrowser.h"
#include "WW3D2/mesh.h"
#include "WW3D2/hlod.h"
#include "WW3D2/meshmatdesc.h"
#include "WW3D2/meshmdl.h"
#include "WW3D2/rddesc.h"
#include "WWLib/TARGA.h"

#include "GameLogic/ScriptEngine.h"		// For TheScriptEngine - jkmcd
#include "GameLogic/GameLogic.h"
#ifdef DUMP_PERF_STATS
#include "GameLogic/PartitionManager.h"
#endif

#include "WinMain.h"


// DEFINE AND ENUMS ///////////////////////////////////////////////////////////

#define no_SAMPLE_DYNAMIC_LIGHT	1
#ifdef SAMPLE_DYNAMIC_LIGHT
static W3DDynamicLight * theDynamicLight = nullptr;
static Real theLightXOffset = 0.1f;
static Real theLightYOffset = 0.07f;
static Int theFlashCount = 0;
#endif

//*****************************************************************************************
//*****************************************************************************************
//**** Start Statistical Dump *************************************************************
//*****************************************************************************************

#ifdef DUMP_PERF_STATS

#include <cstdarg>

class StatDumpClass
{
public:
	StatDumpClass( const char *fname );
	~StatDumpClass();
	void dumpStats();

protected:
	FILE *m_fp;
};

//=============================================================================
//Open the file once at the beginning of the game -- everything appends to it.
//=============================================================================
StatDumpClass::StatDumpClass( const char *fname )
{
	char buffer[ _MAX_PATH ];
	GetModuleFileName( nullptr, buffer, sizeof( buffer ) );
	if (char *pEnd = strrchr(buffer, '\\'))
	{
		*pEnd = 0;
	}
	// TheSuperHackers @fix Caball009 03/06/2025 Don't use AsciiString here anymore because its memory allocator may not have been initialized yet.
	const std::string fullPath = std::string(buffer) + "\\" + fname;
	m_fp = fopen(fullPath.c_str(), "wt");
}

//=============================================================================
//Close the file at the end of the application
//=============================================================================
StatDumpClass::~StatDumpClass()
{
	if( m_fp )
	{
		fclose( m_fp );
	}
}

static const char *getCurrentTimeString()
{
	time_t aclock;
	time(&aclock);
	struct tm *newtime = localtime(&aclock);
	return asctime(newtime);
}

//=============================================================================
//Dump the stats
//=============================================================================
void StatDumpClass::dumpStats()
{
	if( !m_fp )
	{
		return;
	}

	//static char buf[1024];
	fprintf( m_fp, "----------------------------------------------------------------\n" );
	fprintf( m_fp, "Performance Statistical Dump -- Frame %d\n", TheGameLogic->getFrame() );
	fprintf( m_fp, "Time:\t%s", getCurrentTimeString() );
	fprintf( m_fp, "Map:\t%s\n", TheGlobalData->m_mapName.str());
	fprintf( m_fp, "Side:\t%s\n", ThePlayerList->getLocalPlayer()->getSide().str());
	fprintf( m_fp, "----------------------------------------------------------------\n" );

	//FPS
	Real fps = TheDisplay->getAverageFPS();
	fprintf( m_fp, "Average FPS: %.1f (%.5f msec)\n", fps, 1000.0f / fps );

	//Rendering stats
	fprintf( m_fp, "Draws: %d Skins: %d SortedPolys: %d SkinPolys: %d\n",(Int)Debug_Statistics::Get_Draw_Calls(),
		(Int)Debug_Statistics::Get_DX8_Skin_Renders(),
		(Int)Debug_Statistics::Get_Sorting_Polygons(), (Int)Debug_Statistics::Get_DX8_Skin_Polygons());

	//Object stats
	UnsignedInt objCount = TheGameLogic->getObjectCount();
	UnsignedInt objScreenCount = TheGameClient->getRenderedObjectCount();
	fprintf( m_fp, "Objects: %d in world (%d onscreen)\n", objCount, objScreenCount );

	//AI stats
	UnsignedInt numAI, numMoving, numAttacking, numWaitingForPath, overallFailedPathfinds;
	TheGameLogic->getAIMetricsStatistics( &numAI, &numMoving, &numAttacking, &numWaitingForPath, &overallFailedPathfinds );
	fprintf( m_fp, "\n" );
	fprintf( m_fp, "AI Statistics:\n" );
	fprintf( m_fp, "  Total AI Objects: %d\n", numAI );
	fprintf( m_fp, "    -moving: %d\n", numMoving );
	fprintf( m_fp, "    -attacking: %d\n", numAttacking );
	fprintf( m_fp, "    -waiting for path: %d\n", numWaitingForPath );
	fprintf( m_fp, "  Total failed pathfinds: %d\n", overallFailedPathfinds );
	fprintf( m_fp, "\n" );

	// Script stats
	Real timeLastFrame, slowScript1, slowScript2;
	AsciiString slowScripts = TheScriptEngine->getStats(&timeLastFrame, &slowScript1, &slowScript2);
	fprintf( m_fp, "\n" );
	fprintf( m_fp, "Script Engine Statistics:\n" );
	fprintf( m_fp, "  Total time last frame: %.5f msec\n", timeLastFrame*1000 );
	fprintf( m_fp, "    -Slowest 2 scripts %s\n", slowScripts.str() );
	fprintf( m_fp, "    -Slowest 2 script times %.5f msec, %.5f msec \n", slowScript1*1000, slowScript2*1000 );
	fprintf( m_fp, "\n" );



	//PartitionMgr stats
	double gcoTimeThisFrameTotal, gcoTimeThisFrameAvg;
	ThePartitionManager->getPMStats(gcoTimeThisFrameTotal, gcoTimeThisFrameAvg);
	fprintf(m_fp, "Partition Manager Statistics:\n");
	fprintf(m_fp, "  Total time for object scans this frame is %.5f msec\n", gcoTimeThisFrameTotal);
	fprintf(m_fp, "  Avg time per object scan this frame is %.5f msec\n", gcoTimeThisFrameAvg);
	fprintf( m_fp, "\n" );

	// setup texture stats
	Debug_Statistics::Record_Texture_Mode(Debug_Statistics::RECORD_TEXTURE_SIMPLE/*RECORD_TEXTURE_NONE*/);

	fprintf( m_fp, "Video Statistics:\n" );
	//Particle system stats
	fprintf( m_fp, "  Particle Systems: %d\n", TheParticleSystemManager->getParticleSystemCount() );
	Int totalParticles = TheParticleSystemManager->getParticleCount();
	Int onScreenParticleCount = TheParticleSystemManager->getOnScreenParticleCount();
	fprintf( m_fp, "  Particles: %d in world (%d onscreen)\n", totalParticles, onScreenParticleCount );

	// polygons this frame
	Int polyPerFrame = Debug_Statistics::Get_DX8_Polygons();
	Int polyPerSecond = (Int)(polyPerFrame * fps);
	fprintf( m_fp, "  Polygons: %d per frame (%d per second)\n", polyPerFrame, polyPerSecond );

	// vertices this frame
	fprintf( m_fp, "  Vertices: %d\n", Debug_Statistics::Get_DX8_Vertices() );

	//
	// I'm adjusting the texture memory usage counter by subtracting
	// out the terrain alpha texture (since it's really == terrain texture).
	//
	fprintf( m_fp, "  Video RAM: %d\n", Debug_Statistics::Get_Record_Texture_Size() - 1376256 );

	// terrain stats
	fprintf( m_fp, "  3-Way Blends: %d/%d, \n Shoreline Blends: %d/%d\n", TheTerrainRenderObject->getNumExtraBlendTiles(TRUE),TheTerrainRenderObject->getNumExtraBlendTiles(FALSE), TheTerrainRenderObject->getNumShoreLineTiles(TRUE),TheTerrainRenderObject->getNumShoreLineTiles(FALSE));

	fprintf( m_fp, "\n" );

#if defined(RTS_DEBUG)
	TheAudio->audioDebugDisplay( nullptr, nullptr, m_fp );
	fprintf( m_fp, "\n" );
#endif

#ifdef MEMORYPOOL_DEBUG
	//Report memory usage.
	TheMemoryPoolFactory->debugMemoryReport( REPORT_FACTORYINFO | REPORT_POOLINFO, 0, 0, m_fp );
#else
	fprintf( m_fp, "Memory Report -- unavailable (build doesn't have MEMORYPOOL_DEBUG defined)\n" );
#endif
	fprintf( m_fp, "\n" );

	fprintf( m_fp, "%s", TheSubsystemList->dumpTimesForAll().str());

	fprintf( m_fp, "----------------------------------------------------------------\n" );
	fprintf( m_fp, "END -- Frame %d\n", TheGameLogic->getFrame() );
	fprintf( m_fp, "----------------------------------------------------------------\n\n\n" );
	fflush(m_fp);
}

StatDumpClass TheStatDump("StatisticsDump.txt");

#endif //DUMP_PERF_STATS

//*****************************************************************************************
//**** End Statistical Dump ***************************************************************
//*****************************************************************************************
//*****************************************************************************************



///////////////////////////////////////////////////////////////////////////////
// DEFINITIONS ////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

//=============================================================================
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
RTS3DScene *W3DDisplay::m_3DScene = nullptr;
RTS2DScene *W3DDisplay::m_2DScene = nullptr;
RTS3DInterfaceScene *W3DDisplay::m_3DInterfaceScene = nullptr;
W3DAssetManager *W3DDisplay::m_assetManager = nullptr;
// GX-SIMID-END

//=============================================================================
	// note, can't use the ones from PerfTimer.h 'cuz they are currently
	// only valid when "-vtune" is used... (srj)
inline Int64 getPerformanceCounter()
{
	Int64 tmp;
#ifdef _WIN32
	QueryPerformanceCounter((LARGE_INTEGER*)&tmp);
#else
	// Linux: Use time_compat.h wrapper which calls clock_gettime(CLOCK_MONOTONIC)
	QueryPerformanceCounter(&tmp);
#endif
	return tmp;
}

inline Int64 getPerformanceCounterFrequency()
{
	Int64 tmp;
#ifdef _WIN32
	QueryPerformanceFrequency((LARGE_INTEGER*)&tmp);
#else
	// Linux: Use time_compat.h wrapper which returns 10 million (100-nanosecond intervals)
	QueryPerformanceFrequency(&tmp);
#endif
	return tmp;
}

// W3DDisplay::W3DDisplay =====================================================
/** */
//=============================================================================
W3DDisplay::W3DDisplay()
{
	Int i;

	m_initialized = false;
	m_assetManager = nullptr;
	m_3DScene = nullptr;
	m_2DScene = nullptr;
	m_3DInterfaceScene = nullptr;
	// GeneralsX: A seed only, replaced by the first updateAverageFPS(). The cap
	// can be 0 now (unlimited, -fps 0 / -noFPSLimit), and a 0 fps seed would read as a stalled peer.
	m_averageFPS = (TheGlobalData->m_framesPerSecondLimit > 0) ? TheGlobalData->m_framesPerSecondLimit : LOGICFRAMES_PER_SECOND;
#if defined(RTS_DEBUG)
	m_timerAtCumuFPSStart = 0;
#endif
	for (i=0; i<LightEnvironmentClass::MAX_LIGHTS; i++)
		m_myLight[i] = nullptr;
	m_2DRender = nullptr;
	m_isClippedEnabled = FALSE;
	m_clipRegion.lo.x = 0;
	m_clipRegion.lo.y = 0;
	m_clipRegion.hi.x = 0;
	m_clipRegion.hi.y = 0;

	for (i = 0; i < DisplayStringCount; i++)
		m_displayStrings[i] = nullptr;
	m_cinematicDisplayString = nullptr;

	m_batchTexture = nullptr;
	m_batchMode = DRAW_IMAGE_ALPHA;
	m_batchGrayscale = FALSE;
	m_batchNeedsInit = FALSE;

#ifdef PROFILER_ENABLED
	m_profilerFrameCapture = NEW W3DProfilerFrameCapture();
#endif
}

// W3DDisplay::~W3DDisplay ====================================================
/** */
//=============================================================================
W3DDisplay::~W3DDisplay()
{
#ifdef PROFILER_ENABLED
	delete m_profilerFrameCapture;
	m_profilerFrameCapture = nullptr;
#endif

	// get rid of the debug display
	delete m_debugDisplay;
	m_debugDisplay = nullptr;
	m_nativeDebugDisplay = nullptr;

	// delete the display strings
	for (int i = 0; i < DisplayStringCount; i++)
		TheDisplayStringManager->freeDisplayString(m_displayStrings[i]);

	// TheSuperHackers @fix Mauller/Tomsons26 28/04/2025 Free benchmark display string
	if( m_benchmarkDisplayString ) {
		TheDisplayStringManager->freeDisplayString(m_benchmarkDisplayString);
	}

	if( m_cinematicDisplayString ) {
		TheDisplayStringManager->freeDisplayString(m_cinematicDisplayString);
	}

	// delete 2D renderer
	if( m_2DRender )
	{

		m_2DRender->Reset();
		delete m_2DRender;
		m_2DRender = nullptr;

	}

	//
	// delete all our views now since they are W3D views and we need to
	// free them BEFORE we shutdown W3D
	//
	Display::deleteViews();

	REF_PTR_RELEASE( m_3DScene );
	REF_PTR_RELEASE( m_2DScene );
	REF_PTR_RELEASE( m_3DInterfaceScene );
	for (Int j=0; j<LightEnvironmentClass::MAX_LIGHTS; j++)
		REF_PTR_RELEASE( m_myLight[j] );

	PredictiveLODOptimizerClass::Free();

	// shutdown
	Debug_Statistics::Shutdown_Statistics();
	if (!TheGlobalData->m_headless)
		W3DShaderManager::shutdown();
	m_assetManager->Free_Assets();
	delete m_assetManager;
	if (!TheGlobalData->m_headless)
		WW3D::Shutdown();
	WWMath::Shutdown();
	if (!TheGlobalData->m_headless)
		DX8WebBrowser::Shutdown();
	delete TheW3DFileSystem;
	TheW3DFileSystem = nullptr;

}

// TheSuperHackers @tweak valeronm 20/03/2025 No longer filters resolutions by a 4:3 aspect ratio.
inline Bool isResolutionSupported(const ResolutionDescClass &res)
{
	static const Int minBitDepth = 24;

	return res.Width >= DEFAULT_DISPLAY_WIDTH && res.BitDepth >= minBitDepth;
}

// SDL3 display size providers for DX8Wrapper pillarbox (registered at init)
#ifdef SAGE_USE_SDL3
static bool SDL3_GetNativeDisplaySize(int& outW, int& outH, float& outDensity)
{
	extern SDL_Window* TheSDL3Window;
	if (!TheSDL3Window) return false;
	SDL_DisplayID displayId = SDL_GetDisplayForWindow(TheSDL3Window);
	const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(displayId);
	if (!mode || mode->w <= 0 || mode->h <= 0) return false;
	outDensity = mode->pixel_density > 0 ? mode->pixel_density : 1.0f;
	outW = (int)(mode->w * outDensity);
	outH = (int)(mode->h * outDensity);
	return true;
}

static bool SDL3_GetWindowSizeInPixels(int& outW, int& outH, float& outDensity)
{
	extern SDL_Window* TheSDL3Window;
	if (!TheSDL3Window) return false;
	int logW = 0, logH = 0, physW = 0, physH = 0;
	SDL_GetWindowSize(TheSDL3Window, &logW, &logH);
	SDL_GetWindowSizeInPixels(TheSDL3Window, &physW, &physH);
	if (physW <= 0 || physH <= 0) return false;
	outW = physW;
	outH = physH;
	outDensity = (logW > 0) ? (float)physW / (float)logW : 1.0f;
	return true;
}

// GeneralsX: Ensure SDL3 fullscreen transition actually lands in native fullscreen and foreground.
static void SDL3_EnsureNativeFullscreen(SDL_Window* window)
{
	if (!window) return;

	for (int attempt = 0; attempt < 3; ++attempt) {
		SDL_PumpEvents();
		if ((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0) {
			break;
		}
		if (!SDL_SetWindowFullscreen(window, true)) {
			fprintf(stderr, "WARNING: SDL_SetWindowFullscreen(retry) failed: %s\n", SDL_GetError());
			break;
		}
	}

	SDL_RaiseWindow(window);
}

// GeneralsX: ONE density, ONE direction, ONE definition of the
// points <-> render-pixels conversion. Ported from the GeneralsMD twin (d5afaed6f) with the unit
// contract made explicit, because the twin's version is only half a contract and that is a bug -
// see SDL3_ScaleRenderResolutionToPixels below and W3DDisplay::setDisplayMode.
//
// THE UNITS, stated once so no caller has to guess:
//   * POINTS  - what the player configures. Options.ini, TheGlobalData->m_x/yResolution, and the
//               offered mode list (Enumerate_Resolutions -> DXVK's SDL3 WSI -> SDL_DisplayMode.w/h,
//               which SDL fills in points; see the long note on requestAspectMatchesDisplay).
//   * PIXELS  - what is rendered. The D3D8/DXVK backbuffer, Render2DClass' screen resolution, and
//               Display::getWidth()/getHeight() once init() has scaled them.
//
// Returns the POINTS -> PIXELS factor. It is 1.0 anywhere the high-density drawable was not
// requested, which makes every user of it a no-op there by construction rather than by inspection:
// SDL_WINDOW_HIGH_PIXEL_DENSITY is set under "#if defined(__APPLE__)" in Code/Main/SDL3Main.cpp, so
// this guard is the same guard, and a platform that never multiplies can never be caught dividing.
static Real SDL3_GetRenderPixelDensity()
{
#if defined(__APPLE__) && !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
	int nativeW = 0, nativeH = 0;
	float density = 1.0f;
	if (DX8Wrapper::GetNativeDisplaySize(nativeW, nativeH, density) && density > 1.0f)
		return (Real)density;
#endif
	return 1.0f;
}

// GeneralsX: Convert a POINT resolution to the RENDER PIXEL resolution,
// in place. Every place that turns a player-chosen resolution into a render resolution must call
// this and nothing else, so the scaling cannot exist on one path and be missing on another - which
// is exactly the defect the twin still has: it scales in init() only, so an options-screen
// resolution change silently drops back to point size until the next restart.
//
// Both dimensions are rounded down to even. The twin masks the width alone with no stated reason;
// an unbounded search of Core, Generals, GeneralsMD and docs finds no even-dimension requirement
// anywhere, so the mask is either unnecessary (in which case applying it to both costs at most one
// pixel per axis) or it is a real alignment constraint (in which case masking one axis is a bug).
// Masking both is the only choice that is correct under either reading. At the densities macOS
// actually reports - 1.0 or exactly 2.0 - the mask is arithmetically dead in both cases; it only
// does anything if a fractional density ever appears.
static void SDL3_ScaleRenderResolutionToPixels(Int& xres, Int& yres)
{
	const Real density = SDL3_GetRenderPixelDensity();
	if (density <= 1.0f)
		return;

	const Int pxW = (Int)((Real)xres * density) & ~1;
	const Int pxH = (Int)((Real)yres * density) & ~1;

	fprintf(stderr, "INFO: Retina render scale: %dx%d points -> %dx%d pixels (density %.2f)\n",
		xres, yres, pxW, pxH, (double)density);

	xres = pxW;
	yres = pxH;
}

// PORTED VERBATIM from the GeneralsMD twin (0450933cc). The two files' SDL3 helper
// families are the same code; keeping the text identical is what makes a future diff
// between the twins mean something.
// GeneralsX: DOES ANY ATTACHED DISPLAY ACCEPT A FRAMEBUFFER THIS BIG?
//
// WHAT THIS REPLACES. SDL3_ClampRenderResolutionToDisplay, added earlier the same day and blocked
// by a cross-model review for measuring the wrong thing. Its ceiling was
// SDL_GetCurrentDisplayMode multiplied by pixel_density - that is what
// DX8Wrapper::GetNativeDisplaySize resolves to; see SDL3_GetNativeDisplaySize at the top of this
// block - so it capped at whatever mode the DESKTOP happens to be in and silently reduced valid
// requests. A 4K monitor running at 1080p imposed a 1080p ceiling.
//
// Measured on the machine this was written on, a 3024x1964 MacBook panel whose desktop mode is
// 1512x982 at density 2.0:
//
//     SDL_GetCurrentDisplayMode x density      ->  3024x1964   (the blocked version's ceiling)
//     max over SDL_GetFullscreenDisplayModes   ->  3600x2338   (this one's)
//
// The extra headroom is real, not an artifact: macOS "More Space" modes render into a framebuffer
// LARGER than the panel and scale down, so 1800x1169 points at density 2.0 is a mode the OS will
// actually drive. The blocked version reduced a 1800x1169 request on this laptop; this one
// honours it. Deliberate supersampling up to what the hardware will present therefore survives,
// which is the behaviour the review said was being removed.
//
// UNITS. The argument is POINTS - what Options.ini and TheGlobalData->m_x/yResolution hold. The
// comparison happens in PIXELS, because a mode's framebuffer and the backbuffer it would cost are
// both measured there, using the same density every other render-scaling site in this file uses.
//
// EXCEPT ON iOS, where the argument is already PIXELS - and the comparison is still right, by
// construction rather than by luck. SDL3Main injects -xres/-yres from SDL_GetWindowSizeInPixels
// there, which is the split requestAspectMatchesDisplay below spells out in full; and
// SDL3_GetRenderPixelDensity is guarded "__APPLE__ && !TARGET_OS_IPHONE", so it returns 1.0 on iOS
// and the multiplication below is the identity. A pixel request is therefore compared against a
// pixel framebuffer, which is the same comparison, not a coincidence. Worth stating because a
// comment that said "the argument is POINTS" and stopped there would be false on one of the three
// platforms this compiles for - and a half-true unit claim in a comment is what got the version
// this replaces blocked.
//
// EVERY ATTACHED DISPLAY, deliberately, and that is what makes this immune to the review's third
// finding. The window MOVES during a resolution change: DX8Wrapper::Resize_And_Position_Window
// calls ::SetWindowPos(_Hwnd, nullptr, 0, 0, width, height, SWP_NOZORDER) on the non-Windows
// windowed path, its guard is ::GetClientRect which is an empty stub in CompatLib (so the branch
// is taken every time), and the SetWindowPos shim runs SDL_SetWindowPosition(window, 0, 0)
// because SWP_NOMOVE is absent. A ceiling tied to "the display the window is on" is therefore
// read from a display the window is about to leave. Asking whether ANY display can present the
// request makes the answer independent of where the window is, so there is nothing left for a
// move to invalidate. It also errs in the safe direction: the cost of being too permissive is a
// slow frame, the cost of being too strict is silently overriding what the player chose.
//
// IT IS A THRESHOLD, NOT A TARGET. It answers only "nothing here can present this". WHERE to fall
// back to is chosen by the caller from the offered mode list, so the result is always a size the
// player could have picked out of the options combo box and the runtime globals never end up
// holding a size the adapter never enumerated. The one gap is buildFilteredResolutions' permissive
// retry, which drops the taller-than-native filter to avoid an empty list; a mode from that pass
// can exceed this ceiling, and the caller will then have snapped to it anyway. That is the correct
// trade - an empty resolution list is a worse bug - but it is a gap, not a guarantee.
//
// IT IS NOT A CORRECTNESS GUARD, and the blocked version's "a request the display cannot draw"
// was too strong. Read end to end, an oversized request neither fails nor renders wrong at the
// DXVK layer: DX8Wrapper::Pillarbox_Setup creates the offscreen target at the game resolution with
// D3DDevice->CreateTexture(gameW, gameH, 1, D3DUSAGE_RENDERTARGET, ...) and Pillarbox_End
// downsamples it onto the backbuffer. What it costs is real, though. That target is mip-less
// (Levels = 1) and the blit is a single D3DTEXF_LINEAR tap with no MIPFILTER, so past roughly 2x
// it aliases rather than antialiases: a 4x-oversized render buys four times the GPU cost and a
// WORSE picture. And 4x in both axes is not a size a player chose - it is the signature of a
// POINTS value that was written as PIXELS, which is exactly the launcher defect that produced the
// one this exists for.
//
// Returns true only when NO attached display can present the request. Returns false - honour it -
// whenever the evidence is missing: no displays, no modes, or SDL refusing to answer. A reduction
// made on no evidence is the failure mode this function exists to prevent.
// PARTIAL evidence is missing evidence, and that took a second cross-model round to get right. The
// first version skipped a display whose mode list came back empty and carried on with the others,
// so on a two-monitor machine where one enumeration fails it could still answer "nothing here can
// present this" - having never asked the monitor that might have. Any display that cannot be
// enumerated now makes the whole answer false.
//
// SILENT, deliberately. The log line belongs to the caller that ACTS on the answer, not here:
// findBestOfferedMode calls this once per candidate mode, so logging inside would print a line per
// rejected candidate on a path that is already an error path. The largest framebuffer found comes
// back through outBestW/outBestH for whoever wants to say it out loud.
static bool SDL3_RequestExceedsDisplayCapability(Int ptW, Int ptH,
	Int* outBestW = nullptr, Int* outBestH = nullptr)
{
	if (ptW <= 0 || ptH <= 0)
		return false;

	const Real density = SDL3_GetRenderPixelDensity();
	const Real reqPxW = (Real)ptW * density;
	const Real reqPxH = (Real)ptH * density;

	int displayCount = 0;
	SDL_DisplayID* displays = SDL_GetDisplays(&displayCount);
	if (!displays)
		return false;

	bool sawAnyMode = false;
	bool presentable = false;
	bool everyDisplayAnswered = true;
	Int bestW = 0, bestH = 0;	// largest framebuffer seen; for the log line only
	long bestArea = 0;

	for (int d = 0; d < displayCount && !presentable; ++d)
	{
		int modeCount = 0;
		SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(displays[d], &modeCount);
		if (!modes || modeCount <= 0)
		{
			// This display did not answer. It may well be the one that could have presented the
			// request, so the whole result is now unusable - see the note above the signature.
			everyDisplayAnswered = false;
			if (modes)
				SDL_free(modes);
			continue;
		}
		for (int m = 0; m < modeCount; ++m)
		{
			const SDL_DisplayMode* mode = modes[m];
			if (!mode || mode->w <= 0 || mode->h <= 0)
				continue;
			// A mode's w/h are POINTS and pixel_density is the multiplier to its framebuffer -
			// SDL_video.h says so ("a 1920x1080 mode with 2.0 scale would have 3840x2160 pixels")
			// and the Cocoa backend fills the density from
			// CGDisplayModeGetPixelWidth / CGDisplayModeGetWidth. EVERY entry has to be looked at:
			// SDL's cmpmodes sorts by point width descending with pixel_density as the LAST and
			// ASCENDING tiebreak, so modes[0] on a Retina Mac is the 1x mode, not the biggest
			// framebuffer.
			const Real modeDensity = (mode->pixel_density > 0.0f) ? (Real)mode->pixel_density : 1.0f;
			const Real fbW = (Real)mode->w * modeDensity;
			const Real fbH = (Real)mode->h * modeDensity;
			sawAnyMode = true;
			const long area = (long)fbW * (long)fbH;
			if (area > bestArea) { bestArea = area; bestW = (Int)fbW; bestH = (Int)fbH; }
			if (fbW >= reqPxW && fbH >= reqPxH) { presentable = true; break; }
		}
		SDL_free(modes);
	}
	SDL_free(displays);

	if (outBestW) *outBestW = bestW;
	if (outBestH) *outBestH = bestH;

	// Presentable, or the evidence is incomplete in ANY way: honour the request.
	return !presentable && sawAnyMode && everyDisplayAnswered;
}


// GeneralsX: Apply SDL3 window sizing/fullscreen only after the final render resolution is known.
// GeneralsX @info renderWidth/renderHeight are PIXELS. See SDL3_GetRenderPixelDensity for the units.
static void SDL3_ApplyWindowModeForRenderConfig(Bool windowed, Int renderWidth, Int renderHeight)
{
	extern SDL_Window* TheSDL3Window;
	if (!TheSDL3Window) return;

	if (!windowed) {
		if (!SDL_SetWindowFullscreen(TheSDL3Window, false)) {
			fprintf(stderr, "WARNING: SDL_SetWindowFullscreen(false) failed: %s\n", SDL_GetError());
		}

		SDL_DisplayID displayId = SDL_GetDisplayForWindow(TheSDL3Window);
		const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(displayId);
		if (mode) {
			if (!SDL_SetWindowFullscreenMode(TheSDL3Window, mode)) {
				fprintf(stderr, "WARNING: SDL_SetWindowFullscreenMode(native) failed: %s\n", SDL_GetError());
			}
		}
		else {
			fprintf(stderr, "WARNING: SDL_GetCurrentDisplayMode failed for fullscreen transition\n");
		}
	}
	else {
		// GeneralsX: SDL_SetWindowSize takes POINTS; renderWidth/Height are
		// PIXELS. Ported from the GeneralsMD twin (d5afaed6f). The two are the same number only at
		// density 1.0, which is why this was invisible until a Retina Mac ran it: with
		// SDL_WINDOW_HIGH_PIXEL_DENSITY now set on macOS, asking for a 2880-point window on a 2x
		// display would demand 5760 physical pixels and throw most of the window off the screen.
		// Convert back to points so the window is the size the player actually chose while the
		// drawable underneath it stays native.
		//
		// The divisor is SDL3_GetRenderPixelDensity, NOT SDL_GetWindowPixelDensity as in the twin,
		// so that this division is the exact inverse of SDL3_ScaleRenderResolutionToPixels'
		// multiplication - same source, same platform guard. The two queries answer different
		// questions (the window's realised density vs the display's) and can disagree: a window
		// created without SDL_WINDOW_HIGH_PIXEL_DENSITY reports 1.0 from the first while the second
		// still reports the panel's 2.0. So under the twin's version the divide is keyed to the
		// window flag while the multiply is keyed to __APPLE__, and any build where those two stop
		// agreeing divides a size that was never multiplied and halves the window.
		//
		// Exactly a no-op where density is 1.0 - the arithmetic returns the input unchanged - so
		// non-Retina machines are untouched.
		const Real winDensity = SDL3_GetRenderPixelDensity();
		const Int winPtW = (Int)((Real)renderWidth  / winDensity);
		const Int winPtH = (Int)((Real)renderHeight / winDensity);

		if (!SDL_SetWindowSize(TheSDL3Window, winPtW, winPtH)) {
			fprintf(stderr, "WARNING: SDL_SetWindowSize(%d,%d) failed: %s\n", winPtW, winPtH, SDL_GetError());
		}

		// GeneralsX: RE-CENTRE AFTER THE RESIZE, or the window hangs off
		// the right of the display. Same defect and same fix as the Zero Hour twin - see the long
		// note in GeneralsMD/.../W3DDisplay.cpp. SDL centres the window for the placeholder
		// 1024x768 it is created at, and growing it here keeps the left edge, so a 1600-wide window
		// on a 1920 display starts at x=448 and ends 128 px past the screen.
		// NOT ON iOS. A UIWindow is the whole screen and has no position to set; SDL_SetWindowPosition
		// against one is meaningless at best. This branch is already unreachable there - m_windowed
		// defaults to FALSE (Display.cpp:50) and is only ever set TRUE by -win, which the iPad cannot
		// pass because it has no argv (SDL3Main injects only -xres/-yres/-autoload; unbounded search
		// for "-win" finds only WorldBuilder and the parse table). The guard is here anyway because
		// that reachability argument lives in a different file from this call, and the platform it
		// would regress is a live one.
#if !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
		SDL_SetWindowPosition(TheSDL3Window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
#endif
	}

	if (!windowed) {
		if (!SDL_SetWindowFullscreen(TheSDL3Window, true)) {
			fprintf(stderr, "WARNING: SDL_SetWindowFullscreen(true) failed: %s\n", SDL_GetError());
		}
		SDL3_EnsureNativeFullscreen(TheSDL3Window);
	}
}
#endif

// Filtered resolution cache — clamps widths to 4:3..16:9 and deduplicates.
struct FilteredRes { Int w, h, bits; };
static std::vector<FilteredRes> s_filteredResolutions;

// GeneralsX: Mirror of the Zero Hour change; see the long comments in
// GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp. The list used to be
// built once and never invalidated, which only became a bug once it started depending on the
// display and the windowed flag, so it now carries the inputs it was built from.
static bool s_filteredValid    = false;
static Bool s_filteredWindowed = FALSE;
static Int  s_filteredNativeW  = 0;
static Int  s_filteredNativeH  = 0;

// True only when the fullscreen path cannot mode-set and is therefore pinned to the display's
// aspect. The list filter and the startup migration must answer this identically.
static bool fullscreenAspectIsLocked(Bool windowed)
{
#ifdef _WIN32
	(void)windowed;
	return false;
#else
	if (windowed)
		return false;
	int nativeW = 0, nativeH = 0;
	float density = 1.0f;
	DX8Wrapper::GetNativeDisplaySize(nativeW, nativeH, density);
	return nativeW > 0 && nativeH > 0;
#endif
}

// GeneralsX: A request that ALREADY has the display's aspect must not be
// migrated. The startup migration exists to avoid pillarbox BARS, and bars require an aspect
// MISMATCH - Pillarbox_Setup fits the game rect inside the backbuffer preserving aspect, so equal
// aspects give an exact fit with no bars at ANY scale. Migrating such a request therefore buys
// nothing, and on iPadOS it costs half the resolution.
//
// The two sides are in DIFFERENT UNITS there, which is why the request looks "not offered":
//   * the OFFERED list is in POINTS. SDL's UIKit backend fills SDL_DisplayMode.w/h from
//     GetUIScreenModeSize, whose own comment reads "What we want for the mode size is the point
//     size, and the pixel density is the native scale" (SDL_uikitmodes.m), and DXVK's SDL3 WSI
//     copies mode.w/h verbatim into the D3D8 adapter mode list (convertMode,
//     wsi_platform_sdl3.h) - which is the list BOTH Enumerate_Resolutions and Find_Color_Mode
//     walk.
//   * the REQUEST is in PIXELS. SDL3Main injects -xres/-yres from SDL_GetWindowSizeInPixels, and
//     SDL3_GetNativeDisplaySize multiplies the mode by pixel_density.
//
// So on a 2x iPad no pixel-sized request can ever appear in the list, every launch takes the "not
// offered" path, and the fallback (tallest offered mode when none reaches the requested height)
// snaps e.g. 2388x1668 down to 1194x834 - exactly undoing the SDL_WINDOW_HIGH_PIXEL_DENSITY that
// SDL3Main sets to stop the game rendering at point size. Density cancels in a ratio, so the
// aspect test below is unaffected by the unit split and needs no unit conversion of its own.
//
// The size floor keeps the rescue for a corrupt or hand-edited Options.ini: a silly but
// proportional value must still be migrated to something drawable rather than honoured.
static bool requestAspectMatchesDisplay(Int xres, Int yres)
{
	if (xres < DEFAULT_DISPLAY_WIDTH || yres < DEFAULT_DISPLAY_HEIGHT)
		return false;

	int nativeW = 0, nativeH = 0;
	float density = 1.0f;
	if (!DX8Wrapper::GetNativeDisplaySize(nativeW, nativeH, density))
		return false;
	if (nativeW <= 0 || nativeH <= 0)
		return false;

	const float reqAspect    = (float)xres / (float)yres;
	const float nativeAspect = (float)nativeW / (float)nativeH;
	return fabsf(reqAspect - nativeAspect) <= 0.01f;	// same tolerance as the offer filter
}

// PORTED VERBATIM from the GeneralsMD twin (0450933cc). The two files' SDL3 helper
// families are the same code; keeping the text identical is what makes a future diff
// between the twins mean something.
// GeneralsX @tooling /09/2026 The "closest offered mode" search, lifted out of
// W3DDisplay::init's startup migration so that migration and the runtime resolution-change path
// share ONE implementation instead of two copies that can drift. The selection rule is unchanged
// from the migration it came from: prefer the SMALLEST offered height that still reaches the
// requested one - never silently demote the player to a blurrier mode than they asked for - and
// fall back to the TALLEST offered mode when nothing reaches it, which is the case an oversized
// request always takes.
//
// It reads the same list the options combo box is built from (getDisplayModeCount /
// getDisplayModeDescription -> buildFilteredResolutions), so anything it returns is a size the
// player could have selected themselves. That property is what lets the caller write the result
// into TheGlobalData->m_x/yResolution without manufacturing a phantom combo entry.
//
// POINTS in, POINTS out: the offered list is in points. See SDL3_GetRenderPixelDensity.
//
// GeneralsX: A candidate is rejected if IT is unpresentable too, which a
// cross-model review caught this function doing on its first day.
//
// The selection is in POINTS and the caller then multiplies by the render density, so "an offered
// mode" is not by itself a guarantee that the result fits. The height is safe by construction -
// buildFilteredResolutions drops anything taller than the current mode in points, so height x
// density can never exceed the current framebuffer - but the WIDTH is bounded only by the legacy
// 4:3..16:9 sanity range on the windowed/Windows path, where the display-aspect filter is off. So
// on a display whose current mode is 1440x900 at 2x, a 1600x900 mode is offered, survives every
// filter, wins the "same height, prefer wider" tie-break, and comes out of here as a 3200x1800
// request against a 2880x1800 ceiling: the fallback exceeding the limit it was chosen to respect.
//
// Two passes rather than one filter, matching buildFilteredResolutions' own idiom, because an
// empty answer here is worse than an imperfect one: if NOTHING presentable is offered, the second
// pass returns the old unfiltered choice instead of returning false and leaving the caller with
// the oversized request it was trying to correct.
static bool findBestOfferedMode(W3DDisplay* display, Int wantW, Int wantH, Int& outW, Int& outH)
{
	if (display == nullptr)
		return false;

	const Int modeCount = display->getDisplayModeCount();

	const bool passes[2] = { true, false };
	for (int pass = 0; pass < 2; ++pass)
	{
		const bool skipUnpresentable = passes[pass];
		Int bestX = 0, bestY = 0;

		for (Int i = 0; i < modeCount; ++i)
		{
			Int mx = 0, my = 0, mb = 0;
			display->getDisplayModeDescription(i, &mx, &my, &mb);
#ifdef SAGE_USE_SDL3
			if (skipUnpresentable && SDL3_RequestExceedsDisplayCapability(mx, my))
				continue;	// never migrate TO something nothing attached can present
#else
			(void)skipUnpresentable;
#endif
			if (bestY == 0)
			{
				bestX = mx; bestY = my;
				continue;
			}
			// Prefer the smallest height that is still >= the requested one; fall back
			// to the tallest available when every mode is shorter than requested.
			const bool bestReaches = (bestY >= wantH);
			const bool candReaches = (my >= wantH);
			bool better = false;
			if (candReaches && !bestReaches)              better = true;
			else if (candReaches && bestReaches)          better = (my < bestY) || (my == bestY && mx > bestX);
			else if (!candReaches && !bestReaches)        better = (my > bestY) || (my == bestY && mx > bestX);
			if (better) { bestX = mx; bestY = my; }
		}

		if (bestY > 0 && bestX > 0)
		{
			(void)wantW;	// width follows the chosen mode; only height drives the selection
			outW = bestX;
			outH = bestY;
			return true;
		}
#ifndef SAGE_USE_SDL3
		break;	// the two passes are identical without the capability query; do not walk it twice
#endif
	}

	return false;
}

static bool filteredResolutionsStale(Bool windowed)
{
	int nativeW = 0, nativeH = 0;
	float density = 1.0f;
	DX8Wrapper::GetNativeDisplaySize(nativeW, nativeH, density);
	return !s_filteredValid
		|| s_filteredWindowed != windowed
		|| s_filteredNativeW  != nativeW
		|| s_filteredNativeH  != nativeH;
}

static void buildFilteredResolutions(Bool windowed)
{
	s_filteredResolutions.clear();
	const RenderDeviceDescClass &devDesc = WW3D::Get_Render_Device_Desc(0);
	const DynamicVectorClass<ResolutionDescClass> &resolutions = devDesc.Enumerate_Resolutions();

	int nativeW = 0, nativeH = 0;
	float density = 1.0f;
	DX8Wrapper::GetNativeDisplaySize(nativeW, nativeH, density);

	// Fullscreen on a platform that cannot mode-set always presents into the desktop-sized
	// backbuffer, so a mode whose aspect differs from the display can only ever mean black bars.
	// Windows is excluded because it performs a real exclusive mode set; windowed is excluded
	// because the window is sized to the game resolution and cannot bar.
	const bool applyAspectFilter = fullscreenAspectIsLocked(windowed);
	const float nativeAspect = (nativeH > 0) ? ((float)nativeW / (float)nativeH) : 0.0f;

	// The width clamp used to FABRICATE resolutions (a real 1280x1024 was offered as 1365x1024,
	// which Find_Color_Mode can never match because it walks the raw EnumAdapterModes list). Drop
	// out-of-range modes instead of inventing them, and fall back to a permissive pass if the
	// strict one empties the list - an empty resolution combo box would be worse than the original
	// bug. See the Zero Hour twin for the full reasoning.
	const bool passes[2] = { true, false };
	for (int pass = 0; pass < 2; ++pass) {
		const bool strict = passes[pass];
		if (!strict && !s_filteredResolutions.empty()) break;

		for (int i = 0; i < resolutions.Count(); i++) {
			if (!isResolutionSupported(resolutions[i])) continue;
			Int w = resolutions[i].Width;
			Int h = resolutions[i].Height;
			Int bits = resolutions[i].BitDepth;
			if (nativeH > 0 && h > nativeH) continue;
			if (strict) {
				if (applyAspectFilter) {
					// The display's own aspect is the allowed range; the legacy 4:3..16:9 bounds
					// predate ultrawide and would reject every native-aspect mode on 21:9 and 5:4.
					const float modeAspect = (float)w / (float)h;
					if (fabsf(modeAspect - nativeAspect) > 0.01f) continue;
				}
				else {
					const Int minW = h * 4 / 3;
					const Int maxW = h * 16 / 9;
					if (w < minW || w > maxW) continue;
				}
			}
			bool duplicate = false;
			for (const auto& e : s_filteredResolutions) {
				if (e.w == w && e.h == h && e.bits == bits) { duplicate = true; break; }
			}
			if (!duplicate) s_filteredResolutions.push_back({w, h, bits});
		}
	}
	s_filteredWindowed = windowed;
	s_filteredNativeW  = nativeW;
	s_filteredNativeH  = nativeH;
	s_filteredValid    = true;
}

Int W3DDisplay::getDisplayModeCount()
{
	if (filteredResolutionsStale(getWindowed())) buildFilteredResolutions(getWindowed());
	return (Int)s_filteredResolutions.size();
}

void W3DDisplay::getDisplayModeDescription(Int modeIndex, Int *xres, Int *yres, Int *bitDepth)
{
	if (filteredResolutionsStale(getWindowed())) buildFilteredResolutions(getWindowed());
	if (modeIndex >= 0 && modeIndex < (Int)s_filteredResolutions.size()) {
		*xres = s_filteredResolutions[modeIndex].w;
		*yres = s_filteredResolutions[modeIndex].h;
		*bitDepth = s_filteredResolutions[modeIndex].bits;
	}
}

void W3DDisplay::setGamma(Real gamma, Real bright, Real contrast, Bool calibrate)
{
	if (m_windowed)
		return;	//we don't allow gamma to change in window because it would affect desktop.

	Renderer::Set_Gamma(gamma,bright,contrast,calibrate, false);
}

/** Set resolution of display */
//=============================================================================
Bool W3DDisplay::setDisplayMode( UnsignedInt xres, UnsignedInt yres, UnsignedInt bitdepth, Bool windowed )
{
	const UnsignedInt oldWidth = getWidth();
	const UnsignedInt oldHeight = getHeight();
	const UnsignedInt oldBitDepth = getBitDepth();
	const Bool oldWindowed = getWindowed();

	// GeneralsX: xres/yres arrive in POINTS and everything below this line
	// wants PIXELS. Without this the Retina scaling that W3DDisplay::init() applies exists on the
	// startup path only, and the first options-screen resolution change on a 2x Mac undoes it:
	//
	//   * SDL3_ApplyWindowModeForRenderConfig divides by the density, so a point value passed here
	//     comes out half size and the window shrinks to half what the player picked;
	//   * Set_Device_Resolution builds a point-sized backbuffer into a pixel-sized drawable, so the
	//     render resolution silently reverts to point size and stays there until restart;
	//   * Display::setDisplayMode rescales the tactical view by new/old, and getWidth() is still in
	//     pixels from init(), so the view is scaled by 1/density on top of that.
	//
	// Only the render side becomes pixels, exactly as in init(). A no-op at density 1.0.
	//
	// GeneralsX: The EFFECTIVE POINT size is published here, not assumed
	// by the caller. Ported from the GeneralsMD twin (0450933cc).
	//
	//
	//
	//
	//
	// The options combo box then
	// preselected a resolution that was not running, the confirm dialog named it, and the next
	// launch read the same oversized value back off disk - so the correction had to be made again
	// every single time and was never visible anywhere the player could see it.
	//
	// So the engine now owns the answer: on success the globals carry the POINT size that is
	// actually running, and OptionsMenu / MainMenu read it back instead of assuming their request
	// survived. Unconditional and not inside any platform guard, because when nothing is reduced
	// this writes exactly the value the caller was about to write itself - byte for byte the old
	// behaviour on Windows, Linux and every non-Retina display. Still no Options.ini write from
	// here: persisting it stays the options screen's job, which is the rule the startup migration
	// in init() already follows.
	Int pointW = (Int)xres;
	Int pointH = (Int)yres;
	#ifdef SAGE_USE_SDL3
	// POINTS in, POINTS out, and only when NO attached display can present the request. Snapping to
	// the offered mode list - the same list the options combo box is built from - is what keeps the
	// value written below selectable rather than a phantom row. See
	// SDL3_RequestExceedsDisplayCapability and findBestOfferedMode.
	Int capW = 0, capH = 0;
	if (SDL3_RequestExceedsDisplayCapability(pointW, pointH, &capW, &capH))
	{
		const Real density = SDL3_GetRenderPixelDensity();
		fprintf(stderr,
			"INFO: %dx%d points wants a %dx%d pixel render (density %.2f); the largest framebuffer "
			"any attached display offers is %dx%d\n",
			pointW, pointH, (Int)((Real)pointW * density), (Int)((Real)pointH * density),
			(double)density, capW, capH);
		Int bestX = 0, bestY = 0;
		if (findBestOfferedMode(this, pointW, pointH, bestX, bestY))
		{
			fprintf(stderr,
				"INFO: resolution %dx%d cannot be presented by any attached display; using %dx%d\n",
				pointW, pointH, bestX, bestY);
			pointW = bestX;
			pointH = bestY;
		}
	}
	#endif

	Int renderW = pointW;
	Int renderH = pointH;
	#ifdef SAGE_USE_SDL3
	SDL3_ScaleRenderResolutionToPixels(renderW, renderH);
	#endif

	if (WW3D_ERROR_OK == WW3D::Set_Device_Resolution(renderW,renderH,bitdepth,windowed,true))
	{
		#ifdef SAGE_USE_SDL3
		SDL3_ApplyWindowModeForRenderConfig(windowed, renderW, renderH);
		#endif
		Render2DClass::Set_Screen_Resolution(RectClass(0, 0, renderW, renderH));
		Display::setDisplayMode(renderW, renderH, bitdepth, windowed);
		TheWritableGlobalData->m_xResolution = pointW;
		TheWritableGlobalData->m_yResolution = pointH;
		return TRUE;
	}

	//set back to the original mode.
	// GeneralsX @info oldWidth/oldHeight come from getWidth()/getHeight(), which are already PIXELS.
	// Do NOT scale them - they have been through SDL3_ScaleRenderResolutionToPixels once already.
	WW3D::Set_Device_Resolution(oldWidth, oldHeight, oldBitDepth, oldWindowed, true);
	Render2DClass::Set_Screen_Resolution(RectClass(0, 0, oldWidth, oldHeight));
	Display::setDisplayMode(oldWidth, oldHeight, oldBitDepth, oldWindowed);
	return FALSE;	//did not change to a new mode.
}

Bool W3DDisplay::getViewportRect( Int& x, Int& y, Int& width, Int& height ) const
{
	return DX8Wrapper::Pillarbox_Get_Rect(x, y, width, height) ? TRUE : FALSE;
}

// GeneralsX: The game window's size in logical points and in pixels, for
// game code that must not call SDL itself (the [GXPICK] probe in CommandXlat.cpp).
Bool W3DDisplay::getWindowSize( Int& width, Int& height, Int& pixelWidth, Int& pixelHeight ) const
{
#ifdef SAGE_USE_SDL3
	extern SDL_Window* TheSDL3Window;
	int w = 0, h = 0, pw = 0, ph = 0;
	if (TheSDL3Window && SDL_GetWindowSize(TheSDL3Window, &w, &h) && SDL_GetWindowSizeInPixels(TheSDL3Window, &pw, &ph))
	{
		width = w;
		height = h;
		pixelWidth = pw;
		pixelHeight = ph;
		return TRUE;
	}
#endif
	return FALSE;
}

/** Set width of display */
//=============================================================================
void W3DDisplay::setWidth( UnsignedInt width )
{

	// extending functionality
	Display::setWidth( width );

	// our 2D renderer will use mapping coords to make (0,0) the upper left
	// of the screen with (width,height) at the lower right
	m_2DRender->Set_Coordinate_Range( RectClass( 0, 0, getWidth(), getHeight() ) );

}

// W3DDisplay::setHeight ======================================================
/** Set height of display */
//=============================================================================
void W3DDisplay::setHeight( UnsignedInt height )
{

	// extending functionality
	Display::setHeight( height );

	// our 2D renderer will use mapping coords to make (0,0) the upper left
	// of the screen with (width,height) at the lower right
	m_2DRender->Set_Coordinate_Range( RectClass( 0, 0, getWidth(), getHeight() ) );

}

void W3DDisplay::onBeginBatch()
{
	m_batchTexture = nullptr;
	m_batchMode = DRAW_IMAGE_ALPHA;
	m_batchGrayscale = FALSE;
	m_batchNeedsInit = TRUE;

	if (m_2DRender)
	{
		m_2DRender->Reset();
	}
}

void W3DDisplay::onEndBatch()
{
	REF_PTR_RELEASE(m_batchTexture);
}

void W3DDisplay::onFlush()
{
	if (m_2DRender && !m_batchNeedsInit)
	{
		m_2DRender->Render();
		m_2DRender->Reset();
		m_batchNeedsInit = TRUE;
	}
}

void W3DDisplay::setup2DRenderState(TextureClass *tex, DrawImageMode mode, Bool grayscale, Int vertexCount, Int indexCount)
{
	if (m_isBatching)
	{
		// TheSuperHackers @fix arcticdolphin 12/10/2026 Flush when the upcoming draw would exceed the renderer capacity.
		const Bool hasRoom = !m_2DRender || m_2DRender->Can_Add(vertexCount, indexCount);

		if (!m_batchNeedsInit && hasRoom && m_batchTexture == tex && m_batchMode == mode && m_batchGrayscale == grayscale)
		{
			return;
		}

		onFlush();

		if (tex != m_batchTexture)
		{
			if (tex)
			{
				tex->Add_Ref();
			}
			if (m_batchTexture)
			{
				m_batchTexture->Release_Ref();
			}

			m_batchTexture = tex;
		}

		m_batchMode = mode;
		m_batchGrayscale = grayscale;
		m_batchNeedsInit = FALSE;
	}
	else if (m_2DRender)
	{
		m_2DRender->Reset();
	}

	if (m_2DRender)
	{
		if (tex)
		{
			m_2DRender->Enable_Texturing(TRUE);
			m_2DRender->Set_Texture(tex);
		}
		else
		{
			m_2DRender->Enable_Texturing(FALSE);
		}

		switch (mode)
		{
			default:
			case DRAW_IMAGE_ALPHA:
				m_2DRender->Enable_Additive(FALSE);
				m_2DRender->Enable_Alpha(TRUE);
				m_2DRender->Enable_Grayscale(grayscale);
				break;
			case DRAW_IMAGE_GRAYSCALE:
				m_2DRender->Enable_Additive(FALSE);
				m_2DRender->Enable_Alpha(TRUE);
				m_2DRender->Enable_Grayscale(TRUE);
				break;
			case DRAW_IMAGE_ADDITIVE:
				m_2DRender->Enable_Additive(TRUE);
				m_2DRender->Enable_Alpha(FALSE);
				m_2DRender->Enable_Grayscale(grayscale);
				break;
			case DRAW_IMAGE_SOLID:
				m_2DRender->Enable_Additive(FALSE);
				m_2DRender->Enable_Alpha(FALSE);
				m_2DRender->Enable_Grayscale(grayscale);
				break;
		}
	}
}

// W3DDisplay::initAssets =====================================================
/** */
//=============================================================================
void W3DDisplay::initAssets()
{

}

// W3DDisplay::init3DScene ====================================================
/** */
//=============================================================================
void W3DDisplay::init3DScene()
{

}

// W3DDisplay::init2DScene ====================================================
/** This is the 2D scene, you can use it to draw on a 2D plane over the
	* 3D background */
//=============================================================================
void W3DDisplay::init2DScene()
{

}

#if defined(__APPLE__) && !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
// GeneralsX: Twin of GeneralsMD's W3DDisplay.cpp: macOS presents with vsync for any
// max-FPS cap, the uncapped sentinel counting as none, on a panel faster than the tick rate. The reasoning is
// recorded there.
static bool gxMacVSyncFor(Int cap, Int refreshHz)
{
	return cap > 0 && cap < (Int)RenderFpsPreset::UncappedFpsValue && refreshHz > (Int)LOGICFRAMES_PER_SECOND;
}
#endif

// W3DDisplay::init ===========================================================
/** Initialize or re-initialize the W3D display system.  Here we need to
  * create our window, and get our 3D hardware setup and online */
//=============================================================================
void W3DDisplay::init()
{

	//
	// call our base class init, this method should be able to handle re-entry
	// with its own logic
	//
	Display::init();

	// handle re-entry for ourselves
	if( m_initialized )
	{

		/// @todo W3DDisplay needs RE-init logic!
		return;

	}
	// Override the W3D File system
	TheW3DFileSystem = NEW W3DFileSystem;

	// init the Westwood math library
	WWMath::Init();

	if (!TheGlobalData->m_headless)
	{

		// create our 3D interface scene
		m_3DInterfaceScene = NEW_REF( RTS3DInterfaceScene, () );
		m_3DInterfaceScene->Set_Ambient_Light( Vector3( 1, 1, 1 ) );

		// create our 2D scene
		m_2DScene = NEW_REF( RTS2DScene, () );
		m_2DScene->Set_Ambient_Light( Vector3( 1, 1, 1 ) );

		// create our 3D scene
		m_3DScene =NEW_REF( RTS3DScene, () );
	#if defined(RTS_DEBUG)
		if( TheGlobalData->m_wireframe )
			m_3DScene->Set_Polygon_Mode( SceneClass::LINE );
	#endif
	//============================================================================
		// m_myLight = NEW_REF
	//============================================================================
		Int lindex;
		for (lindex=0; lindex<TheGlobalData->m_numGlobalLights; lindex++)
		{	m_myLight[lindex] = NEW_REF( LightClass, (LightClass::DIRECTIONAL) );
		}

		setTimeOfDay( TheGlobalData->m_timeOfDay );	//set each light to correct values for given time

		for (lindex=0; lindex<TheGlobalData->m_numGlobalLights; lindex++)
		{	m_3DScene->setGlobalLight( m_myLight[lindex], lindex );
		}

	#ifdef SAMPLE_DYNAMIC_LIGHT
		theDynamicLight = NEW_REF(W3DDynamicLight, ());
		Real red = 1;
		Real green = 1;
		Real blue = 0;
		if(red==0 && blue==0 && green==0) {
			red = green = blue = 1;
		}
		theDynamicLight->Set_Ambient( Vector3( red, green, blue ) );
		theDynamicLight->Set_Diffuse( Vector3( red, green, blue) );
		theDynamicLight->Set_Position(Vector3(0, 0, 4));
		theDynamicLight->Set_Far_Attenuation_Range(1, 8);
		// Note: Don't Add_Render_Object dynamic lights.
		m_3DScene->addDynamicLight( theDynamicLight );
	#endif

	}

	// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake (pristine bones load models through it)
	// create a new asset manager
	m_assetManager = NEW W3DAssetManager;
	m_assetManager->Register_Prototype_Loader(&_ParticleEmitterLoader );
	m_assetManager->Register_Prototype_Loader(&_AggregateLoader);
	m_assetManager->Set_WW3D_Load_On_Demand( true );
	// GX-SIMID-END

	if (!TheGlobalData->m_headless)
	{

		if (TheGlobalData->m_incrementalAGPBuf)
		{
			SortingRendererClass::SetMinVertexBufferSize(1);
		}
		// Register SDL3 display size providers for pillarbox before device init
#ifdef SAGE_USE_SDL3
		DX8Wrapper::Set_Display_Size_Provider(SDL3_GetNativeDisplaySize, SDL3_GetWindowSizeInPixels);
#endif

		// GeneralsX: Twin of GeneralsMD's W3DDisplay::init: vsync on macOS whenever the
		// max-FPS cap is set, chosen before the device exists with GameEngine::init's own resolveCap expression.
#if defined(__APPLE__) && !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
		{
			const Bool fromArgv = CommandLine::hasFpsLimitArgument();
			const Bool legacyUnlimited = !fromArgv && OptionPreferences().getLegacyFPSLimitOff();
			const Int cap = FpsLimitPolicy::resolveCap(fromArgv || legacyUnlimited,
				legacyUnlimited ? 0 : TheGlobalData->m_framesPerSecondLimit, 0, LOGICFRAMES_PER_SECOND);
			const Int refreshHz = TheGameEngine != nullptr ? TheGameEngine->getDisplayRefreshRate() : 0;
			DX8Wrapper::Set_Windowed_VSync(gxMacVSyncFor(cap, refreshHz));
			fprintf(stderr, "[GXVSYNC] %s (max-FPS cap %s, display %d Hz, tick %d, at start)\n",
			        gxMacVSyncFor(cap, refreshHz) ? "on" : "off", cap > 0 ? "set" : "unlimited", refreshHz, (Int)LOGICFRAMES_PER_SECOND);
			fflush(stderr);
		}
#endif

		if (WW3D::Init( ApplicationHWnd ) != WW3D_ERROR_OK)
			throw ERROR_INVALID_D3D;	//failed to initialize.  User probably doesn't have DX 8.1

		// GeneralsX @bugfix felipebraz 16/02/2026 Show window after DirectX8/DXVK initialized
		#ifndef _WIN32
		extern SDL_Window* TheSDL3Window;
		if (TheSDL3Window) {
			fprintf(stderr, "DEBUG: Showing SDL3 window after WW3D init...\n");
			SDL_ShowWindow(TheSDL3Window);
		}
		#endif

		WW3D::Set_Prelit_Mode( WW3D::PRELIT_MODE_LIGHTMAP_MULTI_PASS );
		WW3D::Set_Collision_Box_Display_Mask(0x00);	///<set to 0xff to make collision boxes visible
		WW3D::Enable_Static_Sort_Lists(true);
		WW3D::Set_Thumbnail_Enabled(false);
		WW3D::Set_Screen_UV_Bias( TRUE );  ///< this makes text look good :)

		setWindowed( TheGlobalData->m_windowed );

		// create a 2D renderer helper
		m_2DRender = NEW Render2DClass;
		DEBUG_ASSERTCRASH( m_2DRender, ("Cannot create Render2DClass") );

		WW3DErrorType renderDeviceError;
		Int attempt = 0;
		do
		{
			switch (attempt)
			{
			case 0:
			{
				Int xres = TheGlobalData->m_xResolution;
				Int yres = TheGlobalData->m_yResolution;

				// GeneralsX: Mirror of the Zero Hour change. A resolution
				// stored before the fullscreen aspect filter existed can now be absent from the
				// offered list; left alone it renders the wrong aspect into the fullscreen
				// backbuffer (black bars) and leaves the options combo box with nothing selected.
				// Snap to the nearest offered mode, preferring the next height UP. Runtime globals
				// only - no Options.ini write as a side effect of device init.
				// Only when the fullscreen path is pinned to the display aspect. Windowed runs and
				// Windows can legitimately use a size the adapter never enumerated.
				// GeneralsX: ...and NOT when the request already has the
				// display's own aspect. See requestAspectMatchesDisplay: migrating one of those is
				// how the iPad silently loses half its render resolution.
				// GeneralsX: ...and ALSO when no attached display can
				// present the request at all. Wrong SHAPE and wrong SIZE are different faults and
				// only the first one was handled here. Ported from the GeneralsMD twin (0450933cc).
				//
				// The snap-to-nearest-mode rescue above is gated on an aspect MISMATCH, because
				// bars require one. A resolution that was doubled by the unit bug this project
				// spent 2026-09-14 on - POINTS written into a field that is read as POINTS but
				// sourced from PIXELS - keeps its aspect ratio exactly, so it looks perfectly
				// shaped and was waved straight through to a 4x-oversized backbuffer.
				//
				// Only the THRESHOLD is new. Where to land is still the offered mode list, and
				// the tallest-offered fallback that findBestOfferedMode inherits from this block
				// is exactly right for a request nothing can present. Doing it here rather than
				// after the points->pixels scale below is deliberate: the mode list is in POINTS,
				// the globals are in POINTS, and a correction that has to be converted back is a
				// correction that can be converted back WRONG.
#ifdef SAGE_USE_SDL3
				Int capW = 0, capH = 0;
				const bool tooBigForAnyDisplay =
					SDL3_RequestExceedsDisplayCapability(xres, yres, &capW, &capH);
				if (tooBigForAnyDisplay)
				{
					const Real density = SDL3_GetRenderPixelDensity();
					fprintf(stderr,
						"INFO: %dx%d points wants a %dx%d pixel render (density %.2f); the largest "
						"framebuffer any attached display offers is %dx%d\n",
						xres, yres, (Int)((Real)xres * density), (Int)((Real)yres * density),
						(double)density, capW, capH);
				}
#else
				const bool tooBigForAnyDisplay = false;
#endif

				const bool mayMigrate = tooBigForAnyDisplay
					|| (fullscreenAspectIsLocked(getWindowed())
						&& !requestAspectMatchesDisplay(xres, yres));

				Int modeCount = 0;
				bool exactMatch = false;
				if (mayMigrate)
				{
					modeCount = getDisplayModeCount();
					for (Int i = 0; i < modeCount && !exactMatch; ++i)
					{
						Int mx = 0, my = 0, mb = 0;
						getDisplayModeDescription(i, &mx, &my, &mb);
						if (mx == xres && my == yres)
							exactMatch = true;
					}
				}

				// An exact match in the offered list normally ends it - the request IS one of the
				// modes on offer. It does not end it when nothing can present that mode, which
				// buildFilteredResolutions' permissive retry can produce: that pass drops the
				// taller-than-native filter so the combo box can never come back empty, and a mode
				// it restores may be larger than anything attached will drive.
				if (mayMigrate && modeCount > 0 && (tooBigForAnyDisplay || !exactMatch))
				{
					Int bestX = 0, bestY = 0;
					if (findBestOfferedMode(this, xres, yres, bestX, bestY))
					{
						if (tooBigForAnyDisplay)
						{
							fprintf(stderr,
								"INFO: resolution %dx%d cannot be presented by any attached display; using %dx%d\n",
								xres, yres, bestX, bestY);
						}
						else
						{
							fprintf(stderr,
								"INFO: resolution %dx%d is not offered in this display mode; using %dx%d\n",
								xres, yres, bestX, bestY);
						}
						xres = bestX;
						yres = bestY;
						// Keep the options screen's preselection honest about what is running.
						TheWritableGlobalData->m_xResolution = xres;
						TheWritableGlobalData->m_yResolution = yres;
					}
				}

#ifdef SAGE_USE_SDL3
				// GeneralsX: Render at NATIVE pixels on a Retina Mac.
				// Ported from the GeneralsMD twin (d5afaed6f).
				//
				// Options.ini stores a resolution in POINTS - it is the size of the window the player
				// wants - but with SDL_WINDOW_HIGH_PIXEL_DENSITY the drawable underneath that window
				// is density times larger. Leaving the render resolution at the point value builds a
				// half-size backbuffer and lets the compositor upscale it, which is the blur reported
				// on the Zero Hour twin from an M5 MacBook: everything game-drawn twice the size it
				// should be and fuzzy, while the OS-drawn cursor stayed sharp and small.
				//
				// Scaling HERE rather than at the top of the block on purpose: the migration above
				// compares against the adapter's offered mode list, which SDL fills in POINTS, so it
				// has to run in point units or every comparison is a unit error. Only the final
				// render resolution becomes pixels.
				//
				// GeneralsX: The globals stay in POINTS - that has not
				// changed and is what Options.ini holds and what the options combo box offers.
				//
				//
				//
				//
				// The scaled PIXEL value below is what is never written
				// back, and it is the only thing that was ever meant.
				//
				// The window is put back to the player's chosen POINT size by
				// SDL3_ApplyWindowModeForRenderConfig, which divides by the same density - so the
				// window does not change size on screen, only its sharpness.
				//
				// The platform guard lives inside SDL3_GetRenderPixelDensity rather than here, so
				// there is exactly one place that decides whether this build scales at all. A no-op
				// at density 1.0, which is every non-Retina Mac and every non-Apple platform.
				SDL3_ScaleRenderResolutionToPixels(xres, yres);
#endif

				// set our default width and height and bit depth
				setWidth( xres );
				setHeight( yres );
				setBitDepth( DEFAULT_DISPLAY_BIT_DEPTH );
				break;
			}
			case 1:
			{
				// Getting the device at the default bit depth (32) didn't work, so try
				// getting a 16 bit display.  (Voodoo 1-3 only supported 16 bit.) jba.
				setBitDepth( MIN_DISPLAY_BIT_DEPTH );
				break;
			}
			case 2:
			{
				// TheSuperHackers @bugfix xezon 11/06/2025 Now tries a safe default resolution
				// if the custom resolution did not succeed. This is unlikely to happen but is possible
				// if the user writes an unsupported resolution in the Option Preferences or if the
				// graphics adapter does not support the minimum display resolution to begin with.
				Int xres = DEFAULT_DISPLAY_WIDTH;
				Int yres = DEFAULT_DISPLAY_HEIGHT;
				Int bitDepth = DEFAULT_DISPLAY_BIT_DEPTH;
				Int displayModeCount = getDisplayModeCount();
				Int displayModeIndex = 0;
				for (; displayModeIndex < displayModeCount; ++displayModeIndex)
				{
					getDisplayModeDescription(displayModeIndex, &xres, &yres, &bitDepth);
					if (xres * yres >= DEFAULT_DISPLAY_WIDTH * DEFAULT_DISPLAY_HEIGHT)
						break; // Is good enough. Use it.
				}
				TheWritableGlobalData->m_xResolution = xres;
				TheWritableGlobalData->m_yResolution = yres;
#ifdef SAGE_USE_SDL3
				// GeneralsX: This fallback picks from the offered mode list,
				// which is in POINTS, so it needs the same points -> pixels conversion case 0 does.
				// Without it a Retina Mac that fell back here would build a point-sized backbuffer
				// and then have SDL3_ApplyWindowModeForRenderConfig divide it by the density again,
				// giving a half-size window - the exact defect this file is fixing, on the one path
				// that is only taken when something has already gone wrong.
				//
				// AFTER the globals are written, not before: m_x/yResolution is the point-valued
				// copy that Options.ini and the options combo box speak, and only getWidth()/
				// getHeight() carry pixels.
				//
				// GeneralsX: NO capability correction here, deliberately,
				// and this is the one path that does not get one. Unlike case 0 this does not take
				// a value off disk - it walks the offered mode list and stops at the first entry
				// big enough to be usable, so the result is a mode the adapter enumerated and the
				// oversized-Options.ini fault case 0 guards against cannot arise. The residual is
				// buildFilteredResolutions' permissive retry, which can restore a mode larger than
				// anything attached will drive; correcting for that here would mean either skipping
				// entries (leaving xres/yres holding a skipped mode when the loop runs out) or
				// snapping to the TALLEST offered mode, which is the wrong direction for a
				// last-resort fallback that is trying to find something SMALL that works. Reaching
				// this case already means two device creations failed. Left alone on purpose.
				SDL3_ScaleRenderResolutionToPixels(xres, yres);
#endif
				setWidth( xres );
				setHeight( yres );
				setBitDepth( bitDepth );
				break;
			}
			}

			// TheSuperHackers @feature Mauller 13/03/2026 Add native MSAA support, must be set before creating render device
			WW3D::Set_MSAA_Mode((WW3D::MultiSampleModeEnum)TheWritableGlobalData->m_antiAliasLevel);

			renderDeviceError = WW3D::Set_Render_Device(
				0,
				getWidth(),
				getHeight(),
				getBitDepth(),
				getWindowed(),
				true );

			// TheSuperHackers @info Update the MSAA mode that was set as some GPU's may not support certain levels
			// Texture filtering must also be updated after render device initialization
			if (renderDeviceError == WW3D_ERROR_OK) {
				TheWritableGlobalData->m_antiAliasLevel = (UnsignedInt)WW3D::Get_MSAA_Mode();
				WW3D::Set_Texture_Filter(TheWritableGlobalData->m_textureFilteringMode);
				TheWritableGlobalData->m_textureFilteringMode = WW3D::Get_Texture_Filter();
				WW3D::Set_Anisotropy_Level(TheWritableGlobalData->m_textureAnisotropyLevel);
				TheWritableGlobalData->m_textureAnisotropyLevel = WW3D::Get_Anisotropy_Level();
			}

			++attempt;
		}
		while (attempt < 3 && renderDeviceError != WW3D_ERROR_OK);

		if (renderDeviceError != WW3D_ERROR_OK)
		{
			WW3D::Shutdown();
			WWMath::Shutdown();
			throw ERROR_INVALID_D3D;	//failed to initialize.  User probably doesn't have DX 8.1
			DEBUG_CRASH( ("Unable to set render device") );
			return;
		}
		WW3D::Set_Texture_Bitdepth(getBitDepth());

		#ifdef SAGE_USE_SDL3
		SDL3_ApplyWindowModeForRenderConfig(getWindowed(), getWidth(), getHeight());
		#endif

		//Check if level was never set and default to setting most suitable for system.
		if (TheGameLODManager->getStaticLODLevel() == STATIC_GAME_LOD_UNKNOWN)
		{
			TheGameLODManager->setStaticLODLevel(TheGameLODManager->getRecommendedStaticLODLevel());
		}
		else
		{
			//Static LOD level was applied during GameLOD manager init except for texture reduction
			//which needs to be applied here.
			TheGameClient->setTextureLOD(TheWritableGlobalData->m_textureReductionFactor);
		}

		if (TheGlobalData->m_displayGamma != 1.0f)
			setGamma(TheGlobalData->m_displayGamma,0.0f,1.0f,FALSE);
	}

	initAssets();

	if (!TheGlobalData->m_headless)
	{
		init2DScene();
		init3DScene();
		W3DShaderManager::init();

		// Create and initialize the debug display
		m_nativeDebugDisplay = NEW W3DDebugDisplay();
		m_debugDisplay = m_nativeDebugDisplay;
		if ( m_nativeDebugDisplay )
		{
			m_nativeDebugDisplay->init();
			GameFont *font;

			if (TheGlobalLanguageData && TheGlobalLanguageData->m_nativeDebugDisplay.name.isNotEmpty())
			{
				font=TheFontLibrary->getFont(
					TheGlobalLanguageData->m_nativeDebugDisplay.name,
					TheGlobalLanguageData->m_nativeDebugDisplay.size,
					TheGlobalLanguageData->m_nativeDebugDisplay.bold);
			}
			else
				font=TheFontLibrary->getFont( "FixedSys", 8, FALSE );

			m_nativeDebugDisplay->setFont( font );
			m_nativeDebugDisplay->setFontHeight( 13 );
			m_nativeDebugDisplay->setFontWidth( 9 );
		}

		DX8WebBrowser::Initialize();
	}

	// we're now online
	m_initialized = true;
	if( TheGlobalData->m_displayDebug )
	{
		m_debugDisplayCallback = StatDebugDisplay;
	}
}

// W3DDisplay::reset ===========================================================
/** Reset the W3D display system.  Here we need to
  * remove the objects from the previous map. */
//=============================================================================
extern void skyBoxMapReset();	// W3DWater.cpp
void W3DDisplay::reset()
{

	Display::reset();

	// Remove all render objects.

	if (m_3DScene != nullptr)
	{
		SceneIterator *sceneIter = m_3DScene->Create_Iterator();
		sceneIter->First();
		while(!sceneIter->Is_Done()) {
			RenderObjClass * robj = sceneIter->Current_Item();
			robj->Add_Ref();
			m_3DScene->Remove_Render_Object(robj);
			robj->Release_Ref();
			sceneIter->Next();
		}
		m_3DScene->Destroy_Iterator(sceneIter);
	}

	m_isClippedEnabled = FALSE;

	// release any unused assets from W3D
	/// @todo really need that "scene abstraction", having this stuff in the display is icky
	m_assetManager->Release_Unused_Assets();

	if (TheWritableGlobalData)
		TheWritableGlobalData->m_drawSkyBox =0;
	skyBoxMapReset();	// GeneralsX @feature skybox-1: a script's "sky off" ends with the map
}

const UnsignedInt START_CUMU_FRAME = LOGICFRAMES_PER_SECOND / 2;	// skip first half-sec

void W3DDisplay::updateAverageFPS()
{
	constexpr const Int FPS_HISTORY_SIZE = 30;

	static Int64 lastUpdateTime64 = 0;
	static Int historyOffset = 0;
	// Holds per-frame ELAPSED SECONDS, not fps - see the averaging note below.
	static Real frameTimeHistory[FPS_HISTORY_SIZE] = {0};
	// How many slots actually hold a measured interval. Averaging over the full array while it is
	// still filling divides by zeros and reports a rate nobody achieved.
	static Int historyCount = 0;

	const Int64 freq64 = getPerformanceCounterFrequency();
	const Int64 time64 = getPerformanceCounter();

#if defined(RTS_DEBUG)
	if (TheGameLogic->getFrame() == START_CUMU_FRAME)
	{
		m_timerAtCumuFPSStart = time64;
	}
#endif

	const Int64 timeDiff = time64 - lastUpdateTime64;

	// convert elapsed time to seconds
	Real elapsedSeconds = (Real)timeDiff/(Real)freq64;

	// append new sample to fps history.
	if (historyOffset >= FPS_HISTORY_SIZE)
		historyOffset = 0;

	m_currentFPS = 1.0f/elapsedSeconds;

	// GeneralsX @bugfix THIS AVERAGE USED TO BE A MEAN OF RECIPROCALS, AND IT OVER-REPORTED.
	// The history held 1/dt per frame and this took their arithmetic mean. By Jensen's inequality
	// mean(1/dt) >= 1/mean(dt), with equality only when every frame time is identical - so the
	// reported rate was always at or above the true rate over the window, and the gap grows with
	// frame-time VARIANCE. One slow frame among fast ones barely moves the sum while contributing
	// a tiny reciprocal; the fast frames' large reciprocals dominate.
	//
	// Measured on an iPad Air M3 over 392 one-second windows of a 25000-frame match: 1.232x the
	// true loop rate on average and 3.14x at worst. Measured on this Mac at a 480 fps cap, where
	// the limiter makes frame times uniform, the same estimator reads exact (479.9 vs 479.8) -
	// so the error is driven by variance, not by platform, and it is LARGEST on the jittery weak
	// peers that matter most.
	//
	// Why that is not merely a cosmetic HUD bug: this value is sampled by FrameMetrics once a
	// second (FrameMetrics.cpp:125) and feeds ConnectionManager's minimum-fps clamp, which sets
	// the simulation rate for the WHOLE lobby. An over-reporting weak peer therefore talks the
	// lobby into a rate it cannot actually sustain, and the resulting stutter looks like a
	// network fault rather than a local one.
	//
	// Averaging the frame TIMES and inverting gives the true mean rate over the window, which is
	// what every consumer already assumes it is getting. Guard the divide: an all-zero history on
	// the first frames would otherwise produce inf.
	// GeneralsX: SKIP THE FIRST CALL - IT HAS NO INTERVAL TO MEASURE.
	// lastUpdateTime64 starts at 0, so on the very first draw timeDiff is the performance
	// counter's ABSOLUTE value - the machine's uptime, not a frame time. Storing reciprocals hid
	// that: 1/uptime is ~0 and barely moved a sum of real rates. Storing the interval does not:
	// one uptime-sized sample dominates the sum and pins m_averageFPS near ZERO until it is
	// overwritten 30 draws later, and that near-zero is what FrameMetrics samples into the
	// lobby-wide minimum-fps clamp. Seed the timestamp and record nothing on that pass; the
	// constructor already leaves m_averageFPS at the configured cap, which is a far better
	// starting estimate than any value derivable from a non-interval.
	//
	// Divide by the number of POPULATED slots for the same reason: while the array is filling,
	// the unused entries are 0, so summing all 30 understates the elapsed time and overstates
	// the rate.
	if (lastUpdateTime64 != 0)
	{
		frameTimeHistory[historyOffset++] = elapsedSeconds;
		if (historyCount < FPS_HISTORY_SIZE)
		{
			++historyCount;
		}

		const Real elapsedSum = std::accumulate(frameTimeHistory, frameTimeHistory + historyCount, 0.0f);
		m_averageFPS = (elapsedSum > 0.0f) ? ((Real)historyCount / elapsedSum) : 0.0f;
	}

	lastUpdateTime64 = time64;
}

#if defined(RTS_DEBUG)	//debug hack to view object under mouse stats
ICoord2D TheMousePos;
#endif

// W3DDisplay::gatherDebugStats ===================================================
/** Compute and display debug stats on screen */
//=============================================================================
void W3DDisplay::gatherDebugStats()
{
	static UnsignedInt s_framesRenderedSinceLastUpdate = 0;
	static Int64 s_lastUpdateTime64 = 0;
	static double s_timeSinceLastUpdateInSecs = 0.0;
	static Int s_drawCallsSinceLastUpdate = 0;
	static Int s_sortedPolysSinceLastUpdate = 0;

	// allocate the display strings if needed
	if( m_displayStrings[0] == nullptr )
	{
		GameFont *font;
		if (TheGlobalLanguageData && TheGlobalLanguageData->m_nativeDebugDisplay.name.isNotEmpty())
		{
			font=TheFontLibrary->getFont(
				TheGlobalLanguageData->m_nativeDebugDisplay.name,
				TheGlobalLanguageData->m_nativeDebugDisplay.size,
				TheGlobalLanguageData->m_nativeDebugDisplay.bold);
		}
		else
			font = TheFontLibrary->getFont( "FixedSys", 8, FALSE );

		for (int i = 0; i < DisplayStringCount; i++)
		{
			if (m_displayStrings[i] == nullptr)
			{
				m_displayStrings[i] = TheDisplayStringManager->newDisplayString();
				DEBUG_ASSERTCRASH( m_displayStrings[i], ("Failed to create DisplayString") );
				m_displayStrings[i]->setFont( font );
			}
		}

	}

	if (m_benchmarkDisplayString == nullptr)
	{
		GameFont *thisFont = TheFontLibrary->getFont( "FixedSys", 8, FALSE );
		m_benchmarkDisplayString = TheDisplayStringManager->newDisplayString();
		DEBUG_ASSERTCRASH( m_benchmarkDisplayString, ("Failed to create DisplayString") );
		m_benchmarkDisplayString->setFont( thisFont );
	}

	++s_framesRenderedSinceLastUpdate;
  s_drawCallsSinceLastUpdate += Debug_Statistics::Get_Draw_Calls();
	s_sortedPolysSinceLastUpdate += Debug_Statistics::Get_Sorting_Polygons();

	Int64 freq64 = getPerformanceCounterFrequency();
	Int64 time64 = getPerformanceCounter();

	s_timeSinceLastUpdateInSecs = ((double)(time64 - s_lastUpdateTime64) / (double)(freq64));

#ifdef EXTENDED_STATS
		static FILE *pListFile = nullptr;
		static Int64 lastFrameTime=0;
		static samples = 0;
		if (pListFile == nullptr) {
			pListFile = fopen("FrameRateLog.txt", "w");
		}
		samples++;
		if (pListFile && lastFrameTime && samples<100) {
			float timeSinceLastFrame = (float)((double)(time64-lastFrameTime) / (double)(freq64));
			fprintf(pListFile, "%d ", (int)(1/timeSinceLastFrame));
		}
		lastFrameTime = time64;
#endif

	// we update stats on a delay
	const Real UPDATE_RATE_SECS = 2.0;
	if( s_timeSinceLastUpdateInSecs >= UPDATE_RATE_SECS || TheGlobalData->m_constantDebugUpdate )
	{
		UnicodeString unibuffer, unibuffer2;
		UnicodeString fpsString;

		// setup texture stats
		Debug_Statistics::Record_Texture_Mode(Debug_Statistics::RECORD_TEXTURE_SIMPLE/*RECORD_TEXTURE_NONE*/);

		// frames per second
		double fps = (Real)s_framesRenderedSinceLastUpdate / s_timeSinceLastUpdateInSecs;
		double drawsPerFrame = Debug_Statistics::Get_Draw_Calls(); //(Real)s_drawCallsSinceLastUpdate / (Real)s_framesRenderedSinceLastUpdate;
		double sortPolysPerFrame = Debug_Statistics::Get_Sorting_Polygons();  //(Real)s_sortedPolysSinceLastUpdate / (Real)s_framesRenderedSinceLastUpdate;
		double skinDrawsPerFrame = Debug_Statistics::Get_DX8_Skin_Renders();

		if (fps<0.1) fps = 0.1;

		double ms = 1000.0f/fps;


#if defined(RTS_DEBUG)
		double cumuTime = ((double)(time64 - m_timerAtCumuFPSStart) / (double)(freq64));
		if (cumuTime < 0.0) cumuTime = 0.0;
		Int numFrames = (Int)TheGameLogic->getFrame() - (Int)START_CUMU_FRAME;
		double cumuFPS = (numFrames > 0 && cumuTime > 0.0) ? (numFrames / cumuTime) : 0.0;
		double skinPolysPerFrame = Debug_Statistics::Get_DX8_Skin_Polygons();

		//Int LOD = TheGlobalData->m_terrainLOD;
		//unibuffer.format( L"FPS: %.2f, %.2fms mapLOD=%d [cumu FPS=%.2f] draws: %.2f sort: %.2f", fps, ms, LOD, cumuFPS, drawsPerFrame,sortPolysPerFrame);
		if (TheGlobalData->m_useFpsLimit)
				unibuffer.format( L"%.2f/%d FPS, ", fps, TheFramePacer->getFramesPerSecondLimit());
		else
				unibuffer.format( L"%.2f FPS, ", fps);

				unibuffer2.format( L"%.2fms [cumuFPS=%.2f] draws: %d skins: %d sortP: %d skinP: %d", ms, cumuFPS, (Int)drawsPerFrame,(Int)skinDrawsPerFrame,(Int)sortPolysPerFrame, (Int)skinPolysPerFrame);
				unibuffer.concat(unibuffer2);
#else
		//Int LOD = TheGlobalData->m_terrainLOD;
		//unibuffer.format( L"FPS: %.2f, %.2fms mapLOD=%d draws: %.2f sort %.2f", fps, ms, LOD, drawsPerFrame,sortPolysPerFrame);
		unibuffer.format( L"FPS: %.2f, %.2fms draws: %.2f skins: %.2f sort %.2f", fps, ms, drawsPerFrame,skinDrawsPerFrame,sortPolysPerFrame);
		if (TheGlobalData->m_useFpsLimit)
		{
			unibuffer2.format(L", FPSLock %d",TheGlobalData->m_framesPerSecondLimit);
			unibuffer.concat(unibuffer2);
		}
#endif

		fpsString.format( L"FPS: %.2f", fps);
		m_benchmarkDisplayString->setText( fpsString );

		Int polyPerFrame = Debug_Statistics::Get_DX8_Polygons();

#ifdef EXTENDED_STATS
		static float gameOverheadMS = 0.0f;
		static float consoleMS = 0.0f;
		static float threeDOverheadMS = 0.0f;
		static float terrainMS = 0.0f;
		static float objectMS = 0.0f;
		static float overlapMS = 0.0f;
		static int  extendedStats = 0;
		const int SHOW_STATS_TIME=12; // show extended stats for 5 cycles == 10 seconds.
		static enum {disabled, sync, gameOverhead, console, threeDOverhead, terrain, objects, overlap, normal} statMode = disabled;

		if (statMode == sync) {
			extendedStats = SHOW_STATS_TIME;
			statMode = gameOverhead;
		} else if (statMode == gameOverhead) {
			gameOverheadMS = ms;
			statMode = console;
			DX8Wrapper::stats.m_disableTerrain = true;
			DX8Wrapper::stats.m_disableOverhead = true;
			DX8Wrapper::stats.m_disableWater = true;
			DX8Wrapper::stats.m_disableObjects = true;
			DX8Wrapper::stats.m_disableConsole = false;
			DX8Wrapper::stats.m_debugLinesToShow = 1;
		} else if (statMode == console) {
			consoleMS = ms;
			statMode = threeDOverhead;
			DX8Wrapper::stats.m_disableTerrain = true;
			DX8Wrapper::stats.m_disableOverhead = true;
			DX8Wrapper::stats.m_disableWater = true;
			DX8Wrapper::stats.m_disableObjects = true;
			DX8Wrapper::stats.m_disableConsole = true;
			DX8Wrapper::stats.m_debugLinesToShow = 1;
		} else if (statMode == threeDOverhead) {
			threeDOverheadMS = ms;
			statMode = terrain;
			DX8Wrapper::stats.m_disableTerrain = false;
			DX8Wrapper::stats.m_disableOverhead = true;
			DX8Wrapper::stats.m_disableWater = true;
			DX8Wrapper::stats.m_disableObjects = true;
			DX8Wrapper::stats.m_disableConsole = true;
			DX8Wrapper::stats.m_debugLinesToShow = 1;
		} else if (statMode == terrain) {
			terrainMS = ms;
			statMode = objects;
			DX8Wrapper::stats.m_disableOverhead = true;
			DX8Wrapper::stats.m_disableTerrain = true;
			DX8Wrapper::stats.m_disableWater = true;
			DX8Wrapper::stats.m_disableObjects = false;
			DX8Wrapper::stats.m_disableConsole = true;
			DX8Wrapper::stats.m_debugLinesToShow = 1;
		} else if (statMode == objects) {
			objectMS = ms;
			statMode = overlap;
			DX8Wrapper::stats.m_disableOverhead = false;
			DX8Wrapper::stats.m_disableTerrain = false;
			DX8Wrapper::stats.m_disableWater = false;
			DX8Wrapper::stats.m_disableObjects = false;
			DX8Wrapper::stats.m_disableConsole = true;
			DX8Wrapper::stats.m_sleepTime = (int)(terrainMS);
			DX8Wrapper::stats.m_debugLinesToShow = 1;
		} else if (statMode == overlap) {
			overlapMS = ms;
			statMode = normal;
			DX8Wrapper::stats.m_disableOverhead = false;
			DX8Wrapper::stats.m_disableTerrain = false;
			DX8Wrapper::stats.m_disableWater = false;
			DX8Wrapper::stats.m_disableObjects = false;
			DX8Wrapper::stats.m_disableConsole = true;
			DX8Wrapper::stats.m_sleepTime = 0;
			DX8Wrapper::stats.m_debugLinesToShow = 1;
		} else if (statMode == normal) {
			overlapMS = (ms + ((int)terrainMS) - overlapMS );
			statMode = disabled;
			extendedStats = SHOW_STATS_TIME;

			// Done collecting stats. Re-enable stuff
			DX8Wrapper::stats.m_disableConsole = false;
			DX8Wrapper::stats.m_debugLinesToShow = -1;
		} else if (!DX8Wrapper::stats.m_showingStats) {
			// start collecting extended info.
			DX8Wrapper::stats.m_showingStats = true;
			DX8Wrapper::stats.m_disableOverhead = false;
			DX8Wrapper::stats.m_disableTerrain = true;
			DX8Wrapper::stats.m_disableWater = true;
			DX8Wrapper::stats.m_disableObjects = true;
			DX8Wrapper::stats.m_disableConsole = true;
			DX8Wrapper::stats.m_debugLinesToShow = 1;
			statMode = sync;
			gameOverheadMS = 0.0f;
			threeDOverheadMS = 0.0f;
			terrainMS = 0.0f;
			objectMS = 0.0f;
		}
		if (statMode != disabled) {
			unibuffer.format(L"FPS: %.2f, %.2fms - Collecting extended stats.", fps, ms);
		} else if (extendedStats>0) {
			extendedStats--;
			unibuffer.format( L"FPS: %.2f, %.2fms - OH %.2fms, Console %.2fms, 3D OH %.2fms, Terrain %.2fms, Obs %.2fms, CPU %.2fms",
				fps, ms, gameOverheadMS, consoleMS, threeDOverheadMS, terrainMS, objectMS, overlapMS);
			if (extendedStats==SHOW_STATS_TIME-2) {
				char bufferA[ 256 ];
				sprintf( bufferA, "FPS: %.2f, %.2fms - OH %.2fms, Console %.2fms, 3D OH %.2fms, Terrain %.2fms, Obs %.2fms, CPU %.2fms\n",
					fps, ms, gameOverheadMS, consoleMS, threeDOverheadMS, terrainMS, objectMS, overlapMS);
				::OutputDebugString(bufferA);
				if (pListFile) {
					fprintf(pListFile, "\n%s", bufferA);
				}
				sprintf( bufferA, "Polygons: per frame %d, per second %d\n", polyPerFrame,
						(Int)(polyPerFrame*fps));
				::OutputDebugString(bufferA);
				if (pListFile) {
					fprintf(pListFile, "%s", bufferA);
					fflush(pListFile);
				}
			}
		}
 		if (pListFile) {
			fprintf(pListFile, "\nFPS: %.2f, %.2fms\n", fps, ms);
			fflush(pListFile);
		}
		if (pListFile) {
			samples = 0;
			if (statMode != disabled) {
				fprintf(pListFile, "Stat%d-", statMode);
			}
		}

#endif
		// check for debug D3D
		Bool debugD3D=false;
		RegistryClass registry ("Software\\Microsoft\\Direct3d");
		if (registry.Is_Valid ()) {
			if (registry.Get_Int ("LoadDebugRuntime", 0) == 1) {
				debugD3D = true;
			}
		}
		if (debugD3D) {
			unibuffer.concat(L", DEBUG D3D");
		}
#ifdef RTS_DEBUG
		unibuffer.concat(L", DEBUG app");
#endif

		m_displayStrings[FPS]->setText( unibuffer );

		// Actual GameLogic frame number
		unibuffer.format(L"Frame: %d", TheGameLogic->getFrame());
		m_displayStrings[Frame]->setText( unibuffer );

		// polygons this frame
		unibuffer.format( L"Polygons: per frame %d, per second %d", polyPerFrame,
				(Int)(polyPerFrame*fps));
		m_displayStrings[Polygons]->setText( unibuffer );

		// vertices this frame
		unibuffer.format( L"Vertices: %d", Debug_Statistics::Get_DX8_Vertices() );
		m_displayStrings[Vertices]->setText( unibuffer );

		//
		// I'm adjusting the texture memory usage counter by subtracting
		// out the terrain alpha texture (since it's really == terrain texture).
		//
		unibuffer.format( L"Video RAM: %d", Debug_Statistics::Get_Record_Texture_Size() - 1376256 );
		m_displayStrings[VideoRam]->setText( unibuffer );

		s_lastUpdateTime64 = time64;
		s_timeSinceLastUpdateInSecs = 0.0f;
		s_framesRenderedSinceLastUpdate = 0;
		s_drawCallsSinceLastUpdate = 0;
		s_sortedPolysSinceLastUpdate = 0;

		// terrain stats
		unibuffer.format( L"3-Way Blends: %d/%d, Shoreline Blends: %d/%d", TheTerrainRenderObject->getNumExtraBlendTiles(TRUE),
			TheTerrainRenderObject->getNumExtraBlendTiles(FALSE),
			TheTerrainRenderObject->getNumShoreLineTiles(TRUE),
			TheTerrainRenderObject->getNumShoreLineTiles(FALSE));
		m_displayStrings[TerrainStats]->setText( unibuffer );

		// misc debug info
		Coord3D camPos = TheTacticalView->getPosition();
		Real zoom = TheTacticalView->getZoom();
		Real pitch = TheTacticalView->getPitch();
		Real FXPitch = TheTacticalView->getFXPitch();
		Real angle = TheTacticalView->getAngle();
		Real FOV = TheTacticalView->getFieldOfView();
		Real terrainHeight = TheTacticalView->getTerrainHeightAtPivot();
		Real actualHeightAboveGround = TheTacticalView->getCurrentHeightAboveGround();

		unibuffer.format(
			L"Camera zoom: %.3f, pitch: %.2f, FXpitch: %.2f, yaw: %.2f, pos: (%.2f, %.2f, %.2f), FOV: %.2f\n"
			L"Height above ground: %.2f, Terrain height at camera pivot: %.2f",
			zoom,
			RAD_TO_DEGF(pitch),
			RAD_TO_DEGF(FXPitch),
			RAD_TO_DEGF(angle),
			camPos.x, camPos.y, camPos.z,
			RAD_TO_DEGF(FOV),
			actualHeightAboveGround, terrainHeight );

		m_displayStrings[DebugInfo]->setText( unibuffer );

		// display the keyboard modifier and mouse states.
		unibuffer.format( L"States: " );
		if( TheKeyboard->isShift() )
		{
			unibuffer.concat( L"Shift(" );
			if( TheKeyboard->getModifierFlags() & KEY_STATE_LSHIFT )
			{
				unibuffer.concat( L"L" );
			}
			if( TheKeyboard->getModifierFlags() & KEY_STATE_RSHIFT )
			{
				unibuffer.concat( L"R" );
			}
			unibuffer.concat( L") " );
		}
		if( TheKeyboard->isCtrl() )
		{
			unibuffer.concat( L"Ctrl(" );
			if( TheKeyboard->getModifierFlags() & KEY_STATE_LCONTROL )
			{
				unibuffer.concat( L"L" );
			}
			if( TheKeyboard->getModifierFlags() & KEY_STATE_RCONTROL )
			{
				unibuffer.concat( L"R" );
			}
			unibuffer.concat( L") " );
		}
		if( TheKeyboard->isAlt() )
		{
			unibuffer.concat( L"Alt(" );
			if( TheKeyboard->getModifierFlags() & KEY_STATE_LALT )
			{
				unibuffer.concat( L"L" );
			}
			if( TheKeyboard->getModifierFlags() & KEY_STATE_RALT )
			{
				unibuffer.concat( L"R" );
			}
			unibuffer.concat( L") " );
		}

		const MouseIO *mouseStatus = TheMouse->getMouseStatus();

		if( mouseStatus->leftState )
		{
			unibuffer.concat( L"LMB " );
		}
		if( mouseStatus->middleState )
		{
			unibuffer.concat( L"MMB " );
		}
		if( mouseStatus->rightState )
		{
			unibuffer.concat( L"RMB " );
		}

		Object *object = nullptr;
#if defined(RTS_DEBUG)	//debug hack to view object under mouse stats
		Drawable *draw = 	TheTacticalView->pickDrawable(&TheMousePos, FALSE, (PickType)0xffffffff );
#else
		Drawable *draw = TheGameClient->findDrawableByID( TheInGameUI->getMousedOverDrawableID() );
#endif
		if( draw  )
			object = draw->getObject();
		if( object )
		{
			unibuffer2.format( L"Moused over object: %S (%d) ", object->getTemplate()->getName().str(), object->getID() );
			unibuffer.concat( unibuffer2 );
		}
		else
		{
			unibuffer.concat( L"Moused over object: TERRAIN " );
		}

		m_displayStrings[ KEY_MOUSE_STATES ]->setText( unibuffer );

		//display the x and y mouse coordinates
		const MouseIO *mouseIO = TheMouse->getMouseStatus();
		Coord3D worldPos;
		if( TheTacticalView->screenToTerrain(&mouseIO->pos, &worldPos) )
		{
			unibuffer.format( L"Mouse position: screen: (%d, %d), world: (%g, %g, %g)",
				mouseIO->pos.x, mouseIO->pos.y, worldPos.x, worldPos.y, worldPos.z);
		}
		else
		{
			unibuffer.format( L"Mouse position: screen: (%d, %d), world: none",
				mouseIO->pos.x, mouseIO->pos.y);
		}
		m_displayStrings[MousePosition]->setText( unibuffer );

		//display the number of particles in the world and being displayed on screen
		Int totalParticles = TheParticleSystemManager->getParticleCount();
		Int onScreenParticleCount = TheParticleSystemManager->getOnScreenParticleCount();
		unibuffer.format( L"Particles: %d in world, %d being displayed", totalParticles, onScreenParticleCount );
		m_displayStrings[Particles]->setText( unibuffer );

		//display the number of objects in the world
		UnsignedInt objCount = TheGameLogic->getObjectCount();
		UnsignedInt objScreenCount = TheGameClient->getRenderedObjectCount();

		unibuffer.format(L"Objects: %d in world, %d being displayed", objCount, objScreenCount );
		m_displayStrings[Objects]->setText( unibuffer );

		// Network incoming bandwidth stats
		if (TheNetwork != nullptr) {
			unibuffer.format(L"IN: %.2f bytes/sec, %.2f packets/sec",
				TheNetwork->getIncomingBytesPerSecond(), TheNetwork->getIncomingPacketsPerSecond());
			m_displayStrings[NetIncoming]->setText( unibuffer );

			// Network outgoing bandwidth stats
			unibuffer.format(L"OUT: %.2f bytes/sec, %.2f packets/sec",
				TheNetwork->getOutgoingBytesPerSecond(), TheNetwork->getOutgoingPacketsPerSecond());
			m_displayStrings[NetOutgoing]->setText( unibuffer );

			// Network performance stats
			unibuffer.format(L"Run Ahead: %d, Net FPS: %d, Packet arrival cushion: %d",
				TheNetwork->getRunAhead(), TheNetwork->getFrameRate(), TheNetwork->getPacketArrivalCushion());
			m_displayStrings[NetStats]->setText( unibuffer );

			// Client frame rate averages for all players in the game.  This only works right for the packet router.
			unibuffer.clear();
			Int numPlayers = TheNetwork->getNumPlayers();
			for (Int i = 0; i < numPlayers; ++i) {
				UnicodeString tempstr;
				tempstr.format(L"%s: %d ", TheNetwork->getPlayerName(i).str(), TheNetwork->getSlotAverageFPS(i));
				unibuffer.concat(tempstr);
			}
			m_displayStrings[NetFPSAverages]->setText( unibuffer );
		} else {
//			unibuffer.format(L"IN: 0.0 bytes/sec, 0.0 packets/sec");
//			m_displayStrings[NetIncoming]->setText( unibuffer );

			// Network outgoing bandwidth stats
//			unibuffer.format(L"OUT: 0.0 bytes/sec, 0.0 packets/sec");
//			m_displayStrings[NetOutgoing]->setText( unibuffer );
			unibuffer.clear();
//			unibuffer.format(L"Network not present");
			m_displayStrings[NetOutgoing]->setText(unibuffer);
			m_displayStrings[NetIncoming]->setText(unibuffer);
			m_displayStrings[NetStats]->setText(unibuffer);
			m_displayStrings[NetFPSAverages]->setText( unibuffer );
		}

		// selected object info stats
		unibuffer.format( L"Select Info: '%d' drawables selected", TheInGameUI->getSelectCount() );



		//Sorry, guys. I need a special kluge here to get constantdebug results for angry mob.
		//Do no be cross with me.
		//if there is not exactly one drawable selected it will report on the moused-over drawable
		if (TheInGameUI->getSelectCount() == 1)
			draw = TheInGameUI->getFirstSelectedDrawable();


		if( draw )
		{
			Object *obj = draw->getObject();
			AsciiString objectName;

			objectName.set( "No-Name" );
			if( obj && obj->getName().isEmpty() == FALSE )
				objectName = obj->getName();

			unibuffer.format( L"Select Info: '%S'(%S) at (%.3f,%.3f,%.3f)",
												draw->getTemplate()->getName().str(),
												objectName.str(),
												draw->getPosition()->x,
												draw->getPosition()->y,
												draw->getPosition()->z );

			// (gth) compute some stats about the rendering cost of this drawable
#if defined(RTS_DEBUG)
			RenderCost rcost;
			for (DrawModule** dm = draw->getDrawModules(); *dm; ++dm)
			{
				(*dm)->getRenderCost(rcost);
			}
			if (rcost.getDrawCallCount() > 0)
			{
				unibuffer2.format( L"\ndraw calls: %d(+%d) sort meshes: %d skins: %d  bones: %d",rcost.getDrawCallCount(),rcost.getShadowDrawCount(),rcost.getSortedMeshCount(),rcost.getSkinMeshCount(),rcost.getBoneCount());
				unibuffer.concat( unibuffer2 );
			}
#endif

			unibuffer.concat( L"\nModelStates: " );
			ModelConditionFlags mcFlags = draw->getModelConditionFlags();
			const int numEntriesPerLine = 4;
			int lineCount = 0;

			for( int i = 0; i < MODELCONDITION_COUNT; i++ )
			{
				if( mcFlags.test( i ) )
				{
					unibuffer2.format( L"%S ", ModelConditionFlags::getBitNames()[ i ] );
					unibuffer.concat( unibuffer2 );
					lineCount++;
					if( lineCount == numEntriesPerLine )
					{
						lineCount = 0;
						unibuffer.concat( L"\n" );
					}
				}
			}

			//Render ALL modelcondition statii

		}
		m_displayStrings[ SelectedInfo ]->setText( unibuffer );

	}

}

// W3DDisplay::drawDebugStats =================================================
/** Draw debug statistics */
//=============================================================================
void W3DDisplay::drawDebugStats()
{
	Int	x = 3;
	Int	y = 30;
	Color textColor = GameMakeColor( 255, 255, 255, 255 );
	Color dropColor = GameMakeColor( 0, 0, 0, 255 );

	int linesOfStrings = DisplayStringCount;
#ifdef EXTENDED_STATS
	if (DX8Wrapper::stats.m_debugLinesToShow > -1)
	{
		linesOfStrings = DX8Wrapper::stats.m_debugLinesToShow;
	}

#endif


	Int w, h;
	for (int i = 0; i < linesOfStrings; i++)
	{
		m_displayStrings[i]->draw( x, y, textColor, dropColor );
		m_displayStrings[i]->getSize(&w, &h);
		y += h;
	}

}

// W3DDisplay::drawFPSStats =================================================
/** Draw the FPS on the screen */
//=============================================================================
void W3DDisplay::drawFPSStats()
{
	Int	x = 3;
	Int	y = 20;
	Color textColor = GameMakeColor( 255, 255, 255, 255 );
	Color dropColor = GameMakeColor( 0, 0, 0, 255 );

	int linesOfStrings = 1;

	for (int i = 0; i < linesOfStrings; i++)
	{
		m_benchmarkDisplayString->draw( x, y, textColor, dropColor );
	}
}


//=============================================================================
void StatDebugDisplay( DebugDisplayInterface *, void *, FILE *fp )
{
	DEBUG_CRASH(("This should never be called directly, but is just a placeholder for drawDebugStats()"));
}

// W3DDisplay::drawCurrentDebugDisplay =================================================
/** Draw current debug display */
//=============================================================================
void W3DDisplay::drawCurrentDebugDisplay()
{
	if (m_debugDisplayCallback == StatDebugDisplay)
	{
		drawDebugStats();
	}
	else
	{
		if ( m_debugDisplay && m_debugDisplayCallback )
		{
			m_debugDisplay->reset();
			m_debugDisplayCallback( m_debugDisplay, m_debugDisplayUserData, nullptr );
		}
	}
}

// W3DDisplay::calculateTerrainLOD =================================================
/** Calculates an adequately speedy terrain Level Of Detail. */
//=============================================================================
void W3DDisplay::calculateTerrainLOD()
{
	const Int NUM_SAMPLES=20;
	const Int NUM_TO_DISCARD=5;

	Int64 freq64 = getPerformanceCounterFrequency();

	char buf[_MAX_PATH];
	float frameTime = 0;
	float maxTimeLimit = TheGlobalData->m_terrainLODTargetTimeMS/1000.0f;
	TerrainLOD goodLOD = TERRAIN_LOD_MIN;
	TerrainLOD curLOD = TERRAIN_LOD_AUTOMATIC;
	Int count = 0;
#ifdef RTS_DEBUG
	// just go to TERRAIN_LOD_NO_WATER, mirror off.
	TheWritableGlobalData->m_terrainLOD = TERRAIN_LOD_NO_WATER;
	m_3DScene->drawTerrainOnly(false);
	TheTerrainRenderObject->adjustTerrainLOD(0);
	return;
#endif
	do {
		Int i;
		float timeForFrame=0;
		frameTime = 0;
		switch(curLOD) {
			default: curLOD = TERRAIN_LOD_DISABLE; break;
			case TERRAIN_LOD_AUTOMATIC: curLOD = TERRAIN_LOD_MAX; break;
			case TERRAIN_LOD_MAX: curLOD = TERRAIN_LOD_NO_WATER; break;
			case TERRAIN_LOD_NO_WATER: curLOD = TERRAIN_LOD_DISABLE; break;
		}
		if (curLOD == TERRAIN_LOD_DISABLE) {
			break;
		}
		TheWritableGlobalData->m_terrainLOD = curLOD;
		m_3DScene->drawTerrainOnly(true);
		TheTerrainRenderObject->adjustTerrainLOD(0);
		for (i=0; i<NUM_SAMPLES; i++) {
			Int64 startTime64 = getPerformanceCounter();
			// start render block
			updateViews();
			if (WW3D::Begin_Render( true, true, Vector3( 0.0f, 0.0f, 0.0f ) ) == WW3D_ERROR_OK)
			{	// draw all views of the world
				drawViews();
				// render is all done!
				WW3D::End_Render();
			}
			Int64 time64 = getPerformanceCounter();
			timeForFrame = (float)((double)(time64-startTime64) / (double)(freq64));
			sprintf(buf, "%.2fms ", timeForFrame*1000.0f);
			::OutputDebugString(buf);
			if (i>=NUM_TO_DISCARD) {
				frameTime += timeForFrame;
				if (i>NUM_TO_DISCARD+1 &&
					(timeForFrame / ((i+1)-NUM_TO_DISCARD)) > 2*maxTimeLimit) {
					i++;
					break;
				}
			}
		}
		frameTime /= ((i)-NUM_TO_DISCARD);
		count++;
		sprintf(buf, "\n LOD %d, time %.2fms\n", curLOD, frameTime*1000.0f);
		::OutputDebugString(buf);
		if (frameTime<maxTimeLimit && goodLOD<curLOD) {
			goodLOD = curLOD;
		}
		if (frameTime < maxTimeLimit) break;
	} while (count<10);

	TheWritableGlobalData->m_terrainLOD = goodLOD;
	m_3DScene->drawTerrainOnly(false);
	TheTerrainRenderObject->adjustTerrainLOD(0);
#ifdef RTS_DEBUG
	DEBUG_ASSERTCRASH(count<10, ("calculateTerrainLOD") );
#endif

}


Real W3DDisplay::getAverageFPS()
{
	return m_averageFPS;
}

Real W3DDisplay::getCurrentFPS()
{
	return m_currentFPS;
}

Int W3DDisplay::getLastFrameDrawCalls()
{
	return Debug_Statistics::Get_Draw_Calls();
}

//=============================================================================
void W3DDisplay::step()
{
	stepViews();
}

//DECLARE_PERF_TIMER(BigAssRenderLoop)

// W3DDisplay::draw ===========================================================
/** Draw the entire W3D Display */
//=============================================================================
//DECLARE_PERF_TIMER(W3DDisplay_draw)
void W3DDisplay::draw()
{
	//USE_PERF_TIMER(W3DDisplay_draw)

	// GeneralsX @feature xxorza 15/04/2026 Process deferred window resize for pillarbox
	DX8Wrapper::Pillarbox_Process_Resize();

#if defined(__APPLE__) && !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
	// GeneralsX: Twin of GeneralsMD's W3DDisplay::draw: follow the cap when it moves
	// in a match (the max-FPS keys, the Debug screen, a script's visual speed multiplier) or the display under the
	// window changes refresh, on a change only.
	if (TheFramePacer != nullptr && TheGameEngine != nullptr)
	{
		// The refresh too: the window can move to a slower display, or the panel's mode change,
		// with the cap untouched. Two SDL getters a frame (SDL3GameEngine::getDisplayRefreshRate).
		static Int s_vsyncCap = -1, s_vsyncHz = -1;	// -1: none seen yet, so the first draw applies it too
		// The limiter's own switches count as no cap too: nothing to be tear-free for if the loop is
		// meant to run free. FramePacer::isActualFramesPerSecondLimitEnabled minus the replay fast mode.
		const Bool limiterOn = TheGlobalData->m_useFpsLimit && TheFramePacer->isFramesPerSecondLimitEnabled()
			&& !TheFramePacer->isScriptFastForward();
		const Int cap = limiterOn ? TheFramePacer->getFramesPerSecondLimit() : 0;
		const Int refreshHz = TheGameEngine->getDisplayRefreshRate();
		if (cap != s_vsyncCap || refreshHz != s_vsyncHz)
		{
			DX8Wrapper::Set_Windowed_VSync(gxMacVSyncFor(cap, refreshHz));
			fprintf(stderr, "[GXVSYNC] %s (max-FPS cap %d, display %d Hz)\n", gxMacVSyncFor(cap, refreshHz) ? "on" : "off", cap, refreshHz);
			fflush(stderr);
			s_vsyncCap = cap;
			s_vsyncHz = refreshHz;
		}
	}
#endif

	// GeneralsX: Sample the frame rate BEFORE the two early returns. Twin of
	// the fix in GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp - see
	// there for the full reasoning. In short: m_averageFPS is the sole input to the lobby-wide rate
	// negotiation via FrameMetrics and ConnectionManager::getMinimumFps, which takes the MINIMUM
	// across peers. A headless peer never reached this call at all and reported its constructor seed
	// forever; a minimised one froze at whatever it last held. Either way one peer pins the rate for
	// everybody. draw() is still called per main-loop iteration when headless, and updateAverageFPS()
	// touches only performance counters, so hoisting it is safe and measures the true loop rate.
	updateAverageFPS();

	extern HWND ApplicationHWnd;
	if (ApplicationHWnd && ::IsIconic(ApplicationHWnd)) {
		return;
	}

	if (TheGlobalData->m_headless)
		return;

	// TheSuperHackers @feature bobtista 10/07/2026 Show messages for screenshots finished by the screenshot thread.
	W3D_UpdateScreenshotMessages();

	if (TheGlobalData->m_enableDynamicLOD && TheGameLogic->getShowDynamicLOD())
	{
		DynamicGameLODLevel lod=TheGameLODManager->findDynamicLODLevel(m_averageFPS);
		TheGameLODManager->setDynamicLODLevel(lod);
	}
	else
	{	//if dynamic LOD is turned off, force highest LOD
		TheGameLODManager->setDynamicLODLevel(DYNAMIC_GAME_LOD_VERY_HIGH);
	}

	if (TheGlobalData->m_terrainLOD == TERRAIN_LOD_AUTOMATIC && TheTerrainRenderObject)
	{
		calculateTerrainLOD();
	}
#ifdef EXTENDED_STATS
AGAIN:
#endif

#ifdef DUMP_PERF_STATS
	if( TheGlobalData->m_dumpPerformanceStatistics )
	{
		TheStatDump.dumpStats();
		TheWritableGlobalData->m_dumpPerformanceStatistics = FALSE;
	}
#endif

	// compute debug statistics for display later
	if ( m_debugDisplayCallback == StatDebugDisplay
#if defined(RTS_DEBUG)
				|| TheGlobalData->m_benchmarkTimer > 0
#endif
			)
	{
		gatherDebugStats();
	}
#ifdef EXTENDED_STATS
	else
	{
		DX8Wrapper::stats.m_showingStats = false;
	}
#endif

#ifdef SAMPLE_DYNAMIC_LIGHT
	Vector3 loc;
	loc = theDynamicLight->Get_Position();
	loc.X += theLightXOffset;
	if(loc.X>128) theLightXOffset = -theLightXOffset;
	if(loc.X<0) theLightXOffset = -theLightXOffset;
	loc.Y += theLightYOffset;
	if(loc.Y>128) theLightYOffset = -theLightYOffset;
	if(loc.Y<0) theLightYOffset = -theLightYOffset;
	theDynamicLight->Set_Position(loc);
#endif


	/// @todo Make more explicit drawing layers(ground, ground UI, objects, object UI, overlay UI)

	///@todo: Ask Vegas why the LOD optimizer hangs particle system.
 	//
  	// Predictive LOD optimizer optimizes the mesh LOD levels to match
  	// the given polygon budget
  	//
	//PredictiveLODOptimizerClass::Optimize_LODs( 5000 );

	Bool freezeTime = TheFramePacer->isTimeFrozen() || TheFramePacer->isGameHalted();

	/// @todo: I'm assuming the first view is our main 3D view.
	W3DView *primaryW3DView=(W3DView *)getFirstView();

	if (!freezeTime && TheScriptEngine->isTimeFast())
	{
		primaryW3DView->updateCameraMovements();  // Update camera motion effects.
		return;
	}

	Debug_Statistics::Begin_Statistics();	//reset all counters (polygons, vertices, etc) before drawing

	//update state of all the terrain tracks (fade, remove, etc.)
	/// @todo: Is there a better place to put per-frame updates like this?

	if(TheGlobalData->m_loadScreenRender != TRUE)
	{

		if (TheTerrainTracksRenderObjClassSystem)
			TheTerrainTracksRenderObjClassSystem->update();

		//Shroud data is needed to render all other views, so handle this first.
		if (TheTerrainRenderObject)
		{
			//update the shroud surface here since it may be needed by reflections
			if (TheTerrainRenderObject->getMap())	//make sure a valid map is loaded into terrain.
			{
				if (TheTerrainRenderObject->getShroud())
				{
					TheTerrainRenderObject->getShroud()->render(primaryW3DView->get3DCamera());
				}
			}
		}
	}

	// GeneralsX: GX_SHOT capture clock. WW3D's animation clock (SyncTime: UV mappers, texture and
	// mesh animation) and the terrain clouds (W3DShaderManager TerrainShader2Stage::updateCloud) advance by
	// getLogicTimeStepMilliseconds() per draw: one logic step scaled by min(1, tick rate / measured render rate). At a
	// render cap equal to the tick rate the pacer's late frames are clipped to one step while its early ones count short,
	// so every run loses a different amount of time, and two captures of one logic frame showed two animation phases
	// (Mini 2026-09-28: same binary, same sim CRCs, mean abs diff 2.8-5.0 over 53-56% of the pixels). While GX_SHOT
	// captures, WW3D steps by the capture clock instead (GameClient.cpp gxRenderNowMs: whole ms of the match's logic frame,
	// counted from the match's frame 0 and still in the shell and while loading), the same clock the other render-side
	// animations read, so the phase is a function of the match's logic frame (scripts/test/gx-shotdiff). The step is the
	// difference of two whole-ms readings of the ABSOLUTE frame, never a sum of per-draw float slices: WW3D::Sync
	// truncates, and three single steps would sum to 49 ms where one triple step gives 50. A
	// draw with no new logic frame reads the same value and steps nothing. Capture runs only: the shell's own animations
	// stand still until the last shot, where capture mode ends (gxShotDrive). gx-shotdiff keys its noise floors to the
	// text of the "capture clock on" line below: change that text whenever the capture clock's behaviour changes.
	if (gxShotCaptureOn())
	{
		static Bool s_said = FALSE;
		if (!s_said)
		{
			s_said = TRUE;
			fprintf(stderr, "[GXSHOT] capture clock on: WW3D and the render-side animations step one logic frame per match frame\n");
			fflush(stderr);
		}
		static UnsignedInt s_msThen = 0;
		const UnsignedInt msNow = gxRenderNowMs();
		WW3D::Update_Logic_Frame_Time((Real)(msNow - s_msThen));
		WW3D::Sync(msNow != s_msThen);
		s_msThen = msNow;
	}
	else
	{
		WW3D::Update_Logic_Frame_Time(TheFramePacer->getLogicTimeStepMilliseconds());

		// TheSuperHackers @info This binds the WW3D update to the logic update.
		WW3D::Sync(TheGameLogic->hasUpdated());
	}

	static Int now;
	now=timeGetTime();

	// GeneralsX: ...but never in frozen time, where no logic frame passes: the simulation ends a
	// camera-only freeze's move before its next update (GameLogic/ScriptedCamera.h), so the draw after the freeze is the one
	// whose freeze loop shows it (a skipped one lost it: camera-sim-B)
	if (TheTacticalView->getTimeMultiplier()>1 && !freezeTime)
	{
		static Int timeMultiplierCounter = 1;
		timeMultiplierCounter--;
		if (timeMultiplierCounter>1)
			return;
		timeMultiplierCounter = TheTacticalView->getTimeMultiplier();
		// limit the framerate, because while fast time is on, the game logic is running as fast as it can.
	}

	do {

		// update all views of the world - recomputes data which will affect drawing
		if (DX8Wrapper::_Get_D3D_Device8() && (DX8Wrapper::_Get_D3D_Device8()->TestCooperativeLevel()) == D3D_OK)
		{	//Checking if we have the device before updating views because the heightmap crashes otherwise while
			//trying to refresh the visible terrain geometry.
//			if(TheGlobalData->m_loadScreenRender != TRUE)
				updateViews();

			if (TheWaterRenderObj && TheGlobalData->m_waterType == 2)
				TheWaterRenderObj->updateRenderTargetTextures(primaryW3DView->get3DCamera());	//do a render into each texture

			//Can't render into textures while rendering to screen so these textures need to be updated
			//before we enter main rendering loop.
			if (TheW3DProjectedShadowManager)
				TheW3DProjectedShadowManager->updateRenderTargetTextures();
		}

		// Switch to offscreen RT AFTER pre-render (shadows/water) completes, BEFORE main render.
		DX8Wrapper::Pillarbox_Begin();

		Debug_Statistics::End_Statistics();	//record number of polygons rendered in RenderTargetTextures.

		//Store number of polygons rendered in renderTargetTextures.
		Int numRenderTargetPolygons=Debug_Statistics::Get_DX8_Polygons();
		Int numRenderTargetVertices=Debug_Statistics::Get_DX8_Vertices();

		// start render block
		#if defined(_ALLOW_DEBUG_CHEATS_IN_RELEASE)
    if ( (TheGameLogic->getFrame() % 30 == 1) || ( ! ( !TheGameLogic->isGamePaused() && TheGlobalData->m_TiVOFastMode) ) )
		#else
	    if ( (TheGameLogic->getFrame() % 30 == 1) || ( ! (!TheGameLogic->isGamePaused() && TheGlobalData->m_TiVOFastMode && TheGameLogic->isInReplayGame())) )
    #endif
		{
			//USE_PERF_TIMER(BigAssRenderLoop)
			static Bool couldRender = true;
			if ((TheGlobalData->m_breakTheMovie == FALSE) && (TheGlobalData->m_disableRender == false) && WW3D::Begin_Render( true, true, Vector3( 0.0f, 0.0f, 0.0f ), TheWaterTransparency->m_minWaterOpacity ) == WW3D_ERROR_OK)
			{

				if(TheGlobalData->m_loadScreenRender == TRUE)
				{
					TheInGameUI->draw();
					if( TheMouse )
						TheMouse->draw();	//keep applying the current cursor style so it remains hidden if needed.
					WW3D::End_Render();
					continue;
				}
				couldRender = true;
				// add the number of verts/polygons drawn before the main scene
				if (numRenderTargetPolygons || numRenderTargetVertices)
					Debug_Statistics::Record_DX8_Polys_And_Vertices(numRenderTargetPolygons,numRenderTargetVertices,ShaderClass::_PresetOpaqueShader);

				// draw all views of the world
				drawViews();

				// draw the user interface
				TheInGameUI->DRAW();

				TheGameClient->DRAW();

				// draw the mouse
				if( TheMouse )
					TheMouse->DRAW();

				if ( m_videoStream && m_videoBuffer )
				{
					// TheSuperHackers @bugfix Mauller 20/07/2025 scale videos based on screen size so they are shown in their original aspect
					drawScaledVideoBuffer( m_videoBuffer, m_videoStream );
				}

				// render letter box before debug display so debug info isn't hidden
				renderLetterBox(now);

				// display cinematicText over the black
				if( m_cinematicText != AsciiString::TheEmptyString && m_cinematicTextDeadline != 0 && timeGetTime() < m_cinematicTextDeadline)
				{
					// GeneralsX: One DisplayString for the caption, kept across frames. A new
					// one was allocated every rendered frame and never freed, so each frame leaked a string and
					// rebuilt its glyph texture; setText/setFont/setWordWrap are no-ops when unchanged.
					if( m_cinematicDisplayString == nullptr )
						m_cinematicDisplayString = TheDisplayStringManager->newDisplayString();
					DisplayString *displayString = m_cinematicDisplayString;

					// set word wrap if necessary

					Int wordWrapWidth = TheDisplay->getWidth() - 20;
					displayString->setWordWrap( wordWrapWidth );
					displayString->setWordWrapCentered( TRUE );

					UnicodeString text;
					text.translate( m_cinematicText );
					displayString->setText( text );
					Color color = GameMakeColor( 255, 255, 255, 255 );  // white
					Color backColor = GameMakeColor( 0, 0, 0, 0 );      // black
					displayString->setFont( m_cinematicFont );
					Int height = TheDisplay->getHeight() * .9;

					Int width;
					if( displayString->getWidth() > TheDisplay->getWidth() )
						width = 20;
					else
						width = ( TheDisplay->getWidth() - displayString->getWidth() ) / 2;
					displayString->draw( width, height, color, backColor );

					// GeneralsX: The per-render decrement is gone; the deadline
					// in the condition above does the work. See Display::setCinematicTextFrames.
				}

				if ( m_debugDisplayCallback )
				{
					// draw the current debug display
					drawCurrentDebugDisplay();
				}

#if defined(RTS_DEBUG)
				if (TheGlobalData->m_benchmarkTimer > 0)
				{
					drawFPSStats();
				}
#endif


#if defined(RTS_DEBUG)
				if (TheGlobalData->m_debugShowGraphicalFramerate)
				{
					drawFramerateBar();
				}
#endif

#ifdef PERF_TIMERS
				TheGraphDraw->render();
				TheGraphDraw->clear();
#endif

#ifdef PROFILER_ENABLED
				if (m_profilerFrameCapture && !TheGlobalData->m_headless)
				{
					m_profilerFrameCapture->Capture(getWidth(), getHeight());
				}
#endif
				// render is all done!
				WW3D::End_Render();
				gxShotDrawRendered();	// GeneralsX: GX_SHOT reads only a rendered frame (GameClient.cpp)
			}
			else
			{
				if (couldRender)
				{
					couldRender = false;
					DEBUG_LOG(("Could not do WW3D::Begin_Render()!  Are we ALT-Tabbed out?"));
				}
			}
		}

		if (TheScriptEngine->isTimeFrozenDebug() || TheScriptEngine->isTimeFrozenScript() || TheGameLogic->isGamePaused())
		{
			freezeTime = false; // We're frozen for debug or for pause, and need to continue out of the loop.
		}

	// GeneralsX: Never repeat this loop while the game is HALTED (network stall, or SP pause).
	// Upstream reference: Nagyhoho1234, PR #2939 (closed unmerged) https://github.com/TheSuperHackers/GeneralsGameCode/pull/2939
	// Nothing inside draw() can end a halt - Network::update and logic run outside it - and the camera
	// zoom/pitch/rotate counters only advance on a new logic frame (W3DView::updateCameraMovements), so a
	// Ctrl+Numpad rotate still running when a lockstep stall began spun here forever. Only a genuine time
	// freeze (SP script/debug freeze; always false in MP) may loop. Also closes upstream #1995.
	} while (freezeTime && !TheFramePacer->isGameHalted() && !TheTacticalView->isCameraMovementFinished());

#ifdef EXTENDED_STATS
	if (DX8Wrapper::stats.m_disableOverhead) {
		goto AGAIN;
	}
#endif
}

#define LETTER_BOX_FADE_TIME	1000.0f		///1000 ms.

/** Render letter-box border at top/bottom of display
*/
void W3DDisplay::renderLetterBox(UnsignedInt currentTime)
{
		if (m_letterBoxEnabled)
		{	if (m_letterBoxFadeLevel != 1.0f)
			{
				m_letterBoxFadeLevel = (currentTime - m_letterBoxFadeStartTime)/LETTER_BOX_FADE_TIME;
				if (m_letterBoxFadeLevel > 1.0f)
					m_letterBoxFadeLevel = 1.0f;
			}

			UnsignedInt lbcolor = (Int)(m_letterBoxFadeLevel * 255.0f) << 24;

#ifdef SLIDE_LETTERBOX
			Int height = (Int)(getHeight() * 0.12f * m_letterBoxFadeLevel);
			TheTacticalView->setOrigin(0, height);
#else
			drawFillRect( 0, 0, m_width, (m_height-(9.0f/16.0f * m_width))*0.5f, lbcolor );
			drawFillRect( 0, m_height-(m_height-(9.0f/16.0f * m_width))*0.5f, m_width, m_height, lbcolor );
#endif
		}
		else
		{	//letter box is disabled, but may still be fading out
			if (m_letterBoxFadeLevel != 0.0f)
			{
				m_letterBoxFadeLevel = 1.0f - (currentTime - m_letterBoxFadeStartTime)/LETTER_BOX_FADE_TIME;
				if (m_letterBoxFadeLevel < 0.0f)
					m_letterBoxFadeLevel = 0.0f;

				UnsignedInt lbcolor = (Int)(m_letterBoxFadeLevel * 255.0f) << 24;

#ifdef SLIDE_LETTERBOX
				Int height = (Int)(getHeight() * 0.12f * m_letterBoxFadeLevel);
				TheTacticalView->setOrigin(0, height);
#else
				drawFillRect( 0, 0, m_width, (m_height-(9.0f/16.0f * m_width))*0.5f, lbcolor );
				//drawFillRect( 0, m_height-(m_height-(9.0f/16.0f * m_width))*0.5f, m_width, m_height, lbcolor );
#endif
			}
			else
			{	//box has finished fading out
#ifdef SLIDE_LETTERBOX
				TheTacticalView->setOrigin(0, 0);
#else
				m_letterBoxEnabled = FALSE;
#endif
			}
		}
}

Bool W3DDisplay::isLetterBoxFading()
{
	if (m_letterBoxEnabled && m_letterBoxFadeLevel != 1.0f)
		return TRUE;
	if (!m_letterBoxEnabled && m_letterBoxFadeLevel != 0.0f)
		return TRUE;
	return FALSE;
}

//WST 10/2/2002 added query function.  JSC Integrated 5/20/03
Bool W3DDisplay::isLetterBoxed()
{
	return (m_letterBoxEnabled);
}

// W3DDisplay::createLightPulse ===============================================
/** Create a "light pulse" which is a dynamic light that grows, decays
	* and vanishes over several frames */
//=============================================================================
void W3DDisplay::createLightPulse( const Coord3D *pos, const RGBColor *color,
																	 Real innerRadius, Real attenuationWidth,
																	 UnsignedInt increaseFrameTime,
																	 UnsignedInt decayFrameTime//, Bool donut
																	 )
{
	if (m_3DScene == nullptr)
		return;
	if (innerRadius+attenuationWidth<2.0*PATHFIND_CELL_SIZE_F + 1.0f) {
		return; // it basically won't make any visual difference.  jba.
	}
	W3DDynamicLight * theDynamicLight = m_3DScene->getADynamicLight();
	// turn it on.
	theDynamicLight->setEnabled(true);

	theDynamicLight->Set_Ambient( Vector3( color->red, color->green, color->blue ) );
	theDynamicLight->Set_Diffuse( Vector3( color->red, color->green, color->blue) );
	theDynamicLight->Set_Position(Vector3(pos->x, pos->y, pos->z));
	theDynamicLight->Set_Far_Attenuation_Range(innerRadius, innerRadius + attenuationWidth);
	theDynamicLight->setFrameFade(increaseFrameTime, decayFrameTime);
	theDynamicLight->setDecayRange();
	theDynamicLight->setDecayColor();
	//theDynamicLight->setDonut(donut);
}

void W3DDisplay::toggleLetterBox()
{
	m_letterBoxEnabled = !m_letterBoxEnabled;
	m_letterBoxFadeStartTime = timeGetTime();
}

void W3DDisplay::enableLetterBox(Bool enable)
{
	if (enable)
	{
		if (!m_letterBoxEnabled)
		{	//letterbox mode not previously enabled
			m_letterBoxEnabled = TRUE;
			m_letterBoxFadeStartTime = timeGetTime();
		}
	}
	else
	{
		if (m_letterBoxEnabled)
		{	//letterbox mode no previously disabled
			m_letterBoxEnabled = FALSE;
			m_letterBoxFadeStartTime = timeGetTime();
		}
	}
}

// W3DDisplay::setTimeOfDay ===================================================
/** */
//=============================================================================
void W3DDisplay::setTimeOfDay( TimeOfDay tod )
{
	const GlobalData::TerrainLighting *ol=&TheGlobalData->m_terrainObjectsLighting[tod][0];

	if( m_3DScene )
	{
		m_3DScene->Set_Ambient_Light( Vector3(ol->ambient.red, ol->ambient.green, ol->ambient.blue) );
	}

	for (Int i=0; i<LightEnvironmentClass::MAX_LIGHTS; i++)
	{
		if( m_myLight[i] )
		{
			ol=&TheGlobalData->m_terrainObjectsLighting[tod][i];

			m_myLight[i]->Set_Ambient( Vector3( 0.0f, 0.0f, 0.0f ) );
			m_myLight[i]->Set_Diffuse( Vector3(ol->diffuse.red, ol->diffuse.green, ol->diffuse.blue ) );
			m_myLight[i]->Set_Specular( Vector3(0,0,0) );
			Matrix3D mtx;
			mtx.Set(Vector3(1,0,0), Vector3(0,1,0), Vector3(ol->lightPos.x, ol->lightPos.y, ol->lightPos.z), Vector3(0,0,0));
			m_myLight[i]->Set_Transform(mtx);
		}
	}
	if(TheTerrainRenderObject) {
		TheTerrainRenderObject->setTimeOfDay(tod);
		TheTacticalView->forceRedraw();
	}
}

// W3DDisplay::drawLine =======================================================
/** draw a line on the display in pixel coordinates with the specified color */
//=============================================================================
void W3DDisplay::drawLine( Int startX, Int startY,
													 Int endX, Int endY,
													 Real lineWidth,
													 UnsignedInt lineColor )
{
	setup2DRenderState(nullptr, DRAW_IMAGE_ALPHA, FALSE, 4, 6);

	m_2DRender->Add_Line( Vector2( startX, startY ), Vector2( endX, endY ),
												lineWidth, lineColor );
	if (!m_isBatching)
	{
		m_2DRender->Render();
	}

}

// W3DDisplay::drawLine =======================================================
/** draw a line on the display in pixel coordinates with the specified color */
//=============================================================================
void W3DDisplay::drawLine( Int startX, Int startY,
													 Int endX, Int endY,
													 Real lineWidth,
													 UnsignedInt lineColor1,UnsignedInt lineColor2 )
{
	setup2DRenderState(nullptr, DRAW_IMAGE_ALPHA, FALSE, 4, 6);

	m_2DRender->Add_Line( Vector2( startX, startY ), Vector2( endX, endY ),
												lineWidth, lineColor1, lineColor2 );

	if (!m_isBatching)
	{
		m_2DRender->Render();
	}

}


// W3DDisplay::drawOpenRect ===================================================
//=============================================================================
void W3DDisplay::drawOpenRect( Int startX, Int startY, Int width, Int height,
															 Real lineWidth, UnsignedInt lineColor )
{

	if (m_isClippedEnabled)
	{
		ICoord2D start, end, returnStart, returnEnd;
		start.x = startX;
		start.y = startY;

		end.x = start.x;
		end.y = start.y + height;
		if(ClipLine2D(&start, &end, &returnStart, &returnEnd, &m_clipRegion ))
			drawLine( returnStart.x, returnStart.y, returnEnd.x, returnEnd.y, lineWidth, lineColor);

		end.x = start.x + width;
		end.y = start.y;
		if(ClipLine2D(&start, &end, &returnStart, &returnEnd, &m_clipRegion ))
			drawLine( returnStart.x, returnStart.y, returnEnd.x, returnEnd.y, lineWidth, lineColor);

		start.x = startX + width;
		start.y = startY;
		end.x = start.x;
		end.y = start.y + height;
		if(ClipLine2D(&start, &end, &returnStart, &returnEnd, &m_clipRegion ))
			drawLine( returnStart.x, returnStart.y, returnEnd.x, returnEnd.y, lineWidth, lineColor);

		start.x = startX;
		start.y = startY + height;
		end.x = start.x + width;
		end.y = start.y;
		if(ClipLine2D(&start, &end, &returnStart, &returnEnd, &m_clipRegion ))
			drawLine( returnStart.x, returnStart.y, returnEnd.x, returnEnd.y, lineWidth, lineColor);
	}
	else
	{
		setup2DRenderState(nullptr, DRAW_IMAGE_ALPHA, FALSE, 16, 24);

		m_2DRender->Add_Outline( RectClass( startX, startY,
																				startX + width, startY + height ),
														 lineWidth, lineColor );

		if (!m_isBatching)
		{
			m_2DRender->Render();
		}
	}

}

// W3DDisplay::drawFillRect ===================================================
//=============================================================================
void W3DDisplay::drawFillRect( Int startX, Int startY, Int width, Int height,
															 UnsignedInt color )
{
	setup2DRenderState(nullptr, DRAW_IMAGE_ALPHA, FALSE, 4, 6);

	m_2DRender->Add_Rect( RectClass( startX, startY,
																	 startX + width, startY + height ),
												0, 0, color );

	if (!m_isBatching)
	{
		m_2DRender->Render();
	}
}

void W3DDisplay::drawRectClock(Int startX, Int startY, Int width, Int height, Int percent, UnsignedInt color)
{
	// sanity
	if(percent < 1 || percent > 100)
		return;

// The rectanges are numberd as follows
//(x,y)	|---------|
//			| 4  | 1  |
//			|----+----|
//			| 3  | 2  |
//			|---------| (x + width, y + width)
//
	// we're done, lets just draw one rectangle for it all.
	if(percent == 100)
	{
		setup2DRenderState(nullptr, DRAW_IMAGE_ALPHA, FALSE, 4, 6);

		m_2DRender->Add_Rect(RectClass( startX, startY,
																		startX + width, startY + height), 0,0, color);
	}
	else if( percent> 75)
	{
		Int triangleCount;
		if (percent > 87)
		{
			triangleCount = 2;
		}
		else
		{
			triangleCount = 1;
		}
		setup2DRenderState(nullptr, DRAW_IMAGE_ALPHA, FALSE, 8 + 3*triangleCount, 12 + 3*triangleCount);

		//rectangle #1 & 2
		m_2DRender->Add_Rect(RectClass( startX + width/2, startY,
																		startX + width, startY + height), 0,0, color);
		// rectangle #3
		m_2DRender->Add_Rect(RectClass( startX, startY + height/2,
																		startX + width/2, startY + height), 0,0, color);
		// draw the part of rectangle 4
		Real remain = percent - 75;
		if(remain > 12)
		{
			//draw the full triangle
			m_2DRender->Add_Tri(Vector2(startX, startY),
													Vector2(startX, startY + height/2),
													Vector2(startX + width/2, startY + height/2),
													Vector2(0,0),Vector2(0,0),Vector2(0,0),color);

			// draw the part of triangle
			Real percentDraw = (Real)(remain - 12)/ 13;
			m_2DRender->Add_Tri(Vector2(startX, startY),
													Vector2(startX + width/2, startY + height/2),
													Vector2(startX + (width/2 * percentDraw), startY),
													Vector2(0,0),Vector2(0,0),Vector2(0,0),color);
		}
		else
		{
			// draw the part of triangle
			Real percentDraw = (Real)(remain)/ 12;
			m_2DRender->Add_Tri(Vector2(startX, startY + height/2 - (height/2 * percentDraw)),
													Vector2(startX, startY + height/2),
													Vector2(startX + width/2, startY + height/2),
													Vector2(0,0),Vector2(0,0),Vector2(0,0),color);
		}

	}
	else if( percent > 50)
	{
		Int triangleCount;
		if (percent > 62)
		{
			triangleCount = 2;
		}
		else
		{
			triangleCount = 1;
		}
		setup2DRenderState(nullptr, DRAW_IMAGE_ALPHA, FALSE, 4 + 3*triangleCount, 6 + 3*triangleCount);

		//rectangle #1 & 2
		m_2DRender->Add_Rect(RectClass( startX + width/2, startY,
																		startX + width, startY + height), 0,0, color);
		// draw the part of rectangle 3
		Real remain = percent - 50;
		if(remain > 12)
		{
			//draw the full triangle
			m_2DRender->Add_Tri(Vector2(startX + width/2, startY + height/2),
													Vector2(startX, startY + height),
													Vector2(startX + width/2, startY + height),
													Vector2(0,0),Vector2(0,0),Vector2(0,0),color);

			// draw the part of triangle
			Real percentDraw = (Real)(remain - 12)/ 13;
			m_2DRender->Add_Tri(Vector2(startX, startY + height - (height/2 * percentDraw)),
													Vector2(startX, startY + height),
													Vector2(startX + width/2, startY + height/2),
													Vector2(0,0),Vector2(0,0),Vector2(0,0),color);
		}
		else
		{
			// draw the part of triangle
			Real percentDraw = (Real)(remain)/ 12;
			m_2DRender->Add_Tri(Vector2(startX + width/2, startY + height),
													Vector2(startX + width/2, startY + height/2),
													Vector2(startX + width/2 - ( width/2 * percentDraw), startY + height),
													Vector2(0,0),Vector2(0,0),Vector2(0,0),color);
		}
	}
	else if(percent > 25)
	{
		Int triangleCount;
		if (percent > 37)
		{
			triangleCount = 2;
		}
		else
		{
			triangleCount = 1;
		}
		setup2DRenderState(nullptr, DRAW_IMAGE_ALPHA, FALSE, 4 + 3*triangleCount, 6 + 3*triangleCount);

		// rectangel #1
		m_2DRender->Add_Rect(RectClass( startX + width/2, startY,
																		startX + width, startY + height/2), 0,0, color);
		// draw the part of rectangle 2
		Real remain = percent - 25;
		if(remain > 12)
		{
			//draw the full triangle
			m_2DRender->Add_Tri(Vector2(startX + width/2, startY + height/2),
													Vector2(startX + width, startY + height),
													Vector2(startX + width, startY + height/2),
													Vector2(0,0),Vector2(0,0),Vector2(0,0),color);

			// draw the part of triangle
			Real percentDraw = (Real)(remain - 12)/ 13;
			m_2DRender->Add_Tri(Vector2(startX + width/2, startY + height/2),
													Vector2(startX + width - (width/2 * percentDraw), startY + height),
													Vector2(startX + width, startY + height),
													Vector2(0,0),Vector2(0,0),Vector2(0,0),color);
		}
		else
		{
			// draw the part of triangle
			Real percentDraw = (Real)(remain)/ 12;
			m_2DRender->Add_Tri(Vector2(startX + width, startY + height/2),
													Vector2(startX + width/2, startY + height/2),
													Vector2(startX + width, startY + height/2 + ( height/2 * percentDraw)),
													Vector2(0,0),Vector2(0,0),Vector2(0,0),color);
		}
	}
	else
	{
		Int triangleCount;
		if (percent > 12)
		{
			triangleCount = 2;
		}
		else
		{
			triangleCount = 1;
		}
		setup2DRenderState(nullptr, DRAW_IMAGE_ALPHA, FALSE, 3*triangleCount, 3*triangleCount);

				// draw the part of rectangle 1

		if(percent > 12)
		{
			//draw the full triangle
			m_2DRender->Add_Tri(Vector2(startX + width/2, startY),
													Vector2(startX + width/2, startY + height/2),
													Vector2(startX + width, startY),
													Vector2(0,0),Vector2(0,0),Vector2(0,0),color);

			// draw the part of triangle
			Real percentDraw = (Real)(percent - 12)/ 13;
			m_2DRender->Add_Tri(Vector2(startX + width, startY),
													Vector2(startX + width/2, startY + height/2),
													Vector2(startX + width, startY + (height/2 * percentDraw)),
													Vector2(0,0),Vector2(0,0),Vector2(0,0),color);
		}
		else
		{
			// draw the part of triangle
			Real percentDraw = (Real)(percent)/ 12;
			m_2DRender->Add_Tri(Vector2(startX + width/2, startY),
													Vector2(startX + width/2, startY + height/2),
													Vector2(startX + width/2 + (width/2 * percentDraw), startY ),
													Vector2(0,0),Vector2(0,0),Vector2(0,0),color);
		}
	}

	if (!m_isBatching)
	{
		m_2DRender->Render();
	}
}


//--------------------------------------------------------------------------------------------------------------------
// W3DDisplay::drawRemainingRectClock
// Variation added by Kris -- October 2002
// This version will overlay a clock progress from the specified percentage to 100%. Essentially, this function will
// "reveal" an icon as it progresses towards completion.
//--------------------------------------------------------------------------------------------------------------------
void W3DDisplay::drawRemainingRectClock(Int startX, Int startY, Int width, Int height, Int percent, UnsignedInt color)
{
	// sanity
	if( percent < 0 || percent > 99 )
		return;

// The rectanges are numbered as follows
//(x,y)	|---------|
//			| 4  | 1  |
//			|----+----|
//			| 3  | 2  |
//			|---------| (x + width, y + width)
//

	Int midX = startX + width/2;
	Int midY = startY + height/2;
	Int endX = startX + width;
	Int endY = startY + height;
	Int halfWidth = width/2;
	Int halfHeight = height/2;

	if( percent == 0 )
	{
		setup2DRenderState(nullptr, DRAW_IMAGE_ALPHA, FALSE, 4, 6);

		// We just started, so draw the entire remaining rectangle.
		// #1, #2, #3, and #4
		m_2DRender->Add_Rect( RectClass( startX, startY, endX, endY ), 0, 0, color );
	}
	else if( percent < 25 )
	{
		Int triangleCount;
		if (percent < 13)
		{
			triangleCount = 2;
		}
		else
		{
			triangleCount = 1;
		}
		setup2DRenderState(nullptr, DRAW_IMAGE_ALPHA, FALSE, 8 + 3*triangleCount, 12 + 3*triangleCount);

		//1-25%
		//-----

		//Rectangle #3 & 4
		m_2DRender->Add_Rect( RectClass( startX, startY, midX, endY ), 0, 0, color );

		//Rectangle #2
		m_2DRender->Add_Rect( RectClass( midX, midY, endX, endY ), 0, 0, color );

		//Handle rectangle #1 than needs partial rendering.
		if( percent < 13 )
		{
			//1-12%
  		//-----

			//Draw the 2nd half of rectangle #1
			m_2DRender->Add_Tri( Vector2( midX, midY ), Vector2( endX, midY ), Vector2( endX, startY ),
													 Vector2( 0, 0 ), Vector2( 0, 0 ), Vector2( 0, 0 ), color );

			//Draw the last part of the 1st portion of rectangle #1
			Real percentDraw = (Real)( 13 - percent ) / 13;
			m_2DRender->Add_Tri( Vector2( midX, midY ), Vector2( endX, startY ), Vector2( endX - halfWidth * percentDraw, startY ),
													 Vector2( 0, 0 ), Vector2( 0, 0 ), Vector2( 0, 0 ), color );
		}
		else
		{
			//13-24%
			//------

			//Draw the last part of the 2nd half of rectangle #1
			Real percentDraw = (Real)( percent - 13 ) / 12;
			m_2DRender->Add_Tri( Vector2( midX, midY ), Vector2( endX, midY ), Vector2( endX, startY + halfHeight * percentDraw ),
													 Vector2( 0, 0 ), Vector2( 0, 0 ), Vector2( 0, 0 ), color );
		}
	}
	else if( percent < 50 )
	{
		Int triangleCount;
		if (percent < 38)
		{
			triangleCount = 2;
		}
		else
		{
			triangleCount = 1;
		}
		setup2DRenderState(nullptr, DRAW_IMAGE_ALPHA, FALSE, 4 + 3*triangleCount, 6 + 3*triangleCount);

		//25-49%
		//------

		//rectangle #3 & 4
		m_2DRender->Add_Rect( RectClass( startX, startY, midX, endY ), 0, 0, color );

		//Handle rectangle #2 that needs partial rendering.
		if( percent < 38 )
		{
			//25-37%
  		//-----

			//Draw the 2nd half of rectangle #2
			m_2DRender->Add_Tri( Vector2( midX, midY ), Vector2( midX, endY ), Vector2( endX, endY ),
													 Vector2( 0, 0 ), Vector2( 0, 0 ), Vector2( 0, 0 ), color );

			//Draw the last part of the 1st portion of rectangle #2
			Real percentDraw = (Real)( percent - 25 ) / 13;
			m_2DRender->Add_Tri( Vector2( midX, midY ), Vector2( endX, endY ), Vector2( endX, midY + halfHeight * percentDraw ),
													 Vector2( 0, 0 ), Vector2( 0, 0 ), Vector2( 0, 0 ), color );
		}
		else
		{
			//38-49%
			//------

			//Draw the last part of the 2nd half of rectangle #1
			Real percentDraw = (Real)( percent - 38 ) / 12;
			m_2DRender->Add_Tri( Vector2( midX, midY ), Vector2( midX, endY ), Vector2( endX - halfWidth * percentDraw, endY ),
													 Vector2( 0, 0 ), Vector2( 0, 0 ), Vector2( 0, 0 ), color );
		}
	}
	else if( percent < 75 )
	{
		Int triangleCount;
		if (percent < 63)
		{
			triangleCount = 2;
		}
		else
		{
			triangleCount = 1;
		}
		setup2DRenderState(nullptr, DRAW_IMAGE_ALPHA, FALSE, 4 + 3*triangleCount, 6 + 3*triangleCount);

		//50-74%
		//------

		//Rectangle #4
		m_2DRender->Add_Rect( RectClass( startX, startY, midX, midY ), 0, 0, color );

		//Handle rectangle #3 that needs partial rendering.
		if( percent < 63 )
		{
			//50-62%
  		//-----

			//Draw the 2nd half of rectangle #3
			m_2DRender->Add_Tri( Vector2( midX, midY ), Vector2( startX, midY ), Vector2( startX, endY ),
													 Vector2( 0, 0 ), Vector2( 0, 0 ), Vector2( 0, 0 ), color );

			//Draw the last part of the 1st portion of rectangle #3
			Real percentDraw = (Real)( percent - 50 ) / 13;
			m_2DRender->Add_Tri( Vector2( midX, midY ), Vector2( startX, endY ), Vector2( midX - halfWidth * percentDraw, endY ),
													 Vector2( 0, 0 ), Vector2( 0, 0 ), Vector2( 0, 0 ), color );
		}
		else
		{
			//62-74%
			//------

			//Draw the last part of the 2nd half of rectangle #3
			Real percentDraw = (Real)( percent - 62 ) / 12;
			m_2DRender->Add_Tri( Vector2( midX, midY ), Vector2( startX, midY ), Vector2( startX, endY - halfHeight * percentDraw ),
													 Vector2( 0, 0 ), Vector2( 0, 0 ), Vector2( 0, 0 ), color );
		}
	}
	else
	{
		Int triangleCount;
		if (percent < 87)
		{
			triangleCount = 2;
		}
		else
		{
			triangleCount = 1;
		}
		setup2DRenderState(nullptr, DRAW_IMAGE_ALPHA, FALSE, 3*triangleCount, 3*triangleCount);

		//75-99%
		//------

		//Handle rectangle #4 that needs partial rendering.
		if( percent < 87 )
		{
			//75-87%
  		//-----

			//Draw the 2nd half of rectangle #4
			m_2DRender->Add_Tri( Vector2( midX, midY ), Vector2( midX, startY ), Vector2( startX, startY ),
													 Vector2( 0, 0 ), Vector2( 0, 0 ), Vector2( 0, 0 ), color );

			//Draw the last part of the 1st portion of rectangle #4
			Real percentDraw = (Real)( percent - 75 ) / 13;
			m_2DRender->Add_Tri( Vector2( midX, midY ), Vector2( startX, startY ), Vector2( startX, midY - halfHeight * percentDraw ),
													 Vector2( 0, 0 ), Vector2( 0, 0 ), Vector2( 0, 0 ), color );
		}
		else
		{
			//88-99%
			//------

			//Draw the last part of the 2nd half of rectangle #4
			Real percentDraw = (Real)( percent - 88 ) / 12;
			m_2DRender->Add_Tri( Vector2( midX, midY ), Vector2( midX, startY ), Vector2( startX + halfWidth * percentDraw, startY ),
													 Vector2( 0, 0 ), Vector2( 0, 0 ), Vector2( 0, 0 ), color );
		}
	}

	if (!m_isBatching)
	{
		m_2DRender->Render();
	}
}


// W3DDisplay::drawImage ======================================================
/** Draws an images at the screen coordinates and keeps it within the end
	* screen coords specified */
//=============================================================================
void W3DDisplay::drawImage( const Image *image, Int startX, Int startY,
														Int endX, Int endY, Color color, DrawImageMode mode)
{

	// sanity
	if( image == nullptr )
		return;

	if (m_isClippedEnabled)
	{
		if (endX <= m_clipRegion.lo.x ||
			endY <= m_clipRegion.lo.y ||
			startX >= m_clipRegion.hi.x ||
			startY >= m_clipRegion.hi.y)
		{
			return;	//nothing to render
		}
	}

	// !!
	// Remember to update the GUIEditDisplay::drawImage when you make
	// changes to this, it technically uses W3D code to render itself,
	// but it not derived on the W3DDisplay
	// !!

	const Region2D *uv = image->getUV();

	TextureClass *tex = nullptr;
	if (BitIsSet(image->getStatus(), IMAGE_STATUS_RAW_TEXTURE))
		tex = (TextureClass *)(image->getRawTextureData());
	else
		tex = WW3DAssetManager::Get_Instance()->Get_Texture(image->getFilename().str(), MIP_LEVELS_1);

	Bool grayscale = (mode == DRAW_IMAGE_GRAYSCALE);
	Int vertexCount;
	if (BitIsSet(image->getStatus(), IMAGE_STATUS_ROTATED_90_CLOCKWISE))
	{
		vertexCount = 6;
	}
	else
	{
		vertexCount = 4;
	}
	setup2DRenderState(tex, mode, grayscale, vertexCount, 6);

	RectClass screen_rect(startX,startY,endX,endY);
	RectClass uv_rect(uv->lo.x,uv->lo.y,uv->hi.x,uv->hi.y);

	if (m_isClippedEnabled)
	{	//need to clip this quad to clip rectangle
		if (screen_rect.Left < m_clipRegion.lo.x || screen_rect.Right > m_clipRegion.hi.x || screen_rect.Top < m_clipRegion.lo.y || screen_rect.Bottom > m_clipRegion.hi.y)
		{
			RectClass clipped_rect;
			RectClass clipped_uv_rect;

			if( BitIsSet( image->getStatus(), IMAGE_STATUS_ROTATED_90_CLOCKWISE ) )
			{


				//
				//	Clip the polygons to the specified area
				//

				// GeneralsX: Use MAX/MIN macros (cross-platform, defined in BaseTypeCore.h)
				clipped_rect.Left		= MAX (screen_rect.Left, m_clipRegion.lo.x);
				clipped_rect.Right	= MIN (screen_rect.Right, m_clipRegion.hi.x);
				clipped_rect.Top		= MAX (screen_rect.Top, m_clipRegion.lo.y);
				clipped_rect.Bottom	= MIN (screen_rect.Bottom, m_clipRegion.hi.y);

				//
				//	Clip the texture to the specified area
				//

				float percent				= ((clipped_rect.Left - screen_rect.Left) / screen_rect.Width ());
				clipped_uv_rect.Top		= uv_rect.Top + (uv_rect.Height () * percent);

				percent						= ((clipped_rect.Right - screen_rect.Left) / screen_rect.Width ());
				clipped_uv_rect.Bottom	= uv_rect.Top + (uv_rect.Height () * percent);

				percent						= ((clipped_rect.Top - screen_rect.Top) / screen_rect.Height ());
				clipped_uv_rect.Right	= uv_rect.Right - (uv_rect.Width () * percent);

				percent						= ((clipped_rect.Bottom - screen_rect.Top) / screen_rect.Height ());
				clipped_uv_rect.Left		= uv_rect.Right - (uv_rect.Width () * percent);
			}
			else

			{

				//
				//	Clip the polygons to the specified area
				//

				// GeneralsX: Use MAX/MIN macros (cross-platform, defined in BaseTypeCore.h)
				clipped_rect.Left		= MAX (screen_rect.Left, m_clipRegion.lo.x);
				clipped_rect.Right	= MIN (screen_rect.Right, m_clipRegion.hi.x);
				clipped_rect.Top		= MAX (screen_rect.Top, m_clipRegion.lo.y);
				clipped_rect.Bottom	= MIN (screen_rect.Bottom, m_clipRegion.hi.y);

				//
				//	Clip the texture to the specified area
				//

				float percent				= ((clipped_rect.Left - screen_rect.Left) / screen_rect.Width ());
				clipped_uv_rect.Left		= uv_rect.Left + (uv_rect.Width () * percent);

				percent						= ((clipped_rect.Right - screen_rect.Left) / screen_rect.Width ());
				clipped_uv_rect.Right	= uv_rect.Left + (uv_rect.Width () * percent);

				percent						= ((clipped_rect.Top - screen_rect.Top) / screen_rect.Height ());
				clipped_uv_rect.Top		= uv_rect.Top + (uv_rect.Height () * percent);

				percent						= ((clipped_rect.Bottom - screen_rect.Top) / screen_rect.Height ());
				clipped_uv_rect.Bottom	= uv_rect.Top + (uv_rect.Height () * percent);
			}

			//
			//	Use the clipped rectangles to render
			//
			screen_rect = clipped_rect;
			uv_rect		= clipped_uv_rect;
		}
	}

	// if rotated 90 degrees clockwise we have to adjust the uv coords
	if( BitIsSet( image->getStatus(), IMAGE_STATUS_ROTATED_90_CLOCKWISE ) )
	{

		m_2DRender->Add_Tri( Vector2( screen_rect.Left, screen_rect.Top ),
												 Vector2( screen_rect.Left, screen_rect.Bottom ),
												 Vector2( screen_rect.Right, screen_rect.Top ),
												 Vector2( uv_rect.Right, uv_rect.Top),
												 Vector2( uv_rect.Left, uv_rect.Top),
												 Vector2( uv_rect.Right, uv_rect.Bottom ),
												 color );

		m_2DRender->Add_Tri( Vector2( screen_rect.Right, screen_rect.Bottom ),
												 Vector2( screen_rect.Right, screen_rect.Top ),
												 Vector2( screen_rect.Left, screen_rect.Bottom ),
												 Vector2( uv_rect.Left, uv_rect.Bottom ),
												 Vector2( uv_rect.Right, uv_rect.Bottom ),
												 Vector2( uv_rect.Left, uv_rect.Top ),
												 color );

	}
	else
	{

		// just draw as normal
		m_2DRender->Add_Quad( screen_rect, uv_rect, color );

	}

	if (!m_isBatching)
	{
		m_2DRender->Render();
		m_2DRender->Enable_Grayscale(false);
		if (mode == DRAW_IMAGE_ADDITIVE || mode == DRAW_IMAGE_SOLID)
		{
			m_2DRender->Enable_Alpha(true);
		}
	}

	if (tex != nullptr && !BitIsSet(image->getStatus(), IMAGE_STATUS_RAW_TEXTURE))
	{
		tex->Release_Ref();
	}

}

//============================================================================
// W3DDisplay::createVideoBuffer
//============================================================================

VideoBuffer*	W3DDisplay::createVideoBuffer()
{
	VideoBuffer::Type format = VideoBuffer::TYPE_UNKNOWN;

	/// @todo query video player for supported formats - we assume bink formats here

	// first try to use the native format

	WW3DFormat displayFormat = DX8Wrapper::getBackBufferFormat();

	if ( DX8Wrapper::Get_Current_Caps()->Support_Texture_Format( displayFormat ))
	{
		format = W3DVideoBuffer::W3DFormatToType( displayFormat );
	}

	if ( format == VideoBuffer::TYPE_UNKNOWN )
	{
		if ( DX8Wrapper::Get_Current_Caps()->Support_Texture_Format( WW3D_FORMAT_X8R8G8B8 ))
		{
			format = VideoBuffer::TYPE_X8R8G8B8;
		}
		else if ( DX8Wrapper::Get_Current_Caps()->Support_Texture_Format( WW3D_FORMAT_R8G8B8 ))
		{
			format = VideoBuffer::TYPE_R8G8B8;
		}
		else if ( DX8Wrapper::Get_Current_Caps()->Support_Texture_Format( WW3D_FORMAT_R5G6B5 ))
		{
			format = VideoBuffer::TYPE_R5G6B5;
		}
		else if ( DX8Wrapper::Get_Current_Caps()->Support_Texture_Format( WW3D_FORMAT_X1R5G5B5 ))
		{
			format = VideoBuffer::TYPE_X1R5G5B5;
		}
		else
		{
			// card does not support any of the formats we need
			return nullptr;
		}
	}
	// on low mem machines, render every video in 16bit
	if (TheGameLODManager && (!TheGameLODManager->didMemPass() || W3DShaderManager::getChipset() == DC_GEFORCE2))
		format = VideoBuffer::TYPE_R5G6B5;

	W3DVideoBuffer *buffer = NEW W3DVideoBuffer( format );

	return buffer;
}

//============================================================================
// W3DDisplay::drawScaledVideoBuffer
//============================================================================

void W3DDisplay::drawScaledVideoBuffer( VideoBuffer *buffer, VideoStreamInterface *stream )
{
	// TheSuperHackers @bugfix Mauller 20/07/2025 scale videos based on screen size so they are shown in their original aspect
	Real videoAspect = (Real)stream->width() / (Real)stream->height();
	Real displayAspect = (Real)getWidth() / (Real)getHeight();
	Bool wideAspect = displayAspect >= videoAspect;

	Int startX = 0;
	Int endX = 0;
	Int startY = 0;
	Int endY = 0;

	if (wideAspect)
	{
		// TheSuperHackers @info if we are in a wide aspect, we scale the videos width and fill the height
		Real heightScale = (Real)getHeight() / (Real)stream->height();
		startX = (getWidth() / 2.0f) - (stream->width() * heightScale / 2.0f);
		endX = (getWidth() / 2.0f) + (stream->width() * heightScale / 2.0f);

		endY = getHeight();
	}
	else
	{
		// TheSuperHackers @info if we are in a narrow aspect, we scale the videos height and fill the width
		Real widthScale = (Real)getWidth() / (Real)stream->width();
		startY = (getHeight() / 2.0f) - (stream->height() * widthScale / 2.0f);
		endY = (getHeight() / 2.0f) + (stream->height() * widthScale / 2.0f);

		endX = getWidth();
	}

	drawVideoBuffer( buffer, startX, startY, endX, endY );
}

//============================================================================
// W3DDisplay::drawVideoBuffer
//============================================================================

void W3DDisplay::drawVideoBuffer( VideoBuffer *buffer, Int startX, Int startY, Int endX, Int endY )
{
	W3DVideoBuffer *vbuffer = (W3DVideoBuffer*) buffer;

	setup2DRenderState(vbuffer->texture(), DRAW_IMAGE_ALPHA, FALSE, 4, 6);

	m_2DRender->Add_Quad( RectClass( startX, startY, endX, endY ),
												vbuffer->Rect( 0, 0, 1, 1) );
	
	if (!m_isBatching)
	{
		m_2DRender->Render();
	}

}

// W3DDisplay::setClipRegion ============================================
/** Set the clipping region for images.
  @todo: Make this work for all primitives, not just drawImage. */
//=============================================================================
void W3DDisplay::setClipRegion( IRegion2D *region )
{
		// assign new region
		m_clipRegion = *region;
		m_isClippedEnabled = TRUE;

}

//=============================================================================
/* we don't really need to override this call, since we will soon be called to
	update every shroud cell explicitly...
*/
void W3DDisplay::clearShroud()
{
	// nothing
}

//=============================================================================
void W3DDisplay::setBorderShroudLevel(UnsignedByte level)
{
	if (TheTerrainRenderObject && TheTerrainRenderObject->getShroud())
	{
		TheTerrainRenderObject->getShroud()->setBorderShroudLevel((W3DShroudLevel)level);
	}
}

//=============================================================================
void W3DDisplay::setShroudLevel( Int x, Int y, CellShroudStatus setting )
{
	if (TheTerrainRenderObject && TheTerrainRenderObject->getShroud())
	{
		#ifdef INTENSE_DEBUG
		TheTerrainRenderObject->getShroud()->setShroudFilter(false);
		#endif
		if( setting == CELLSHROUD_SHROUDED )
			TheTerrainRenderObject->getShroud()->setShroudLevel(x, y, (W3DShroudLevel)TheGlobalData->m_shroudAlpha );
		else if( setting == CELLSHROUD_FOGGED )
			TheTerrainRenderObject->getShroud()->setShroudLevel(x, y, (W3DShroudLevel)TheGlobalData->m_fogAlpha );///< @todo placeholder to get feedback on logic work while graphic side being decided
		else
			TheTerrainRenderObject->getShroud()->setShroudLevel(x, y, (W3DShroudLevel)TheGlobalData->m_clearAlpha );
		//Logic is saying shroud.  We can add alpha levels here in client if needed.
		// W3DShroud is a 0-255 alpha byte.  Logic shroud is a double reference count.

		// GeneralsX: As in ZH: re-read each prop's shroud status. Props now skip
		// the shroud pass when fully clear (W3DPropBuffer::drawProps), so a stale clear status would draw a
		// fogged prop unfogged. This only sets a flag; drawProps re-reads the props once per frame.
		TheTerrainRenderObject->notifyShroudChanged();
	}
}

/** Start/Stop capturing an AVI movie*/
void W3DDisplay::toggleMovieCapture()
{
	WW3D::Toggle_Movie_Capture("Movie",30);
}

void W3DDisplay::takeScreenShot(ScreenshotFormat format, Int jpegQuality)
{
	W3D_TakeCompressedScreenshot(format, jpegQuality);
}


#if defined(RTS_DEBUG)

static FILE *AssetDumpFile=nullptr;

void dumpMeshAssets(MeshClass *mesh)
{
	if (mesh)
	{
		TextureClass *texture;
		//MaterialInfoClass	*material = mesh->Get_Material_Info();
		MeshModelClass *model=mesh->Get_Model();
		for (int stage=0;stage<MeshMatDescClass::MAX_TEX_STAGES;++stage)
		{
			for (int pass=0;pass<model->Get_Pass_Count();++pass)
			{
				if (model->Has_Texture_Array(pass,stage))
				{
					for (int i=0;i<model->Get_Polygon_Count();++i)
					{
						if ((texture=model->Peek_Texture(i,pass,stage)) != nullptr)
						{
							fprintf(AssetDumpFile,"\t%s\n",texture->Get_Texture_Name().str());
						}
					}
				}
				else
				{
					if ((texture=model->Peek_Single_Texture(pass,stage)) != nullptr)
					{
						fprintf(AssetDumpFile,"\t%s\n",texture->Get_Texture_Name().str());
					}
				}
			}
		}
	}
}

void dumpHLODAssets(HLodClass *hlod)
{
	if (hlod)
	{
		//model composed of multiple meshes.
		for (Int i=0; i<hlod->Get_Num_Sub_Objects(); i++)
		{
			RenderObjClass *subObj=hlod->Get_Sub_Object(i);
			if (subObj->Class_ID() == RenderObjClass::CLASSID_HLOD)
				dumpHLODAssets((HLodClass *)subObj);
			else
			if (subObj->Class_ID() == RenderObjClass::CLASSID_MESH)
				dumpMeshAssets((MeshClass *)subObj);
		}
	}
}

//-------------------------------------------------------------------------------------------------
/**  dump all used models/textures to a file.*/
//-------------------------------------------------------------------------------------------------
void W3DDisplay::dumpModelAssets(const char *path)
{
	if (m_3DScene)
	{
		AssetDumpFile=fopen(path,"w");
		if (AssetDumpFile)
		{
			fprintf(AssetDumpFile,"Models and Textures used on %s:\n\n",TheGlobalData->m_mapName.str());
			SceneIterator *sceneIter = m_3DScene->Create_Iterator();
			sceneIter->First();
			while(!sceneIter->Is_Done())
			{
				RenderObjClass * robj = sceneIter->Current_Item();
				if (robj->Class_ID() == RenderObjClass::CLASSID_HLOD)
				{	fprintf(AssetDumpFile,"%s.W3D:\n",robj->Get_Name());
					dumpHLODAssets((HLodClass *)robj);
				}
				else
				if (robj->Class_ID() == RenderObjClass::CLASSID_MESH)
				{	fprintf(AssetDumpFile,"%s.W3D:\n",robj->Get_Name());
					dumpMeshAssets((MeshClass *)robj);
				}
				sceneIter->Next();
			}
			m_3DScene->Destroy_Iterator(sceneIter);
			fclose(AssetDumpFile);
		}
	}
}
#endif	//only include above code in debug and internal
//-------------------------------------------------------------------------------------------------
/** Preload using the W3D asset manager the model referenced by the string parameter */
//-------------------------------------------------------------------------------------------------
void W3DDisplay::preloadModelAssets( AsciiString model )
{

	if( m_assetManager )
	{
		AsciiString nameWithExtension;

		nameWithExtension.format( "%s.w3d", model.str() );
		m_assetManager->Load_3D_Assets( nameWithExtension.str() );

	}

}

//-------------------------------------------------------------------------------------------------
/** Preload using the W3D asset manager the texture referenced by the string parameter */
//-------------------------------------------------------------------------------------------------
void W3DDisplay::preloadTextureAssets( AsciiString texture )
{

	if( m_assetManager )
	{
		TextureClass *theTexture = m_assetManager->Get_Texture( texture.str() );
		theTexture->Release_Ref();//release reference
	}

}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void W3DDisplay::doSmartAssetPurgeAndPreload(const char* usageFileName)
{
	if (!m_assetManager || !usageFileName || !*usageFileName)
		return;

	DynamicVectorClass<StringClass> names(8000);

	// use TheFileSystem here so we can bigify these files
	File* f = TheFileSystem->openFile(usageFileName, File::READ | File::TEXT);
	if (f)
	{
		for (;;)
		{
			AsciiString tmp;
			if (f->scanString(tmp) == FALSE)
				break;

			// allow for comments in the file. Note that this doesn't allow for comments
			// with spaces! doh. oh well. better than nothing.
			if (tmp.str()[0] == ';')
				continue;

			names.Add(StringClass(tmp.str()));
		}
		f->close();
	}

	// just free everything if there's no exclusion list file (send in an empty list)
	m_assetManager->Free_Assets_With_Exclusion_List(names);
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
#if defined(RTS_DEBUG)
void W3DDisplay::dumpAssetUsage(const char* mapname)
{
	if (!m_assetManager || !mapname || !*mapname)
		return;

	DynamicVectorClass<StringClass> names(8000);
	m_assetManager->Create_Asset_List(names);

	const char* leafname = strrchr(mapname, '\\');
	if (leafname)
		++leafname;					// point to first character after the last backslash
	else
		leafname = mapname;		// point to the start of the filename

	char buf[256];
	int idx = 1;
	while (true)
	{
		sprintf(buf, "AssetUsage_%s_%04d.txt",leafname,idx);
		if (_access(buf, 0) != 0)
			break;	// it exists, we're good
		++idx;
	}

	FILE *fp = fopen(buf, "w");
	if (fp)
	{
		for (int i=0; i<names.Count(); i++)
		{
			const char* n = names[i];
			fprintf(fp, "%s\n", n);
		}
		fclose(fp);
	}
}
#endif

//-------------------------------------------------------------------------------------------------
static void drawFramerateBar()
{
	static DWORD prevTime = timeGetTime();
	DWORD now = timeGetTime();
	// GeneralsX: 0 is the unlimited cap now (Common/FpsLimitPolicy.h) and an unlimited loop
	// can draw twice in one millisecond, so this divided by zero twice over (NaN width and colour). Measure against the
	// tick rate when there is no cap, as the average-FPS seed does, and never against a zero interval.
	const Int targetFps = (TheGlobalData->m_framesPerSecondLimit > 0) ? TheGlobalData->m_framesPerSecondLimit : (Int)LOGICFRAMES_PER_SECOND;
	const DWORD elapsedMs = (now > prevTime) ? (now - prevTime) : 1;
	Real percTime = (1000.0f / elapsedMs ) / (1000.0f / targetFps);

	if (percTime > 1.0f)
		percTime = 1.0f;
	else if (percTime < 0.0f)
		percTime = 0.0f;
	Int width = REAL_TO_INT(percTime * TheDisplay->getWidth());
	UnsignedInt colorToUse = GameMakeColor( REAL_TO_UNSIGNEDBYTE((1.0f - percTime) * 255),
																					REAL_TO_UNSIGNEDBYTE(percTime * 255),
																					0,
																					0x7F);

	TheDisplay->drawFillRect(1, 1, width, 15, colorToUse);
	prevTime = now;
}
