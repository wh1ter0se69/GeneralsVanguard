/* GeneralsX launcher - checks for a newer build, applies it, then starts the game.
 *
 * GeneralsX @feature Claude 08/08/2026
 *
 * WHAT IT IS FOR. The friends run an unzipped folder. Today a new build means the maintainer rebuilds,
 * repacks, and every one of them re-downloads a zip by hand - and a peer that skips it cannot
 * join, because sourceID refuses mismatched peers by design. Worse, a friend who lacks the
 * community-patch archive has a different dataID and cannot join AT ALL, which is why this ships
 * DATA as well as the executable: an update that could only replace generalszh.exe would not fix
 * the failure people actually hit.
 *
 * WHY IT IS A SEPARATE PROGRAM AND NOT PART OF THE ENGINE. HTTP, signature verification and file
 * replacement have no business inside the game process. The engine has no HTTP client on Windows
 * at all (vcpkg.json gates the curl port on "!windows"), a running .exe cannot replace itself, and
 * anything added to the engine's sources risks the simulation digest and therefore a
 * lockstep-atomic rebuild of all four platforms. Nothing here is in GameEngine, GameEngineDevice or
 * any digested path; it is a 60 KB program that runs for a second and exits.
 *
 * WHY C AND MSVC. It must be double-clickable, so it must be an .exe: a .ps1 is not, and telling a
 * friend to right-click-Run-with-PowerShell fails on execution policy and on mark-of-the-web (the
 * two obstacles GeneralsX-Setup.cmd already exists to route around). MSVC is what is installed on
 * the build box, and everything this needs is an OS DLL already present on every Windows: Winsock
 * for the UDP check, WinHTTP for the download, BCrypt for SHA-256, and tar.exe or PowerShell for
 * the unzip. No vcpkg, no CMake, no third-party runtime, nothing for a friend to install. The one
 * thing Windows does NOT provide is Ed25519 - see Core/GameEngine/Source/GameClient/GXEd25519.cpp.
 *
 * NO UAC, EVER. Everything lives under %LOCALAPPDATA%\Programs\GeneralsX, which the user already
 * owns, exactly where install-windows-share.ps1 puts it and for the reasons written there. An
 * unsigned executable asking for administrator is the shape of malware, and it is also simply not
 * needed. Two consequences are structural rather than stylistic:
 *   - the link uses an explicit asInvoker manifest, and
 *   - the program is NOT called anything containing "update", "setup", "install" or "patch",
 *     because Windows' installer-detection heuristic auto-elevates a legacy-looking executable
 *     whose FILENAME contains one of those words. The manifest already prevents it; the name is
 *     the belt to that pair of braces.
 *
 * IT FAILS SAFE, ALWAYS. No relay, no answer, no manifest, a bad signature, a failed or partial
 * download, a corrupt archive, a locked file - every one of them logs a line and starts the game
 * that is already installed. There is exactly one exit path and it goes through launch_game(). A
 * broken update must never be able to leave a friend with nothing, and an update check must never
 * be able to stop a launch; that is worth more than any update it might have applied.
 *
 * NOBODY TYPES A SERVER ADDRESS. This program also answers the bootstrap question - how a machine
 * that has never spoken to a relay finds one - by fetching a signed document over HTTPS from one
 * compiled-in name and reading the relay list out of it. It then writes the relay it picked into
 * the GAME's own Options.ini, so the engine is zero-config too. See GX_BOOTSTRAP_URL and the
 * bootstrap section. The sidecar file that used to carry the address (relay-settings.ini, handed
 * over by whoever invited you) is gone, along with the dead-end it produced for anybody who was
 * given a copy second-hand.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>		/* SIO_UDP_CONNRESET - see udp_ignore_conn_reset */

/* mstcpip.h declares SIO_UDP_CONNRESET only behind a _WIN32_WINNT gate this translation unit does
 * not satisfy, and it is absent from the BuildTools 2022 SDK as included here - measured, not
 * assumed: the build failed with C2065 on exactly this identifier. The value is the documented,
 * stable one and has been since Windows 2000; spelling it out is better than raising _WIN32_WINNT
 * for the whole program to reach one ioctl code. */
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET	_WSAIOW(IOC_VENDOR, 12)
#endif
#include <winhttp.h>
#include <shlobj.h>
/* KnownFolders.h for FOLDERID_Documents, which userdata_path resolves through SHGetKnownFolderPath
 * to match the engine (GlobalData.cpp, BuildUserDataPathFromRegistry). */
#include <knownfolders.h>
/* ShellExecuteW, to open the login page. shlobj.h does not declare it. */
#include <shellapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>	/* wall-clock reads: the assertion's iat/exp check, and the manifest serial horizon */

/* Moved out of this directory 11/08/2026 - the engine verifies Ed25519 too now. build-launcher.cmd
 * adds Core/GameEngine/Include to the include path and defines GX_ED25519_WITH_CNG, which is what
 * keeps gx_sha256/gx_sha256_file below compiled in. */
#include "GameClient/GXEd25519.h"
/* GeneralsX @feature Claude 11/08/2026 The ENGINE's assertion verifier, through its C seam. One
 * verifier, one pinned key: a launcher with its own looser check writes documents the game
 * refuses while reporting success. build-launcher.cmd compiles GXIdentityAssertion.cpp too. */
#include "GameClient/GXIdentityAssertion.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
/* CoTaskMemFree, for the buffer SHGetKnownFolderPath allocates in userdata_path. shell32.lib does
 * not bring it - the allocator is COM's, not the shell's. */
#pragma comment(lib, "ole32.lib")

/* The trust anchor. Produced by `gx-manifest.py pubkey --c-array` rather than typed, because a
 * single wrong byte makes every manifest fail to verify forever, which in the field is
 * indistinguishable from a compromised signing key.
 *
 * This is a PUBLIC key and belongs in a public repository: it is what makes the relay untrusted.
 * A hostile relay, or anyone able to forge a UDP source address, can WITHHOLD an update with it,
 * but cannot fabricate one.
 *
 * GX_TEST_MANIFEST_PUBKEY replaces it in a TEST BUILD ONLY (test-single-instance.cmd), so the
 * concurrency suite can sign its fixtures with a throwaway key instead of minting documents with
 * the release key. build-launcher.cmd and CI never define it; a shipped binary cannot carry it. */
#ifdef GX_TEST_MANIFEST_PUBKEY
static const unsigned char GX_MANIFEST_PUBKEY[32] = { GX_TEST_MANIFEST_PUBKEY };
#else
static const unsigned char GX_MANIFEST_PUBKEY[32] = {
	0x28, 0xc8, 0xd4, 0x77, 0x3c, 0xef, 0xa3, 0x27,
	0x45, 0xc6, 0x18, 0x99, 0xde, 0x54, 0x77, 0x90,
	0x65, 0x23, 0x66, 0x68, 0xdf, 0x80, 0xed, 0x33,
	0x1b, 0xa4, 0x93, 0x1e, 0x80, 0x26, 0x20, 0xc4,
};
#endif

/* GeneralsX @feature Claude 29/10/2026 S1-3 relay-list-v2: THE RANKED KEY SET (modernize/PLAN_smooth-1.md §3.4).
 *
 * K1 < K2 < K3. K1 is the key above. K2 and K3 are its successors: the operator generates them offline
 * (`gx-manifest.py keygen --successor k2`) and pastes `pubkey --key k2 --c-array` here, into gxcore verify.rs and
 * into GXRelayList.cpp BEFORE a release that must be able to rotate. Until then the slots are all zero and
 * parse_manifest skips them explicitly. A document's RANK is the slot whose key verifies it, and
 * manifest_supersedes never lets a lower-ranked document replace a higher-ranked one: one K2 document retires K1 on
 * this machine. The test build may replace each slot (GX_TEST_MANIFEST_PUBKEY2/3), like K1 above. */
#ifdef GX_TEST_MANIFEST_PUBKEY2
static const unsigned char GX_MANIFEST_K2[32] = { GX_TEST_MANIFEST_PUBKEY2 };
#else
static const unsigned char GX_MANIFEST_K2[32] = { 0 };
#endif
#ifdef GX_TEST_MANIFEST_PUBKEY3
static const unsigned char GX_MANIFEST_K3[32] = { GX_TEST_MANIFEST_PUBKEY3 };
#else
static const unsigned char GX_MANIFEST_K3[32] = { 0 };
#endif
static const unsigned char *const GX_MANIFEST_KEYS[3] = { GX_MANIFEST_PUBKEY, GX_MANIFEST_K2, GX_MANIFEST_K3 };

/* THE ONE ADDRESS COMPILED INTO THIS PROGRAM, AND WHY IT IS ALLOWED TO BE ONE.
 *
 * A client that has never spoken to a relay cannot ask a relay where the relays are. That
 * bootstrap used to be answered by relay-settings.ini, a sidecar dropped in the zip by whoever
 * invited you - which is precisely the "somebody hands you a server address" step this project
 * exists to delete. So the client is compiled with ONE name and fetches a signed document from it.
 *
 * This is not the compiled-in relay address CLAUDE.md forbids. The rule there is that "adding a
 * relay must not require updating clients", and it does not: the relay LIST is inside the signed
 * document, so relays are still data. What is compiled in is a name the project owns, serving a
 * document signed by a key that is also compiled in - the same shape Generals Online uses
 * (api.playgenerals.online, hardcoded per build environment).
 *
 * The transport is not what makes this trustworthy - the signature is. HTTPS here buys privacy and
 * a little integrity against a bored network, nothing more; a document that does not verify is
 * refused whether it arrived over TLS or not. That is why an override is safe, see load_config.
 *
 * gxmanifest.txt, NOT manifest.json: the published file is the flat "<payload>|<sig>" wire form
 * (gx-manifest.py --wire), byte-identical to what a relay puts after its "GXMANI " tag. Same
 * bytes, same parser, same verifier, whichever transport answered. This program has no JSON parser
 * and must not grow one. */
#define GX_BOOTSTRAP_URL	"https://dl.wh1ter0se.dev/gxmanifest.txt"

/* The login service. A SIBLING of the bootstrap URL rather than a field of the manifest, and that
 * is deliberate: the manifest says which binary to download and is signed with the RELEASE key,
 * while this is where identities come from and is signed with a DIFFERENT one. Putting the login
 * origin inside a document signed by the release key would give that key authority over identity
 * too, which is exactly the coupling GXIdentityAssertion.cpp refuses at the pin.
 *
 * GX_OAUTH_BASE in the environment overrides it, for testing against a local test server. It is
 * LOGGED whenever it is in use, because a test that silently spoke to production and still
 * reported success has already happened once on this project. */
#define GX_OAUTH_BASE_URL	"https://oauth.wh1ter0se.dev"

#define RELAY_LOBBY_PORT	8086		/* Transport.cpp: RELAY_LOBBY_PORT */
#define MANIFEST_VERSION	2			/* gx-manifest.py: MANIFEST_VERSION */
/* Big enough for the HTTPS form, which has no datagram limit. The relay's own copy is bounded at
 * 476 by RELAY_MANIFEST_MAX and always fits inside this; sizing the buffer to the UDP budget would
 * make the bootstrap document inherit a constraint the bootstrap transport does not have. */
#define WIRE_MAX			4096
#define UPD_ATTEMPTS		3
#define UPD_TIMEOUT_MS		1200
/* GeneralsX @bugfix Claude 19/08/2026 8 was a Windows-only number.
 *
 * The cap is on the PARSED list across all platforms, not on the ones this client ends up
 * wanting, so three platforms at engine + runtime + optional plus the shared 'any' row is
 * about ten and the ninth onward were dropped SILENTLY by the loop below - no error, no log
 * line, the artifact simply never existed as far as this client was concerned.
 *
 * 16 rather than 12: the parse cost is a fixed-size struct per row and nothing here is hot,
 * so the headroom is free, and the next platform should not need this touched again.
 *
 * NOT bounded by the relay's 476-byte datagram. That budget governs the LEGACY GXUPD/GXMANI
 * exchange, which is the last of three manifest sources rather than the first; the bootstrap
 * arrives over HTTPS into WIRE_MAX, which is 4096. See the note by WIRE_MAX above.
 *
 * A client still on 8 keeps working through the transition because publish-bootstrap.sh now
 * orders the win32 rows FIRST, so everything such a client needs lands inside its first
 * eight. That ordering is load-bearing, not cosmetic. */
#define MAX_ARTIFACTS		16
#define MAX_RELAYS			8
/* One window for ALL relays, not one per relay: they are probed on a single socket in parallel, so
 * a list of dead relays costs this once rather than this times N. That matters because it sits in
 * front of a launch. */
#define RELAY_PROBE_MS		800
#define MAX_DOWNLOAD_BYTES	(512u * 1024u * 1024u)
#define OLD_SUFFIX			L".gxold"
#define MANIFEST_CACHE		L"gxmanifest.txt"

/* ============================================================================================
 * Logging. Appended to gxlaunch.log beside the executable, because the only debugging channel
 * that survives contact with a friend three time zones away is "send me that file".
 * ========================================================================================== */

/* The commit this exe was built from, as a plain narrow string so `strings` / Select-String finds it and
 * package-windows-share.ps1 can refuse an updater that does not say. build-launcher.cmd and
 * build-launcher.yml pass /DGX_UPDATER_COMMIT=\"<40 hex>\". Printed by the startup log line in run(), which
 * is what keeps /O2 from dropping it. Without the define it carries a fixed word, which the packager refuses. */
#ifndef GX_UPDATER_COMMIT
#define GX_UPDATER_COMMIT "none"
#endif
static const char g_updater_commit[] = "GX_UPDATER_COMMIT=" GX_UPDATER_COMMIT;

static wchar_t g_dir[MAX_PATH];			/* the install directory - where this exe lives */
static wchar_t g_log[MAX_PATH];

static void logf_(const char *fmt, ...)
{
	char line[1024];
	char stamp[64];
	SYSTEMTIME st;
	va_list ap;
	HANDLE f;
	DWORD wrote;
	int n;

	GetLocalTime(&st);
	sprintf_s(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u  ",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

	va_start(ap, fmt);
	n = vsprintf_s(line, sizeof(line) - 2, fmt, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	strcat_s(line, sizeof(line), "\r\n");

	printf("%s%s", stamp, line);

	f = CreateFileW(g_log, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, NULL);
	if (f != INVALID_HANDLE_VALUE) {
		WriteFile(f, stamp, (DWORD)strlen(stamp), &wrote, NULL);
		WriteFile(f, line, (DWORD)strlen(line), &wrote, NULL);
		CloseHandle(f);
	}
}

/* ============================================================================================
 * Small helpers
 * ========================================================================================== */

static void path_join(wchar_t *out, size_t cap, const wchar_t *a, const wchar_t *b)
{
	swprintf_s(out, cap, L"%s\\%s", a, b);
}

static int file_exists(const wchar_t *p)
{
	DWORD a = GetFileAttributesW(p);
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int dir_exists(const wchar_t *p)
{
	DWORD a = GetFileAttributesW(p);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* mkdir -p, so a staged archive can create nested directories that do not exist yet. */
static int make_dirs(const wchar_t *path)
{
	wchar_t tmp[MAX_PATH];
	size_t i;
	wcscpy_s(tmp, MAX_PATH, path);
	for (i = 3; tmp[i]; i++) {		/* skip "C:\" */
		if (tmp[i] == L'\\') {
			tmp[i] = 0;
			CreateDirectoryW(tmp, NULL);
			tmp[i] = L'\\';
		}
	}
	CreateDirectoryW(tmp, NULL);
	return dir_exists(path);
}

static void rmtree(const wchar_t *dir)
{
	WIN32_FIND_DATAW fd;
	wchar_t glob[MAX_PATH], child[MAX_PATH];
	HANDLE h;

	path_join(glob, MAX_PATH, dir, L"*");
	h = FindFirstFileW(glob, &fd);
	if (h == INVALID_HANDLE_VALUE) {
		return;
	}
	do {
		if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) {
			continue;
		}
		path_join(child, MAX_PATH, dir, fd.cFileName);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			rmtree(child);
		} else {
			SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
			DeleteFileW(child);
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	RemoveDirectoryW(dir);
}

/* Why read_text_why returned NULL. MISSING is the only answer that means "there is no file": a caller
 * that is about to REPLACE the file must treat every other failure as "there is a file and it could
 * not be read", or it replaces somebody's data with nothing (publish_relay_file). */
enum { RT_OK = 0, RT_MISSING = 1, RT_UNREADABLE = 2 };

/* Read a whole file into a NUL-terminated buffer. NULL on failure, with the reason in *why (and the
 * Win32 error, or ERROR_FILE_TOO_LARGE for a file over `cap`, in *err) when they are not NULL. A
 * short read is a failure: half a file is not the file. *len is the byte count read, which is more
 * than strlen() when the file holds a NUL byte. Caller frees. */
static char *read_text_why(const wchar_t *path, DWORD cap, int *why, DWORD *err, DWORD *len)
{
	HANDLE f;
	LARGE_INTEGER whole;
	DWORD size, got = 0, e;
	char *buf;

	if (why) {
		*why = RT_UNREADABLE;
	}
	if (err) {
		*err = 0;
	}
	if (len) {
		*len = 0;
	}
	f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		e = GetLastError();
		if (err) {
			*err = e;
		}
		if (why && (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)) {
			*why = RT_MISSING;
		}
		return NULL;
	}
	/* GetFileSizeEx, not GetFileSize: the low DWORD alone reads an exactly-4-GiB file as 0 bytes, and
	 * an empty read of a file that is not empty is the replace-everything bug again (Codex uow-r1). */
	whole.QuadPart = 0;
	if (!GetFileSizeEx(f, &whole) || whole.QuadPart < 0 || whole.QuadPart > (LONGLONG)cap) {
		if (err) {
			*err = (whole.QuadPart > (LONGLONG)cap) ? ERROR_FILE_TOO_LARGE : GetLastError();
		}
		CloseHandle(f);
		return NULL;
	}
	size = (DWORD)whole.QuadPart;
	buf = (char *)malloc(size + 1);
	if (!buf) {
		if (err) {
			*err = ERROR_NOT_ENOUGH_MEMORY;
		}
		CloseHandle(f);
		return NULL;
	}
	if (!ReadFile(f, buf, size, &got, NULL) || got != size) {
		if (err) {
			*err = got != size ? ERROR_HANDLE_EOF : GetLastError();
		}
		free(buf);
		CloseHandle(f);
		return NULL;
	}
	buf[got] = 0;
	CloseHandle(f);
	if (why) {
		*why = RT_OK;
	}
	if (len) {
		*len = got;
	}
	return buf;
}

/* read_text_why for a caller to whom every failure means the same thing. */
static char *read_text(const wchar_t *path, DWORD cap)
{
	return read_text_why(path, cap, NULL, NULL, NULL);
}

/* base64url, no padding, decode. -1 on any character outside the alphabet. */
static int b64url_decode(const char *in, size_t inlen, unsigned char *out, size_t outcap)
{
	static const char *ALPHA =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	unsigned int acc = 0;
	int bits = 0;
	size_t i, n = 0;

	for (i = 0; i < inlen; i++) {
		const char *p = strchr(ALPHA, in[i]);
		if (!p || in[i] == 0) {
			return -1;
		}
		acc = (acc << 6) | (unsigned int)(p - ALPHA);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (n >= outcap) {
				return -1;
			}
			out[n++] = (unsigned char)((acc >> bits) & 0xff);
		}
	}
	return (int)n;
}

static void hex_of(const unsigned char *b, size_t n, char *out)
{
	static const char *H = "0123456789abcdef";
	size_t i;
	for (i = 0; i < n; i++) {
		out[2 * i] = H[b[i] >> 4];
		out[2 * i + 1] = H[b[i] & 15];
	}
	out[2 * n] = 0;
}

/* ============================================================================================
 * Configuration
 *
 * NOBODY TYPES A SERVER ADDRESS. The relay comes from the signed document, and this program then
 * writes it into the game's own Options.ini so the engine is zero-config too. Options.ini is still
 * READ first, but only as the thing an older install already has: it is a starting point for the
 * legacy UDP fallback, never a requirement, and an empty one is no longer an error.
 *
 * gxlaunch.ini beside the executable can override two things, and both exist for testing rather
 * than for a friend:
 *
 *   relay = host[:port]   PIN a relay. Suppresses selection AND the Options.ini write, so a
 *                         harness pointed at a private relay is not quietly repointed at the
 *                         published one halfway through a run.
 *   bootstrap = URL       fetch the signed document from somewhere else.
 *
 * The bootstrap override is SAFE to expose, and the reason is worth stating because it looks like
 * a hole: the document is refused unless it verifies against the key compiled in above. Pointing
 * this at an attacker's server gets you a signature failure and a normal launch. What an override
 * can do is serve an OLDER genuine document, and the ordinal check in wmain already refuses to
 * apply one older than the install.
 * ========================================================================================== */

typedef struct {
	char relay_host[128];
	int relay_port;
	int relay_pinned;		/* gxlaunch.ini named a relay: do not select, do not publish */
	char platform[16];		/* the manifest platform token this machine wants */
	char bootstrap[256];
} config;

/* Pull "key = value" out of an INI-ish text file. Case-insensitive key, trimmed value. */
static int ini_get(const char *text, const char *key, char *out, size_t cap)
{
	const char *p = text;
	size_t klen = strlen(key);

	/* GeneralsX @bugfix Claude 22/10/2026 A UTF-8 BOM at the start of the file (a hand edit in an older Notepad) is not
	 * part of the first key - the rule the game (UserPreferences::load) and the launcher (gxio strip_bom) read by, so a
	 * BOM-first RelayAddress is the relay all three see (asm-0967, Codex r2 P2). */
	if (p && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
		p += 3;
	}

	while (p && *p) {
		const char *eol = strchr(p, '\n');
		const char *line = p;
		while (*line == ' ' || *line == '\t') {
			line++;
		}
		if (_strnicmp(line, key, klen) == 0) {
			const char *q = line + klen;
			while (*q == ' ' || *q == '\t') {
				q++;
			}
			if (*q == '=') {
				size_t n = 0;
				q++;
				while (*q == ' ' || *q == '\t') {
					q++;
				}
				while (*q && *q != '\r' && *q != '\n' && n + 1 < cap) {
					out[n++] = *q++;
				}
				while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t')) {
					n--;
				}
				out[n] = 0;
				return n > 0;
			}
		}
		p = eol ? eol + 1 : NULL;
	}
	return 0;
}

/* The engine's user-data directory, resolved EXACTLY as the engine resolves it, with leafFile appended.
 *
 * GeneralsX @feature Claude 23/09/2026 That is now GeneralsX's OWN folder,
 * <Documents>\GeneralsX\GeneralsZH (GlobalData.cpp, BuildUserDataPathFromRegistry). It used to be
 * Documents\<registry UserDataLeafName, default "Command and Conquer Generals Zero Hour Data">, the
 * folder retail Zero Hour and the Generals Online client share. This program ships beside
 * generalszh.exe in the same bundle, so it switches with the engine and no version skew is possible.
 * The engine copies the player's old settings across once, on its first start (GXUserDataMigrate.h).
 *
 * This is the ONLY resolver here. There used to be two: options_ini_path hardcoded the English leaf
 * and used SHGetFolderPathW, while the assertion path went through this one. On a localised install
 * the two named different folders. Both callers now go through this one.
 *
 * SHGetKnownFolderPath(FOLDERID_Documents) is the engine's own call: it follows OneDrive Known Folder
 * Move and Group Policy redirection, which a %USERPROFILE%\Documents guess does not. CSIDL_PERSONAL is
 * the engine's fallback for a shell32 without it, so it is ours too. */
typedef HRESULT (WINAPI *pfn_known_folder)(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR *);

static int userdata_path(wchar_t *out, size_t cap, const wchar_t *leafFile)
{
	wchar_t docs[MAX_PATH];
	HMODULE shell32;
	pfn_known_folder known = NULL;
	int have_docs = 0;

	docs[0] = 0;
	shell32 = GetModuleHandleW(L"shell32.dll");
	if (shell32) {
		known = (pfn_known_folder)GetProcAddress(shell32, "SHGetKnownFolderPath");
	}
	if (known) {
		PWSTR wide = NULL;
		if (SUCCEEDED(known(&FOLDERID_Documents, 0, NULL, &wide)) && wide) {
			wcscpy_s(docs, MAX_PATH, wide);
			CoTaskMemFree(wide);
			have_docs = 1;
		}
	}
	if (!have_docs) {
		if (FAILED(SHGetFolderPathW(NULL, 0x0005 /* CSIDL_PERSONAL */, NULL, 0, docs))) {
			return 0;
		}
	}

	swprintf_s(out, cap, L"%s\\GeneralsX\\GeneralsZH\\%s", docs, leafFile);
	return 1;
}

static void load_config(config *c)
{
	wchar_t path[MAX_PATH];
	char *text;
	char value[256];

	memset(c, 0, sizeof(*c));
	c->relay_port = RELAY_LOBBY_PORT;
	strcpy_s(c->platform, sizeof(c->platform), "win32");
	strcpy_s(c->bootstrap, sizeof(c->bootstrap), GX_BOOTSTRAP_URL);

	/* What the game already believes, if anything. No longer required, and no longer a warning
	 * when it is missing - a fresh install has no Options.ini and that is the normal case now. */
	if (userdata_path(path, MAX_PATH, L"Options.ini")) {
		text = read_text(path, 1 << 20);
		if (text) {
			if (ini_get(text, "RelayAddress", value, sizeof(value))) {
				/* S1-3: publish_relay writes "host:port" for a relay off the default lobby port (relay_line). A
				 * colon followed by digits only is that port; anything else stays the host, as before. */
				char *colon = strrchr(value, ':');
				if (colon && colon[1] && strspn(colon + 1, "0123456789") == strlen(colon + 1)
					&& atoi(colon + 1) > 0 && atoi(colon + 1) <= 65533) {
					*colon = 0;
					c->relay_port = atoi(colon + 1);
				}
				strcpy_s(c->relay_host, sizeof(c->relay_host), value);
			}
			free(text);
		}
	}

	/* And the testing override beside the executable. */
	path_join(path, MAX_PATH, g_dir, L"gxlaunch.ini");
	text = read_text(path, 1 << 16);
	if (text) {
		if (ini_get(text, "relay", value, sizeof(value))) {
			char *colon = strrchr(value, ':');
			if (colon) {
				*colon = 0;
				c->relay_port = atoi(colon + 1);
			}
			strcpy_s(c->relay_host, sizeof(c->relay_host), value);
			c->relay_pinned = 1;
			logf_("config: relay PINNED by gxlaunch.ini - selection and the Options.ini write "
				"are both suppressed");
		}
		if (ini_get(text, "platform", value, sizeof(value))) {
			strcpy_s(c->platform, sizeof(c->platform), value);
		}
		if (ini_get(text, "bootstrap", value, sizeof(value))) {
			strcpy_s(c->bootstrap, sizeof(c->bootstrap), value);
			logf_("config: bootstrap URL overridden by gxlaunch.ini");
		}
		free(text);
	}
}

/* WINDOWS REPORTS SOMEBODY ELSE'S ICMP ON YOUR NEXT RECV, AND IT WILL COST YOU A RELAY.
 *
 * A UDP sendto to a host that is up but has nothing listening draws an ICMP port-unreachable, and
 * Winsock delivers that to the sending socket as WSAECONNRESET (10054) on the NEXT recv - not on
 * the send that caused it. On a socket used to probe several relays at once, one dead relay
 * therefore makes the recv that was waiting for the LIVE relay's reply fail instead. The live relay
 * is then never selected, and the symptom is "selection picks the wrong relay, but only sometimes,
 * and only when one is down" - which is a fault nobody finds by reading the selection code, because
 * the selection code is correct.
 *
 * SIO_UDP_CONNRESET FALSE turns it off. It is Windows-specific, it has to be set per socket, and it
 * is ignored on anything that does not implement it - so it is safe to call unconditionally.
 * Callers ALSO have to treat a recv error as "keep waiting" rather than "stop", because this is not
 * the only way a UDP recv can fail spuriously; see the loop in select_relay.
 *
 * MEASURED, not reasoned about. test-bootstrap.js lists a dark relay FIRST and a live one second,
 * and the four builds were run on the Windows box 09/08/2026:
 *
 *     ioctl   tolerant loop   result
 *     yes     yes             selects the live relay
 *     no      yes             selects the live relay
 *     yes     no              selects the live relay
 *     no      no              SELECTS THE DEAD RELAY - "no relay answered inside 800 ms"
 *
 * So the defect is real and reproducible, and either mechanism alone closes it. Both are kept
 * because they close it in different places: the ioctl stops the error being raised at all, and the
 * loop survives any other transient this socket might ever be handed. The suite passes with the
 * ioctl removed, which is why removing it is not evidence that it is doing nothing - the row that
 * matters is the last one. */
static void udp_ignore_conn_reset(SOCKET s)
{
	DWORD off = 0, ret = 0;
	WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), NULL, 0, &ret, NULL, NULL);
}

/* ============================================================================================
 * The LEGACY update check: GXUPD out, GXMANI back. See tools/relay/relay.js.
 *
 * This was the only way to get a manifest before the HTTPS bootstrap existed, and it is now the
 * last of three sources rather than the first - see bootstrap_manifest. It survives for one
 * reason: an install made before the bootstrap has a RelayAddress in Options.ini and nothing else,
 * and it must keep updating itself even if it can never reach the published document. Deleting it
 * would strand exactly the installs that most need an update.
 * ========================================================================================== */

static int fetch_manifest(const config *c, unsigned long installed_ordinal,
                          char *out, size_t outcap)
{
	WSADATA wsa;
	SOCKET s = INVALID_SOCKET;
	struct addrinfo hints, *res = NULL;
	char port[16], req[64];
	int attempt, got = -1;

	if (!c->relay_host[0]) {
		logf_("update: no signed document could be fetched and this install has no relay to ask "
			"- starting the game");
		return -1;
	}
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		logf_("update: WSAStartup failed");
		return -1;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	sprintf_s(port, sizeof(port), "%d", c->relay_port);
	if (getaddrinfo(c->relay_host, port, &hints, &res) != 0 || !res) {
		logf_("update: cannot resolve relay '%s' - starting the game", c->relay_host);
		WSACleanup();
		return -1;
	}

	s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET) {
		freeaddrinfo(res);
		WSACleanup();
		return -1;
	}
	{
		DWORD tv = UPD_TIMEOUT_MS;
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
	}
	udp_ignore_conn_reset(s);

	sprintf_s(req, sizeof(req), "GXUPD %lu", installed_ordinal);

	/* Three tries, because this is UDP and one lost datagram must not read as "no manifest".
	 * Three failures DO read as that - an update check is not allowed to hold up a launch. */
	for (attempt = 0; attempt < UPD_ATTEMPTS; attempt++) {
		int n;
		sendto(s, req, (int)strlen(req), 0, res->ai_addr, (int)res->ai_addrlen);
		n = recv(s, out, (int)outcap - 1, 0);
		if (n > 0) {
			out[n] = 0;
			got = n;
			break;
		}
	}
	closesocket(s);
	freeaddrinfo(res);
	WSACleanup();

	if (got < 0) {
		logf_("update: relay did not answer in %d tries - starting the game",
			UPD_ATTEMPTS);
		return -1;
	}
	return got;
}

/* ============================================================================================
 * The manifest
 *
 * Parsed by splitting on '|', ';' and ',' - no field may contain any of them, which the signer
 * enforces. That is the entire reason the wire form is not JSON: this is the parser, in a program
 * that has no JSON library and must not grow one.
 * ========================================================================================== */

typedef struct {
	char platform[16];
	char name[128];
	char hash[32];			/* base64url of the first 16 bytes of SHA-256 */
	unsigned __int64 size;
} artifact;

typedef struct {
	char host[128];
	int port;				/* an EMPTY port field in the document means RELAY_LOBBY_PORT */
	char region[16];
} relay;

typedef struct {
	int version;
	unsigned long ordinal;
	unsigned long minimum;
	int rank;				/* S1-3: which pinned key verified it, 1 = K1 */
	int has_serial;			/* S1-3: field 9 present and decimal */
	unsigned __int64 serial;	/* field 9, the signing time */
	char base[160];
	relay relays[MAX_RELAYS];
	int nrelays;
	artifact items[MAX_ARTIFACTS];
	int nitems;
} manifest;

/* Split `s` in place on `sep`, returning the number of pieces written to `parts`. */
static int split(char *s, char sep, char **parts, int maxparts)
{
	int n = 0;
	parts[n++] = s;
	while (*s && n < maxparts) {
		if (*s == sep) {
			*s = 0;
			parts[n++] = s + 1;
		}
		s++;
	}
	return n;
}

/* Verify the signature and parse. `wire` is the datagram body after "GXMANI ". Returns 1 on a
 * manifest that is BOTH signed by our key and understood. */
static int parse_manifest(char *wire, manifest *m)
{
	char *cut, *sigtext;
	unsigned char sig[64];
	char *f[16];
	int nf, i, rank = 0;

	/* The signature is everything after the LAST separator; the payload is everything before
	 * it, verbatim, exactly as it was signed. No re-serialisation, no canonical form to agree
	 * on with the signer - that is the whole point of the encoding. */
	cut = strrchr(wire, '|');
	if (!cut) {
		logf_("manifest: no signature separator - ignoring");
		return 0;
	}
	*cut = 0;
	sigtext = cut + 1;
	if (strlen(sigtext) != 86 || b64url_decode(sigtext, 86, sig, sizeof(sig)) != 64) {
		logf_("manifest: signature is not 64 bytes of base64url - ignoring");
		return 0;
	}

	/* S1-3: every provisioned key, rank order; an all-zero slot is skipped, never "verified" against. */
	for (i = 0; i < 3 && rank == 0; i++) {
		int k, any = 0;
		for (k = 0; k < 32; k++) {
			any |= GX_MANIFEST_KEYS[i][k];
		}
		if (any && gx_ed25519_verify(GX_MANIFEST_KEYS[i], sig, wire, strlen(wire))) {
			rank = i + 1;
		}
	}
	if (rank == 0) {
		/* The one refusal that matters. A manifest that does not verify is not a damaged
		 * manifest to be worked around - it is either corruption or somebody trying to choose
		 * what executable this machine downloads, and there is no way to tell which. */
		logf_("manifest: SIGNATURE DOES NOT VERIFY - refusing it and starting the installed game");
		return 0;
	}
	logf_("manifest: signature OK (key k%d, %d bytes signed)", rank, (int)strlen(wire));

	nf = split(wire, '|', f, 16);
	if (nf < 7) {
		logf_("manifest: %d fields, expected at least 7 - ignoring", nf);
		return 0;
	}
	memset(m, 0, sizeof(*m));
	m->version = atoi(f[0]);
	if (m->version != MANIFEST_VERSION) {
		/* Refusing to guess. A newer format may mean the same bytes differently, and this
		 * program's job when it does not understand something is to start the game. */
		logf_("manifest: version %d, this launcher speaks %d - ignoring",
			m->version, MANIFEST_VERSION);
		return 0;
	}
	m->rank = rank;
	/* S1-3 (plan §3.3, review r2): BOUND-CHECKED, not strtoul's silent saturation. A signed ordinal past 32 bits
	 * used to read as ULONG_MAX and became this machine's floor for good - every later document "older". Such a
	 * document is refused instead; the ordinal is a %ct, which fits 32 bits until 2106. */
	{
		unsigned __int64 o = _strtoui64(f[1], NULL, 10), mn = _strtoui64(f[2], NULL, 10);
		if (!*f[1] || strspn(f[1], "0123456789") != strlen(f[1]) || strlen(f[1]) > 10 || o > 0xFFFFFFFFull
			|| strspn(f[2], "0123456789") != strlen(f[2]) || strlen(f[2]) > 10 || mn > 0xFFFFFFFFull) {
			logf_("manifest: ordinal or minimum is not a 32-bit decimal - ignoring");
			return 0;
		}
		m->ordinal = (unsigned long)o;
		m->minimum = (unsigned long)mn;
	}
	/* Fields 7-9 (master, alt, serial) are appended by S1-3 signers. Only the serial is read here, and only
	 * when it is a plain decimal: it breaks a same-ordinal tie in manifest_supersedes. The standbys in field
	 * 8 are the ENGINE's (GXRelayList); this program publishes the home only. */
	m->has_serial = 0;
	m->serial = 0;
	if (nf >= 10 && *f[9] && strlen(f[9]) <= 12 && strspn(f[9], "0123456789") == strlen(f[9])) {
		m->has_serial = 1;
		m->serial = _strtoui64(f[9], NULL, 10);
	}
	strcpy_s(m->base, sizeof(m->base), f[5]);

	/* THE RELAY LIST, field 4. This program used to skip it, because the relay it was talking to
	 * was the relay it had been handed. It is now the whole point of the document: this is where
	 * "adding a relay is data, not a release" actually lands. */
	if (*f[4]) {
		char *items[MAX_RELAYS + 1];
		int ni = split(f[4], ';', items, MAX_RELAYS + 1);
		for (i = 0; i < ni && m->nrelays < MAX_RELAYS; i++) {
			char *p[5];
			int np = split(items[i], ',', p, 5);
			relay *r;
			if (np != 3) {
				logf_("manifest: relay item %d has %d parts, expected 3 - ignoring it", i, np);
				continue;
			}
			if (!*p[0]) {
				continue;
			}
			r = &m->relays[m->nrelays++];
			strcpy_s(r->host, sizeof(r->host), p[0]);
			/* An empty port is the default, and it is empty in every normal document precisely
			 * because spelling it out costs four bytes of a 476-byte datagram. */
			r->port = *p[1] ? atoi(p[1]) : RELAY_LOBBY_PORT;
			if (r->port <= 0 || r->port > 65535) {
				r->port = RELAY_LOBBY_PORT;
			}
			strcpy_s(r->region, sizeof(r->region), p[2]);
		}
	}

	/* Fields beyond the seventh are IGNORED on purpose (the serial above aside): appending one is how the
	 * format grows without breaking a client that has not been rebuilt, and a parser that refused them would
	 * be the thing that broke it. */

	if (*f[6]) {
		char *items[MAX_ARTIFACTS + 1];
		int ni = split(f[6], ';', items, MAX_ARTIFACTS + 1);
		for (i = 0; i < ni && m->nitems < MAX_ARTIFACTS; i++) {
			char *p[6];
			int np = split(items[i], ',', p, 6);
			artifact *a;
			if (np != 4) {
				logf_("manifest: build item %d has %d parts, expected 4 - ignoring it", i, np);
				continue;
			}
			a = &m->items[m->nitems++];
			strcpy_s(a->platform, sizeof(a->platform), p[0]);
			strcpy_s(a->name, sizeof(a->name), p[1]);
			strcpy_s(a->hash, sizeof(a->hash), p[2]);
			a->size = _strtoui64(p[3], NULL, 10);
		}
	}
	logf_("manifest: ordinal %lu, minimum %lu, %d relay(s), %d artifact(s), base '%s'",
		m->ordinal, m->minimum, m->nrelays, m->nitems, m->base);
	return 1;
}

/* ============================================================================================
 * Download, over WinHTTP
 *
 * The transport is not what makes this trustworthy - the signed manifest and the hash below are -
 * which is why plain http is accepted as well as https. That is the same judgement apt makes, and
 * it matters here because the payload host is manifest data and may one day be something without
 * a certificate. What is NOT optional is the hash: an artifact that does not match is deleted and
 * the whole update is abandoned.
 * ========================================================================================== */

/* A GET that has already returned 200, or nothing. Both readers below need exactly this much and
 * nothing about it differs between them, so it is one function - a second copy of the URL cracking
 * and the status check is a second place for them to drift, and the status check in particular is
 * the one this program cannot afford to get differently wrong in two places. */
typedef struct {
	HINTERNET ses, con, req;
} http;

static void http_close(http *h)
{
	if (h->req) {
		WinHttpCloseHandle(h->req);
	}
	if (h->con) {
		WinHttpCloseHandle(h->con);
	}
	if (h->ses) {
		WinHttpCloseHandle(h->ses);
	}
	memset(h, 0, sizeof(*h));
}

static int http_begin(const char *url, const char *what, http *h)
{
	wchar_t wurl[1024], host[256], path[1024];
	URL_COMPONENTS uc;
	DWORD status = 0, statuslen = sizeof(status);

	memset(h, 0, sizeof(*h));
	MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024);

	memset(&uc, 0, sizeof(uc));
	uc.dwStructSize = sizeof(uc);
	uc.lpszHostName = host;
	uc.dwHostNameLength = 256;
	uc.lpszUrlPath = path;
	uc.dwUrlPathLength = 1024;
	if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
		logf_("%s: '%s' is not a URL this launcher can parse", what, url);
		return 0;
	}
	if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS) {
		logf_("%s: refusing scheme %d - only http and https are fetched", what, uc.nScheme);
		return 0;
	}

	h->ses = WinHttpOpen(L"GeneralsX-Launcher/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!h->ses) {
		logf_("%s: WinHttpOpen failed (%lu)", what, GetLastError());
		return 0;
	}
	WinHttpSetTimeouts(h->ses, 10000, 10000, 30000, 60000);

	h->con = WinHttpConnect(h->ses, host, uc.nPort, 0);
	if (h->con) {
		h->req = WinHttpOpenRequest(h->con, L"GET", path, NULL, WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES,
			uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
	}
	if (!h->req) {
		logf_("%s: cannot open a request to %S (%lu)", what, host, GetLastError());
		http_close(h);
		return 0;
	}
	if (!WinHttpSendRequest(h->req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
			0, 0, 0)
		|| !WinHttpReceiveResponse(h->req, NULL)) {
		logf_("%s: no response from %S (%lu)", what, host, GetLastError());
		http_close(h);
		return 0;
	}
	/* The status code is CHECKED. The engine's existing UpdateChecker never inspected one, so a
	 * rate-limit page was indistinguishable from "no update"; here it would be indistinguishable
	 * from a game build, which is worse. */
	if (!WinHttpQueryHeaders(h->req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &status, &statuslen, WINHTTP_NO_HEADER_INDEX)
		|| status != 200) {
		logf_("%s: HTTP %lu for %s", what, status, url);
		http_close(h);
		return 0;
	}
	return 1;
}

/* Read a small body into memory. Used for the signed bootstrap document, which is a few hundred
 * bytes. A body that does not fit is a REFUSAL rather than a truncation: a truncated signed
 * document is not a smaller signed document, it is a signature failure that would be reported as a
 * possible key compromise. */
static int http_get_text(const char *url, char *out, size_t outcap)
{
	http h;
	size_t total = 0;
	int ok = 0;

	if (!http_begin(url, "bootstrap", &h)) {
		return -1;
	}
	for (;;) {
		DWORD got = 0;
		if (!WinHttpReadData(h.req, out + total, (DWORD)(outcap - 1 - total), &got)) {
			logf_("bootstrap: read failed after %llu bytes (%lu)",
				(unsigned __int64)total, GetLastError());
			goto done;
		}
		if (got == 0) {
			ok = 1;
			break;
		}
		total += got;
		if (total >= outcap - 1) {
			logf_("bootstrap: document is larger than %llu bytes - refusing it",
				(unsigned __int64)(outcap - 1));
			goto done;
		}
	}

done:
	http_close(&h);
	if (!ok) {
		return -1;
	}
	out[total] = 0;
	/* Trailing whitespace is trimmed because a text file is the kind of thing that acquires a
	 * newline on its way through a tool, and a byte after the signature is a byte that breaks the
	 * signature. Nothing INSIDE the document can be whitespace: every field's charset forbids it,
	 * enforced by gx-manifest.py at signing time. */
	while (total > 0 && (unsigned char)out[total - 1] <= ' ') {
		out[--total] = 0;
	}
	return (int)total;
}

/* ============================================================================================
 * ONE UPDATER PER INSTALL
 *
 * GeneralsX @bugfix Claude 19/09/2026 Nothing stopped two of these running against one install,
 * and every caller spawns one per press: the Tauri launcher's UPDATE, the C# loader's Play. The
 * owner's gxlaunch.log on 19/09 shows ELEVEN runs in three minutes, overlapping. Each one begins
 * with rmtree(gxwork) - deleting the files another run had already downloaded - then races it for
 * the same file names ("download: cannot create ...gxwork\gxrt_d983ed3a.zip (32)", a sharing
 * violation). None of them ever reached "applied". And a run that ABANDONED still exited 0 under
 * --no-launch, so the launcher re-armed UPDATE and the next press started another one.
 *
 * AN EXCLUSIVE FILE IN THE INSTALL DIRECTORY makes a second run impossible rather than unlikely.
 * It is taken before this program writes anything - before sweep_old, before gxwork - and a second
 * run does not fail: it ATTACHES, relaying the running update's progress from a small shared-memory
 * block until the first one exits, then takes the lock and runs normally. That run then finds
 * nothing to fetch if the first succeeded, and finishes the job if it did not. --check never waits;
 * it answers "busy" and exits 3, which both launchers already read as "unknown".
 *
 * GeneralsX @bugfix Claude 20/09/2026 THIS WAS A NAMED MUTEX IN "Local\", AND THAT SCOPE WAS WRONG.
 * "Local\" is per TERMINAL SERVICES SESSION - the object lives in \Sessions\<n>\BaseNamedObjects -
 * not per directory. A console logon and an OpenSSH logon are different sessions, which is exactly
 * how this project drives the Windows box, so two runs against ONE install took two DIFFERENT
 * mutexes, both believed they were alone, and both reached rmtree(gxwork): the original bug
 * verbatim. The install is not per-user either - g_dir is only ever "where the exe lives", and this
 * repo builds a machine-wide C:\dev\GeneralsX-run.
 *
 * The thing being protected is A DIRECTORY, so the lock is a FILE in that directory: CREATE_ALWAYS,
 * dwShareMode 0, FILE_FLAG_DELETE_ON_CLOSE. It needs no privilege (unlike "Global\", which wants
 * SeCreateGlobalPrivilege that a standard user does not have), it is one lock across every session
 * and every user, a stale one left by a power cut is simply overwritten, and the kernel releases it
 * when this process dies - which is the job WAIT_ABANDONED used to do.
 *
 * The progress block is still a "Local\" mapping named after the directory's FINAL path,
 * lowercased, so a different spelling of the same folder (case, a junction, an 8.3 name) relays
 * through the same block and two separate installs never share one. ponytail: an attached run in a
 * DIFFERENT session now waits correctly but shows no byte counts, because that mapping is still
 * session-scoped; give it a "Global\" name with a "Local\" fallback if that case ever needs numbers.
 * ========================================================================================== */

static HANDLE g_lock;
/* [0] bytes done, [1] bytes total (0 = unknown), across the WHOLE update; [2] a GENERATION counter
 * bumped once per update that replaced GeneralsXLauncher.exe. Written by the lock holder, read by
 * an attached run. Aligned 64-bit reads and writes are atomic on x64.
 *
 * GeneralsX @bugfix Claude 20/09/2026 [2] used to be a FLAG that the first waiter cleared on
 * acquiring the lock, so with three or more runs the restart reached exactly one of them and the
 * rest kept running the executable the update had just replaced. A counter that is never cleared,
 * compared against the value each run snapshotted before it started waiting, reaches all of them. */
static volatile LONG64 *g_shared;
/* An attached run's caller may be a DIFFERENT launcher from the one that started the holder - the
 * holder's caller may have been closed. So a restart the holder asked for is passed on, or the
 * live launcher keeps running the executable the update just replaced. */
static int g_relay_restart;

static void instance_name(wchar_t *out, size_t cap, const wchar_t *suffix)
{
	wchar_t canon[MAX_PATH + 8];
	const wchar_t *p;
	unsigned __int64 h = 1469598103934665603ULL;		/* FNV-1a 64 */
	DWORD n = 0;
	HANDLE d = CreateFileW(g_dir, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
		OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);

	/* The RETURN VALUE decides, never wcslen: a target longer than the buffer returns the size it
	 * needs and leaves the buffer unterminated. Falling back to g_dir costs only the junction case. */
	if (d != INVALID_HANDLE_VALUE) {
		n = GetFinalPathNameByHandleW(d, canon, MAX_PATH + 8, FILE_NAME_NORMALIZED);
	}
	if (n == 0 || n >= MAX_PATH + 8) {
		wcscpy_s(canon, MAX_PATH + 8, g_dir);
	}
	if (d != INVALID_HANDLE_VALUE) {
		CloseHandle(d);
	}
	_wcslwr_s(canon, MAX_PATH + 8);
	for (p = canon; *p; p++) {
		h ^= (unsigned short)*p;
		h *= 1099511628211ULL;
	}
	swprintf_s(out, cap, L"Local\\GeneralsXUpdate-%016llx%s", h, suffix);
}

/* Returns 1 holding the lock, 0 when this run must not touch the install (--check found it busy,
 * or the lock could not be created at all - fail closed: an update skipped is recoverable, two
 * updaters in one directory are not). */
static int acquire_instance(int check_only)
{
	wchar_t name[96], lockpath[MAX_PATH];
	HANDLE map;
	LONG64 last_done = -1, last_total = -1, seen_gen = 0;
	int attached = 0, denied = 0;

	instance_name(name, 96, L"-progress");
	map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, name);
	if (map) {
		g_shared = (volatile LONG64 *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
	}
	/* The restart generation is snapshotted BEFORE the wait, so every run that waits through an
	 * update sees the counter move - not just whichever one happened to take the lock first. */
	if (g_shared) {
		seen_gen = g_shared[2];
	}

	path_join(lockpath, MAX_PATH, g_dir, L"gxwork.lock");
	for (;;) {
		DWORD e;
		g_lock = CreateFileW(lockpath, GENERIC_WRITE | DELETE, 0, NULL, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, NULL);
		if (g_lock != INVALID_HANDLE_VALUE) {
			break;		/* held, and held until this process closes the handle or dies */
		}
		g_lock = NULL;		/* CreateFileW fails with INVALID_HANDLE_VALUE, which is not NULL */
		e = GetLastError();
		/* ERROR_ACCESS_DENIED is AMBIGUOUS here and both meanings are reachable: the holder has
		 * closed its handle but the delete is still pending (milliseconds, and exactly the instant
		 * an attached run retries), or this directory is not writable at all. So it is retried like
		 * a sharing violation, but only briefly, and then fails closed. */
		if (e == ERROR_SHARING_VIOLATION) {
			denied = 0;
		} else if (e != ERROR_ACCESS_DENIED || ++denied > 25) {
			logf_("update: cannot take the single-instance lock %S (%lu) - not updating",
				lockpath, e);
			return 0;
		}
		if (check_only) {
			printf("busy: an update is already running for this install\n");
			return 0;
		}
		if (!attached) {
			attached = 1;
			logf_("update: another update is already running for this install - following it");
			printf("attached: an update is already running for this install\n");
		}
		if (g_shared) {
			LONG64 d = g_shared[0], t = g_shared[1];
			if (d != last_done || t != last_total) {
				last_done = d;
				last_total = t;
				printf("progress: %lld %lld\n", d, t);
			}
		}
		Sleep(200);
	}
	if (g_shared) {
		g_relay_restart = attached && g_shared[2] != seen_gen;
		g_shared[0] = 0;
		g_shared[1] = 0;
	}
	if (attached) {
		logf_("update: the other update finished - checking again");
	}
	return 1;
}

/* The progress line and the shared block, together, so an attached run always shows exactly what
 * the owner shows. `done` and `total` are for the WHOLE update, not the current file: per-file
 * numbers are what made the owner's button read "5.2 / 7.1 MB" and then "1.0 / 4.3 MB". */
static void report_progress(unsigned __int64 done, unsigned __int64 total)
{
	if (g_shared) {
		g_shared[1] = (LONG64)total;
		g_shared[0] = (LONG64)done;
	}
	printf("progress: %llu %llu\n", done, total);
}

/* `base` is what earlier files of this update already delivered and `grand` the whole update's
 * size (0 when any artifact's size is unknown), so the progress this reports is aggregate. */
static int download(const char *url, const wchar_t *dest, unsigned __int64 expect_size,
	unsigned __int64 base, unsigned __int64 grand, unsigned __int64 *received)
{
	http h;
	HANDLE out = INVALID_HANDLE_VALUE;
	unsigned __int64 total = 0;
	int ok = 0;
	char *buf = NULL;

	if (!http_begin(url, "download", &h)) {
		logf_("download: abandoning the update");
		return 0;
	}

	out = CreateFileW(dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (out == INVALID_HANDLE_VALUE) {
		logf_("download: cannot create %S (%lu)", dest, GetLastError());
		goto done;
	}
	buf = (char *)malloc(1 << 16);
	if (!buf) {
		goto done;
	}
	{
	/* Machine-readable transfer progress on stdout, for the launcher's progress bar.
	 *
	 * Deliberately NOT routed through logf_: that stamps every line with a timestamp and appends
	 * it to gxlaunch.log, and a 14 MB download at 64 KB a chunk is 230 lines the log does not
	 * want and a human reading it does not want either. This is a separate one-line channel the
	 * launcher parses and the user never sees.
	 *
	 * The total is the sum of the expected sizes from the SIGNED manifest, not Content-Length - a
	 * server is free to lie about the latter, and the bar should reflect what we agreed to fetch.
	 * It is 0 when any artifact omits a size, and the launcher renders a marquee in that case
	 * rather than inventing a denominator. Both numbers cover the whole update (report_progress).
	 *
	 * Emitted at most ~20 times a second: a 64 KB chunk on a fast link arrives far more often
	 * than a progress bar can repaint, and an unbuffered write per chunk would make the pipe the
	 * bottleneck for the download it is measuring. The final chunk always reports, so the bar
	 * always lands exactly on full rather than stopping at 99%. */
	DWORD last_tick = 0;
	for (;;) {
		DWORD got = 0, wrote = 0, now;
		if (!WinHttpReadData(h.req, buf, 1 << 16, &got)) {
			logf_("download: read failed after %llu bytes (%lu)", total, GetLastError());
			goto done;
		}
		if (got == 0) {
			report_progress(base + total, grand);
			break;
		}
		total += got;
		if (total > MAX_DOWNLOAD_BYTES) {
			logf_("download: over %u bytes - abandoning", MAX_DOWNLOAD_BYTES);
			goto done;
		}
		if (!WriteFile(out, buf, got, &wrote, NULL) || wrote != got) {
			logf_("download: write failed - is the disk full?");
			goto done;
		}
		now = GetTickCount();
		if (now - last_tick >= 50) {
			last_tick = now;
			report_progress(base + total, grand);
		}
	}
	}

	/* A truncated transfer that ended cleanly is the failure this catches: the hash would catch
	 * it too, but this names it, and naming it is the difference between "your download was cut
	 * short" and "your update is corrupt", which send a friend to two different places. */
	if (expect_size && total != expect_size) {
		logf_("download: got %llu bytes, the manifest says %llu - abandoning", total, expect_size);
		goto done;
	}
	ok = 1;
	*received = total;
	logf_("download: %llu bytes from %s", total, url);

done:
	free(buf);
	if (out != INVALID_HANDLE_VALUE) {
		CloseHandle(out);
	}
	if (!ok) {
		DeleteFileW(dest);
	}
	http_close(&h);
	return ok;
}

/* The manifest carries the first 16 bytes of the artifact's SHA-256. See gx-manifest.py for why
 * 128 bits is the right length in a SIGNED document and not a weakening. */
static int hash_matches(const wchar_t *file, const char *want_b64)
{
	unsigned char full[32], want[32];
	int wantlen;
	char got[65], exp[65];

	wantlen = b64url_decode(want_b64, strlen(want_b64), want, sizeof(want));
	if (wantlen <= 0 || wantlen > 32) {
		logf_("verify: manifest hash '%s' is not base64url", want_b64);
		return 0;
	}
	if (!gx_sha256_file(file, full)) {
		logf_("verify: cannot hash %S", file);
		return 0;
	}
	if (memcmp(full, want, (size_t)wantlen) == 0) {
		return 1;
	}
	hex_of(full, (size_t)wantlen, got);
	hex_of(want, (size_t)wantlen, exp);
	logf_("verify: HASH MISMATCH for %S - got %s..., manifest says %s...", file, got, exp);
	return 0;
}

/* ============================================================================================
 * The bootstrap: how a machine that has never spoken to a relay finds one
 *
 * The old answer was relay-settings.ini, dropped in the zip by whoever invited you. That is the
 * step this whole feature exists to delete, and it failed in the most annoying possible way: a copy
 * of the game passed on second-hand had no sidecar, so the launcher dead-ended on "this copy did
 * not come with a server address" and the friend had to go and ask somebody for one.
 *
 * The new answer is a signed document fetched over HTTPS from GX_BOOTSTRAP_URL. It is the SAME
 * document the relay serves over UDP, so the relay list, the build list and the signature are all
 * one thing. Three sources are tried in order, and every one of them fails into the next rather
 * than into a dialog:
 *
 *   1. HTTPS. Authoritative. This is the publisher's copy and it is the one the operator updates.
 *   2. The on-disk cache of the last document that VERIFIED. Re-verified on load, never trusted
 *      because it is local - a file beside the executable is not more trustworthy than the network,
 *      it is just closer. This is what makes a launch work on a machine that is briefly offline.
 *   3. The legacy UDP GXUPD, against whatever RelayAddress the install already had. Only reachable
 *      by an install that predates this, and kept for exactly that install.
 *
 * And if all three fail, the game starts. That rule is older than this feature and outranks it.
 * ========================================================================================== */

/* `at`: the acceptance time to stamp as the file's write time (Unix seconds; 0 = leave the OS's). manifest_supersedes
 * judges the held serial on that time, so it must never move BACK: the caller passes max(now, the previous cache's
 * time) - a document accepted while the clock was wound back would otherwise carry the wound-back time, and on the
 * next check an older same-ordinal list could replace it (Codex relaylist-v2-r8). */
static void cache_save(const char *wire, unsigned __int64 at)
{
	wchar_t path[MAX_PATH], tmp[MAX_PATH];
	HANDLE f;
	DWORD wrote;
	size_t len = strlen(wire);

	path_join(path, MAX_PATH, g_dir, MANIFEST_CACHE);
	path_join(tmp, MAX_PATH, g_dir, L"gxmanifest.tmp");

	f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		return;			/* a cache that cannot be written costs the next OFFLINE launch, nothing else */
	}
	WriteFile(f, wire, (DWORD)len, &wrote, NULL);
	if (at > 0) {
		ULARGE_INTEGER t;
		FILETIME ft;
		t.QuadPart = at * 10000000ull + 116444736000000000ull;
		ft.dwLowDateTime = t.LowPart;
		ft.dwHighDateTime = t.HighPart;
		SetFileTime(f, NULL, NULL, &ft);	/* after the last write; a move keeps it */
	}
	CloseHandle(f);
	/* Atomic, so a crash mid-write cannot leave a half-document that fails to verify on the next
	 * launch and gets reported as a bad signature. */
	MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING);
}

/* GeneralsX @feature Claude 29/10/2026 S1-3: may `n` replace the held document `h`? RANK FIRST: a lower-ranked
 * document never replaces a higher-ranked one, and a higher-ranked one always does, whatever its ordinal - so a K2
 * recovery document lands even on a machine whose K1 ordinal floor a stolen key has poisoned (the floor is per
 * rank, plan §3.3). Within a rank the old rule: an older ordinal is a rollback, and at an EQUAL ordinal so is a
 * lower serial (a relay-only re-sign). Equal everything is a re-fetch, not a rollback. gxcore supersedes is the
 * same rule; the shared fixtures hold both to it.
 *
 * THE HORIZON (Codex relaylist-v2-r2 P1, r3 P2). A serial more than SERIAL_HORIZON_S past the moment it is judged
 * on counts as NO serial: a cached far-future serial (a stolen key's) cannot block every relay-only recovery at its
 * ordinal - and with it every RelayAddress this program publishes - and a far-future newcomer cannot win by its
 * serial. The HELD document is judged on the time it was ACCEPTED (`held_at`, the cache file's write time), never
 * on today's clock: judged on today's, a clock wound back a week made the held serial "too far ahead", and an
 * older same-ordinal list then replaced it (r3). The newcomer is judged on max(now, held_at). Only this tie-break
 * reads a clock; the update half (rank, ordinal) never does. GXRelayList.h GX_RELAYLIST_HORIZON_S. */
#define SERIAL_HORIZON_S	(7ull * 24ull * 3600ull)
static int manifest_serial_usable(const manifest *m, unsigned __int64 at)
{
	return m->has_serial && m->serial <= at + SERIAL_HORIZON_S;
}

static int manifest_supersedes(const manifest *n, const manifest *h, unsigned __int64 now, unsigned __int64 held_at)
{
	if (n->rank != h->rank) {
		return n->rank > h->rank;
	}
	if (n->ordinal != h->ordinal) {
		return n->ordinal > h->ordinal;
	}
	if (manifest_serial_usable(h, held_at)) {
		return manifest_serial_usable(n, now > held_at ? now : held_at) && n->serial >= h->serial;
	}
	return 1;
}

/* The last-write time of `path`, Unix seconds; 0 when it cannot be read. The cache is written only when a document
 * is ACCEPTED (bootstrap_manifest skips an identical re-fetch), so this is that document's acceptance time. */
static unsigned __int64 file_mtime_unix(const wchar_t *path)
{
	WIN32_FILE_ATTRIBUTE_DATA fa;
	ULARGE_INTEGER t;
	if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) {
		return 0;
	}
	t.LowPart = fa.ftLastWriteTime.dwLowDateTime;
	t.HighPart = fa.ftLastWriteTime.dwHighDateTime;
	return t.QuadPart > 116444736000000000ull ? (t.QuadPart - 116444736000000000ull) / 10000000ull : 0;
}

/* Fill `m` from the best source available. `wire` is scratch of at least WIRE_MAX and is
 * DESTROYED by parsing - parse_manifest splits in place. Returns 1 if `m` holds a document that
 * verified against the compiled-in key. */
static int bootstrap_manifest(const config *c, manifest *m, char *wire, size_t wirecap)
{
	char raw[WIRE_MAX];
	wchar_t path[MAX_PATH];
	manifest cached;
	char *text;
	char cached_raw[WIRE_MAX];
	unsigned __int64 cached_at = 0;
	int have_cached = 0;
	int n;

	/* THE CACHED DOCUMENT IS READ FIRST, AND ITS ORDINAL IS A FLOOR.
	 *
	 * A signature says who wrote a document. It says NOTHING about when. So anyone who can decide
	 * which bytes this machine receives - a CDN edge holding a stale object, an operator error, a
	 * compromised bucket - can serve an OLDER but perfectly genuine document, and without this the
	 * launcher would take its relay list, publish a retired relay into the game's settings, and
	 * overwrite the newer cached document on the way. That is a rollback, and the fact that it is
	 * built entirely out of authentic signed bytes is exactly what makes it worth refusing here.
	 *
	 * wmain already applies the same rule to the BUILD half (`m.ordinal < st.ordinal` refuses to
	 * apply an older update). This applies it to the relay half, which the ordinal check downstream
	 * does not cover because selection happens before it. */
	/* Zeroed rather than left to parse_manifest, which only writes it on success. `have_cached`
	 * already guards every read, but /W4 cannot see that through the call and warns C4701 - and a
	 * warning that has to be explained every time somebody reads this file is worth one memset. */
	memset(&cached, 0, sizeof(cached));

	path_join(path, MAX_PATH, g_dir, MANIFEST_CACHE);
	cached_raw[0] = 0;
	text = read_text(path, WIRE_MAX - 1);
	if (text) {
		strcpy_s(wire, wirecap, text);
		strcpy_s(cached_raw, sizeof(cached_raw), text);
		cached_at = file_mtime_unix(path);
		free(text);
		if (parse_manifest(wire, &cached)) {
			have_cached = 1;
		} else {
			logf_("bootstrap: the cached document no longer verifies - ignoring it");
		}
	}

	n = http_get_text(c->bootstrap, raw, sizeof(raw));
	if (n > 0) {
		/* Parse a COPY: parse_manifest splits the buffer in place, and the bytes that get cached
		 * have to be the ones that were signed, not the ones with NULs punched through them. */
		strcpy_s(wire, wirecap, raw);
		if (parse_manifest(wire, m)) {
			const unsigned __int64 now = (unsigned __int64)time(NULL);
			const unsigned __int64 held_at = cached_at ? cached_at : now;
			if (have_cached && !manifest_supersedes(m, &cached, now, held_at)) {
				/* "is OLDER than the cached one" is test-bootstrap.js's anchor: kept word for word. */
				logf_("bootstrap: the published document is OLDER than the cached one "
					"(key k%d ordinal %lu vs key k%d ordinal %lu) - keeping the cached one and NOT caching this",
					m->rank, m->ordinal, cached.rank, cached.ordinal);
				*m = cached;
				return 1;
			}
			logf_("bootstrap: %d bytes from %s", n, c->bootstrap);
			/* Not rewritten when it is the cached document again: the file's write time is its ACCEPTANCE time,
			 * which manifest_supersedes judges the held serial on - a re-fetch must not move it. */
			if (!have_cached || strcmp(raw, cached_raw) != 0) {
				cache_save(raw, (have_cached && held_at > now) ? held_at : now);
			}
			return 1;
		}
		/* Verified-and-refused is deliberately NOT cached, and neither is it fatal. Fall through to
		 * whatever we had before, which at least once verified. */
		logf_("bootstrap: the published document was refused - trying the cached one");
	}

	if (have_cached) {
		logf_("bootstrap: using the cached document - the signature still verifies");
		*m = cached;
		return 1;
	}
	return 0;
}

/* ============================================================================================
 * Relay selection
 *
 * Every relay in the signed list is probed AT ONCE on one socket, and the first to answer wins.
 * One socket rather than one probe each, because the alternative puts N timeouts in a row in front
 * of a launch: a list with two dead relays would cost two full windows before reaching the live
 * one, and the person waiting has no idea why.
 *
 * The probe is GXUPD, which is a tag every relay already answers - so selection needs no relay
 * change, works against the relay deployed today, and measures the thing that matters: whether
 * this machine can get a reply from that host right now. A relay that is up but unreachable from
 * here is not a relay this machine can use, and a health check run anywhere else cannot tell.
 *
 * FIRST TO ANSWER, not lowest round trip over several samples. One sample is a noisy estimate of
 * latency, but it is an excellent test of reachability, and under lockstep the quantity that
 * actually binds is the WORST peer's link - which is unknowable from here at selection time. So
 * this deliberately measures reachability and treats the ordering as a tie-break rather than
 * dressing one sample up as a routing decision.
 * ========================================================================================== */

static int select_relay(const manifest *m, unsigned long installed_ordinal,
                        char *out_host, size_t hostcap, int *out_port)
{
	WSADATA wsa;
	SOCKET s = INVALID_SOCKET;
	struct sockaddr_in addr[MAX_RELAYS];
	int have[MAX_RELAYS];
	char req[64], buf[WIRE_MAX];
	DWORD started;
	int i, nresolved = 0, picked = -1;

	if (m->nrelays <= 0) {
		return 0;
	}
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		return 0;
	}

	memset(have, 0, sizeof(have));
	for (i = 0; i < m->nrelays; i++) {
		struct addrinfo hints, *res = NULL;
		char port[16];
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		sprintf_s(port, sizeof(port), "%d", m->relays[i].port);
		if (getaddrinfo(m->relays[i].host, port, &hints, &res) == 0 && res) {
			memcpy(&addr[i], res->ai_addr, sizeof(struct sockaddr_in));
			have[i] = 1;
			nresolved++;
			freeaddrinfo(res);
		} else {
			logf_("relay: cannot resolve '%s'", m->relays[i].host);
		}
	}
	if (nresolved == 0) {
		WSACleanup();
		return 0;
	}

	s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET) {
		WSACleanup();
		return 0;
	}
	{
		DWORD tv = RELAY_PROBE_MS;
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
	}
	udp_ignore_conn_reset(s);

	sprintf_s(req, sizeof(req), "GXUPD %lu", installed_ordinal);
	started = GetTickCount();
	for (i = 0; i < m->nrelays; i++) {
		if (have[i]) {
			sendto(s, req, (int)strlen(req), 0, (struct sockaddr *)&addr[i], sizeof(addr[i]));
		}
	}

	/* Loop rather than a single recv: a datagram from something that is not on the list at all is
	 * possible on a socket bound to an ephemeral port, and taking it as an answer would let anyone
	 * who can guess that port choose this machine's relay. The source is matched against the SIGNED
	 * list, and anything else is discarded and waited past. */
	while (GetTickCount() - started < RELAY_PROBE_MS) {
		struct sockaddr_in from;
		int fromlen = sizeof(from);
		int n = recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fromlen);
		if (n <= 0) {
			/* A TIMEOUT means the window is genuinely spent and there is nothing more to wait for.
			 * Any OTHER error is somebody else's problem arriving on our socket - see
			 * udp_ignore_conn_reset - and must not be allowed to abandon a live relay's reply that
			 * has not arrived yet. The deadline in the while() is what bounds this either way. */
			if (WSAGetLastError() == WSAETIMEDOUT) {
				break;
			}
			continue;
		}
		for (i = 0; i < m->nrelays; i++) {
			if (have[i] && from.sin_addr.s_addr == addr[i].sin_addr.s_addr
				&& from.sin_port == addr[i].sin_port) {
				picked = i;
				break;
			}
		}
		if (picked >= 0) {
			logf_("relay: %s:%d (%s) answered first, %lu ms", m->relays[picked].host,
				m->relays[picked].port, m->relays[picked].region, GetTickCount() - started);
			break;
		}
	}
	closesocket(s);
	WSACleanup();

	if (picked < 0) {
		/* Nobody answered. Still publish the first relay we could resolve, because the signed list
		 * saying a relay exists is better information than whatever this machine had before, and
		 * "no UDP got through in 800 ms" is a very common thing on a network that works fine a
		 * second later. */
		for (i = 0; i < m->nrelays; i++) {
			if (have[i]) {
				picked = i;
				break;
			}
		}
		logf_("relay: no relay answered inside %d ms - taking %s from the signed list anyway",
			RELAY_PROBE_MS, m->relays[picked].host);
	}

	strcpy_s(out_host, hostcap, m->relays[picked].host);
	*out_port = m->relays[picked].port;
	return 1;
}

/* What publish_relay_file did, for its callers and the self-test. */
enum { PUBLISH_WROTE = 0, PUBLISH_CURRENT = 1, PUBLISH_KEPT = 2, PUBLISH_FAILED = 3 };

static int publish_relay_file(const wchar_t *path, const char *host, const char *want);
static int publish_ini_key(const wchar_t *path, const char *key, const char *value, const char *want);

/* Write the chosen relay into the GAME's own settings, which is what makes the engine zero-config
 * as well as this program.
 *
 * Options.ini belongs to the game, so every other key is preserved verbatim and only RelayAddress is
 * replaced. (It is GeneralsX's own copy since 23/09/2026 - see userdata_path - and no longer the one
 * retail Zero Hour and Generals Online share, but the settings in it are still the player's.) RelayRoom is deliberately NOT written:
 * an unset RelayRoom makes Transport::getLocalHostRoom() generate a private one per host, which is
 * what makes a game created here the only game in its room. Pinning everyone to one room would make
 * hosts silently replace each other's listing.
 *
 * Rewritten only when the value actually changes, so a launch does not touch the file - and cannot
 * damage it - for no reason. */
/* GeneralsX @feature Claude 29/10/2026 S1-3: the RelayAddress line for a relay. The default lobby port is left
 * implicit - byte for byte what every earlier build wrote - and any other is written "host:port", which an S1-3
 * engine reads (OptionPreferences::getRelayPort; the game port is that + 2). This used to REFUSE a non-default port,
 * because the engine then read a bare host only. 0 if the line does not fit or the port leaves no game port. */
static int relay_line(const char *host, int port, char *value, size_t vcap, char *want, size_t cap)
{
	int v, n;
	if (port <= 0 || port > 65533) {
		return 0;
	}
	/* The VALUE is what publish_relay_file compares with the current RelayAddress: the whole endpoint, port
	 * included. Comparing the bare host (as it did) left "relay.example" in place when the signed list moved that
	 * host to 9086 - PUBLISH_CURRENT, nothing written, the engine still on 8086 (Codex relaylist-v2-r1 P2). */
	v = (port == RELAY_LOBBY_PORT) ? sprintf_s(value, vcap, "%s", host) : sprintf_s(value, vcap, "%s:%d", host, port);
	n = sprintf_s(want, cap, "RelayAddress = %s\r\n", value);
	return v > 0 && (size_t)v < vcap && n > 0 && (size_t)n < cap;
}

static void publish_relay(const char *host, int port)
{
	wchar_t path[MAX_PATH];
	char want[192], value[160];
	int r;

	if (!relay_line(host, port, value, sizeof(value), want, sizeof(want))) {
		logf_("relay: %s:%d cannot be written as a RelayAddress line - not writing it into Options.ini", host, port);
		return;
	}

	if (!userdata_path(path, MAX_PATH, L"Options.ini")) {
		return;
	}
	r = publish_relay_file(path, value, want);
	/* GeneralsX @feature Claude 29/10/2026 S1-3 (Codex relaylist-v2-r6) AND WHICH VALUE THIS PROGRAM WROTE. An S1-3 engine
	 * reads RelayAddress as the player's PIN, which outranks its own signed list - so this program's choice, made on its
	 * update rule (rank, ordinal), overrode the engine's relay-list floor (rank, serial) on Windows alone. The marker says
	 * "this RelayAddress is the launcher's, not the player's": while the two agree the engine follows its own list and
	 * keeps this value only as the fallback; a player who edits RelayAddress breaks the match and is a pin again. Written
	 * after RelayAddress, so a write that fails between them leaves a mismatch - read as a pin, today's behaviour. */
	if (r == PUBLISH_WROTE || r == PUBLISH_CURRENT) {
		char mark[200];
		if (sprintf_s(mark, sizeof(mark), "RelayAddressLauncher = %s\r\n", value) > 0) {
			publish_ini_key(path, "RelayAddressLauncher", value, mark);
		}
	}
}

/* publish_relay's file half, on any path (the self-test runs it on scratch files). `want` is the
 * whole RelayAddress line, CRLF included.
 *
 * GeneralsX @bugfix Claude 22/10/2026 AN OPTIONS.INI THAT CANNOT BE READ IS LEFT ALONE. This used to
 * take read_text's NULL as "no file", copy nothing, write the RelayAddress line alone and move that
 * over Options.ini - every setting the player had was replaced by one line. read_text returns NULL
 * for a file over the 1 MiB cap, one CreateFileW cannot open (a sharing violation, access denied, a
 * OneDrive placeholder that cannot be fetched) and a failed read. Only a file that is NOT THERE may
 * be created from nothing; anything else costs this launch's relay setting and nothing more (Codex
 * llt-r2, found while reviewing the launcher's Look switches; lead C44 22/10/2026). */
static int publish_relay_file(const wchar_t *path, const char *host, const char *want)
{
	return publish_ini_key(path, "RelayAddress", host, want);
}

/* GeneralsX @feature Claude 29/10/2026 S1-3 (Codex relaylist-v2-r6) publish_relay_file's body, for any one key: `want` is
 * the whole "<key> = <value>" line, CRLF included, and every existing line of exactly that key is replaced by it. It
 * exists for RelayAddressLauncher - see publish_relay. A key is matched as the line's first word followed by '=' (after
 * blanks), so "RelayAddress" never takes a "RelayAddressLauncher" line, nor the other way round. */
static int publish_ini_key(const wchar_t *path, const char *key, const char *value, const char *want)
{
	const size_t klen = strlen(key);
	wchar_t tmp[MAX_PATH];
	char *text = NULL;
	char cur[256];
	HANDLE f;
	DWORD wrote, rerr = 0, len = 0;
	const char *p;
	int ok = 1, why = RT_UNREADABLE;
	int out_any = 0;		/* a byte is in the temporary file */

	text = read_text_why(path, 1 << 20, &why, &rerr, &len);
	if (!text && why != RT_MISSING) {
		logf_("relay: cannot read %S (%lu) - the game's Options.ini is left exactly as it is, and "
			"RelayAddress is not published this launch", path, rerr);
		return PUBLISH_KEPT;
	}
	/* The copy below walks C strings, so a NUL byte inside would end it there and drop every line after. */
	if (text && strlen(text) != (size_t)len) {
		logf_("relay: %S holds a NUL byte - the game's Options.ini is left exactly as it is, and "
			"RelayAddress is not published this launch", path);
		free(text);
		return PUBLISH_KEPT;
	}
	if (text && ini_get(text, key, cur, sizeof(cur)) && !strcmp(cur, value)) {
		free(text);
		return PUBLISH_CURRENT;	/* already what we would write */
	}

	swprintf_s(tmp, MAX_PATH, L"%s.gxtmp", path);
	/* The directory may not exist at all on a machine that has never started the game. */
	{
		wchar_t dir[MAX_PATH];
		wchar_t *slash;
		wcscpy_s(dir, MAX_PATH, path);
		slash = wcsrchr(dir, L'\\');
		if (slash) {
			*slash = 0;
			make_dirs(dir);
		}
	}

	f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		logf_("relay: cannot write %S (%lu) - the game keeps its previous setting",
			tmp, GetLastError());
		free(text);
		return PUBLISH_FAILED;
	}
	/* Every line that is not ours, unchanged and in its original order.
	 *
	 * The key match is "RelayAddress", then whitespace, then '=' - the same shape ini_get uses. A
	 * bare prefix test would also eat a hypothetical RelayAddressFoo, and silently deleting a
	 * setting this program does not own out of the game's settings is exactly the kind of damage
	 * nobody would ever trace back to a launcher.
	 *
	 * EVERY WRITE IS CHECKED, and the file is only replaced if all of them succeeded. An ignored
	 * WriteFile on a full disk produces a TRUNCATED temporary file, and moving that over
	 * Options.ini would destroy every setting the player has - to publish a relay address, which is
	 * the least important thing in the file. A failure here must cost this launch's relay setting and
	 * nothing else. */
	for (p = text; p && *p && ok; ) {
		const char *eol = strchr(p, '\n');
		size_t line_len = eol ? (size_t)(eol - p + 1) : strlen(p);
		const char *k = p;
		int ours = 0;
		int lead_bom = 0;
		/* The file's leading BOM is not part of the first key (ini_get): a BOM-first RelayAddress is ours too; any other
		 * first line is written back with its BOM, where it was. */
		if (p == text && (unsigned char)k[0] == 0xEF && (unsigned char)k[1] == 0xBB && (unsigned char)k[2] == 0xBF) {
			k += 3;
			lead_bom = 1;
		}
		while (*k == ' ' || *k == '\t') {
			k++;
		}
		if (_strnicmp(k, key, klen) == 0) {
			k += klen;
			while (*k == ' ' || *k == '\t') {
				k++;
			}
			ours = (*k == '=');
		}
		/* A dropped BOM-first RelayAddress line leaves the file's BOM at byte zero: dropped with its line, a BOM at the
		 * start of the NEXT line would become the file's, and that line's key would start to count (Codex asm0967 r3). */
		if (ours && lead_bom) {
			if (!WriteFile(f, "\xEF\xBB\xBF", 3, &wrote, NULL) || wrote != 3) {
				ok = 0;
				break;
			}
			out_any = 1;
		}
		if (!ours) {
			/* Nor may a BOM that was data (not the file's first bytes) become the first bytes of the new file when the
			 * lines before it were RelayAddress: a line break goes first (Codex asm0967 r5). */
			if (!out_any && p != text && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF
				&& (!WriteFile(f, "\r\n", 2, &wrote, NULL) || wrote != 2)) {
				ok = 0;
				break;
			}
			if (!WriteFile(f, p, (DWORD)line_len, &wrote, NULL) || wrote != (DWORD)line_len) {
				ok = 0;
				break;
			}
			out_any = 1;
			/* A final line with no newline is normal in a hand-edited ini, and appending straight
			 * onto it would fuse somebody else's setting with ours into one unparseable line. */
			if (!eol && (!WriteFile(f, "\r\n", 2, &wrote, NULL) || wrote != 2)) {
				ok = 0;
				break;
			}
		}
		p = eol ? eol + 1 : NULL;
	}
	if (ok) {
		DWORD want_len = (DWORD)strlen(want);
		if (!WriteFile(f, want, want_len, &wrote, NULL) || wrote != want_len) {
			ok = 0;
		}
	}
	/* FlushFileBuffers before the rename, because MoveFileEx does not order the data against it.
	 * Without it a power loss between the two can leave Options.ini present, named correctly, and
	 * empty - which the game reads as "no settings at all". */
	if (ok && !FlushFileBuffers(f)) {
		ok = 0;
	}
	CloseHandle(f);
	free(text);

	if (!ok) {
		logf_("relay: could not write %S in full (%lu) - the game keeps its previous settings, "
			"and nothing was replaced", tmp, GetLastError());
		DeleteFileW(tmp);
		return PUBLISH_FAILED;
	}
	/* A file that was not there when it was read is created, never replaced: if something (a sync client
	 * restoring it) put one there since, the rename fails and that file is kept (Codex uow-r1). */
	if (MoveFileExW(tmp, path, why == RT_MISSING ? 0 : MOVEFILE_REPLACE_EXISTING)) {
		logf_("relay: RelayAddress published to the game's Options.ini");
		return PUBLISH_WROTE;
	}
	logf_("relay: could not replace %S (%lu)", path, GetLastError());
	DeleteFileW(tmp);
	return PUBLISH_FAILED;
}

/* ============================================================================================
 * Discord login STATE. This program signs nobody in: it reads the assertion file the ENGINE
 * writes and says once per run whose it is and how long it has left.
 *
 * GeneralsX @bugfix Claude 11/08/2026 The RFC 8628 device flow that used to live here - POST
 * /device, print a typed user code, poll /claim until somebody finished - is gone, together with
 * the loader's LOG IN button that drove it. tools/oauth/worker.js routes only /login, /callback
 * and /pubkey and its tests assert the retired pair 404s, so every press of that button failed on
 * its very first request; the button was a permanently broken control, not a feature waiting for a
 * server. Signing in is the engine's own loopback flow (/login?p=<port>&n=<nonce>). A second
 * implementation here would be a second trust decision to keep in step forever, which is the exact
 * thing the shared verifier seam below exists to prevent.
 *
 * WHAT IS READ IS VERIFIED HERE, with the ENGINE's own verifier through the C seam in
 * GXIdentityAssertion.h - same pinned key, same charsets, same clock rules. A launcher with its
 * own looser check is a launcher that reports a login the game refuses.
 * ========================================================================================== */

/* GeneralsX @bugfix Claude 11/08/2026 oauth_base() was REMOVED with the device flow it served.
 * It was briefly kept for the GX_OAUTH_BASE override, but the CI workflow compiles this file
 * with /W4 /WX (.github/workflows/build-launcher.yml:93 - the local build-launcher.cmd uses
 * /W4 alone, which is why the warning looked harmless), so an unreferenced static is a BUILD
 * FAILURE in CI, not warning noise. The override belongs with a caller: the engine owns the
 * loopback login now, so reinstate it there if a local test server is ever needed
 * from the launcher again. GX_OAUTH_BASE_URL is left defined and unused on purpose - a macro
 * costs nothing and documents the production endpoint. */

/* Report what login state this install is in, once per normal run. Silent when there is no file,
 * because that is what every player who never wanted an account has, forever. */
static void report_login_state(void)
{
	wchar_t path[MAX_PATH];
	char *text;
	char *nl;
	struct GXAssertionClaims claims;
	int st;
	long long now = (long long)time(NULL);

	if (!userdata_path(path, MAX_PATH, L"" GX_ASSERTION_FILENAME)) {
		return;
	}
	text = read_text(path, 4096);
	if (!text) {
		return;
	}
	nl = strchr(text, '\n');
	if (nl) {
		*nl = 0;
	}

	st = gx_identity_verify_text(text, now, &claims);
	if (st == 0 /* GX_ASSERT_OK */) {
		int days = (int)((claims.exp - now) / (24 * 3600));
		if (days <= 5) {
			logf_("login: expiring %s %dd", claims.name[0] ? claims.name : "(no name)", days);
			printf("login-state: expiring %s %d\n", claims.name[0] ? claims.name : "", days);
		} else {
			logf_("login: ok %s %dd", claims.name[0] ? claims.name : "(no name)", days);
			printf("login-state: ok %s %d\n", claims.name[0] ? claims.name : "", days);
		}
	} else {
		logf_("login: %s", gx_identity_status_name(st));
		printf("login-state: fail %s\n", gx_identity_status_name(st));
	}
	free(text);
}

/* ============================================================================================
 * Installed state
 * ========================================================================================== */

typedef struct {
	unsigned long ordinal;
	artifact items[MAX_ARTIFACTS];
	int nitems;
} state;

static void state_load(state *st)
{
	wchar_t path[MAX_PATH];
	char *text, *p;

	memset(st, 0, sizeof(*st));
	path_join(path, MAX_PATH, g_dir, L"gxinstalled.txt");
	text = read_text(path, 1 << 16);
	if (!text) {
		logf_("state: no gxinstalled.txt - treating this install as ordinal 0");
		return;
	}
	p = text;
	while (p && *p) {
		char *eol = strchr(p, '\n');
		if (eol) {
			*eol = 0;
		}
		if (!strncmp(p, "ordinal ", 8)) {
			st->ordinal = strtoul(p + 8, NULL, 10);
		} else if (!strncmp(p, "item ", 5) && st->nitems < MAX_ARTIFACTS) {
			artifact *a = &st->items[st->nitems];
			if (sscanf_s(p + 5, "%15s %127s %31s", a->platform, (unsigned)sizeof(a->platform),
					a->name, (unsigned)sizeof(a->name), a->hash, (unsigned)sizeof(a->hash)) == 3) {
				st->nitems++;
			}
		}
		p = eol ? eol + 1 : NULL;
	}
	free(text);
	logf_("state: installed ordinal %lu, %d known artifact(s)", st->ordinal, st->nitems);

	/* GeneralsX @bugfix wh1ter0se 12/08/2026 DOES THE RECORD MATCH WHAT IS ACTUALLY ON DISK?
	 *
	 * Everything above trusts gxinstalled.txt, and until now nothing ever questioned it. That was
	 * fine while this program was the only writer - and it is not. Running setup over an existing
	 * install extracts a payload straight over {app} without touching the state file, so the record
	 * can survive describing a build that is no longer here. The install is then permanently
	 * stranded: we compare a stale record to the manifest, conclude "already current", and never
	 * look at the files. Measured on a real machine, two days stale, reporting itself up to date.
	 *
	 * The installer now deletes this file, which fixes it going forward. This check is the other
	 * half, and it is the more valuable one: it makes the whole CLASS transient rather than
	 * permanent, for installs already broken and for whatever writes here next.
	 *
	 * BUILD.txt ships inside the payload, so it is written by the same extraction that would have
	 * invalidated the record. Its commit and the sha embedded in the artifact NAME come from the
	 * same build, so a disagreement means the files were replaced behind our back. Comparing what
	 * the record CLAIMS against what the payload SAYS costs one small file read.
	 *
	 * Deliberately forgiving: no BUILD.txt, an unparsable one, or an artifact name without a sha
	 * all leave the record alone. This must only fire on a POSITIVE contradiction, because the
	 * failure mode of a false positive is a needless 67 MB re-download on every launch. */
	if (st->nitems > 0) {
		wchar_t bpath[MAX_PATH];
		char *btext;

		path_join(bpath, MAX_PATH, g_dir, L"BUILD.txt");
		btext = read_text(bpath, 1 << 14);
		if (btext) {
			const char *commit = strstr(btext, "built from commit");
			if (commit) {
				commit += strlen("built from commit");
				while (*commit == ' ' || *commit == ':' || *commit == '\t') {
					commit++;
				}
				/* Every artifact name carries the sha it was built from. One matching prefix is
				 * enough - the record may legitimately list artifacts for other platforms. */
				int i, agrees = 0, checkable = 0;
				for (i = 0; i < st->nitems; i++) {
					const char *dash = strrchr(st->items[i].name, '-');
					if (dash && strlen(dash + 1) >= 7) {
						checkable = 1;
						if (!strncmp(dash + 1, commit, 7)) {
							agrees = 1;
						}
					}
				}
				if (checkable && !agrees) {
					logf_("state: gxinstalled.txt says '%s' but BUILD.txt was built from %.9s - the "
						"files were replaced behind the launcher (a setup run over this install?). "
						"Discarding the record so this launch re-checks the manifest.",
						st->items[0].name, commit);
					memset(st, 0, sizeof(*st));
				}
			}
			free(btext);
		}
	}
}

/* Record what this install now has. Returns 1 only if the record is REALLY on disk.
 *
 * The return value is not decoration. If this file does not persist, the next launch re-reads a
 * stale record, decides every artifact is wanted again, and re-downloads the whole payload - which
 * used to be a merely wasteful papercut, because the game still started. Once a failed write can
 * also trigger a launcher restart, the same fault becomes Play -> 67 MB -> restart -> Play -> ...
 * with the game never starting at all. So every write is checked for its byte count as well as its
 * return, exactly as publish_relay does, and the caller gates the restart on the answer. */
static int state_save(const manifest *m, const artifact *applied, int napplied)
{
	wchar_t path[MAX_PATH], tmp[MAX_PATH];
	HANDLE f;
	char line[512];
	DWORD wrote, len;
	int i, ok = 1;

	path_join(path, MAX_PATH, g_dir, L"gxinstalled.txt");
	path_join(tmp, MAX_PATH, g_dir, L"gxinstalled.tmp");

	f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		logf_("state: cannot write gxinstalled.tmp (%lu) - the next launch will re-download",
			GetLastError());
		return 0;
	}
	sprintf_s(line, sizeof(line),
		"# GeneralsX installed state. Written by the launcher; delete to force a re-download.\r\n"
		"ordinal %lu\r\n", m->ordinal);
	len = (DWORD)strlen(line);
	if (!WriteFile(f, line, len, &wrote, NULL) || wrote != len) {
		ok = 0;
	}
	for (i = 0; ok && i < napplied; i++) {
		sprintf_s(line, sizeof(line), "item %s %s %s\r\n",
			applied[i].platform, applied[i].name, applied[i].hash);
		len = (DWORD)strlen(line);
		if (!WriteFile(f, line, len, &wrote, NULL) || wrote != len) {
			ok = 0;
		}
	}
	/* Before the rename, because MoveFileEx does not order the data against it. */
	if (ok && !FlushFileBuffers(f)) {
		ok = 0;
	}
	CloseHandle(f);

	if (!ok) {
		logf_("state: gxinstalled.tmp could not be written in full (%lu) - not replacing the record",
			GetLastError());
		DeleteFileW(tmp);
		return 0;
	}

	/* A read-only gxinstalled.txt would fail the replace on every run and pin the install to a
	 * stale record forever. rmtree already clears the attribute for the same reason. */
	SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);

	/* Replace atomically, so a crash here cannot leave a half-written record that makes the
	 * next launch believe it has an artifact it does not have.
	 *
	 * GeneralsX @bugfix Claude 20/09/2026 AND RETRIED, because the usual reason this fails is not
	 * permanent: an antivirus scanner or the search indexer opened gxinstalled.txt the moment it
	 * was last written and holds it for a few hundred milliseconds without FILE_SHARE_DELETE, which
	 * is all MoveFileEx needs to fail. Giving up there leaves the artifacts applied and the record
	 * stale, so the NEXT launch wants every artifact again - the re-offer loop this whole change
	 * exists to close, and a retry is the only thing that closes it. The exit code cannot: the
	 * install really is current, so reporting failure would be false AND would still re-offer.
	 * Ten tries over two seconds; past that it is something else (a policy, a read-only volume)
	 * and the caller's own fallback applies. */
	for (i = 0; ; i++) {
		DWORD e;
		if (MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
			return 1;
		}
		e = GetLastError();
		if (i >= 10 || (e != ERROR_SHARING_VIOLATION && e != ERROR_ACCESS_DENIED)) {
			logf_("state: could not replace gxinstalled.txt (%lu)", e);
			DeleteFileW(tmp);
			return 0;
		}
		logf_("state: gxinstalled.txt is in use (%lu) - retrying (%d)", e, i + 1);
		Sleep(200);
	}
}

static const char *state_hash_of(const state *st, const artifact *a)
{
	int i;
	for (i = 0; i < st->nitems; i++) {
		if (!strcmp(st->items[i].platform, a->platform) && !strcmp(st->items[i].name, a->name)) {
			return st->items[i].hash;
		}
	}
	return NULL;
}

/* ============================================================================================
 * Applying
 * ========================================================================================== */

/* Unpack with the OS. tar.exe has shipped in System32 since Windows 10 1803 and handles .zip;
 * Expand-Archive is the fallback for anything older. Neither is a dependency a friend installs. */
static int extract(const wchar_t *archive, const wchar_t *into)
{
	wchar_t cmd[2048], sysdir[MAX_PATH], tarexe[MAX_PATH];
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	DWORD rc = 1;

	if (!make_dirs(into)) {
		logf_("apply: cannot create the staging directory");
		return 0;
	}
	GetSystemDirectoryW(sysdir, MAX_PATH);
	path_join(tarexe, MAX_PATH, sysdir, L"tar.exe");

	if (file_exists(tarexe)) {
		swprintf_s(cmd, 2048, L"\"%s\" -x -f \"%s\" -C \"%s\"", tarexe, archive, into);
	} else {
		swprintf_s(cmd, 2048,
			L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
			L"\"Expand-Archive -LiteralPath '%s' -DestinationPath '%s' -Force\"", archive, into);
	}

	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	memset(&pi, 0, sizeof(pi));
	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, into, &si, &pi)) {
		logf_("apply: cannot run the unpacker (%lu)", GetLastError());
		return 0;
	}
	WaitForSingleObject(pi.hProcess, 120000);
	GetExitCodeProcess(pi.hProcess, &rc);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	if (rc != 0) {
		logf_("apply: the unpacker exited %lu - abandoning this artifact", rc);
		return 0;
	}
	return 1;
}

/* Move one staged file over its installed counterpart.
 *
 * A RUNNING .exe cannot be overwritten but CAN be renamed, and that is the whole trick: the old
 * file is moved aside to <name>.gxold and the new one takes its place, so an update applies even
 * while the game is open, and the .gxold is swept on a later launch when nothing holds it. */
/* Set when this update replaced GeneralsXLauncher.exe itself.
 *
 * THE FILE ON DISK IS NEW AND THE PROCESS ON SCREEN IS OLD. Windows lets a running .exe be renamed
 * but keeps executing the image it already loaded, so replacing the launcher while it is running
 * leaves the user looking at the previous build - the update worked, and every visible sign says it
 * did not. Reported on stdout so the launcher can restart itself; see place_file. */
static int g_launcher_replaced;

/* True when dst is THIS install's launcher and the bytes about to replace it are different.
 *
 * All three conditions matter, and a bare "is it called GeneralsXLauncher.exe" gets every one of
 * them wrong. The full path is compared against g_dir, so a nested tools\GeneralsXLauncher.exe in
 * some future payload cannot claim to be the running image. The destination must already EXIST,
 * because a first install is not a replacement and telling a brand-new user "update installed,
 * restarting" is nonsense. And the content must actually differ: every win32 payload carries
 * GeneralsXLauncher.exe (package-windows-share.ps1 makes it mandatory and staleness is decided per
 * ARCHIVE, not per file), so an engine-only release re-places a byte-identical launcher - without
 * this check every future update would demand a restart and a second press of Play for nothing. */
static int is_self_replacement(const wchar_t *src, const wchar_t *dst)
{
	wchar_t self[MAX_PATH];
	unsigned char a[32], b[32];

	path_join(self, MAX_PATH, g_dir, L"GeneralsXLauncher.exe");
	if (_wcsicmp(dst, self) != 0) {
		return 0;
	}
	if (GetFileAttributesW(dst) == INVALID_FILE_ATTRIBUTES) {
		return 0;			/* first install, nothing is being replaced */
	}
	/* ~60 KB each and already linked for the manifest check, so this costs nothing measurable. A
	 * hash that cannot be taken is treated as "changed": a needless restart is a papercut, a
	 * missed one puts the user back on the stale-window bug this whole change exists to fix. */
	if (!gx_sha256_file(src, a) || !gx_sha256_file(dst, b)) {
		return 1;
	}
	return memcmp(a, b, sizeof(a)) != 0;
}

static int place_file(const wchar_t *src, const wchar_t *dst)
{
	wchar_t old[MAX_PATH];
	int self = is_self_replacement(src, dst);

	if (MoveFileExW(src, dst, MOVEFILE_REPLACE_EXISTING)) {
		/* Only after the replace actually happened. */
		g_launcher_replaced |= self;
		return 1;
	}
	swprintf_s(old, MAX_PATH, L"%s%s", dst, OLD_SUFFIX);
	DeleteFileW(old);
	if (!MoveFileExW(dst, old, MOVEFILE_REPLACE_EXISTING)) {
		logf_("apply: %S is locked and cannot be renamed (%lu)", dst, GetLastError());
		return 0;
	}
	if (!MoveFileExW(src, dst, MOVEFILE_REPLACE_EXISTING)) {
		/* Put it back rather than leaving the install one file short. */
		MoveFileExW(old, dst, MOVEFILE_REPLACE_EXISTING);
		logf_("apply: cannot write %S even after renaming the old one (%lu)", dst, GetLastError());
		return 0;
	}
	logf_("apply: %S was in use - the old copy is now %S", dst, old);
	g_launcher_replaced |= self;
	return 1;
}

static int place_tree(const wchar_t *stage, const wchar_t *dest)
{
	WIN32_FIND_DATAW fd;
	wchar_t glob[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
	HANDLE h;
	int ok = 1;

	path_join(glob, MAX_PATH, stage, L"*");
	h = FindFirstFileW(glob, &fd);
	if (h == INVALID_HANDLE_VALUE) {
		return 1;
	}
	do {
		if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) {
			continue;
		}
		path_join(src, MAX_PATH, stage, fd.cFileName);
		path_join(dst, MAX_PATH, dest, fd.cFileName);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			make_dirs(dst);
			if (!place_tree(src, dst)) {
				ok = 0;
			}
		} else if (!place_file(src, dst)) {
			ok = 0;
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return ok;
}

/* Sweep the .gxold files a previous update left behind. Best effort by definition: one of them
 * may still be the executable of a game that is running right now. */
static void sweep_old(const wchar_t *dir)
{
	WIN32_FIND_DATAW fd;
	wchar_t glob[MAX_PATH], child[MAX_PATH];
	HANDLE h;

	path_join(glob, MAX_PATH, dir, L"*");
	h = FindFirstFileW(glob, &fd);
	if (h == INVALID_HANDLE_VALUE) {
		return;
	}
	do {
		if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) {
			continue;
		}
		path_join(child, MAX_PATH, dir, fd.cFileName);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			sweep_old(child);
		} else {
			size_t n = wcslen(fd.cFileName);
			size_t s = wcslen(OLD_SUFFIX);
			if (n > s && !_wcsicmp(fd.cFileName + n - s, OLD_SUFFIX)) {
				if (DeleteFileW(child)) {
					logf_("swept %S", child);
				}
			}
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

/* Sweep the files the OLD install flow left behind. The installer deletes these when it upgrades
 * over an old install, but an install that only ever self-updates from here never runs the
 * installer again, so without this they stay on disk forever. relay-settings.ini is the one that
 * does harm: it looks like it configures which server you connect to, and nothing has read it
 * since this program started fetching a signed relay list, so anyone who edits it is debugging a
 * file with no effect.
 *
 * Exact names, in this directory only, no pattern and no recursion. The install folder also holds
 * the game's own data and the friend's Options.ini, and a wildcard here would be one typo away
 * from deleting something that cannot be downloaded again. */
static void sweep_legacy(const wchar_t *dir)
{
	static const wchar_t *const names[] = {
		L"relay-settings.ini",
		L"install-windows-share.ps1",
		L"GeneralsX-Setup.cmd",
		L"upd.txt",
		NULL
	};
	wchar_t path[MAX_PATH];
	int i;

	for (i = 0; names[i]; i++) {
		path_join(path, MAX_PATH, dir, names[i]);
		if (DeleteFileW(path)) {
			logf_("swept %S", path);
		}
	}
}

/* ============================================================================================
 * Launching, which is the one thing that always happens
 * ========================================================================================== */

static int launch_game(int argc, wchar_t **argv)
{
	wchar_t cmdfile[MAX_PATH], exe[MAX_PATH], cmd[2048];
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	int i;

	path_join(cmdfile, MAX_PATH, g_dir, L"GeneralsX.cmd");
	path_join(exe, MAX_PATH, g_dir, L"generalszh.exe");

	if (file_exists(cmdfile)) {
		/* Preferred, because GeneralsX.cmd is what already knows the two things the engine
		 * cannot work out for itself: CNC_GENERALS_ZH_PATH, and that the working directory has
		 * to be the install folder - dataID is cwd-relative, and launching from anywhere else
		 * refuses every join with DATA_DIFFERS, which reads like a real data mismatch. Running
		 * it rather than reimplementing it means this launcher cannot drift from the installer. */
		swprintf_s(cmd, 2048, L"cmd.exe /c \"\"%s\"", cmdfile);
	} else if (file_exists(exe)) {
		swprintf_s(cmd, 2048, L"\"%s\"", exe);
	} else {
		logf_("FATAL: neither GeneralsX.cmd nor generalszh.exe is in %S", g_dir);
		/* A dialog, because this is the one failure a friend has to be TOLD about - every other
		 * one ends with the game starting, and a silent exit here would just look broken.
		 *
		 * Gated on there being no stdout, and that gate is not tidiness. This is a
		 * /SUBSYSTEM:WINDOWS program: double-clicked from Explorer it has no standard output and
		 * the dialog is right, but run from a shell or a test harness it does have one, and a
		 * modal box nobody can click is an unkillable hang. That is not hypothetical - it hung
		 * the end-to-end suite on this exact scenario before the gate existed. */
		HANDLE so = GetStdHandle(STD_OUTPUT_HANDLE);
		if (so == NULL || so == INVALID_HANDLE_VALUE) {
			MessageBoxW(NULL, L"GeneralsX is not installed in this folder.\r\n"
				L"Run GeneralsX-Setup.cmd from the folder you unzipped.",
				L"GeneralsX", MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		}
		return 1;
	}

	/* Pass the caller's arguments through to the game, minus this program's own switches - so
	 * "-win" or "-quickstart" on a shortcut still reaches the engine, and "--check" does not. */
	for (i = 1; i < argc; i++) {
		if (argv[i][0] == L'-' && argv[i][1] == L'-') {
			continue;
		}
		wcscat_s(cmd, 2048, L" ");
		wcscat_s(cmd, 2048, argv[i]);
	}
	if (file_exists(cmdfile)) {
		wcscat_s(cmd, 2048, L"\"");
	}

	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	memset(&pi, 0, sizeof(pi));
	logf_("launching: %S", cmd);
	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, g_dir, &si, &pi)) {
		logf_("FATAL: cannot start the game (%lu)", GetLastError());
		return 1;
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return 0;
}

/* ============================================================================================
 * The update, as one all-or-nothing transaction
 *
 * Every artifact is downloaded AND hash-checked before ANY of them is unpacked into the install.
 * A half-applied update is the one outcome worse than no update: it produces a build whose
 * executable and data disagree, which fails at dataID with a message about a mismatch rather than
 * about a failed download.
 * ========================================================================================== */

/* WHAT THIS RUN WOULD FETCH. The ONLY place that decision is made.
 *
 * GeneralsX @bugfix Claude 13/08/2026 It used to be inlined at the top of do_update, which meant
 * the only way to learn the answer was to perform it. The loader therefore could not show it before
 * the user pressed Play, and what it showed instead was a SECOND rule, written in C#, reading the
 * on-disk manifest CACHE (LauncherForm.ReadManifestStatus). The two disagreed in three ways - the
 * cache is a release behind by construction, the C# had no platform filter, and it matched on name
 * where this matches on (platform, name) - so the panel said "up to date" and Play then downloaded
 * 60 MB. Reported by the owner on the e458d41d6 release.
 *
 * Extracting it is what makes "the panel shows what Play will do" true by construction rather than
 * by two implementations agreeing. --check prints this; do_update performs it; there is one rule.
 *
 * `bytes` receives the total transfer size, which is the number the user actually wants and which
 * nothing computed before: the manifest carries a size per artifact and it was parsed and ignored.
 */
static int plan_update(const manifest *m, const state *st, const config *c,
	artifact *wanted, int cap, unsigned long long *bytes)
{
	int nwanted = 0, i;

	*bytes = 0;

	/* THE ROLLBACK REFUSAL LIVES HERE, not in the caller, and that placement is the point. It used
	 * to sit in wmain around the do_update call, so --check could not see it and would have
	 * reported a pending update that Play would then refuse to apply - the panel lying again, in
	 * the other direction. A manifest older than the install yields an empty plan for everyone. */
	if (m->ordinal < st->ordinal) {
		logf_("update: the manifest is OLDER than this install (%lu < %lu) - ignoring it",
			m->ordinal, st->ordinal);
		return 0;
	}

	for (i = 0; i < m->nitems && nwanted < cap; i++) {
		const artifact *a = &m->items[i];
		const char *have;
		/* "any" is a platform-independent artifact - the community-patch overlay is one, and it
		 * is why this feature exists at all: a friend without it has a different dataID and
		 * cannot join, which is not a thing an executable-only updater could ever fix. */
		if (_stricmp(a->platform, c->platform) != 0 && _stricmp(a->platform, "any") != 0) {
			continue;
		}
		have = state_hash_of(st, a);
		if (have && !strcmp(have, a->hash)) {
			logf_("update: %s/%s is already current", a->platform, a->name);
			continue;
		}
		*bytes += a->size;
		wanted[nwanted++] = *a;
	}
	return nwanted;
}

/* Print the plan for --check, in a form the loader parses.
 *
 * "plan-total:" IS PRINTED EVEN WHEN THE PLAN IS EMPTY, and that is deliberate. If a count of zero
 * were signalled by printing nothing, then "nothing to fetch" and "this updater is too old to
 * answer" would be the same observation - and the loader would read an ancient updater as "up to
 * date", which is the exact bug this whole change exists to remove. An absent line means UNKNOWN;
 * "plan-total: 0 0" means known-and-nothing.
 */
static void print_plan(const manifest *m, const state *st, const config *c)
{
	artifact wanted[MAX_ARTIFACTS];
	unsigned long long bytes = 0;
	int n, i;

	n = plan_update(m, st, c, wanted, MAX_ARTIFACTS, &bytes);
	for (i = 0; i < n; i++) {
		printf("plan: %s %s %s %llu\n", wanted[i].platform, wanted[i].name,
			wanted[i].hash, wanted[i].size);
	}
	printf("plan-total: %d %llu\n", n, bytes);
}

/* Returns 1 when the install is now what the manifest asks for (applied, or nothing to fetch) and
 * 0 when an update was needed and did NOT happen - which --no-launch now reports as exit 3, so a
 * launcher stops offering UPDATE as though the last press had worked. */
static int do_update(const manifest *m, const state *st, const config *c)
{
	wchar_t work[MAX_PATH], stage[MAX_PATH], file[MAX_PATH], wname[MAX_PATH];
	artifact wanted[MAX_ARTIFACTS];
	unsigned long long bytes = 0, done = 0, grand, got = 0;
	int nwanted, i, state_ok = 0;

	nwanted = plan_update(m, st, c, wanted, MAX_ARTIFACTS, &bytes);

	if (nwanted == 0) {
		/* Deliberately not "this install matches the manifest": plan_update also returns 0 for a
		 * manifest OLDER than the install, where that sentence would be false. It logs its own
		 * reason in that case, and the per-artifact "already current" lines cover this one. */
		logf_("update: nothing to fetch");
		return 1;
	}
	logf_("update: %d artifact(s) to fetch, %llu bytes", nwanted, bytes);

	/* One denominator for the whole update - unless any artifact has no size, in which case there
	 * is no honest denominator and 0 tells the launcher to show an indeterminate bar. */
	grand = bytes;
	for (i = 0; i < nwanted; i++) {
		if (wanted[i].size == 0) {
			grand = 0;
		}
	}

	path_join(work, MAX_PATH, g_dir, L"gxwork");
	rmtree(work);
	if (!make_dirs(work)) {
		logf_("update: cannot create %S - starting the game as it is", work);
		return 0;
	}

	/* Phase one: fetch and verify everything. Nothing has touched the install yet. */
	for (i = 0; i < nwanted; i++) {
		char url[512];
		const char *name = wanted[i].name;
		if (strstr(name, "://")) {
			strcpy_s(url, sizeof(url), name);
		} else if (strstr(m->base, "://")) {
			sprintf_s(url, sizeof(url), "%s%s", m->base, name);
		} else {
			sprintf_s(url, sizeof(url), "https://%s%s", m->base, name);
		}

		MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, MAX_PATH);
		path_join(file, MAX_PATH, work, wname);
		logf_("update: fetching %s/%s (%llu bytes)", wanted[i].platform, name, wanted[i].size);
		if (!download(url, file, wanted[i].size, done, grand, &got) ||
			!hash_matches(file, wanted[i].hash)) {
			logf_("update: ABANDONED - nothing has been changed in the install");
			rmtree(work);
			return 0;
		}
		done += got;		/* what arrived, which is the manifest size unless that was unknown */
	}

	/* Phase two: unpack everything into one staging tree, still without touching the install.
	 * One tree for all artifacts on purpose - if two of them disagree about a file, that is a
	 * mistake in the release rather than something to resolve at apply time, and it shows up
	 * here as the second one winning consistently instead of racing. */
	path_join(stage, MAX_PATH, work, L"stage");
	for (i = 0; i < nwanted; i++) {
		MultiByteToWideChar(CP_UTF8, 0, wanted[i].name, -1, wname, MAX_PATH);
		path_join(file, MAX_PATH, work, wname);
		if (!extract(file, stage)) {
			logf_("update: ABANDONED - nothing has been changed in the install");
			rmtree(work);
			return 0;
		}
	}

	/* Phase three, and only now. */
	if (!place_tree(stage, g_dir)) {
		/* Partial by definition if this fails, and it is logged as such rather than papered
		 * over: the state file is NOT written, so the next launch re-downloads and re-applies
		 * everything instead of believing the install is current. */
		logf_("update: some files could not be replaced - the install is PARTIAL and the next "
			"launch will redo it. Close the game and run this again.");
		rmtree(work);
		return 0;
	}

	{
		/* Everything applied. The record is every artifact the install now has, which is the
		 * ones just applied plus the ones that were already current. */
		artifact all[MAX_ARTIFACTS];
		int n = 0;
		for (i = 0; i < m->nitems && n < MAX_ARTIFACTS; i++) {
			const artifact *a = &m->items[i];
			if (_stricmp(a->platform, c->platform) == 0 || _stricmp(a->platform, "any") == 0) {
				all[n++] = *a;
			}
		}
		state_ok = state_save(m, all, n);
	}
	rmtree(work);
	logf_("update: applied %d artifact(s), now at ordinal %lu", nwanted, m->ordinal);

	/* Machine-readable, same convention as "progress: " - the launcher consumes this line and does
	 * not show it. Emitted only after the state file is written and the work directory is gone, so
	 * the restarted launcher comes up on a fully applied install and does not decide it has work to
	 * do all over again.
	 *
	 * GATED ON state_ok, and that gate is the difference between a papercut and an unplayable game.
	 * If the record did not persist, the next launch wants every artifact again - so asking for a
	 * restart here would give Play -> full re-download -> restart -> Play -> ... with the game never
	 * starting. Staying silent falls back to exactly the behaviour this change replaced: the user
	 * plays on the old launcher image, which is cosmetic, and the install is still correct. */
	if (g_launcher_replaced && state_ok) {
		logf_("update: the launcher replaced itself - asking it to restart");
		if (g_shared) {
			/* A GENERATION, never a flag and never cleared: each waiting run compares it against
			 * what it saw before it started waiting, so all of them are told, not just the first. */
			g_shared[2] = g_shared[2] + 1;
		}
		printf("restart: launcher\n");
		fflush(stdout);
	} else if (g_launcher_replaced) {
		logf_("update: the launcher was replaced but the install record did not persist - NOT "
			"restarting, because a restart on an unrecorded update never reaches the game");
	}
	return 1;		/* applied - even if the record did not persist, which only costs a redo */
}

/* ============================================================================================
 * Self-test, so the crypto is provable rather than asserted
 *
 * Vectors are produced by scripts/tooling/launcher-vectors.py through Python's `cryptography`,
 * which is OpenSSL - a real Ed25519 implementation this one has to agree with. Each line is
 *
 *     <expect 0|1> <pk-hex> <sig-hex> <msg-hex>
 *
 * and the corrupted cases matter more than the good ones: a verifier that returned 1
 * unconditionally would pass a file containing only valid signatures.
 * ========================================================================================== */

static int hexbytes(const char *s, unsigned char *out, size_t cap)
{
	size_t n = strlen(s), i;
	if (n % 2 || n / 2 > cap) {
		return -1;
	}
	for (i = 0; i < n / 2; i++) {
		unsigned v;
		if (sscanf_s(s + 2 * i, "%2x", &v) != 1) {
			return -1;
		}
		out[i] = (unsigned char)v;
	}
	return (int)(n / 2);
}

/* ============================================================================================
 * GeneralsX @test Claude 22/10/2026 publish_relay_file on scratch files under %TEMP%, never the
 * player's own Options.ini: the five states an Options.ini can be in when the updater wants to write
 * RelayAddress. Cases 4 and 5 are the data-loss bug - before the fix both went on to replace the file
 * with the one RelayAddress line (case 5's lock shares DELETE, so the rename over it can succeed).
 * ========================================================================================== */

static int st_write(const wchar_t *path, const char *bytes, size_t n)
{
	HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD wrote = 0;
	int ok;

	if (f == INVALID_HANDLE_VALUE) {
		return 0;
	}
	ok = WriteFile(f, bytes, (DWORD)n, &wrote, NULL) && wrote == (DWORD)n;
	CloseHandle(f);
	return ok;
}

/* The file holds exactly these bytes. */
static int st_same(const wchar_t *path, const char *bytes, size_t n)
{
	char *got = read_text_why(path, 8u << 20, NULL, NULL, NULL);
	int same = got && strlen(got) == n && memcmp(got, bytes, n) == 0;

	free(got);
	return same;
}

static int st_check(int cond, const char *what, int *fail)
{
	printf("%s  options.ini: %s\n", cond ? "PASS" : "FAIL", what);
	if (!cond) {
		(*fail)++;
	}
	return cond;
}

static int selftest_options_ini(void)
{
	static const char want[] = "RelayAddress = relay.example\r\n";
	static const char before[] = "Gamma = 50\r\nRelayAddress = old.example\r\nGXGround = no\r\n";
	static const char merged[] = "Gamma = 50\r\nGXGround = no\r\nRelayAddress = relay.example\r\n";
	static const char pad[] = "; a long hand-written comment that makes this Options.ini bigger than the cap\r\n";
	wchar_t tmpdir[MAX_PATH], dir[MAX_PATH], path[MAX_PATH], gxtmp[MAX_PATH];
	size_t bign = 0, padn = sizeof(pad) - 1;
	char *big;
	HANDLE lock;
	int fail = 0, r;

	/* Room for "gxselftest-<pid>\\Options.ini.gxtmp" after it, or swprintf_s stops the program. */
	r = (int)GetTempPathW(MAX_PATH, tmpdir);
	if (r == 0 || r > MAX_PATH - 48) {
		st_check(0, "no temp directory short enough for the scratch files", &fail);
		return fail;
	}
	/* A FRESH folder, never one that is already there (it could be a junction somewhere real). */
	swprintf_s(dir, MAX_PATH, L"%sgxselftest-%lu", tmpdir, GetCurrentProcessId());
	if (!CreateDirectoryW(dir, NULL)) {
		st_check(0, "could not make a fresh scratch folder in the temp directory", &fail);
		return fail;
	}
	path_join(path, MAX_PATH, dir, L"Options.ini");
	swprintf_s(gxtmp, MAX_PATH, L"%s.gxtmp", path);

	/* 1. No file: the one case that may be created from nothing. */
	DeleteFileW(path);
	r = publish_relay_file(path, "relay.example", want);
	st_check(r == PUBLISH_WROTE && st_same(path, want, sizeof(want) - 1),
		"1 missing file - created with the RelayAddress line only", &fail);

	/* 2. An ordinary file: every other line kept in order, RelayAddress replaced. */
	st_write(path, before, sizeof(before) - 1);
	r = publish_relay_file(path, "relay.example", want);
	st_check(r == PUBLISH_WROTE && st_same(path, merged, sizeof(merged) - 1),
		"2 ordinary file - other lines kept, RelayAddress replaced", &fail);

	/* 3. Already current: not touched. */
	r = publish_relay_file(path, "relay.example", want);
	st_check(r == PUBLISH_CURRENT && st_same(path, merged, sizeof(merged) - 1),
		"3 already current - not rewritten", &fail);

	/* 4. Over the 1 MiB cap: left exactly as it is. */
	big = (char *)malloc((1u << 20) + 2 * padn + sizeof(before));
	if (big) {
		memcpy(big, before, sizeof(before) - 1);
		bign = sizeof(before) - 1;
		while (bign <= (1u << 20)) {
			memcpy(big + bign, pad, padn);
			bign += padn;
		}
		st_write(path, big, bign);
		r = publish_relay_file(path, "relay.example", want);
		st_check(r == PUBLISH_KEPT && st_same(path, big, bign) && GetFileAttributesW(gxtmp) == INVALID_FILE_ATTRIBUTES,
			"4 file over 1 MiB - kept byte for byte, nothing written", &fail);
		free(big);
	} else {
		st_check(0, "4 file over 1 MiB - no memory for the test file", &fail);
	}

	/* 5. Open elsewhere without read sharing (an AV scan, a sync client): left exactly as it is. */
	st_write(path, before, sizeof(before) - 1);
	lock = CreateFileW(path, GENERIC_READ, FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (lock != INVALID_HANDLE_VALUE) {
		r = publish_relay_file(path, "relay.example", want);
		CloseHandle(lock);
		st_check(r == PUBLISH_KEPT && st_same(path, before, sizeof(before) - 1)
				&& GetFileAttributesW(gxtmp) == INVALID_FILE_ATTRIBUTES,
			"5 file that cannot be opened for reading - kept byte for byte, nothing written", &fail);
	} else {
		st_check(0, "5 locked file - the test could not take its own lock", &fail);
	}

	/* 6. A NUL byte inside: the copy loop stops at it, so the replacement would drop every line after it. */
	{
		static const char nul[] = "Gamma = 50\r\n; note\0\r\nGXGround = no\r\n";
		HANDLE g;
		DWORD wrote = 0;
		char *back;

		g = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (g != INVALID_HANDLE_VALUE) {
			WriteFile(g, nul, (DWORD)(sizeof(nul) - 1), &wrote, NULL);
			CloseHandle(g);
		}
		r = publish_relay_file(path, "relay.example", want);
		back = read_text_why(path, 1 << 16, NULL, NULL, NULL);
		st_check(r == PUBLISH_KEPT && back && strcmp(back, "Gamma = 50\r\n; note") == 0
				&& GetFileAttributesW(gxtmp) == INVALID_FILE_ATTRIBUTES,
			"6 file with a NUL byte inside - kept, nothing written", &fail);
		free(back);
	}

	/* 7. A UTF-8 BOM at the start (a hand edit in an older Notepad) is not part of the first key: a BOM-first
	 * RelayAddress is found and replaced, never left behind as a second one; any other first line keeps its BOM. */
	{
		static const char bom_relay[] = "\xEF\xBB\xBFRelayAddress = old.example\r\nGamma = 50\r\n";
		static const char bom_relay_after[] = "\xEF\xBB\xBFGamma = 50\r\nRelayAddress = relay.example\r\n";
		static const char bom_two[] = "\xEF\xBB\xBFRelayAddress = old.example\r\n\xEF\xBB\xBFGXRenderer = native\r\n";
		static const char bom_two_after[] = "\xEF\xBB\xBF\xEF\xBB\xBFGXRenderer = native\r\nRelayAddress = relay.example\r\n";
		static const char bom_mid[] = "RelayAddress = old.example\r\n\xEF\xBB\xBFGXRenderer = native\r\n";
		static const char bom_mid_after[] = "\r\n\xEF\xBB\xBFGXRenderer = native\r\nRelayAddress = relay.example\r\n";
		static const char bom_other[] = "\xEF\xBB\xBFGamma = 50\r\nRelayAddress = old.example\r\n";
		static const char bom_other_after[] = "\xEF\xBB\xBFGamma = 50\r\nRelayAddress = relay.example\r\n";
		char v[64];

		st_check(ini_get(bom_relay, "RelayAddress", v, sizeof(v)) && strcmp(v, "old.example") == 0,
			"7a BOM-first RelayAddress - read past the BOM", &fail);
		st_write(path, bom_relay, sizeof(bom_relay) - 1);
		r = publish_relay_file(path, "relay.example", want);
		st_check(r == PUBLISH_WROTE && st_same(path, bom_relay_after, sizeof(bom_relay_after) - 1),
			"7b BOM-first RelayAddress - replaced, no second RelayAddress left, the file keeps its BOM", &fail);
		st_write(path, bom_other, sizeof(bom_other) - 1);
		r = publish_relay_file(path, "relay.example", want);
		st_check(r == PUBLISH_WROTE && st_same(path, bom_other_after, sizeof(bom_other_after) - 1),
			"7c BOM-first other key - kept with its BOM, RelayAddress replaced", &fail);
		/* d. A second BOM on the next line stays data: it must not become the file's when line 1 goes. */
		st_write(path, bom_two, sizeof(bom_two) - 1);
		r = publish_relay_file(path, "relay.example", want);
		st_check(r == PUBLISH_WROTE && st_same(path, bom_two_after, sizeof(bom_two_after) - 1),
			"7d BOM-first RelayAddress before a BOM-first line - that line stays behind a BOM that is data", &fail);
		/* e. No leading BOM, RelayAddress first, a BOM that is data on line 2: it must not become the file's first bytes. */
		st_write(path, bom_mid, sizeof(bom_mid) - 1);
		r = publish_relay_file(path, "relay.example", want);
		st_check(r == PUBLISH_WROTE && st_same(path, bom_mid_after, sizeof(bom_mid_after) - 1),
			"7e RelayAddress before a line that starts with a BOM (data) - that BOM does not reach byte zero", &fail);
	}

	DeleteFileW(gxtmp);
	DeleteFileW(path);
	RemoveDirectoryW(dir);
	printf("options.ini publish: %d failed\n", fail);
	return fail;
}

static int selftest(const wchar_t *vectors)
{
	char *text, *p;
	int pass = 0, fail = 0;

	printf("group order check ([L]B == identity): %s\n",
		gx_ed25519_selftest_group() ? "PASS" : "FAIL");
	if (!gx_ed25519_selftest_group()) {
		return 1;
	}
	if (selftest_options_ini() != 0) {
		return 1;
	}

	text = read_text(vectors, 8u << 20);
	if (!text) {
		printf("cannot read the vector file\n");
		return 1;
	}
	p = text;
	while (p && *p) {
		char *eol = strchr(p, '\n');
		char expect[4], pkh[200], sigh[300], msgh[4096];
		if (eol) {
			*eol = 0;
		}
		if (sscanf_s(p, "%3s %199s %299s %4095s", expect, 4u, pkh, 200u, sigh, 300u,
				msgh, 4096u) == 4) {
			unsigned char pk[32], sig[64], msg[2048];
			/* "-" is the vector file's spelling of a zero-length message; see the generator. */
			int mlen = !strcmp(msgh, "-") ? 0 : hexbytes(msgh, msg, sizeof(msg));
			int got;
			if (hexbytes(pkh, pk, sizeof(pk)) == 32 && hexbytes(sigh, sig, sizeof(sig)) == 64
				&& mlen >= 0) {
				got = gx_ed25519_verify(pk, sig, msg, (size_t)mlen);
				if (got == atoi(expect)) {
					pass++;
				} else {
					fail++;
					printf("FAIL  expected %s got %d for %.24s...\n", expect, got, sigh);
				}
			} else {
				fail++;
				printf("FAIL  unparseable vector line\n");
			}
		}
		p = eol ? eol + 1 : NULL;
	}
	free(text);
	printf("%d vectors, %d passed, %d failed\n", pass + fail, pass, fail);
	return fail ? 1 : 0;
}

/* ========================================================================================== */

static int run(int argc, wchar_t **argv);

/* The lock file is closed here rather than left to process teardown, so the next waiter sees it go
 * at a point this program chose. A crash gives the same result one poll later: FILE_FLAG_DELETE_ON_CLOSE
 * means the kernel closes and deletes it, with none of the "was it abandoned?" ambiguity a mutex had. */
int wmain(int argc, wchar_t **argv)
{
	int rc = run(argc, argv);
	if (g_lock) {
		CloseHandle(g_lock);
	}
	return rc;
}

static int run(int argc, wchar_t **argv)
{
	config c;
	state st;
	manifest m;
	char wire[WIRE_MAX];
	int n, i, check_only = 0, no_launch = 0;

	/* UNBUFFERED STDOUT, AND IT IS NOT A MICRO-OPTIMISATION.
	 *
	 * logf_ writes progress with printf. When this program runs from a console the CRT
	 * line-buffers and everything appears as it happens - which is why this was never noticed.
	 * When stdout is a PIPE, which is exactly how the launcher runs it, the MSVC CRT switches to
	 * FULL buffering: 4 KB of output accumulates and is flushed at exit.
	 *
	 * The visible consequence is the whole complaint about this program. The launcher's log pane
	 * stayed empty for the entire download and then filled in one burst at the end, so a friend
	 * watching a 14 MB transfer saw a frozen window and no evidence anything was happening. No
	 * amount of work on the progress bar helps while the data driving it arrives only after the
	 * work it describes has finished. */
	setvbuf(stdout, NULL, _IONBF, 0);

	GetModuleFileNameW(NULL, g_dir, MAX_PATH);
	{
		wchar_t *slash = wcsrchr(g_dir, L'\\');
		if (slash) {
			*slash = 0;
		}
	}
	path_join(g_log, MAX_PATH, g_dir, L"gxlaunch.log");

	for (i = 1; i < argc; i++) {
		if (!_wcsicmp(argv[i], L"--selftest") && i + 1 < argc) {
			return selftest(argv[i + 1]);
		}
		/* GeneralsX @bugfix Claude 11/08/2026 There is no --login any more. It ran the RFC 8628
		 * device flow against /device and /claim, which tools/oauth/worker.js no longer serves, so
		 * the flag could only ever fail. An old shortcut still carrying it is harmless: it matches
		 * nothing here and launch_game drops every "--" argument before the engine sees it. Signing
		 * in is the engine's loopback flow; this program only reports the state of the file the
		 * engine writes. */
		if (!_wcsicmp(argv[i], L"--check")) {
			check_only = 1;		/* fetch, verify, report; change nothing, start nothing */
		}
		if (!_wcsicmp(argv[i], L"--no-launch")) {
			no_launch = 1;		/* update if there is one, but do not start the game */
		}
	}

	/* GeneralsX @bugfix Claude 28/09/2026 NEVER ON A TEST PROFILE. This program resolves the OWNER's
	 * user-data folder itself (userdata_path): it reads and writes RelayAddress in that Options.ini and
	 * reads GXLogin.txt. It does not follow GX_USERDATA_DIR (GameClient/GXUserDataDir.h), so a caller who
	 * set it - any value - wants a test profile this program cannot give, and it stops here, before any
	 * file is read or written: not even gxlaunch.log, which is in the install. Start generalszh.exe
	 * directly or gxlauncher, which follow it. The dialog only without stdout, as in launch_game: under a
	 * harness a modal box is a hang. Checked by test-test-profile.sh, which compiles this block. */
	if (_wgetenv(L"GX_USERDATA_DIR") != NULL) {
		const char *why = "GeneralsX launcher: REFUSED - GX_USERDATA_DIR is set (a test profile) and this "
			"program does not follow it: it would read and write the owner's Options.ini. Start "
			"generalszh.exe directly, or gxlauncher.";
		HANDLE so = GetStdHandle(STD_OUTPUT_HANDLE);
		fprintf(stderr, "%s\n", why);
		if (so == NULL || so == INVALID_HANDLE_VALUE) {
			MessageBoxA(NULL, why, "GeneralsX", MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		}
		return 4;
	}

	logf_("--- GeneralsX launcher, install %S", g_dir);
	logf_("%s", g_updater_commit);

	/* BEFORE ANY WRITE. See "ONE UPDATER PER INSTALL". A run that cannot hold the lock changes
	 * nothing: --check answers busy (3), an update is skipped (3 under --no-launch), and a plain
	 * launch still starts the game, which is what every other failure here does. */
	if (!acquire_instance(check_only)) {
		return (check_only || no_launch) ? 3 : launch_game(argc, argv);
	}

	/* Before anything else: clear what a previous update left behind. A .gxold is the old copy
	 * of a file that was in use at the time, so this is the first moment it can be removed. */
	sweep_old(g_dir);

	/* And what the old INSTALL flow left behind, which only the installer removes - an install
	 * that has updated itself ever since never runs the installer again to get that cleanup. */
	sweep_legacy(g_dir);

	load_config(&c);
	/* One line about the account, on every normal run. The launcher is the only thing on Windows
	 * that can tell a player their login is about to expire, because it is the only thing that
	 * runs before the game and can still do something about it. */
	report_login_state();
	state_load(&st);

	/* THE BOOTSTRAP, and it comes before everything because everything else needs what it
	 * produces. Nobody types a server address: the signed document says which relays exist, and
	 * this program hands the game one. */
	if (!bootstrap_manifest(&c, &m, wire, sizeof(wire))) {
		/* No signed document from the publisher and none cached. Everything below this line is the
		 * pre-bootstrap path, reachable only by an install that already had a relay address. */
		n = fetch_manifest(&c, st.ordinal, wire, sizeof(wire));
		if (n <= 0) {
			return check_only ? 1 : (no_launch ? 0 : launch_game(argc, argv));
		}
		if (!strncmp(wire, "GXMANI -", 8)) {
			logf_("update: this relay is serving no manifest - starting the game");
			return check_only ? 0 : (no_launch ? 0 : launch_game(argc, argv));
		}
		if (strncmp(wire, "GXMANI ", 7)) {
			logf_("update: unexpected reply from the relay - starting the game");
			return check_only ? 1 : (no_launch ? 0 : launch_game(argc, argv));
		}
		{
			/* Parse a COPY: parse_manifest splits in place, and the cached bytes must be the signed ones. */
			static char udp_raw[WIRE_MAX];
			strcpy_s(udp_raw, sizeof(udp_raw), wire + 7);
			if (!parse_manifest(wire + 7, &m)) {
				return check_only ? 1 : (no_launch ? 0 : launch_game(argc, argv));
			}
			/* GeneralsX @bugfix Claude 29/10/2026 S1-3 (Codex relaylist-v2-r3 P2) CACHED, like the HTTPS document. This
			 * path runs only when nothing is cached, and a document accepted here used to be forgotten - so the key
			 * RANK it was signed with was never persisted, and a K1 document was taken again on the next launch after
			 * a K2 one had been seen, which one-way key retirement exists to prevent. */
			cache_save(udp_raw, (unsigned __int64)time(NULL));
		}
	} else if (!c.relay_pinned && m.nrelays > 0) {
		/* Selection and publication are BEST EFFORT and deliberately not gates. A machine that
		 * cannot reach any relay right now must still get its update and still start its game -
		 * it may well be about to play a skirmish. */
		char host[128];
		int port = RELAY_LOBBY_PORT;
		if (select_relay(&m, st.ordinal, host, sizeof(host), &port)) {
			strcpy_s(c.relay_host, sizeof(c.relay_host), host);
			c.relay_port = port;
			/* --check probes and REPORTS but never writes. Its contract is "change nothing, start
			 * nothing", and a diagnostic mode that quietly edits the game's settings is a
			 * diagnostic mode nobody can safely run on somebody else's machine. */
			if (check_only) {
				printf("relay selected: %s:%d (not written - this is --check)\n", host, port);
			} else {
				publish_relay(host, port);
			}
		}
	}

	if (check_only) {
		printf("manifest OK: ordinal %lu, %d relay(s), %d artifact(s)\n",
			m.ordinal, m.nrelays, m.nitems);
		for (i = 0; i < m.nrelays; i++) {
			printf("  relay %s:%d %s\n", m.relays[i].host, m.relays[i].port, m.relays[i].region);
		}
		for (i = 0; i < m.nitems; i++) {
			printf("  %s %s %s %llu\n", m.items[i].platform, m.items[i].name,
				m.items[i].hash, m.items[i].size);
		}
		/* The lines the LAUNCHER reads. Everything above is for a human running this by hand; this
		 * is the machine-readable answer to "what would Play download right now", computed by the
		 * same function that would perform it. Printed last so a parser can rely on plan-total:
		 * being the final word. */
		print_plan(&m, &st, &c);
		return 0;
	}

	/* The manifest-older-than-install refusal now lives in plan_update, so that --check and Play
	 * cannot disagree about it. */
	if (!do_update(&m, &st, &c) && no_launch) {
		return 3;
	}
	if (g_relay_restart && !g_launcher_replaced) {
		logf_("update: the update this run followed replaced the launcher - passing its restart on");
		printf("restart: launcher\n");
	}
	return no_launch ? 0 : launch_game(argc, argv);
}
