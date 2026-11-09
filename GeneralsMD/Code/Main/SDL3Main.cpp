/*
**	Command & Conquer Generals Zero Hour(tm)
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

/*
** SDL3Main.cpp
**
** Entry point for Linux builds using SDL3 windowing and DXVK graphics.
**
** TheSuperHackers @feature CnC_Generals_Linux 07/02/2026
** Entry point replaces WinMain() for Linux builds.
** Instantiates SDL3GameEngine and calls GameMain().
*/

#ifndef _WIN32

// SYSTEM INCLUDES
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#ifdef GX_RENDER_NATIVE
#include "VkWsi.h"	// GeneralsX @feature E · rw3-K 11/10/2026 sdl3PreferVulkanLoader
#include "W3DDevice/GameClient/GxNativeInstall.h"	// GeneralsX @feature E · rw3-L-3 12/10/2026 FlushFinalStats
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
// On iOS, SDL renames main() to SDL_main and provides its own UIApplicationMain
// bootstrap; the app lifecycle (suspend/resume, window) is owned by SDL.
#include <SDL3/SDL_main.h>
#include <cerrno>
#include <sys/stat.h>
#include <fcntl.h>
#include <filesystem>
#include <string>
#endif
#include <cstdlib>
#include <cctype>
#include <filesystem>        // GeneralsX @bugfix Claude 24/09/2026 the DXVK folder, on every POSIX target
#include <initializer_list>
#include <string>
#include <system_error>
#include <cstring>
#include <cstdio>
#include <unistd.h>   // _exit()
#include <dlfcn.h>
#include <dirent.h>
#include <strings.h>  // strcasecmp
#include <glob.h>     // glob() for Vulkan ICD discovery

// USER INCLUDES (match WinMain.cpp pattern)
#include "Lib/BaseType.h"
#include "Common/CommandLine.h"
#include "Common/CriticalSection.h"
#include "Common/GlobalData.h"
#include "Common/GameEngine.h"
#include "Common/GameMemory.h"
#include "Common/Debug.h"
#include "Common/version.h"  // GeneralsX @bugfix BenderAI 14/02/2026 Version class + TheVersion extern
#include "GXStderrLog.h"     // GeneralsX @feature Claude 17/08/2026 desync evidence on disk
#include "GXCrashLog.h"      // GeneralsX @feature Claude 25/09/2026 one crash log per crash, in our folder
#include "GameClient/SaveLoadFeedback.h"  // TheGXCrashLogWriter
#include "gitinfo.h"
#include "GameClient/GXUserDataDir.h"  // GeneralsX @feature Claude 23/09/2026 GX_USERDATA_DIR (iOS Options.ini seed)
#include "GameLogic/GXHookTimer.h"  // GeneralsX @test Claude 23/10/2026 ai-wh1ter0se-econ: GX_HOOK_FLUSH_ALL before _exit (test-instruments builds)
#include "SDL3GameEngine.h"

// DXVK WSI
#define DXVK_WSI_SDL3 1
#include <wsi/native_wsi.h>

// CRITICAL SECTIONS (Linux needs these too)

// GeneralsX @bugfix Claude 04/08/2026 Build these handlers on Linux as well as iOS.
//
// The Linux peer installed no fatal-signal handler at all, so its crashes left only the
// kernel's one-line dmesg record. Reconstructing a symbol from that line means taking the
// fault ip, subtracting the VMA base the kernel prints, and then remembering to ADD the
// executable segment's vaddr, because the binary is PIE. On 2026-08-04 a session skipped
// that last term, resolved an address 0x252000 short of the real one, and got
// INI::parseAnim2DDefinition - load-time code that could not possibly be running at frame
// 10791. Both addr2line and nm "confirmed" it, because both were handed the same wrong
// address, and the impossible answer was written up as evidence of memory corruption. The
// real fault was a plain null dereference in ParticleSystem::attachToObject.
//
// One backtrace at the moment of the crash removes that entire class of arithmetic.
//
// GeneralsX @bugfix Claude 25/09/2026 And on macOS, and actually INSTALLED on Linux. The only call
// to installCrashHandlers() was inside the iOS block of main(), so Linux compiled these and never
// used them, and macOS did not compile them at all. Both now install them after the command line
// is parsed, where the user-data folder is known and GXCrashLog can write into it.
#if !defined(_WIN32)
#define GX_HAVE_CRASH_BACKTRACE 1
#endif

#if defined(GX_HAVE_CRASH_BACKTRACE)
#include <csignal>
#include <execinfo.h>
#include <exception>

// Fatal-signal reporting.
//
// A crash used to leave nothing behind: the log simply stopped mid-line and the
// cause had to be inferred from whatever happened to be the last thing written.
// These handlers make every fatal signal announce itself, with a backtrace, into
// the session log — Documents on iOS, plain stderr on Linux, which is what the
// test harnesses already capture.
//
// This does not prevent crashes — nothing can, in a codebase this size — it
// guarantees you are never left guessing about one.
//
// Everything here must be async-signal-safe: raw write(), no printf, no malloc.
// backtrace()/backtrace_symbols_fd() are safe on Darwin by design. On glibc they
// are NOT, strictly: the first backtrace() dlopens libgcc to get the unwinder,
// which allocates. installCrashHandlers() below forces that work to happen at
// startup so the handler itself never takes that path.
//
// Linux symbolisation note: backtrace_symbols_fd resolves only dynamic symbols,
// so most frames print as GeneralsXZH(+0xOFFSET). That offset is already relative
// to the image base and can be fed straight to addr2line with no PIE arithmetic —
// which is the entire point.
static int s_crashLogFd = -1;

static void crashWrite(const char *s)
{
	if (s_crashLogFd >= 0) { (void)!write(s_crashLogFd, s, strlen(s)); }
	(void)!write(STDERR_FILENO, s, strlen(s));
}

static void crashSignalHandler(int sig)
{
	const char *name = "UNKNOWN";
	switch (sig) {
		case SIGSEGV: name = "SIGSEGV (invalid memory access)";      break;
		case SIGBUS:  name = "SIGBUS (bad memory alignment/access)"; break;
		case SIGABRT: name = "SIGABRT (abort / failed assertion)";   break;
		case SIGILL:  name = "SIGILL (illegal instruction)";         break;
		case SIGFPE:  name = "SIGFPE (arithmetic error)";            break;
		case SIGTRAP: name = "SIGTRAP (trap)";                       break;
		default: break;
	}
	// GeneralsX @perf Claude 02/10/2026 Lines still queued for the stderr pump go out before ours.
	GXStderrLog_Settle();
	void *frames[64];
	const int n = backtrace(frames, 64);
	// GeneralsX @feature Claude 25/09/2026 The crash's own file first: the stderr mirror below is
	// rotated by the next launch, this one is kept until the launcher has asked about it.
	GXCrashLog_Write(name, frames, n);

	crashWrite("\nFATAL: caught ");
	crashWrite(name);
	crashWrite("\nFATAL: backtrace follows\n");
	if (s_crashLogFd >= 0) backtrace_symbols_fd(frames, n, s_crashLogFd);
	backtrace_symbols_fd(frames, n, STDERR_FILENO);
	crashWrite("FATAL: end of backtrace\n");

	// Restore the default action and re-raise so the OS still records its own
	// crash report (Settings > Privacy & Security > Analytics Data).
	signal(sig, SIG_DFL);
	raise(sig);
}

static void crashTerminateHandler()
{
	GXStderrLog_Settle();  // GeneralsX @perf Claude 02/10/2026 queued lines first, as above
	void *frames[64];
	const int n = backtrace(frames, 64);
	GXCrashLog_Write("std::terminate (unhandled C++ exception)", frames, n);
	crashWrite("\nFATAL: std::terminate (unhandled C++ exception)\n");
	if (s_crashLogFd >= 0) backtrace_symbols_fd(frames, n, s_crashLogFd);
	backtrace_symbols_fd(frames, n, STDERR_FILENO);
	crashWrite("FATAL: end of backtrace\n");
	// GeneralsX @bugfix Claude 25/09/2026 abort(), not _exit(1), now that macOS and Linux install
	// this too: an unhandled exception has always ended there in SIGABRT, which harnesses and the
	// OS crash report both key on. SIG_DFL first so the SIGABRT handler does not log it twice.
	signal(SIGABRT, SIG_DFL);
	abort();
}

static void installCrashHandlers()
{
	// Force the unwinder to load NOW, while we are still allowed to allocate. On glibc the
	// first backtrace() dlopens libgcc; doing that for the first time inside a SIGSEGV
	// handler is how a crash reporter turns a crash into a hang or a second crash.
	{
		void *warm[4];
		(void)backtrace(warm, 4);
	}

	const int sigs[] = { SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE, SIGTRAP };
	for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); ++i) {
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = crashSignalHandler;
		sa.sa_flags = SA_RESETHAND | SA_NODEFER;
		sigemptyset(&sa.sa_mask);
		sigaction(sigs[i], &sa, nullptr);
	}
	std::set_terminate(crashTerminateHandler);
}

// "GeneralsXZH <commit> <platform> <arch>": the header line of every crash log (GXCrashLog.h).
static const char *crashBuildLine()
{
	static char line[160];
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	const char *platform = "ios";
#elif defined(__APPLE__)
	const char *platform = "macos";
#else
	const char *platform = "linux";
#endif
#if defined(__aarch64__) || defined(__arm64__)
	const char *arch = "arm64";
#elif defined(__x86_64__)
	const char *arch = "x86_64";
#else
	const char *arch = "other";
#endif
	snprintf(line, sizeof(line), "GeneralsXZH %s%s %s %s", GitShortSHA1, GitUncommittedChanges ? "~" : "", platform, arch);
	return line;
}
#endif // GX_HAVE_CRASH_BACKTRACE

static CriticalSection critSec1;
static CriticalSection critSec2;
static CriticalSection critSec3;
static CriticalSection critSec4;
static CriticalSection critSec5;

// GLOBAL COMMAND LINE ARGUMENTS
// TheSuperHackers @build felipebraz 13/02/2026
// Store argc/argv from main() for use by CommandLine.cpp parseCommandLine() on Linux
// Windows provides these automatically; Linux needs explicit globals
int __argc = 0;          ///< global argument count
char** __argv = nullptr; ///< global argument vector

// GLOBAL WINDOW HANDLE
// TheSuperHackers @build felipebraz 13/02/2026
// ApplicationHWnd is declared extern in GeneralsMD/Code/Main/WinMain.h
// On Linux, we cast SDL_Window* to HWND type for compatibility
HWND ApplicationHWnd = nullptr;  ///< our application window handle

// GLOBAL SDL3 WINDOW
// GeneralsX @feature felipebraz 16/02/2026
// SDL3 window created in main() before GameMain(), stored globally for engine access
SDL_Window* TheSDL3Window = nullptr;

// GAME TEXT FILE PATHS
// TheSuperHackers @build felipebraz 13/02/2026
// GameText.cpp uses these paths to load CSF and STR files (game localization)
// Format %s is replaced with language code in GameTextManager::init()
// GeneralsX @bugfix BenderAI 13/02/2026 - Fix case-sensitivity on Linux (generals.csf vs Generals.csf)
const Char *g_csfFile = "data/%s/generals.csf";  ///< CSF file path (lowercase for Linux compatibility)
const Char *g_strFile = "data/Generals.str";     ///< STR file path

// Extern declarations (from GameMain.cpp)
extern Int GameMain();

/**
 * FilterSoftwareVulkanICDs
 *
 * Sets VK_DRIVER_FILES to only hardware Vulkan ICDs, excluding LLVMpipe/lavapipe.
 *
 * Workaround for Mesa/LLVM 20.x bug: libvulkan_lvp.so (LLVMpipe Vulkan ICD) crashes
 * during dlopen() static initialization with a null-ptr deref in llvm::Regex::Regex().
 * The Vulkan loader loads ALL ICDs found in the ICD directories when
 * vkEnumerateInstanceExtensionProperties() is called, which triggers the crash.
 * Filtering hardware-only ICDs via VK_DRIVER_FILES prevents loading libvulkan_lvp.so.
 *
 * Only applied when neither VK_DRIVER_FILES nor VK_ICD_FILENAMES is already set,
 * so the user can always override by setting those variables externally.
 *
 * GeneralsX @bugfix BenderAI 06/03/2026
 */
static void FilterSoftwareVulkanICDs()
{
	if (getenv("VK_DRIVER_FILES") || getenv("VK_ICD_FILENAMES")) {
		return;
	}

	auto icd_is_software = [](const char *name) -> bool {
		char low[256] = "";
		for (int i = 0; name[i] && i < 255; ++i) {
			low[i] = (char)tolower((unsigned char)name[i]);
		}
		return strstr(low, "lvp") || strstr(low, "lavapipe") || strstr(low, "softpipe") || strstr(low, "llvmpipe");
	};

	static char hw_icds[4096] = "";
	const char *patterns[] = {
		"/usr/share/vulkan/icd.d/*.json",
		"/etc/vulkan/icd.d/*.json",
		nullptr
	};

	glob_t gl = {};
	int gflags = 0;
	for (int i = 0; patterns[i]; ++i) {
		if (glob(patterns[i], gflags, nullptr, &gl) == 0) {
			gflags = GLOB_APPEND;
		}
	}

	bool found_hw = false;
	for (size_t i = 0; i < gl.gl_pathc; ++i) {
		const char *path = gl.gl_pathv[i];
		const char *base = strrchr(path, '/');
		base = base ? base + 1 : path;
		if (icd_is_software(base)) {
			fprintf(stderr, "INFO: Vulkan ICD filter: skipping software ICD '%s'\n", base);
			continue;
		}
		if (found_hw) {
			strncat(hw_icds, ":", sizeof(hw_icds) - strlen(hw_icds) - 1);
		}
		strncat(hw_icds, path, sizeof(hw_icds) - strlen(hw_icds) - 1);
		found_hw = true;
	}
	globfree(&gl);

	if (found_hw) {
		setenv("VK_DRIVER_FILES", hw_icds, 1);
		fprintf(stderr, "INFO: Vulkan ICD filter: VK_DRIVER_FILES=%s\n", hw_icds);
	} else {
		fprintf(stderr, "WARNING: Vulkan ICD filter: no hardware ICDs found, LLVMpipe exclusion skipped\n");
		fprintf(stderr, "WARNING: If startup crashes in libvulkan_lvp.so, set VK_DRIVER_FILES manually\n");
	}
}

/**
 * FilterPipeWireOpenAL
 *
 * Sets ALSOFT_DRIVERS to skip PipeWire, falling back to pulse/alsa.
 *
 * Workaround for openal-soft PipeWire backend crash: alcOpenDevice() segfaults
 * inside the PipeWire backend while opening the default playback device.
 * The crash occurs in PipeWire's stream/context internals and is unrecoverable
 * from userspace. Excluding PipeWire via ALSOFT_DRIVERS causes openal-soft to
 * fall back to the PulseAudio backend, which works correctly on PipeWire systems
 * via the PulseAudio compatibility layer.
 *
 * NOTE: openal-soft reads ALSOFT_DRIVERS from a static global constructor when
 * libopenal.so is loaded by the dynamic linker, which is before main() runs.
 * This function is therefore only effective for builds that use lazy
 * initialization. The authoritative fix is in the launch scripts (run-linux-zh.sh
 * etc.), which set ALSOFT_DRIVERS before the binary starts.
 *
 * Only applied when ALSOFT_DRIVERS is not already set by the user.
 *
 * GeneralsX @bugfix 09/03/2026
 */
static void FilterPipeWireOpenAL()
{
	// GeneralsX @bugfix Copilot 24/03/2026 PipeWire/OpenAL workaround is Linux-only; keep macOS CoreAudio backend selection untouched.
	#if defined(__linux__)
	// Crash: alcOpenDevice() hits 'movaps %xmm1,0x26260(%rbx)' — SSE movaps requires
	// 16-byte alignment; a misaligned ALCdevice struct faults regardless of backend.
	// Disabling CPU extensions forces openal-soft to use scalar code that has no
	// alignment requirements. Also exclude pipewire which has its own crash at
	// device-open time on PipeWire 1.4.x.
	// NOTE: these env vars are authoritative only when set before the binary loads
	// (openal-soft reads them from a static constructor). The launch scripts set them
	// first; this is a best-effort fallback for lazy-init builds.
	if (!getenv("ALSOFT_DISABLE_CPU_EXTS")) {
		setenv("ALSOFT_DISABLE_CPU_EXTS", "all", 1);
		fprintf(stderr, "INFO: OpenAL: ALSOFT_DISABLE_CPU_EXTS=all (movaps alignment crash workaround)\n");
	}
	if (!getenv("ALSOFT_DRIVERS")) {
		setenv("ALSOFT_DRIVERS", "pulse,alsa,oss,jack,null,wave", 1);
		fprintf(stderr, "INFO: OpenAL: ALSOFT_DRIVERS=pulse,alsa,oss,jack,null,wave (pipewire excluded)\n");
	}
	#else
	fprintf(stderr, "INFO: OpenAL: keeping default driver selection on non-Linux platform\n");
	#endif
}

/**
 * CreateGameEngine
 *
 * Factory function for SDL3GameEngine on Linux.
 * Called by GameMain() to instantiate platform-specific engine.
 *
 * @return SDL3GameEngine instance
 */
GameEngine *CreateGameEngine(void)
{
	fprintf(stderr, "INFO: CreateGameEngine() - Creating SDL3GameEngine for Linux\n");
	SDL3GameEngine *engine = NEW SDL3GameEngine();
	return engine;
}

// GeneralsX @perf Claude 01/10/2026 A2: a fresh install starts from the shipped pipeline record (resources/dxvk/seed: the
// DXVK state cache plus the fixed-function shader keys the patched Apple DXVK warms from, see
// LoadScreen::waitForPipelineWarmup), so even its first match compiles its pipelines during the load screen. Each file
// is copied only when absent: a player's own record grows from there and is never replaced. Before the d3d8 dylib
// creates its device, which is when DXVK reads both.
static void gxSeedDxvkCache(const char *seedDir)
{
	const char *cacheDir = getenv("DXVK_STATE_CACHE_PATH");
	if (seedDir == nullptr || *seedDir == '\0' || cacheDir == nullptr || *cacheDir == '\0')
		return;
	std::error_code dirError;
	std::filesystem::create_directories(cacheDir, dirError);	// a caller's DXVK_STATE_CACHE_PATH need not exist yet
	for (const char *name : { "GeneralsXZH.dxvk-cache", "GeneralsXZH.dxvk-ffkeys" })
	{
		const std::filesystem::path src = std::filesystem::path(seedDir) / name;
		const std::filesystem::path dst = std::filesystem::path(cacheDir) / name;
		std::error_code ec;
		if (std::filesystem::exists(dst, ec) || !std::filesystem::exists(src, ec))
			continue;
		std::filesystem::copy_file(src, dst, ec);
		fprintf(stderr, "[GXWARM] seeded %s from %s%s\n", dst.c_str(), seedDir, ec ? " FAILED" : "");
	}
}

/**
 * main
 *
 * Linux entry point (replaces WinMain on Windows).
 * Initializes subsystems and calls GameMain().
 *
 * @param argc Command line argument count
 * @param argv Command line arguments
 * @return Exit code (0 = success)
 */
int main(int argc, char* argv[])
{
	int exitcode = 1;

	// TheSuperHackers @build felipebraz 13/02/2026
	// Store command line arguments in globals for CommandLine.cpp parser
	__argc = argc;
	__argv = argv;

#if !defined(_WIN32) && !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
	// GeneralsX @bugfix Claude 24/09/2026 DXVK WRITES INTO THE WORKING DIRECTORY UNLESS TOLD OTHERWISE, AND ON macOS AND
	// LINUX THE WORKING DIRECTORY IS THE ZERO HOUR FOLDER - often one GeneralsX does not own.
	//
	// fbraz3-dxvk opens <exe>_d3d9.log (src/util/log/log.cpp getFileName) and <exe>.dxvk-cache
	// (src/dxvk/dxvk_state_cache.cpp getCacheDir) relative to the cwd when DXVK_LOG_PATH / DXVK_STATE_CACHE_PATH are
	// empty. The .app exports neither, and the Linux launchers even export DXVK_LOG_LEVEL=info. Both paths are read
	// LAZILY - on the first log line and at device creation - so setting them here, before the renderer exists, is in
	// time. (DXVK_LOG_LEVEL is not: the logger reads it in a static constructor at dylib load, which is why it is not
	// touched here; where the log goes is what matters.) Never over a value the caller set, and dxvk.conf is untouched.
	//
	// The folder is GeneralsX's own user-data root, resolved the way GlobalData::BuildUserDataPathFromRegistry resolves
	// it. ponytail: a second derivation of that root; foreign-folder-engine-test.sh check 5 pins the two together, and a
	// shared helper is the upgrade if a third copy appears. Windows is not here: its cwd is the install folder we own.
	{
		std::string dxvkDir;
		if (!gxUserDataDirOverride(dxvkDir))
		{
			const char *home = getenv("HOME");
#if defined(__APPLE__)
			if (home != nullptr && *home != '\0')
				dxvkDir = std::string(home) + "/Library/Application Support/GeneralsX/GeneralsZH/";
#else
			const char *xdg = getenv("XDG_DATA_HOME");
			if (xdg != nullptr && *xdg != '\0')
				dxvkDir = std::string(xdg) + "/GeneralsX/GeneralsZH/";
			else if (home != nullptr && *home != '\0')
				dxvkDir = std::string(home) + "/.local/share/GeneralsX/GeneralsZH/";
#endif
		}
		if (!dxvkDir.empty())
		{
			if (dxvkDir.back() != '/')
				dxvkDir += '/';
			dxvkDir += "dxvk/";
			std::error_code ec;
			std::filesystem::create_directories(dxvkDir, ec);
			// A caller's value is kept - but an EMPTY one is not a value: DXVK reads "" exactly as unset and
			// falls back to the cwd (review of a0510e7b6, P3-1).
			for (const char *name : { "DXVK_LOG_PATH", "DXVK_STATE_CACHE_PATH" })
			{
				const char *had = getenv(name);
				if (had == nullptr || *had == '\0')
					setenv(name, dxvkDir.c_str(), 1);
			}
		}
		const char *logPath = getenv("DXVK_LOG_PATH");
		const char *cachePath = getenv("DXVK_STATE_CACHE_PATH");
		fprintf(stderr, "[GXDXVK] log=%s cache=%s\n", logPath ? logPath : "(unset)", cachePath ? cachePath : "(unset)");
		// The .app's run.sh names Resources/dxvk-seed; deploy-macos-zh.sh puts it in the game folder, the cwd here,
		// next to the dxvk.conf it deploys there (Codex a2-r5).
		const char *seedDir = getenv("GX_DXVK_SEED_DIR");
		gxSeedDxvkCache(seedDir != nullptr ? seedDir : "dxvk-seed");
	}
#endif

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	// Diagnostic capture: an icon-launched app's stderr goes nowhere we can read,
	// so mirror it to a file in Library/Caches (purgeable, not user-visible). This
	// lets us pull a full engine log after an on-device session — essential for
	// debugging mode-specific issues (e.g. Generals Challenge radar/scripts) that
	// only the user can reproduce. Pull with: devicectl ... copy from
	// Library/Caches/generals-stderr.log. Remove once the relevant bugs are fixed.
	{
		// Quiet DXVK at the source: the d3d8 layer's per-call warns (e.g. an
		// unimplemented render state set every frame) wrote hundreds of MB per
		// long session. The shipped dxvk.conf also sets logLevel=none; the env
		// covers modules that read it before the config.
		setenv("DXVK_LOG_LEVEL", "none", 0);

		// GeneralsX @bugfix Claude 28/09/2026 ONE MoltenVK, not two. The executable force-loads the
		// build host's static libMoltenVK.a (cmake/config-build.cmake, for SDL3), while DXVK dlopens the
		// pinned framework in Frameworks/ (Patches/dxvk-ios.patch) and creates the VkInstance with it.
		// SDL then created the surface through the STATIC copy's vkGetInstanceProcAddr on the
		// framework's instance - two MoltenVK versions (1.4.2 static, 1.4.1 framework in the 22/09
		// package) sharing objects whose layout only happens to agree, and two copies reading any
		// MVK_CONFIG_*. SDL's UIKit loader takes SDL_HINT_VULKAN_LIBRARY before its RTLD_DEFAULT probe,
		// so naming the framework here puts SDL on DXVK's copy. Only when the framework is there:
		// without it the static copy is the one that works, as before. The [GXMVK] line after
		// SDL_Vulkan_LoadLibrary says which copy SDL actually got; nothing is printed here, because this
		// runs before the stderr mirror below exists and an icon launch's bare stderr goes nowhere.
		{
			const char *base = SDL_GetBasePath();
			std::string mvk = std::string(base ? base : "") + "Frameworks/MoltenVK.framework/MoltenVK";
			if (base != nullptr && access(mvk.c_str(), R_OK) == 0)
				SDL_SetHint(SDL_HINT_VULKAN_LIBRARY, mvk.c_str());
		}

		// GeneralsX @feature Claude 07/08/2026 Turn the network trace on by default, on iOS only.
		//
		// [GXSTALL], [GXNET] and the LAN traces are all gated on getenv("GX_NET") - and an
		// icon-launched, sideloaded iOS app has no way to be given an environment variable, so the
		// iPad has NEVER emitted any of them. Every stall number we have is the Linux host's view
		// of the iPad; the iPad's own view has been structurally unavailable.
		//
		// That matters because the iPad is the peer that blocked 23 of 35 stalls, a 4.6:1 skew.
		// The host's trace cannot tell "the iPad hitched" from "the iPad's packet was late" -
		// only iter_max_ms on the iPad itself can, and that is exactly the field needed to decide
		// whether the remaining short stalls are a send-deferral problem or a main-loop problem.
		//
		// Cheap: [GXSTALL] fires per stall event, not per frame, and the sink below is filtered
		// and capped at 8 MB. Uses overwrite=0 so an explicit value still wins.
		setenv("GX_NET", "1", 0);

		const char *diagHome = getenv("HOME");
		if (diagHome != nullptr) {
			// Documents, not Library/Caches: Caches is purgeable (a device restart or
			// storage pressure can empty it), and Documents is user-reachable via the
			// Files app since the bundle enables UIFileSharingEnabled. That is also why
			// this install happens HERE rather than through the common call site after
			// the command line is parsed — the engine's user-data path on iOS is inside
			// Library/Application Support, which the Files app cannot see.
			//
			// GeneralsX @refactor Claude 17/08/2026 The sink itself (rotation, filter,
			// 8 MB cap, error lines past the cap) moved verbatim into GXStderrLog.cpp so
			// Windows and macOS get the same policy rather than a re-derived one. One
			// behaviour did change and it was a latent bug: the past-cap allow-list was
			// err:/ERROR/FATAL, which would have dropped the [CRC] MISMATCH lines that
			// are the entire point of keeping a log.
			//
			// GeneralsX @bugfix Claude 23/09/2026 Under GX_USERDATA_DIR, in its folder
			// (GameClient/GXUserDataDir.h): the rotation below deleted the owner's last log.
			if (GXStderrLog_Install(gxIosStderrLogDir(diagHome).c_str())) {
				// Hand the raw fd to the fatal-signal handlers: inside a signal handler
				// only write() is safe, so they cannot go through the FILE* sink.
				s_crashLogFd = GXStderrLog_GetFd();
			}
			// Beside it, in Documents/CrashLogs/, where the Files app reaches it.
			GXCrashLog_Install(gxIosStderrLogDir(diagHome).c_str(), crashBuildLine());
		}
		installCrashHandlers();
	}

	// The engine resolves all game data relative to the working directory.
	// Preferred layout: assets ship read-only INSIDE the signed app bundle
	// (<bundle>/GameData), the iOS-sanctioned home for app resources — the
	// install is then fully self-contained. Dev builds packaged without
	// assets fall back to the Documents folder (Files-app accessible).
	// User data (saves, Options.ini) always lives in Library/Application
	// Support via the engine's user-data path; never in the bundle.
	{
		const char *home = getenv("HOME");

		// <bundle>/GameData, derived from where this code actually lives.
		//
		// argv[0] is the process's main executable, which is NOT this app when it is
		// hosted inside another process — LiveContainer and similar containers
		// dlopen the target binary, so argv[0] names the host. GameData was then
		// never found and the game silently fell back to the host's Documents.
		// dladdr() reports the binary containing this function, which is right in
		// both the normal and the hosted case; argv[0] remains the fallback.
		char bundleData[1024] = {0};
		const char *imagePath = nullptr;
		{
			Dl_info dlInfo;
			if (dladdr((const void *)&main, &dlInfo) != 0 && dlInfo.dli_fname != nullptr) {
				imagePath = dlInfo.dli_fname;
			}
		}
		if (imagePath == nullptr && argc > 0) {
			imagePath = argv[0];
		}
		if (imagePath != nullptr) {
			const char *slash = strrchr(imagePath, '/');
			if (slash != nullptr) {
				const size_t dirLen = (size_t)(slash - imagePath);
				if (dirLen < sizeof(bundleData) - 16) {
					memcpy(bundleData, imagePath, dirLen);
					snprintf(bundleData + dirLen, sizeof(bundleData) - dirLen, "/GameData");
				}
			}
		}

		bool usingBundleData = false;
		if (bundleData[0] != '\0' && access(bundleData, R_OK) == 0) {
			if (chdir(bundleData) == 0) {
				usingBundleData = true;
				fprintf(stderr, "INFO: iOS working directory (bundle): %s\n", bundleData);
			}
		}
		if (!usingBundleData && home != nullptr) {
			char docs[1024];
			snprintf(docs, sizeof(docs), "%s/Documents", home);
			if (chdir(docs) != 0) {
				fprintf(stderr, "WARNING: chdir(%s) failed: %s\n", docs, strerror(errno));
			} else {
				fprintf(stderr, "INFO: iOS working directory (Documents): %s\n", docs);
			}
		}

		if (home != nullptr) {
			// Keep DXVK's shader cache in Library/Caches: purgeable under
			// storage pressure, excluded from iCloud backup, invisible in the
			// Files app. Must be set before the d3d8 dylib loads.
			char cacheDir[1024];
			snprintf(cacheDir, sizeof(cacheDir), "%s/Library/Caches", home);
			mkdir(cacheDir, 0755);
			setenv("DXVK_STATE_CACHE_PATH", cacheDir, 0);
			{
				const std::string seed = std::string(bundleData) + "/dxvk-seed";	// package-ios-zh.sh ships it
				const char *seedOverride = getenv("GX_DXVK_SEED_DIR");
				gxSeedDxvkCache(seedOverride != nullptr ? seedOverride : (bundleData[0] != '\0' ? seed.c_str() : nullptr));
			}

			{
				// Seed default settings on first run (full detail instead of the
				// 2003 auto-detect, which drops unknown GPUs to Low).
				//
				// No longer gated on running from the bundle: a code-only build runs
				// from Documents, where DefaultOptions.ini now also lives (see the
				// asset seed above). Gating it here meant such a build silently fell
				// back to the 2003 GPU auto-detect — Low LOD and quarter-resolution
				// textures — which the porting playbook calls the number one cause of
				// "looks worse than my PC".
				//
				// GeneralsX @feature Claude 23/09/2026 Into GX_USERDATA_DIR's folder when it applies
				// (GameClient/GXUserDataDir.h), the one GlobalData will use: a test profile gets the same
				// first-run settings, and the owner's folder is not touched.
				std::string userDataDir;
				if (!gxUserDataDirOverride(userDataDir))
					userDataDir = std::string(home) + "/Library/Application Support/GeneralsX/GeneralsZH/";
				const std::string optionsPath = userDataDir + "Options.ini";
				if (access(optionsPath.c_str(), F_OK) != 0 && access("DefaultOptions.ini", R_OK) == 0) {
					std::error_code fsError;
					std::filesystem::create_directories(userDataDir, fsError);
					std::filesystem::copy_file("DefaultOptions.ini", optionsPath, fsError);
					if (!fsError) {
						fprintf(stderr, "INFO: Seeded default Options.ini\n");
					}
				}

				// Seed Documents with the bundled assets, once.
				//
				// A full build carries ~2.7 GB of game data inside the signed bundle,
				// which makes every update a multi-gigabyte transfer — painful when
				// the device is nowhere near the build machine. A code-only build is
				// ~100 MB, but it needs the data to already exist in Documents.
				//
				// So the self-contained build hands the data over: on first launch it
				// copies its GameData into Documents and drops a marker. From then on
				// a code-only build finds it and runs, and updates cost 100 MB instead
				// of 2 GB. Costs one copy and the disk to hold it.
				//
				// Opt out by creating a file named "no-asset-seed" in Documents.
				{
					char docsSeed[1024];
					snprintf(docsSeed, sizeof(docsSeed), "%s/Documents", home);
					char seedMarker[1024];
					snprintf(seedMarker, sizeof(seedMarker), "%s/.assets-seeded", docsSeed);
					char optOut[1024];
					snprintf(optOut, sizeof(optOut), "%s/no-asset-seed", docsSeed);

					if (access(seedMarker, F_OK) != 0 && access(optOut, F_OK) != 0) {
						fprintf(stderr, "INFO: seeding Documents with bundled game data "
						                "(one time; enables ~100MB code-only updates)\n");
						fflush(stderr);
						std::error_code copyError;
						std::filesystem::copy(bundleData, docsSeed,
							std::filesystem::copy_options::recursive |
							std::filesystem::copy_options::overwrite_existing,
							copyError);
						if (copyError) {
							fprintf(stderr, "WARNING: asset seed failed: %s\n",
							        copyError.message().c_str());
						} else {
							FILE *m = fopen(seedMarker, "w");
							if (m != nullptr) { fclose(m); }
							fprintf(stderr, "INFO: asset seed complete\n");
						}
						fflush(stderr);
					}
				}

				// One-time tidy-up: remove asset copies from Documents now that
				// the bundle carries them. Guarded by a sentinel so it truly runs
				// once — Documents is exposed via the Files app, and anything the
				// user places there later (mods, custom maps) must never be touched.
				// "Maps" is deliberately NOT in the list: it is where user maps live.
				//
				// Skipped entirely once the seed above has run: those copies are now
				// deliberate, and deleting them would defeat the whole point.
				char docs[1024];
				snprintf(docs, sizeof(docs), "%s/Documents", home);
				char seededMarker[1024];
				snprintf(seededMarker, sizeof(seededMarker), "%s/.assets-seeded", docs);
				char sentinel[1024];
				snprintf(sentinel, sizeof(sentinel), "%s/.bundle-assets-tidied", docs);
				if (access(sentinel, F_OK) != 0 && access(seededMarker, F_OK) != 0) {
					std::error_code fsError;
					for (const auto &entry : std::filesystem::directory_iterator(docs, fsError)) {
						const std::string name = entry.path().filename().string();
						const bool isShippedAsset =
							(name.size() > 4 && name.compare(name.size() - 4, 4, ".big") == 0) ||
							name == "Data" || name == "Window" || name == "ZH_Generals" ||
							name == "fonts" || name == "_CommonRedist" ||
							name == "dxvk.conf" || name == "GeneralsXZH.dxvk-cache" ||
							name == "GeneralsXZH_d3d9.log";
						if (isShippedAsset) {
							fprintf(stderr, "INFO: tidy-up removing shipped asset copy: %s\n", name.c_str());
							std::error_code removeError;
							std::filesystem::remove_all(entry.path(), removeError);
						}
					}
					if (!fsError) {  // a failed scan must retry next launch, not fail closed forever
						FILE *s = fopen(sentinel, "w");
						if (s) fclose(s);
					}
				}
			}
		}
	}
#endif

	fprintf(stderr, "=================================================\n");
	fprintf(stderr, " Command & Conquer Generals: Zero Hour (Linux)\n");
	fprintf(stderr, " SDL3 + DXVK Build\n");
	fprintf(stderr, "=================================================\n\n");

	try {
		// Initialize critical sections (required by game engine)
		TheAsciiStringCriticalSection = &critSec1;
		TheUnicodeStringCriticalSection = &critSec2;
		TheDmaCriticalSection = &critSec3;
		TheMemoryPoolCriticalSection = &critSec4;
		TheDebugLogCriticalSection = &critSec5;

		// Initialize memory manager early (required by NEW operator)
		initMemoryManager();

		// GeneralsX @bugfix BenderAI 14/02/2026 Initialize Version singleton
		// GameEngine::init() calls updateWindowTitle() which uses TheVersion
		// Must be created before GameMain() to avoid nullptr dereference
		TheVersion = NEW Version;

		// Parse command line (CommandLine class handles argc/argv internally)
		// TheSuperHackers @build felipebraz 10/02/2026 Phase 1.5
		// Store argc/argv for CommandLine parser to access via _NSGetArgc/_NSGetArgv or /proc/self/cmdline
		// For now, let CommandLine::parseCommandLineForStartup() handle this
		CommandLine::parseCommandLineForStartup();

		// GeneralsX @feature Claude 17/08/2026 Give the desync report somewhere to land.
		//
		// A Mac launched from Finder has stderr wired to /dev/null, so GameLogic.cpp:3000's
		// release-build "[CRC] MISMATCH frame=... player=... crc=..." — the only output that
		// names the diverging peer and the frame — has never once been readable after a real
		// game. This mirrors stderr into the same directory the release crash log uses.
		//
		// Deliberately placed after parseCommandLineForStartup(): that is the call which
		// constructs TheGlobalData (CommandLine.cpp:1743), so getPath_UserData() is the real
		// per-platform path from here on and nothing has to be re-derived. The cost is that
		// output from before this line is not in the file; on this path that is the banner
		// above and the memory-manager bring-up, neither of which has ever been the question.
		//
		// A no-op on iOS, which already installed its own sink into Documents above, and on
		// Linux, whose stderr the harnesses capture to a file already.
		if (TheGlobalData != nullptr && GXStderrLog_Install(TheGlobalData->getPath_UserData().str())) {
			fprintf(stderr, "[gx] stderr mirror installed: %sgenerals-stderr.log\n",
				TheGlobalData->getPath_UserData().str());
#if !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
			// GeneralsX @bugfix Claude 24/09/2026 The [GXDXVK] line at the top of main() runs before this mirror
			// exists, so it never reached generals-stderr.log (seen on the Mini's rendered run). Said again here.
			const char *dxvkLog = getenv("DXVK_LOG_PATH");
			const char *dxvkCache = getenv("DXVK_STATE_CACHE_PATH");
			fprintf(stderr, "[GXDXVK] log=%s cache=%s\n", dxvkLog ? dxvkLog : "(unset)", dxvkCache ? dxvkCache : "(unset)");
#endif
		}

#if !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
		// GeneralsX @feature Claude 25/09/2026 macOS and Linux: <userdata>/CrashLogs/, then the
		// handlers. iOS did both before GlobalData existed (above). A crash before this line still
		// reaches only the OS's own report, exactly as before.
		if (TheGlobalData != nullptr)
			GXCrashLog_Install(TheGlobalData->getPath_UserData().str(), crashBuildLine());
		s_crashLogFd = GXStderrLog_GetFd();  // -1 on Linux, which mirrors nothing
		installCrashHandlers();
#endif
		// GeneralsX @feature Claude 25/09/2026 The save gate writes a crash log for a load it stopped
		// because it would have crashed (SaveLoadFeedback.cpp); Core code reaches the writer through this.
		TheGXCrashLogWriter = GXCrashLog_Write;

		// GeneralsX @bugfix Copilot 17/05/2026 Skip SDL3 window bootstrap for CLI/headless replay execution.
		const bool isHeadlessMode = (TheGlobalData != nullptr && TheGlobalData->m_headless);
		if (isHeadlessMode) {
			fprintf(stderr, "INFO: Headless mode detected, skipping SDL3 video/Vulkan window initialization\n");
		} else {

		// GeneralsX @bugfix felipebraz 16/02/2026
		// Initialize SDL3 and Vulkan BEFORE creating GameEngine (fighter19 pattern)
		// This prevents LLVM SIGSEGV crash during Vulkan driver enumeration
		// Must be done here, not in SDL3GameEngine::init() which is too late
		fprintf(stderr, "INFO: Initializing SDL3 video subsystem...\n");
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
		// All mouse events are synthesized by the gesture translator in
		// SDL3GameEngine.cpp; SDL's automatic touch->mouse synthesis would
		// double-deliver finger 1 and fight the two-finger pan logic.
		SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
		// GeneralsX @bugfix Claude 15/09/2026 DO NOT MINIMISE THIS WINDOW WHEN IT LOSES FOCUS.
		//
		// SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS defaults to "auto", which means "minimise if the
		// window is fullscreen_exclusive and mode switching is not emulated". This engine asks for
		// EXCLUSIVE fullscreen (W3DDisplay.cpp passes a non-NULL SDL_DisplayMode to
		// SDL_SetWindowFullscreenMode) and Cocoa does not emulate mode switching, so on macOS that
		// default was armed and every alt-tab minimised the window.
		//
		// Minimising drops the window out of fullscreen, which reverts it to the restored size -
		// SDL3Main creates it at a placeholder 1024x768 POINTS and the fullscreen path never resized
		// it - so the backbuffer was rebuilt at 2048x1536 px on a 2.0-density display: 4:3 on a
		// 1.5397 panel, letterboxed to fit=2048x1330@0,103, and it never recovered. The cursor went
		// with it, because SDL3Mouse grabs the cursor to the WINDOW: the reported "my mouse can only
		// move in a certain box" was the OS correctly confining it to a window that really had shrunk.
		//
		// MEASURED on a 3024x1964 Retina MacBook Pro, one Cmd-Tab, same binary, one variable:
		//   default ("auto")    64 log lines, 1 Device reset, 2x backbuffer=2048x1536, never recovers
		//   this hint = "0"      0 log lines, 0 Device reset, 0x backbuffer=2048x1536
		// with AXMinimized reading false in the second case, catching the minimise itself as the link.
		//
		// WHY NOT ASK FOR FULLSCREEN DESKTOP INSTEAD (pass NULL for the mode)? That also fixes the
		// alt-tab, and it was tried and measured first - but a macOS fullscreen SPACE reserves the
		// notch/menu-bar strip, so the drawable becomes 3024x1898 and the game's 1.5397 surface is
		// pillarboxed into it with 51 px bars at REST. Measured: settled state went from
		// "3024x1964 1:1, no pillarbox" to "backbuffer=3024x1898, fit=2922x1898@51,0". This hint keeps
		// the exclusive path, and with it the whole panel, and costs nothing else.
		//
		// Set before SDL_InitSubSystem and long before SDL_CreateWindow, because SDL reads it when the
		// window handles the focus event, and an env var of the same name still overrides it.
		SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
		if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
			fprintf(stderr, "FATAL: Failed to initialize SDL3: %s\n", SDL_GetError());
			return 1;
		}

		// Set DXVK WSI driver before loading Vulkan
		setenv("DXVK_WSI_DRIVER", "SDL3", 1);

		// GeneralsX @bugfix BenderAI 06/03/2026 - Exclude LLVMpipe Vulkan ICD before loading Vulkan.
		// libvulkan_lvp.so crashes during static initialization with LLVM 20.x when the Vulkan
		// loader enumerates all ICDs. Restrict to hardware ICDs first.
		FilterSoftwareVulkanICDs();
		FilterPipeWireOpenAL();

#ifdef GX_RENDER_NATIVE
		// GeneralsX @feature E · rw3-K 11/10/2026 native-device session (DESIGN_l1-device §8.1): name the Vulkan loader before SDL
		// loads Vulkan, so SDL and the native device share ONE Vulkan implementation; S8 then qualifies the outcome. Only when
		// the process asked for the native session - a classic launch keeps today's SDL choice byte for byte.
		// D · render-line-0967 21/10/2026: the first reader of the renderer switch (Options.ini GXRenderer, GX_RENDERER over it);
		// TheGlobalData exists since parseCommandLineForStartup, so the key is readable here and every later reader agrees.
		if (GxNative::Requested())
			GXRender::Vk::sdl3PreferVulkanLoader();
#endif

		// Load Vulkan library for DXVK DirectX8→Vulkan translation
		fprintf(stderr, "INFO: Loading Vulkan library...\n");
		if (!SDL_Vulkan_LoadLibrary(nullptr)) {
			fprintf(stderr, "WARNING: Failed to load Vulkan: %s\n", SDL_GetError());
			fprintf(stderr, "WARNING: Continuing without Vulkan (may use software rendering)\n");
		}
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
		// GeneralsX @bugfix Claude 29/09/2026 Which MoltenVK SDL got (see the SDL_HINT_VULKAN_LIBRARY note
		// above): the image its vkGetInstanceProcAddr lives in - Frameworks/MoltenVK.framework/MoltenVK when
		// SDL shares DXVK's copy, the executable when it fell back to the static one.
		{
			Dl_info mvkImage;
			const void *gipa = (const void *)SDL_Vulkan_GetVkGetInstanceProcAddr();
			fprintf(stderr, "[GXMVK] SDL Vulkan library: %s (the image SDL's vkGetInstanceProcAddr is in)\n",
				(gipa != nullptr && dladdr(gipa, &mvkImage) != 0 && mvkImage.dli_fname != nullptr)
					? mvkImage.dli_fname : "(unknown: Vulkan not loaded)");
		}
#endif

		// Create SDL3 window with Vulkan support
		fprintf(stderr, "INFO: Creating SDL3 Vulkan window...\n");
		Uint32 windowFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN;  // Start hidden, show after D3D init
#if defined(__APPLE__)
		// Request a native-resolution Metal drawable (e.g. 2868x1320 instead of the
		// 956x440 point size). Without this the swapchain renders at point size and
		// the display upscales 3x, visibly blurring textures and terrain.
		//
		// GeneralsX @bugfix Claude 17/08/2026 This was iOS-only and macOS needs it identically.
		// A Retina Mac has exactly the same 2x point/pixel split as the iPad, so without the flag
		// the swapchain is built at POINT size and the compositor upscales the whole window. The
		// result was reported from an M5 MacBook with screenshots: "everything is very fuzzy and
		// not very defined", "the buttons are huge", "the health bars and stuff are massive" - and,
		// tellingly, "the cursor is tiny", because the OS draws the cursor at NATIVE density while
		// every game-drawn element is being blown up 2x around it. One cause, four symptoms.
		//
		// Safe on non-Retina hardware by construction: where pixel density is 1.0 the drawable and
		// the point size are the same buffer, so this flag changes nothing at all. Verified with the
		// engine's own GX_PBOX probe on a 1x Mac: native=1920x1080 density=1.000.
		windowFlags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif
		TheSDL3Window = SDL_CreateWindow(
			"Command & Conquer Generals: Zero Hour",
			1024, 768,  // Default resolution
			windowFlags
		);

		if (!TheSDL3Window) {
			fprintf(stderr, "FATAL: Failed to create SDL3 window: %s\n", SDL_GetError());
			SDL_Quit();
			return 1;
		}

		// Store window handle globally (cast SDL_Window* to HWND for compatibility)
		ApplicationHWnd = (HWND)TheSDL3Window;
		fprintf(stderr, "INFO: SDL3 window created successfully\n");

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
		// Match the game's internal resolution to the phone screen's aspect ratio.
		// Without this the engine runs its 4:3 default inside the 19.5:9 display:
		// pillarboxed picture and a skewed window->game coordinate mapping. Height
		// stays at the engine's 600px design baseline (UI layouts assume >= 600);
		// width follows the real aspect. Injected as -xres/-yres argv entries so
		// the normal command-line path applies them (user-passed flags still win
		// because the parser lets later arguments override earlier ones... ours go
		// last, so only add them if the user didn't pass explicit -xres/-yres).
		{
			bool userSetRes = false;
			for (int i = 1; i < __argc; ++i) {
				if (strcmp(__argv[i], "-xres") == 0 || strcmp(__argv[i], "-yres") == 0) {
					userSetRes = true;
					break;
				}
			}
			// Use the pixel size of the high-density drawable: the game renders
			// 1:1 into the native-resolution swapchain, and fonts/UI rescale via
			// the engine's resolution-aware font scaling (GlobalLanguage).
			int winW = 0, winH = 0;
			SDL_GetWindowSizeInPixels(TheSDL3Window, &winW, &winH);
			if (!userSetRes && winW > 0 && winH > 0 && winW > winH) {
				static char xresVal[16], yresVal[16];
				static char xresFlag[] = "-xres";
				static char yresFlag[] = "-yres";
				const int yres = winH;
				int xres = winW;
				xres &= ~1;  // keep it even
				snprintf(xresVal, sizeof(xresVal), "%d", xres);
				snprintf(yresVal, sizeof(yresVal), "%d", yres);

				static char* newArgv[64];
				int n = 0;
				for (int i = 0; i < __argc && n < 59; ++i) {
					newArgv[n++] = __argv[i];
				}
				newArgv[n++] = xresFlag;
				newArgv[n++] = xresVal;
				newArgv[n++] = yresFlag;
				newArgv[n++] = yresVal;
				newArgv[n] = nullptr;
				__argv = newArgv;
				__argc = n;
				fprintf(stderr, "INFO: iOS internal resolution set to %sx%s (window %dx%d)\n",
				        xresVal, yresVal, winW, winH);
			}
		}

		// iOS has no command line, so -autoload is switched on by dropping a file
		// named "autoload" into the app's Documents folder — reachable from the
		// Files app, so it can be toggled on device without a rebuild. Resumes the
		// most recent save instead of stopping at the main menu, which is the
		// difference between reproducing an in-game bug in seconds and clicking
		// through the shell every time.
		{
			const char *docHome = getenv("HOME");
			if (docHome != nullptr) {
				// Match case-insensitively, file or folder. The Files app will not
				// create an empty file, only a folder, and it names new folders
				// "untitled folder" — so this has to be forgiving about what the user
				// managed to produce, rather than demanding an exact lowercase name.
				bool wantAutoload = false;
				char docsDir[1024];
				snprintf(docsDir, sizeof(docsDir), "%s/Documents", docHome);
				if (DIR *dp = opendir(docsDir)) {
					for (struct dirent *de = readdir(dp); de != nullptr; de = readdir(dp)) {
						if (strcasecmp(de->d_name, "autoload") == 0) {
							wantAutoload = true;
							break;
						}
					}
					closedir(dp);
				}
				if (wantAutoload) {
					static char autoloadFlag[] = "-autoload";
					static char* alArgv[66];
					int n = 0;
					for (int i = 0; i < __argc && n < 64; ++i) {
						alArgv[n++] = __argv[i];
					}
					alArgv[n++] = autoloadFlag;
					alArgv[n] = nullptr;
					__argv = alArgv;
					__argc = n;
					fprintf(stderr, "INFO: Documents/autoload present - resuming most recent save\n");
				}
			}
		}
#endif
		}

		// Call cross-platform game entry point
		exitcode = GameMain();

		fprintf(stderr, "INFO: GameMain() returned with code %d\n", exitcode);

	} catch (const std::exception& e) {
		fprintf(stderr, "FATAL: Unhandled exception in main(): %s\n", e.what());
		exitcode = 1;
	} catch (...) {
		fprintf(stderr, "FATAL: Unknown exception in main()\n");
		exitcode = 1;
	}

#ifdef GX_RENDER_NATIVE
	// GeneralsX @feature E · rw3-L-3 12/10/2026 the native session's final stats line (DESIGN_l1-device §2.3) if its device outlived
	// the engine's teardown: _exit() below runs no atexit handler, and the line allocates - so here, before the memory manager goes.
	// GeneralsX @bugfix D · rw3-T-5 13/10/2026 and before SDL destroys the window: the flush first joins the native renderer's submit
	// thread (lever 3, PREREG_T A21 am. 4), which may still be presenting to that window's swapchain.
	GxNative::FlushFinalStats();
#endif

	// Cleanup SDL3 resources
	if (TheSDL3Window) {
		SDL_DestroyWindow(TheSDL3Window);
		TheSDL3Window = nullptr;
		ApplicationHWnd = nullptr;
	}
	SDL_Quit();

	// GeneralsX @bugfix BenderAI 14/02/2026 Cleanup Version singleton
	if (TheVersion) {
		delete TheVersion;
		TheVersion = nullptr;
	}

	// GeneralsX @bugfix BenderAI 19/02/2026 Shutdown memory manager BEFORE nulling critical
	// sections. Without this, global pool destructors (ObjectPoolClass) crash during atexit()
	// because they call ::operator delete after the memory manager is already gone (SIGSEGV).
	// Matches WinMain.cpp cleanup order: TheVersion -> shutdownMemoryManager -> null critSecs.
	shutdownMemoryManager();

	// Cleanup critical sections (after memory manager, which may use them during shutdown)
	TheAsciiStringCriticalSection = nullptr;
	TheUnicodeStringCriticalSection = nullptr;
	TheDmaCriticalSection = nullptr;
	TheMemoryPoolCriticalSection = nullptr;
	TheDebugLogCriticalSection = nullptr;

	// GeneralsX @test Claude 23/10/2026 ai-wh1ter0se-econ (Codex ke-timer-*-r1): the _exit below runs no static destructors - print every
	// [GXHOOK] timer's open frame and window and the "[GXHOOK] exit" line now, before the queue is settled (test-instruments only)
	GX_HOOK_FLUSH_ALL();
	fprintf(stderr, "\nExiting with code %d\n", exitcode);
	GXStderrLog_Settle();  // GeneralsX @perf Claude 02/10/2026 _exit runs no atexit: write out the queue first

	// GeneralsX @bugfix BenderAI 25/02/2026 — use _exit() to skip C++ global destructors.
	// On macOS, __cxa_finalize_ranges runs ObjectPoolClass<X,256> global dtors after main() returns.
	// Those dtors crash with a corrupted BlockListHead (SIGSEGV at 0x4ade32ec4ade0018) because
	// pool block memory was already reused/overwritten during game shutdown.
	// Windows never had this problem — ExitProcess() terminates without running C++ global dtors.
	// _exit() matches that behavior. Explicit cleanup already done above (SDL_Quit, shutdownMemoryManager).
	_exit(exitcode);
}

#endif // !_WIN32
