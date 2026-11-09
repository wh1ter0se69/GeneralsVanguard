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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

//////// StdBIGFileSystem.h ///////////////////////////
// Stephan Vedder, April 2025
/////////////////////////////////////////////////////////////

#include "Common/AudioAffect.h"
#include "Common/ArchiveFile.h"
#include "Common/ArchiveFileSystem.h"
#include "Common/file.h"
#include "Common/GameAudio.h"
#include "Common/GameMemory.h"
#include "Common/GlobalData.h"
#include "Common/LocalFileSystem.h"
#include "Common/Registry.h"

#include "StdDevice/Common/StdBIGFile.h"
#include "StdDevice/Common/StdAssetMarker.h"
#include "StdDevice/Common/GXArchiveMount.h"
#include "StdDevice/Common/StdBIGFileSystem.h"
#include "Utility/endian_compat.h"

#include <climits>
#include <cstdlib>
#include <cstdio>
#include <cstring>

// GeneralsX @bugfix wh1ter0se 12/08/2026 Directory walk for the base-Generals sibling scan below.
// This translation unit is compiled on Windows too, so <dirent.h> cannot be included unguarded;
// the arrangement below is the one GameEngine.cpp:63 already uses. The shim provides d_name ONLY -
// there is no d_type, so nothing below may use it.
#ifdef _WIN32
#include "Utility/dirent_compat.h"
#else
#include <dirent.h>
#endif

#if !defined(_WIN32)
// strcasecmp/strncasecmp. Unconditional on every non-Windows target rather than gated on
// _UNIX: the gate happened to work only because <strings.h> arrived transitively.
#include <strings.h>
#endif

#if defined(_UNIX)
#include <unistd.h>
#endif

#if defined(_WIN32)
#include <io.h>
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

// GeneralsX @bugfix Claude 28/10/2026 Reject malformed BIG archives. Ports upstream PR #3588 (c7b7814e9) by hand:
// upstream's loader is Core/GameEngine/Source/Common/System/BIGFileSystem.cpp, which the fork does not build - the same
// directory reader lives here and in its twin (StdBIGFileSystem.cpp / Win32BIGFileSystem.cpp), so the two stay mirrored.
// The header and every directory entry are read whole and the read is checked; the counts are unsigned (a count with the
// top bit set used to read as negative and mount an EMPTY archive); a short header, a directory that runs past the end of
// the file or a path with no terminator inside the buffer rejects the whole archive. In an anonymous namespace because
// Windows compiles both twins into one library. A well-formed archive reads the same bytes in the same order as before.
namespace {

const char BIGFileIdentifier[] = { 'B', 'I', 'G', 'F' };

struct BIGFileHeader {
	char identifier[sizeof(BIGFileIdentifier)];
	UnsignedInt archiveFileSize; // little endian, retail reads it as Int so 2 GB max
	UnsignedInt numFiles; // big endian
	UnsignedInt firstEntryOffset; // big endian
};

struct BIGFileEntryHeader {
	UnsignedInt fileOffset; // big endian
	UnsignedInt fileSize; // big endian
};

static_assert(sizeof(BIGFileHeader) == 16, "Incorrect BIG header size");
static_assert(sizeof(BIGFileEntryHeader) == 8, "Incorrect BIG entry header size");

// Returns the length of the string, or -1 if it is truncated or does not fit the buffer.
Int readNullTerminatedString(File *fp, char *buffer, Int bufferSize) {
	for (Int i = 0; i < bufferSize; ++i) {
		if (fp->read(buffer + i, 1) != 1) {
			return -1;
		}
		if (buffer[i] == 0) {
			return i;
		}
	}
	return -1;
}

} // namespace

namespace {

static const char* kPrimaryAssetEnvGenerals = "CNC_GENERALS_PATH";
static const char* kPrimaryAssetEnvZH = "CNC_GENERALS_ZH_PATH";
static const char* kPrimaryAssetIniKey = "AssetPath";
static const char* kAssetPathSection = "Paths";

// GeneralsX @feature Claude 08/08/2026 Community-patch overlay directory.
static const char* kPatchOverlayEnv = "GENERALSX_PATCH_PATH";
static const char* kPatchOverlayDefaultSubdir = "GeneralsXPatch";

#if RTS_ZEROHOUR
static const char* kBaseGeneralsAssetEnv = "CNC_GENERALS_PATH";
static const char* kBaseGeneralsAssetIniKey = "GeneralsAssetPath";
#endif

static Bool equalsIgnoreCase(const char* lhs, const char* rhs)
{
	if (lhs == nullptr || rhs == nullptr) {
		return FALSE;
	}

#ifdef _WIN32
	return _stricmp(lhs, rhs) == 0;
#else
	return strcasecmp(lhs, rhs) == 0;
#endif
}

// GeneralsX @bugfix Claude 09/09/2026 Does this folder hold THIS build's game data?
//
// The scan itself lives in StdDevice/Common/StdAssetMarker.h so it can be RUN. This decision is
// what stops the game starting, and as a file-static in a translation unit that pulls in most of
// the engine there was no way to execute it short of a full build - it was committed tagged
// EXPECTED for exactly that reason. scripts/test/asset-marker-test.cpp now drives the same code
// this calls, against real directories, with a plain c++ line and no cmake.
//
// UNREADABLE IS NOT ABSENT, and the header returns three outcomes so this can say so. A directory
// we cannot open must not be reported to a player as "your game files are missing" - that sends
// them hunting for data they already own. Only a directory we successfully read and found no
// marker in is grounds for refusing.
static GXAssetMarker::Result variantMarkerResult(const AsciiString& dir)
{
	if (dir.isEmpty()) {
		// Nothing to scan. Every caller now records a real, scannable directory - the cwd
		// fallback records "." and the Resources fallback records its own path - so reaching
		// here means a path this code does not know how to check rather than a missing game.
		// Report Unreadable, which is refused with its own diagnostic rather than being
		// mistaken for "the wrong game's files are here".
		return GXAssetMarker::Unreadable;
	}
#if RTS_ZEROHOUR
	const bool zeroHour = true;
#else
	const bool zeroHour = false;
#endif
	return GXAssetMarker::dirHasMarker(dir.str(), GXAssetMarker::requiredMarkerFor(zeroHour));
}

static const char* trimLeft(const char* text)
{
	while (*text != '\0' && (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')) {
		++text;
	}

	return text;
}

static void trimRight(char* text)
{
	Int len = static_cast<Int>(strlen(text));

	while (len > 0) {
		const char c = text[len - 1];
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			text[len - 1] = '\0';
			--len;
			continue;
		}

		break;
	}
}

static Bool extractIniValue(const char* iniPath, const char* sectionName, const char* keyName, AsciiString& value)
{
	FILE* fp = fopen(iniPath, "r");
	if (fp == nullptr) {
		return FALSE;
	}

	Bool inSection = FALSE;
	char line[2048] = { 0 };

	while (fgets(line, sizeof(line), fp) != nullptr) {
		trimRight(line);

		const char* leftTrimmed = trimLeft(line);
		if (*leftTrimmed == '\0' || *leftTrimmed == ';' || *leftTrimmed == '#') {
			continue;
		}

		if (*leftTrimmed == '[') {
			const char* sectionEnd = strchr(leftTrimmed, ']');
			if (sectionEnd != nullptr) {
				char sectionBuffer[256] = { 0 };
				const size_t sectionLen = static_cast<size_t>(sectionEnd - (leftTrimmed + 1));
				if (sectionLen < sizeof(sectionBuffer)) {
					strncpy(sectionBuffer, leftTrimmed + 1, sectionLen);
					sectionBuffer[sectionLen] = '\0';
					trimRight(sectionBuffer);
					const char* sectionNameTrimmed = trimLeft(sectionBuffer);
					inSection = equalsIgnoreCase(sectionNameTrimmed, sectionName);
				}
				else {
					inSection = FALSE;
				}
			}
			else {
				inSection = FALSE;
			}

			continue;
		}

		if (!inSection) {
			continue;
		}

		const char* equalsPos = strchr(leftTrimmed, '=');
		if (equalsPos == nullptr) {
			continue;
		}

		char keyBuffer[256] = { 0 };
		const size_t keyLen = static_cast<size_t>(equalsPos - leftTrimmed);
		if (keyLen >= sizeof(keyBuffer)) {
			continue;
		}

		strncpy(keyBuffer, leftTrimmed, keyLen);
		keyBuffer[keyLen] = '\0';
		trimRight(keyBuffer);
		const char* keyTrimmed = trimLeft(keyBuffer);
		if (!equalsIgnoreCase(keyTrimmed, keyName)) {
			continue;
		}

		char valueBuffer[2048] = { 0 };
		strncpy(valueBuffer, equalsPos + 1, sizeof(valueBuffer) - 1);
		trimRight(valueBuffer);
		const char* valueTrimmed = trimLeft(valueBuffer);
		if (*valueTrimmed == '\0') {
			continue;
		}

		size_t valueLen = strlen(valueTrimmed);
		if (valueLen >= 2 && ((valueTrimmed[0] == '"' && valueTrimmed[valueLen - 1] == '"') || (valueTrimmed[0] == '\'' && valueTrimmed[valueLen - 1] == '\''))) {
			char unquotedValue[2048] = { 0 };
			const size_t copyLen = valueLen - 2;
			if (copyLen >= sizeof(unquotedValue)) {
				continue;
			}

			strncpy(unquotedValue, valueTrimmed + 1, copyLen);
			unquotedValue[copyLen] = '\0';
			value = unquotedValue;
		}
		else {
			value = valueTrimmed;
		}
		fclose(fp);
		return TRUE;
	}

	fclose(fp);
	return FALSE;
}

static Bool getExecutableDirectory(AsciiString& exeDirectory)
{
	char pathBuffer[4096] = { 0 };

#ifdef _WIN32
	DWORD len = GetModuleFileNameA(nullptr, pathBuffer, sizeof(pathBuffer));
	if (len == 0 || len >= sizeof(pathBuffer)) {
		return FALSE;
	}
#elif defined(__APPLE__)
	uint32_t bufferSize = static_cast<uint32_t>(sizeof(pathBuffer));
	if (_NSGetExecutablePath(pathBuffer, &bufferSize) != 0) {
		return FALSE;
	}
#else
	ssize_t len = readlink("/proc/self/exe", pathBuffer, sizeof(pathBuffer) - 1);
	if (len <= 0) {
		return FALSE;
	}
	pathBuffer[len] = '\0';
#endif

	char* lastSlash = strrchr(pathBuffer, '/');
	char* lastBackslash = strrchr(pathBuffer, '\\');
	char* split = lastSlash;
	if (split == nullptr || (lastBackslash != nullptr && lastBackslash > split)) {
		split = lastBackslash;
	}

	if (split == nullptr) {
		return FALSE;
	}

	*split = '\0';
	exeDirectory = pathBuffer;
	return exeDirectory.isNotEmpty();
}

static Bool sanitizeConfiguredPath(const char* rawValue, AsciiString& sanitizedPath)
{
	if (rawValue == nullptr) {
		return FALSE;
	}

	char pathBuffer[4096] = { 0 };
	strncpy(pathBuffer, rawValue, sizeof(pathBuffer) - 1);
	trimRight(pathBuffer);

	const char* trimmedPath = trimLeft(pathBuffer);
	if (*trimmedPath == '\0') {
		return FALSE;
	}

	const size_t trimmedLen = strlen(trimmedPath);
	if (trimmedLen >= 2 && ((trimmedPath[0] == '"' && trimmedPath[trimmedLen - 1] == '"') || (trimmedPath[0] == '\'' && trimmedPath[trimmedLen - 1] == '\''))) {
		char unquotedPath[4096] = { 0 };
		const size_t copyLen = trimmedLen - 2;
		if (copyLen >= sizeof(unquotedPath)) {
			return FALSE;
		}

		strncpy(unquotedPath, trimmedPath + 1, copyLen);
		unquotedPath[copyLen] = '\0';
		sanitizedPath = unquotedPath;
	}
	else {
		sanitizedPath = trimmedPath;
	}

	return sanitizedPath.isNotEmpty();
}

// GeneralsX @feature Claude 08/08/2026 Community-patch overlay directory - path helpers.

static Int comparePathNoCase(const char* lhs, const char* rhs)
{
	if (lhs == nullptr || rhs == nullptr) {
		return lhs == rhs ? 0 : (lhs == nullptr ? -1 : 1);
	}

#ifdef _WIN32
	return _stricmp(lhs, rhs);
#else
	return strcasecmp(lhs, rhs);
#endif
}

static AsciiString baseNameOfPath(const AsciiString& path)
{
	const char* text = path.str();
	if (text == nullptr) {
		return AsciiString::TheEmptyString;
	}

	const char* lastSlash = strrchr(text, '/');
	const char* lastBackslash = strrchr(text, '\\');
	const char* split = lastSlash;
	if (split == nullptr || (lastBackslash != nullptr && lastBackslash > split)) {
		split = lastBackslash;
	}

	return AsciiString(split != nullptr ? split + 1 : text);
}

// realpath()/_fullpath() collapse "..", "." and symlinks, so two spellings of one
// directory compare equal - which is what the "is the overlay the asset root?" and "did
// the recursive game-folder scan already reach this file?" tests both need. Failure is
// also the existence test: an overlay directory that is not there resolves to nothing and
// every code path below it is skipped.
static Bool resolveExistingPath(const AsciiString& path, AsciiString& resolved)
{
	if (path.isEmpty()) {
		return FALSE;
	}

#ifdef _WIN32
	char buffer[_MAX_PATH] = { 0 };
	if (_fullpath(buffer, path.str(), sizeof(buffer)) == nullptr) {
		return FALSE;
	}
	if (_access(buffer, 0) != 0) {
		return FALSE;
	}
	resolved = buffer;
	return TRUE;
#else
	char* real = realpath(path.str(), nullptr);
	if (real == nullptr) {
		return FALSE;
	}
	resolved = real;
	free(real);
	return TRUE;
#endif
}

// Strict containment on a separator boundary, so "/games/ZeroHourExtra" is never treated
// as living inside "/games/ZeroHour".
static Bool isPathUnderDirectory(const AsciiString& resolvedPath, const AsciiString& resolvedDirectory)
{
	const char* path = resolvedPath.str();
	const char* dir = resolvedDirectory.str();
	if (path == nullptr || dir == nullptr) {
		return FALSE;
	}

	const size_t dirLen = strlen(dir);
	if (dirLen == 0 || strlen(path) <= dirLen) {
		return FALSE;
	}

#ifdef _WIN32
	if (_strnicmp(path, dir, dirLen) != 0) {
		return FALSE;
	}
#else
	if (strncasecmp(path, dir, dirLen) != 0) {
		return FALSE;
	}
#endif

	const char boundary = path[dirLen];
	return boundary == '/' || boundary == '\\';
}

// ENV wins; otherwise a GeneralsX-owned subdirectory of the directory the executable
// lives in. The default deliberately hangs off the EXECUTABLE and not off the asset root:
// the whole point is to be able to hand somebody data without writing into their Zero Hour
// install, and on a normal install the two directories are not the same one. When they
// are, loadPatchOverlay's asset-root guard turns the overlay off rather than mounting the
// game folder twice.
static Bool resolvePatchOverlayDirectory(AsciiString& resolvedOverlayDirectory)
{
	AsciiString candidate;
	if (sanitizeConfiguredPath(getenv(kPatchOverlayEnv), candidate)) {
		if (resolveExistingPath(candidate, resolvedOverlayDirectory)) {
			return TRUE;
		}

		fprintf(stderr, "[PATCH_OVERLAY] %s='%s' does not exist - ignored\n", kPatchOverlayEnv, candidate.str());
		return FALSE;
	}

	AsciiString exeDirectory;
	if (!getExecutableDirectory(exeDirectory)) {
		return FALSE;
	}

	candidate = exeDirectory;
	candidate.concat('/');
	candidate.concat(kPatchOverlayDefaultSubdir);
	return resolveExistingPath(candidate, resolvedOverlayDirectory);
}

template <typename TBigFileSystem>
static Bool tryLoadBigFiles(TBigFileSystem* fileSystem, const AsciiString& directory, const char* sourceTag, Bool overwrite = FALSE)
{
	if (directory.isEmpty()) {
		return FALSE;
	}

	// GeneralsX @bugfix Claude 27/07/2026 Guarantee the trailing separator.
	//
	// StdLocalFileSystem::getFileListInDirectory concatenates directory + searchName with no
	// separator inserted, exactly like its Win32 twin. A configured asset root without a
	// trailing slash therefore searches for "...Zero Hour*.big", matches nothing, and the
	// engine reports "did not provide BIG files" before dying in INI loading - a failure that
	// names neither the separator nor the real cause. Cost real debugging time on Windows;
	// fixed on both arms so it cannot resurface on either.
	AsciiString dirWithSep = directory;
	const char lastCh = dirWithSep.getCharAt(dirWithSep.getLength() - 1);
	if (lastCh != '/' && lastCh != '\\') {
		dirWithSep.concat('/');
	}

	DEBUG_LOG(("StdBIGFileSystem::init - trying '%s' assets directory: %s", sourceTag, directory.str()));
	const Bool loaded = fileSystem->loadBigFilesFromDirectory(dirWithSep, "*.big", overwrite, FALSE);
	if (loaded) {
		DEBUG_LOG(("StdBIGFileSystem::init - loaded BIG files from %s (%s)", directory.str(), sourceTag));
	}

	return loaded;
}

static Bool tryResolveFromIni(const AsciiString& exeDirectory, const char* keyName, AsciiString& resolvedPath)
{
	static const char* iniCandidates[] = {
		"Options.ini"
	};

	for (UnsignedInt i = 0; i < ARRAY_SIZE(iniCandidates); ++i) {
		if (extractIniValue(iniCandidates[i], kAssetPathSection, keyName, resolvedPath)) {
			return TRUE;
		}

		if (exeDirectory.isNotEmpty()) {
			AsciiString iniPath = exeDirectory;
			iniPath.concat('/');
			iniPath.concat(iniCandidates[i]);
			if (extractIniValue(iniPath.str(), kAssetPathSection, keyName, resolvedPath)) {
				return TRUE;
			}
		}
	}

	return FALSE;
}

template <typename TBigFileSystem>
static Bool loadPrimaryGameAssets(TBigFileSystem* fileSystem, AsciiString* loadedDirectory)
{
	AsciiString exeDirectory;
	getExecutableDirectory(exeDirectory);

	// GeneralsX @feature GitHubCopilot 16/03/2026 Resolve primary asset directory by ENV > INI > default > current.
#if RTS_ZEROHOUR
	const char* primaryEnvName = kPrimaryAssetEnvZH;
	const char* primaryEnvValue = getenv(kPrimaryAssetEnvZH);
#else
	const char* primaryEnvName = kPrimaryAssetEnvGenerals;
	const char* primaryEnvValue = getenv(kPrimaryAssetEnvGenerals);
#endif
	AsciiString sanitizedPrimaryEnvPath;
	if (sanitizeConfiguredPath(primaryEnvValue, sanitizedPrimaryEnvPath)) {
		fprintf(stderr, "[ASSET_ROOT] Trying env %s='%s'\n", primaryEnvName, sanitizedPrimaryEnvPath.str());
		if (tryLoadBigFiles(fileSystem, sanitizedPrimaryEnvPath, "env")) {
			fprintf(stderr, "[ASSET_ROOT] Selected source=env path='%s'\n", sanitizedPrimaryEnvPath.str());
			if (loadedDirectory != nullptr) {
				*loadedDirectory = sanitizedPrimaryEnvPath;
			}
			return TRUE;
		}
		fprintf(stderr, "[ASSET_ROOT] Env path '%s' did not provide BIG files\n", sanitizedPrimaryEnvPath.str());
	}

	// Backward compatibility with previous env naming.
	const char* compatibilityEnvValue = getenv("GENERALSX_ASSET_PATH");
	AsciiString sanitizedCompatibilityEnvPath;
	if (sanitizeConfiguredPath(compatibilityEnvValue, sanitizedCompatibilityEnvPath)) {
		if (tryLoadBigFiles(fileSystem, sanitizedCompatibilityEnvPath, "env-compat")) {
			fprintf(stderr, "[ASSET_ROOT] Selected source=env-compat path='%s'\n", sanitizedCompatibilityEnvPath.str());
			if (loadedDirectory != nullptr) {
				*loadedDirectory = sanitizedCompatibilityEnvPath;
			}
			return TRUE;
		}
	}



#if RTS_ZEROHOUR
	const char* legacyEnvValue = getenv("CNC_ZH_INSTALLPATH");
#else
	const char* legacyEnvValue = getenv("CNC_GENERALS_INSTALLPATH");
#endif
	if (legacyEnvValue != nullptr && legacyEnvValue[0] != '\0') {
		if (tryLoadBigFiles(fileSystem, AsciiString(legacyEnvValue), "legacy-env")) {
			if (loadedDirectory != nullptr) {
				*loadedDirectory = legacyEnvValue;
			}
			return TRUE;
		}
	}

	AsciiString iniPath;
	if (tryResolveFromIni(exeDirectory, kPrimaryAssetIniKey, iniPath)) {
		fprintf(stderr, "[ASSET_ROOT] Trying ini [Paths]%s='%s'\n", kPrimaryAssetIniKey, iniPath.str());
		if (tryLoadBigFiles(fileSystem, iniPath, "ini")) {
			fprintf(stderr, "[ASSET_ROOT] Selected source=ini path='%s'\n", iniPath.str());
			if (loadedDirectory != nullptr) {
				*loadedDirectory = iniPath;
			}
			return TRUE;
		}
	}

	AsciiString defaultPath;
	if (GetStringFromRegistry("", "InstallPath", defaultPath)) {
		if (tryLoadBigFiles(fileSystem, defaultPath, "default-registry")) {
			fprintf(stderr, "[ASSET_ROOT] Selected source=default path='%s'\n", defaultPath.str());
			if (loadedDirectory != nullptr) {
				*loadedDirectory = defaultPath;
			}
			return TRUE;
		}
	}

#if RTS_ZEROHOUR
	if (exeDirectory.isNotEmpty()) {
		AsciiString resourcesPath = exeDirectory;
		resourcesPath.concat("/../Resources");
		if (tryLoadBigFiles(fileSystem, resourcesPath, "default-resources")) {
			// GeneralsX @bugfix Claude 09/09/2026 RECORD IT. This was the only success path that
			// returned TRUE without telling the caller where it loaded from, which was harmless
			// while nobody read the answer and fatal the moment the refusal below started checking
			// it: an unset directory reads as empty, empty failed the marker check, and the .app
			// bundle path - the one every shipped Mac build takes - exited with EX_CONFIG.
			if (loadedDirectory != nullptr) {
				*loadedDirectory = resourcesPath;
			}
			return TRUE;
		}
	}
#endif

	if (exeDirectory.isNotEmpty()) {
		if (tryLoadBigFiles(fileSystem, exeDirectory, "default-exedir")) {
			if (loadedDirectory != nullptr) {
				*loadedDirectory = exeDirectory;
			}
			return TRUE;
		}
	}

	DEBUG_LOG(("StdBIGFileSystem::init - trying current working directory as last-resort assets path"));
	if (fileSystem->loadBigFilesFromDirectory("", "*.big", FALSE, FALSE)) {
		fprintf(stderr, "[ASSET_ROOT] Selected source=current-working-directory\n");
		if (loadedDirectory != nullptr) {
			// GeneralsX @bugfix Claude 09/09/2026 "." rather than the empty string. The empty
			// string was correct as a SENTINEL meaning "cwd" while the value was only ever passed
			// back to loadBigFilesFromDirectory, which treats "" as cwd. It is not a path anything
			// can opendir, so the refusal below - which does - read a successful cwd load as no
			// data at all and killed the process. "." is the same directory and is scannable.
			*loadedDirectory = ".";
		}
		return TRUE;
	}

	return FALSE;
}

#if RTS_ZEROHOUR
// GeneralsX @bugfix wh1ter0se 12/08/2026 THE SIBLING GUESS ONLY EVER MATCHED ONE LAYOUT.
// See the twin of this block in Win32BIGFileSystem.cpp for the full measurement; in short, Zero
// Hour's world art lives in the BASE Generals archives, only "<ZH root>/ZH_Generals" (Steam) and a
// sibling literally named "Generals" were ever tried, and every other layout names that sibling
// "Command and Conquer Generals". Unmounted base art renders as MissingTexture magenta while the
// UI stays perfect, which is why it reads as a graphics fault rather than missing data.
//
// Gated on a marker rather than simply mounted: tryLoadBigFiles returns true for any .big it finds,
// so pointed at a second Zero Hour install it would mount the wrong game. A base Generals folder
// has INI.big and no INIZH.big; the expansion is the other way round.
static Bool fileExistsAt(const AsciiString& dir, const char* sep, const char* name)
{
	AsciiString path = dir;
	path.concat(sep);
	path.concat(name);
	FILE* f = fopen(path.str(), "rb");
	if (f != NULL) {
		fclose(f);
		return TRUE;
	}
	return FALSE;
}

static Bool looksLikeBaseGenerals(const AsciiString& dir, const char* sep)
{
	// Lower-case spellings matter here: this build targets case-sensitive filesystems.
	if (!fileExistsAt(dir, sep, "INI.big") && !fileExistsAt(dir, sep, "ini.big")) {
		return FALSE;
	}
	if (fileExistsAt(dir, sep, "INIZH.big") || fileExistsAt(dir, sep, "inizh.big")) {
		return FALSE;
	}
	return TRUE;
}

// GeneralsX @bugfix Claude 09/09/2026 "Some .big loaded" is not "this game's data is here".
//
// The fail-fast below used to gate on `loadedPrimaryAssets`, which `tryLoadBigFiles` sets when
// loadBigFilesFromDirectory reports ANY archive mounted. Point the Zero Hour build at a base
// Generals folder and INI.big mounts, the guard passes, and initialisation continues without the
// expansion data every later subsystem assumes - the exact case the guard's OWN message describes
// ("If you only have INI.big and no INIZH.big, that is the original Generals"). The message was
// right about the failure and the condition could not detect it.
//
// The markers are not new: looksLikeBaseGenerals directly above has tested for both spellings
// since 12/08, to RECOGNISE a base-Generals folder. It was never wired to the refusal.
//

// GeneralsX @bugfix wh1ter0se 12/08/2026 STOP GUESSING HOW THE FOLDER IS SPELLED.
//
// This is the file where it actually matters. The named-candidate list above compares the DIRECTORY
// name literally, and this build targets case-SENSITIVE filesystems - Linux, iOS, a case-sensitive
// APFS volume. A base game in "command and conquer generals" is missed there, and the user gets the
// magenta world plus a warning saying nothing is there while it sits one directory away. Windows
// and default macOS are case-insensitive, which is precisely why the A/Bs that validated the named
// list could not have surfaced this.
//
// Testing each entry for the MARKER rather than the NAME removes the guess: it also covers trailing
// spaces from a bad unzip, localised names, and whatever the next installer decides to call it.
//
// ASCII-only folding on purpose. AsciiString::compareNoCase and rts::less_than_nocase both bottom
// out in strcasecmp (string_compat.h:47 maps _stricmp onto it), whose folding follows LC_CTYPE -
// under a Turkish locale 'I' folds to a dotless i and "INI.big" stops matching itself. Archive
// names are ASCII by construction, so a fixed A-Z fold is correct and immune to the locale.
static Bool asciiEqualNoCase(const char* a, const char* b)
{
	for (; *a != '\0' && *b != '\0'; ++a, ++b) {
		char ca = *a;
		char cb = *b;
		if (ca >= 'A' && ca <= 'Z') {
			ca = (char)(ca - 'A' + 'a');
		}
		if (cb >= 'A' && cb <= 'Z') {
			cb = (char)(cb - 'A' + 'a');
		}
		if (ca != cb) {
			return FALSE;
		}
	}
	return *a == '\0' && *b == '\0';
}

// TRUE when dir is a directory holding base Generals: INI.big present, INIZH.big absent, in any
// casing. opendir returns NULL for a plain file, so this doubles as the is-a-directory test - which
// matters because the Windows shim provides no d_type.

static Bool dirHasBaseGeneralsMarkers(const AsciiString& dir)
{
	DIR* d = opendir(dir.str());
	if (d == NULL) {
		return FALSE;
	}
	Bool hasIni = FALSE;
	Bool hasIniZH = FALSE;
	struct dirent* entry;
	while ((entry = readdir(d)) != NULL) {
		if (asciiEqualNoCase(entry->d_name, "ini.big")) {
			hasIni = TRUE;
		} else if (asciiEqualNoCase(entry->d_name, "inizh.big")) {
			hasIniZH = TRUE;
		}
	}
	closedir(d);
	return hasIni && !hasIniZH;
}

// Walks the asset root's parent ONE level. Never recurses: the parent can be / or C:\, and the
// engine's own getFileListInDirectory recurses into every subdirectory (by bare name, each
// directory once - StdLocalFileSystem.cpp:334,381-384) and follows directory symlinks, which is
// why it is not used here.
static Bool findBaseGeneralsSibling(const AsciiString& zhDirNoSep, const char* sep, AsciiString& out)
{
	AsciiString parent = zhDirNoSep;
	parent.concat(sep);
	parent.concat("..");

	DIR* d = opendir(parent.str());
	if (d == NULL) {
		return FALSE;
	}
	struct dirent* entry;
	Bool found = FALSE;
	while (!found && (entry = readdir(d)) != NULL) {
		if (entry->d_name[0] == '\0' || strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}
		AsciiString candidate = parent;
		candidate.concat(sep);
		candidate.concat(entry->d_name);
		if (dirHasBaseGeneralsMarkers(candidate)) {
			out = candidate;
			found = TRUE;
		}
	}
	closedir(d);
	return found;
}

// GeneralsX @bugfix Claude 25/09/2026 Steam keeps base Generals in <game folder>/ZH_Generals. The
// game-folder scan used to reach it by recursing, so it mounted right after the top-level archives
// and BEFORE the overlay and every base-Generals resolver. Now that the scan is top-level only it is
// mounted here, by name, at that same position, so a Steam install resolves every contested path
// as it did (the resolvers below then find it already mounted). Matched case-insensitively because
// the old walk was: a case-sensitive volume holding "zh_generals" must not lose its world art.
//
// GeneralsX @bugfix Claude 25/09/2026 The lookup is shared with loadBaseGeneralsAssetsForZH's
// "default-zh-generals" candidate, and must be. Spelled "/ZH_Generals" there, a case-sensitive volume
// holding "zh_generals" had its world art mounted HERE while that candidate failed, so the chain went
// on to the sibling candidates - and a base install found beside the game folder was mounted as a
// SECOND set of the same archives, which moves assetID against a peer whose folder is spelled
// "ZH_Generals". Returns "" when there is no such folder.
static AsciiString findSteamBaseGeneralsDir(const AsciiString& zhAssetDirectory)
{
	AsciiString root = zhAssetDirectory.isEmpty() ? AsciiString(".") : zhAssetDirectory;
	while (root.getLength() > 1) {
		const char c = root.getCharAt(root.getLength() - 1);
		if (c != '/' && c != '\\')
			break;
		root.truncateBy(1);
	}
	AsciiString subdir;
	DIR* d = opendir(root.str());
	if (d == NULL) {
		return subdir;
	}
	struct dirent* entry;
	while ((entry = readdir(d)) != NULL) {
		if (asciiEqualNoCase(entry->d_name, "zh_generals")) {
			subdir = root;
			subdir.concat('/');
			subdir.concat(entry->d_name);
			break;
		}
	}
	closedir(d);
	return subdir;
}

template <typename TBigFileSystem>
static void mountSteamBaseGenerals(TBigFileSystem* fileSystem, const AsciiString& zhAssetDirectory)
{
	const AsciiString subdir = findSteamBaseGeneralsDir(zhAssetDirectory);
	if (subdir.isNotEmpty() && tryLoadBigFiles(fileSystem, subdir, "steam-zh-generals")) {
		fprintf(stderr, "[ASSET_ROOT] Steam base Generals mounted from '%s'\n", subdir.str());
	}
}

template <typename TBigFileSystem>
static void loadBaseGeneralsAssetsForZH(TBigFileSystem* fileSystem, const AsciiString& zhAssetDirectory)
{
	// GeneralsX @feature GitHubCopilot 16/03/2026 Resolve base Generals asset directory for ZH by ENV > INI > default.
	const char* baseEnvValue = getenv(kBaseGeneralsAssetEnv);
	if (baseEnvValue != nullptr && baseEnvValue[0] != '\0') {
		if (tryLoadBigFiles(fileSystem, AsciiString(baseEnvValue), "env-generals")) {
			return;
		}
	}

	const char* compatibilityBaseEnvValue = getenv("GENERALSX_GENERALS_ASSET_PATH");
	if (compatibilityBaseEnvValue != nullptr && compatibilityBaseEnvValue[0] != '\0') {
		if (tryLoadBigFiles(fileSystem, AsciiString(compatibilityBaseEnvValue), "env-generals-compat")) {
			return;
		}
	}

	const char* legacyGeneralsEnvValue = getenv("CNC_GENERALS_INSTALLPATH");
	if (legacyGeneralsEnvValue != nullptr && legacyGeneralsEnvValue[0] != '\0') {
		if (tryLoadBigFiles(fileSystem, AsciiString(legacyGeneralsEnvValue), "legacy-env-generals")) {
			return;
		}
	}

	AsciiString exeDirectory;
	getExecutableDirectory(exeDirectory);

	AsciiString iniPath;
	if (tryResolveFromIni(exeDirectory, kBaseGeneralsAssetIniKey, iniPath)) {
		if (tryLoadBigFiles(fileSystem, iniPath, "ini-generals")) {
			return;
		}
	}

	AsciiString installPath;
	GetStringFromGeneralsRegistry("", "InstallPath", installPath);
	if (tryLoadBigFiles(fileSystem, installPath, "default-registry-generals")) {
		return;
	}

	if (zhAssetDirectory.isNotEmpty()) {
		// GeneralsX @bugfix Claude 27/07/2026 Join on a separator-free base.
		//
		// zhAssetDirectory is whatever the asset-root resolver selected, and a configured
		// root very commonly ends with a separator ("...Zero Hour\\"). Concatenating
		// "/ZH_Generals" straight onto that yields "...Zero Hour\\/ZH_Generals" - which
		// opens fine, but is a DIFFERENT STRING from the "...Zero Hour\\ZH_Generals" that
		// the recursive scan of the ZH root already produced. m_archiveFileMap is keyed by
		// that string, so the same 15 base-Generals archives got mounted TWICE under two
		// spellings: 55 mounts for 40 files on disk. That moved the SimID assetID and made
		// an otherwise identical PC disagree with the Mac.
		//
		// Previously masked: without a trailing separator the search became
		// "...ZH_Generals*.big", matched nothing, and the whole path failed silently.
		AsciiString base = zhAssetDirectory;
		while (base.getLength() > 0) {
			const char c = base.getCharAt(base.getLength() - 1);
			if (c != '/' && c != '\\')
				break;
			base.truncateBy(1);
		}

		AsciiString siblingGenerals = base;
		siblingGenerals.concat("/../Generals");
		if (tryLoadBigFiles(fileSystem, siblingGenerals, "default-sibling-generals")) {
			return;
		}

		// Case-folded, and the SAME lookup mountSteamBaseGenerals used - see findSteamBaseGeneralsDir.
		const AsciiString steamGenerals = findSteamBaseGeneralsDir(zhAssetDirectory);
		if (tryLoadBigFiles(fileSystem, steamGenerals, "default-zh-generals")) {
			return;
		}

		static const char* const baseGeneralsSiblingNames[] = {
			"Command and Conquer Generals",
			"Command & Conquer Generals",
			"C&C Generals",
			"Generals",
		};
		for (size_t i = 0; i < sizeof(baseGeneralsSiblingNames) / sizeof(baseGeneralsSiblingNames[0]); ++i) {
			AsciiString candidate = base;
			candidate.concat("/../");
			candidate.concat(baseGeneralsSiblingNames[i]);
			if (!looksLikeBaseGenerals(candidate, "/")) {
				continue;
			}
			if (tryLoadBigFiles(fileSystem, candidate, "sibling-named-generals")) {
				return;
			}
		}

		// STRICTLY LAST, and that placement is the safety argument: every resolver above returns on
		// success, so an install that finds its base assets today never reaches this scan. It can
		// only change the outcome where everything else has already failed - the broken population.
		AsciiString scanned;
		if (findBaseGeneralsSibling(base, "/", scanned)) {
			if (tryLoadBigFiles(fileSystem, scanned, "sibling-scan-generals")) {
				fprintf(stderr, "[ASSET_ROOT] base Generals found by scanning for INI.big: '%s'\n",
					scanned.str());
				return;
			}
		}

		// GeneralsX @bugfix Claude 25/09/2026 A NOTE, NOT THE WARNING. Whether this install can play
		// the same game as everybody else is decided later and directly - can every bridge model load?
		// (TerrainVisual::isEveryBridgeModelAvailable) - because the base archives can also arrive
		// another way: in the Zero Hour folder itself, through a -mod, or split across two sources.
		// That check prints the "[ASSET_ROOT] WARNING" the cross-platform harnesses fail a peer on;
		// printing it here too would fail a merged install that is complete.
		fprintf(stderr, "[ASSET_ROOT] note: no base Generals folder found next to '%s'. Zero Hour "
			"needs Textures.big/W3D.big/Terrain.big from the BASE game; unless they are mounted some "
			"other way the world renders magenta and tree and bridge models fail to load. Looked for ZH_Generals/ "
			"inside it and for a sibling folder containing INI.big.\n", base.str());
	}
}
#endif

}

StdBIGFileSystem::StdBIGFileSystem() : ArchiveFileSystem() {
}

StdBIGFileSystem::~StdBIGFileSystem() {
}

void StdBIGFileSystem::init() {
	DEBUG_ASSERTCRASH(TheLocalFileSystem != nullptr, ("TheLocalFileSystem must be initialized before TheArchiveFileSystem."));
	if (TheLocalFileSystem == nullptr) {
		return;
	}

	// GeneralsX @feature Claude 08/08/2026 Resolve the overlay BEFORE anything is mounted.
	// loadBigFilesFromDirectory has to know about it: a recursive scan (a -mod folder, and the
	// game folder until 25/09/2026) would otherwise mount an overlay living inside it twice -
	// once at path-sort precedence by that scan and once at filename precedence below.
	if (resolvePatchOverlayDirectory(m_patchOverlayDirectory)) {
		fprintf(stderr, "[PATCH_OVERLAY] Overlay directory '%s'\n", m_patchOverlayDirectory.str());
	}

	AsciiString primaryAssetsDirectory;
	const Bool loadedPrimaryAssets = loadPrimaryGameAssets(this, &primaryAssetsDirectory);
	// GeneralsX @bugfix Claude 27/07/2026 Only ever read by the DEBUG_ASSERTCRASH below,
	// which compiles to nothing in release - so the variable is unused there and MSVC's
	// warnings-as-errors rejects it (C4189). Keep the call, silence the release warning.
	(void)loadedPrimaryAssets;
	DEBUG_ASSERTCRASH(loadedPrimaryAssets, ("No BIG files were loaded for the primary game assets."));

	// GeneralsX @bugfix Claude 26/08/2026 SAY IT HERE, WHERE IT IS KNOWN, INSTEAD OF DYING LATER.
	//
	// DEBUG_ASSERTCRASH compiles to nothing in release and the result is discarded on the line
	// above, so a shipped build that found no game data said nothing and carried on. Measured on a
	// real first install (owner, fresh MacBook, 26/08/2026): the engine logged "did not provide BIG
	// files" at this point, ran 12,700 MORE lines of subsystem init, and finally stopped at
	//
	//     [INI] ERROR: No files read from directory 'Data\english\CommandMap'
	//
	// which names a directory the player has never heard of and never mentions the actual problem.
	// There is no crash report, because it is not crashing - it exits. To the player that is
	// indistinguishable from the game being broken, and it is the single thing most likely to end
	// a first install.
	//
	// Nothing downstream can recover from this. Every subsystem after this point reads from
	// archives that are not mounted, so continuing only chooses which later symptom appears.
	// Stopping here costs nothing that was going to work anyway.
	//
	// EX_CONFIG (78) rather than 1: it is specifically "the configuration is wrong", which is what
	// this is, and it gives the wrapper a code to recognise. run.sh turns it into a dialog, because
	// a player who double-clicked an .app never sees stderr.
	// GeneralsX @bugfix Claude 09/09/2026 Also refuse when the WRONG game's data is mounted.
	//
	// `loadedPrimaryAssets` is TRUE when any archive mounted at all. Point the Zero Hour build at a
	// base Generals folder and INI.big mounts, this guard passes, and initialisation runs on data
	// that has no Zero Hour content - which is precisely the case the message below already
	// describes. The wording was right; the condition could not see it.
	//
	// Deliberately NOT folded into loadPrimaryGameAssets: that function's job is "did anything
	// mount", it is called from several places, and narrowing it would change behaviour for callers
	// that legitimately accept a partial mount. The variant requirement belongs at the refusal.
	// GeneralsX @bugfix Claude 09/09/2026 THREE outcomes, and unreadable FAILS CLOSED.
	//
	// An earlier version of this returned TRUE when opendir failed, reasoning that a permissions
	// problem should not be reported as missing game files. That was the wrong half of a real
	// trade-off: it meant "we could not look" silently satisfied the check, which is finding 8
	// again through the unreadable-directory path. Refusing is correct - the player cannot play
	// either way - but it gets its OWN message, because "we cannot read this folder" and "these
	// are the other game's files" send someone to completely different places.
	//
	// MEASURED 09/09/2026 against a real build, and the result is not what this branch's name
	// suggests: pointing the engine at a chmod-000 directory does NOT reach here. Finding
	// archives requires LISTING the directory, so an unreadable one already makes
	// loadedPrimaryAssets false and the generic "cannot find" message below fires first. Exit 78
	// either way, but by the other route.
	//
	// So this is reached only when archives loaded AND the recorded directory cannot be scanned -
	// today that means an EMPTY recorded path, which is precisely the regression this commit
	// series introduced and fixed: two success paths returned TRUE without saying where they had
	// loaded from. It is deliberate defence in depth against the next one. If a third fallback is
	// ever added and forgets to record its directory, this refuses loudly and names the problem
	// instead of silently accepting whatever happened to mount.
	const GXAssetMarker::Result markerResult = variantMarkerResult(primaryAssetsDirectory);
	if (loadedPrimaryAssets && markerResult == GXAssetMarker::Unreadable) {
		fprintf(stderr,
			"\n"
			"================================================================\n"
			"GeneralsX loaded game files but cannot read the folder they are\n"
			"in, so it cannot check they are the right ones.\n"
			"\n"
			"    %s\n"
			"\n"
			"This is usually a permissions problem rather than a missing game.\n"
			"Check that your user account can open that folder.\n"
			"\n"
			"Full instructions: https://wh1ter0se.dev/#download\n"
			"================================================================\n"
			"\n",
			primaryAssetsDirectory.isEmpty() ? "(the launcher did not record which folder)"
			                                 : primaryAssetsDirectory.str());
		fflush(stderr);
		exit(78);   /* EX_CONFIG */
	}
	const Bool haveVariantData = (markerResult == GXAssetMarker::Found);
	if (!loadedPrimaryAssets || !haveVariantData) {
		// Distinguish the two, because they send the player somewhere different: nothing mounted
		// means "we could not find your files", while a mount without the marker means "these are
		// the other game's files". Telling someone who HAS a folder to go and find one is how a
		// correct diagnosis still wastes an afternoon.
		if (loadedPrimaryAssets && !haveVariantData) {
			fprintf(stderr,
				"\n"
				"================================================================\n"
#if RTS_ZEROHOUR
				"GeneralsX found game files, but they are not Zero Hour.\n"
				"\n"
				"This folder has .big archives but no INIZH.big:\n"
				"\n"
				"    %s\n"
				"\n"
				"That is the original Command & Conquer: Generals. Zero Hour is\n"
				"the expansion, and it installs to its own folder - look for one\n"
				"whose name ends in \"Zero Hour\".\n"
#else
				"GeneralsX found game files, but they are not Generals.\n"
				"\n"
				"This folder has .big archives but no INI.big:\n"
				"\n"
				"    %s\n"
				"\n"
				"Point GeneralsX at your Command & Conquer: Generals install.\n"
#endif
				"\n"
				"Full instructions: https://wh1ter0se.dev/#download\n"
				"================================================================\n"
				"\n",
				primaryAssetsDirectory.str());
			fflush(stderr);
			exit(78);   /* EX_CONFIG */
		}
		fprintf(stderr,
			"\n"
			"================================================================\n"
			"GeneralsX cannot find your Command & Conquer: Generals Zero Hour\n"
			"game files.\n"
			"\n"
			"GeneralsX is the engine. It does not include the game's data, so\n"
			"it needs a copy of Zero Hour that you already own.\n"
			"\n"
			"Put your Zero Hour files here:\n"
			"\n"
			"    ~/GeneralsX/GeneralsZH\n"
			"\n"
			"That folder should contain about two dozen .big files, including\n"
			"INIZH.big. If you only have INI.big and no INIZH.big, that is the\n"
			"original Generals rather than the Zero Hour expansion.\n"
			"\n"
			"Full instructions: https://wh1ter0se.dev/#download\n"
			"================================================================\n"
			"\n");
		fflush(stderr);
		exit(78);   /* EX_CONFIG */
	}

	// GeneralsX @bugfix felipebraz 23/03/2026 Propagate the resolved asset root to the local file system.
	// On Linux/macOS the binary cwd and the game data directory (asset root) are separate. Loose files like
	// Data\Scripts\SkirmishScripts.scb must be resolvable from the asset root, not just from cwd.
	if (primaryAssetsDirectory.isNotEmpty()) {
		TheLocalFileSystem->setAssetRootPath(primaryAssetsDirectory);
	}

#if RTS_ZEROHOUR
	mountSteamBaseGenerals(this, primaryAssetsDirectory);
#endif

	loadPatchOverlay(primaryAssetsDirectory);

#if RTS_ZEROHOUR
	loadBaseGeneralsAssetsForZH(this, primaryAssetsDirectory);
#endif

	// GeneralsX @bugfix Claude 23/09/2026 One neutral fact for a "wrong menu" report: the ARCHIVE the
	// default shell-map path resolves to at this point. Not reflected: loose-file overrides (the
	// local file system is asked first), a ShellMapName/-shellMap override, and -mod (loaded later).
	// Precedence is deliberately left alone: dataID, a join-refusal field, depends on which archive
	// won each contested path.
	ArchiveFile *shellMapArchive = getArchiveFile(AsciiString("Maps\\ShellMap1\\ShellMap1.map"));
	fprintf(stderr, "[ARCHPATH] shell map archive fallback (default Maps\\ShellMap1\\ShellMap1.map, before -mod): %s\n",
		shellMapArchive != nullptr ? shellMapArchive->getName().str() : "(none)");
}

// GeneralsX @feature Claude 08/08/2026 Load an extra .big from a GeneralsX-owned directory.
//
// WHY: two peers may only join each other when their dataID matches, and dataID covers the
// archive contents. Handing a friend our community patch therefore meant writing a .big into
// their Zero Hour install. This mounts it from anywhere instead - GENERALSX_PATCH_PATH, else
// <exe dir>/GeneralsXPatch - and produces the SAME dataID as the file sitting in the game
// folder, which is the only thing that makes the join legal.
//
// WHY NOT THE OBVIOUS TWO CALLS. loadIntoDirectoryTree offers exactly two positions in the
// per-filename multimap range, and resolution takes instance 0, so:
//   * overwrite=FALSE appends -> the overlay loses EVERY contested file, including the ones
//     it exists to replace. Measured: Data\INI\CommandSet.ini then resolves to INIZH.big's
//     144715-byte vanilla copy while Data\INI\CommandSet\*.ini comes from the patch, and the
//     resulting half-and-half definition set throws out of INI::load.
//   * overwrite=TRUE prepends -> the overlay wins every contested file, including ones it
//     must NOT win. The Community Patch ships Data\INI\CommandButton.ini at ZERO BYTES on
//     purpose (it blanks the monolithic file and splits it into Data\INI\CommandButton\*.ini),
//     and in the game folder that empty entry legitimately LOSES to the 352561-byte copy in
//     !HotkeysLeikezeIndicatorsZH.big, which sorts first. Forced to the front it wins, the
//     SimID loose scan hashes 0 bytes for that path, and dataID moves FA018128 -> 9F59E16C.
//
// Neither is a bug in loadIntoDirectoryTree. The real rule is the one the game folder already
// implements and neither flag can say: archives resolve in FILENAME order, first match wins.
// That is what the community's numeric filename prefixes ("340_", "500_900_") are FOR. So the
// overlay is inserted at the position its own filename earns among the archives already
// mounted, which reproduces the in-folder layout byte for byte and is independent of where
// the overlay actually lives.
void StdBIGFileSystem::loadPatchOverlay(const AsciiString& primaryAssetsDirectory)
{
	if (m_patchOverlayDirectory.isEmpty()) {
		return;
	}

	// Never overlay the game folder onto itself: loadPrimaryGameAssets already mounted every
	// archive in it, and a second pass would double every entry in the directory tree.
	// primaryAssetsDirectory is empty when the last-resort "current working directory"
	// candidate won, so resolve that spelling too rather than letting it slip past.
	AsciiString primaryResolved;
	if (resolveExistingPath(primaryAssetsDirectory.isNotEmpty() ? primaryAssetsDirectory : AsciiString("."), primaryResolved)
		&& comparePathNoCase(primaryResolved.str(), m_patchOverlayDirectory.str()) == 0) {
		fprintf(stderr, "[PATCH_OVERLAY] Skipped: overlay is the primary asset root ('%s')\n",
			m_patchOverlayDirectory.str());
		return;
	}

	AsciiString overlayWithSep = m_patchOverlayDirectory;
	overlayWithSep.concat('/');

	FilenameList filenameList;
	TheLocalFileSystem->getFileListInDirectory(overlayWithSep, "", "*.big", filenameList, TRUE);

	if (filenameList.empty()) {
		fprintf(stderr, "[PATCH_OVERLAY] No .big files under '%s'\n", m_patchOverlayDirectory.str());
		return;
	}

	FilenameListIter it = filenameList.begin();
	while (it != filenameList.end()) {
		if (m_archiveFileMap.find(*it) != m_archiveFileMap.end()) {
			it++;
			continue;
		}

		ArchiveFile *archiveFile = openArchiveFile((*it).str());
		if (archiveFile != nullptr) {
			const AsciiString priorityName = baseNameOfPath(*it);
			loadIntoDirectoryTreeAtPriority(archiveFile, priorityName);
			m_archiveFileMap[(*it)] = archiveFile;
			fprintf(stderr, "[PATCH_OVERLAY] Mounted '%s' at filename precedence '%s'\n",
				(*it).str(), priorityName.str());
		}

		it++;
	}
	fflush(stderr);
}

// GeneralsX @feature Claude 08/08/2026 Filename-ordered insertion into the shared tree.
//
// The directory walk is a verbatim copy of ArchiveFileSystem::loadIntoDirectoryTree, and it
// has to be: that file is inside the simulation source digest, so extending it would move
// sourceID and force a lockstep-atomic rebuild of all four peers for a change that only ever
// touches asset mounting. The ONLY difference is the insertion position - instead of the
// front (overwrite=TRUE) or the back (FALSE), we walk the existing range and stop at the
// first archive whose filename sorts after ours, which is exactly where the game folder's
// own path-sorted mount order would have put us.
void StdBIGFileSystem::loadIntoDirectoryTreeAtPriority(ArchiveFile *archiveFile, const AsciiString& priorityName)
{
	FilenameList filenameList;
	archiveFile->getFileListInDirectory("", "", "*", filenameList, TRUE);

	FilenameListIter it = filenameList.begin();
	while (it != filenameList.end())
	{
		ArchivedDirectoryInfo *dirInfo = &m_rootDirectory;

		AsciiString path;
		AsciiString token;
		AsciiString tokenizer = *it;
		tokenizer.toLower();
		tokenizer.nextToken(&token, "\\/");

		// GeneralsX @bugfix Claude 01/10/2026 TheSuperHackers #3374 (3f3ed9fc8): walk directories until the path is used up;
		// the dot-guessing condition misfiled dotless file names as directories (tsh batch2 #53).
		while (!tokenizer.isEmpty())
		{
			path.concat(token);
			path.concat('\\');

			ArchivedDirectoryInfoMap::iterator tempiter = dirInfo->m_directories.find(token);
			if (tempiter == dirInfo->m_directories.end())
			{
				dirInfo = &(dirInfo->m_directories[token]);
				dirInfo->m_path = path;
				dirInfo->m_directoryName = token;
			}
			else
			{
				dirInfo = &tempiter->second;
			}

			tokenizer.nextToken(&token, "\\/");
		}

		// <= rather than <: on an exact filename tie the copy that was already mounted keeps
		// the higher precedence, matching "first one loaded wins" everywhere else.
		std::pair<ArchivedFileLocationMap::iterator, ArchivedFileLocationMap::iterator> range =
			dirInfo->m_files.equal_range(token);
		ArchivedFileLocationMap::iterator pos = range.first;
		while (pos != range.second
			&& comparePathNoCase(baseNameOfPath(pos->second->getName()).str(), priorityName.str()) <= 0)
		{
			++pos;
		}

		// multimap::insert(hint, value) places the element immediately before hint, so an
		// empty range (hint == the key's sorted insertion point) is handled by the same call.
		dirInfo->m_files.insert(pos, std::make_pair(token, archiveFile));

		it++;
	}
}

void StdBIGFileSystem::reset() {
}

void StdBIGFileSystem::update() {
}

void StdBIGFileSystem::postProcessLoad() {
}

ArchiveFile * StdBIGFileSystem::openArchiveFile(const Char *filename) {
	File *fp = TheLocalFileSystem->openFile(filename, File::READ | File::BINARY);
	AsciiString archiveFileName;
	archiveFileName = filename;
	archiveFileName.toLower();
	UnsignedInt archiveFileSize = 0;
	UnsignedInt numLittleFiles = 0;
	BIGFileHeader header;

	DEBUG_LOG(("StdBIGFileSystem::openArchiveFile - opening BIG file %s", filename));

	if (fp == nullptr) {
		DEBUG_CRASH(("Could not open archive file %s for parsing", filename));
		return nullptr;
	}

	AsciiString asciibuf;
	char buffer[_MAX_PATH];
	if (fp->read(&header, sizeof(header)) != sizeof(header) || memcmp(header.identifier, BIGFileIdentifier, sizeof(BIGFileIdentifier)) != 0) {
		DEBUG_CRASH(("Error reading BIG file header in file %s", filename));
		// GeneralsX @bugfix Claude 28/10/2026 Said in release builds too, where DEBUG_CRASH is empty: a refused archive must
		// not read as a missing one (Codex tsh1028-3588-sec-r1 P3).
		fprintf(stderr, "[BIGFS] refused %s: not a BIG archive, or its 16-byte header is cut short\n", filename);
		fp->close();
		fp = nullptr;
		return nullptr;
	}

	archiveFileSize = letoh(header.archiveFileSize);
	numLittleFiles = betoh(header.numFiles);

	DEBUG_LOG(("StdBIGFileSystem::openArchiveFile - size of archive file is %u bytes", archiveFileSize));

//	char t;

	DEBUG_LOG(("StdBIGFileSystem::openArchiveFile - %u are contained in archive", numLittleFiles));
//	for (Int i = 0; i < 2; ++i) {
//		t = buffer[i];
//		buffer[i] = buffer[(4-i)-1];
//		buffer[(4-i)-1] = t;
//	}

	// read in each directory listing. The header read above leaves the file at 0x10, where the directory starts.
	ArchivedFileInfo fileInfo;
	// GeneralsX @bugfix Claude 14/09/2026 Allocate the StdBIGFile only after the header is
	// validated. It used to be created before the fp == nullptr and "BIGF" identifier checks,
	// both of which return early - leaking the StdBIGFile every time a .big is missing or not
	// actually a BIG archive. The Win32 twin already got this treatment (Mauller 23/04/2025);
	// this is the Std/OpenAL-side half. Ports upstream PR #3196 (2a5efe289).
	ArchiveFile *archiveFile = NEW StdBIGFile(filename, AsciiString::TheEmptyString);

	Bool corrupt = FALSE;
	UnsignedInt skipped = 0;
	UnsignedInt i = 0;
	for (; i < numLittleFiles; ++i) {
		BIGFileEntryHeader entry;
		if (fp->read(&entry, sizeof(entry)) != sizeof(entry)) {
			corrupt = TRUE;
			break;
		}

		fileInfo.m_archiveFilename = archiveFileName;
		fileInfo.m_offset = betoh(entry.fileOffset);
		fileInfo.m_size = betoh(entry.fileSize);

		// read in the path name of the file. GeneralsX @bugfix Claude 23/09/2026 had already bounded this loop
		// (a name longer than the buffer, or a truncated archive, ran it past the stack buffer); #3588's helper replaces it.
		const Int pathIndex = readNullTerminatedString(fp, buffer, sizeof(buffer));
		if (pathIndex < 0) {
			corrupt = TRUE;
			break;
		}

		// GeneralsX @bugfix Claude 28/10/2026 Fork hardening, not part of upstream #3588: an entry whose offset or size
		// does not fit the Int that RAMFile and StreamingArchiveFile take them as (either >= 2^31, or offset + size past
		// INT_MAX) is left out of the directory. Kept, a size of 0xFFFFFFFF reached RAMFile::openFromArchive as -1 (new[]
		// of a negative length) and StreamingArchiveFile as -1, whose read clamp then handed fread SIZE_MAX, and a sum
		// past INT_MAX overflowed their Int arithmetic (Codex tsh1028-3588-sec-r1 P1). No archive the game can read has
		// such an entry (retail reads the archive size as Int: 2 GB at most), so a well-formed directory is unchanged,
		// and the archive's other entries still mount. The name is not echoed: it is the archive's bytes, not text.
		if (fileInfo.m_offset > (UnsignedInt)INT_MAX || fileInfo.m_size > (UnsignedInt)INT_MAX - fileInfo.m_offset) {
			if (skipped < 3) {
				fprintf(stderr, "[BIGFS] %s: entry %u of %u (offset %u, size %u) does not fit an Int; left out\n",
					filename, i + 1, numLittleFiles, fileInfo.m_offset, fileInfo.m_size);
			}
			++skipped;
			continue;
		}

		Int filenameIndex = pathIndex;
		while ((filenameIndex >= 0) && (buffer[filenameIndex] != '\\') && (buffer[filenameIndex] != '/')) {
			--filenameIndex;
		}

		fileInfo.m_filename = (char *)(buffer + filenameIndex + 1);
		fileInfo.m_filename.toLower();
		buffer[filenameIndex + 1] = 0;

		AsciiString path;
		path = buffer;

		AsciiString debugpath;
		debugpath = path;
		debugpath.concat(fileInfo.m_filename);
//		DEBUG_LOG(("StdBIGFileSystem::openArchiveFile - adding file %s to archive file %s, file number %u", debugpath.str(), fileInfo.m_archiveFilename.str(), i));

		archiveFile->addFile(path, &fileInfo);
	}

	if (corrupt) {
		DEBUG_CRASH(("Truncated or corrupt directory in BIG file %s", filename));
		fprintf(stderr, "[BIGFS] refused %s: its directory is truncated or corrupt at entry %u of %u\n", filename, i + 1, numLittleFiles);
		delete archiveFile;
		fp->close();
		fp = nullptr;
		return nullptr;
	}

	if (skipped > 3) {
		fprintf(stderr, "[BIGFS] %s: %u entries left out in all\n", filename, skipped);
	}

	archiveFile->attachFile(fp);

	// leave fp open as the archive file will be using it.

	return archiveFile;
}

void StdBIGFileSystem::closeArchiveFile(const Char *filename) {
	// Need to close the specified big file
	ArchiveFileMap::iterator it =  m_archiveFileMap.find(filename);
	if (it == m_archiveFileMap.end()) {
		return;
	}

	if (stricmp(filename, MUSIC_BIG) == 0) {
		// Stop the current audio
		TheAudio->stopAudio(AudioAffect_Music);

		// No need to turn off other audio, as the lookups will just fail.
	}
	DEBUG_ASSERTCRASH(stricmp(filename, MUSIC_BIG) == 0, ("Attempting to close Archive file '%s', need to add code to handle its shutdown correctly.", filename));

	// may need to do some other processing here first.

	delete (it->second);
	m_archiveFileMap.erase(it);
}

void StdBIGFileSystem::closeAllArchiveFiles() {
}

void StdBIGFileSystem::closeAllFiles() {
}

// GeneralsX @bugfix Claude 23/09/2026 The single-file -mod half of the guard below: loadMods opens
// that archive itself and never reaches loadBigFilesFromDirectory. Same rule, same helper.
// GeneralsX @bugfix Claude 06/10/2026 See ArchiveFileSystem::isModArchive.
Bool StdBIGFileSystem::isModArchive(const Char *archiveName) const {
	return TheGlobalData != nullptr && GXArchiveMount::isModArchive(archiveName, TheGlobalData->m_modBIG.str(), TheGlobalData->m_modDir.str());
}

Bool StdBIGFileSystem::promoteIfAlreadyMounted(const Char *filename) {
	std::vector<std::string> mountedKeys;
	mountedKeys.reserve(m_archiveFileMap.size());
	for (ArchiveFileMap::const_iterator mapIt = m_archiveFileMap.begin(); mapIt != m_archiveFileMap.end(); ++mapIt) {
		mountedKeys.push_back(mapIt->first.str() != nullptr ? mapIt->first.str() : "");
	}

	bool loaded = false;
	return GXArchiveMount::handleAlreadyMounted(mountedKeys, filename, true, loaded,
		[this](const std::string& key) {
			ArchiveFileMap::iterator existing = m_archiveFileMap.find(AsciiString(key.c_str()));
			if (existing != m_archiveFileMap.end()) {
				loadIntoDirectoryTree(existing->second, TRUE);
			}
		});
}

Bool StdBIGFileSystem::loadBigFilesFromDirectory(AsciiString dir, AsciiString fileMask, Bool overwrite) {
	return loadBigFilesFromDirectory(dir, fileMask, overwrite, TRUE);
}

// GeneralsX @bugfix Claude 25/09/2026 THE GAME FOLDER IS SCANNED AT THE TOP LEVEL ONLY.
//
// Owner's decision (#10, 24/09/2026): GeneralsX loads only its own files. Retail walked every
// subfolder of the game folder, so another launcher's or mod's nested archives mounted too - a
// GeneralsOnline `launcher\MapsZH.big` served its own ShellMap1.map on Windows, because
// "...\launcher\MapsZH.big" sorts before "...\MapsZH.big" and the first archive mounted keeps a
// contested path. Here the walk is by bare name relative to the cwd (StdLocalFileSystem.cpp), and
// measured on a Steam layout (scripts/test/archives-top-level-test.sh) launcher/MapsZH.big mounted
// and served Maps\ShellMap1\ShellMap1.map, which retail keeps only in ZH_Generals/maps.big.
// Every game-folder, base-Generals and cwd scan in this file now passes FALSE; the one retail
// subfolder that holds real data, Steam's ZH_Generals, is mounted by name (mountSteamBaseGenerals).
// A -mod folder keeps retail's recursion through the three-argument virtual above.
Bool StdBIGFileSystem::loadBigFilesFromDirectory(AsciiString dir, AsciiString fileMask, Bool overwrite, Bool searchSubdirectories) {

	FilenameList filenameList;
	TheLocalFileSystem->getFileListInDirectory(dir, "", fileMask, filenameList, searchSubdirectories);

	// GeneralsX @feature Claude 08/08/2026 Leave the overlay's archives to loadPatchOverlay.
	//
	// This scan is recursive, so an overlay directory placed INSIDE the game folder is walked
	// by it too. Mounting an archive here and again in loadPatchOverlay would put it in the
	// directory tree twice under two different keys, double its basename in the assetID
	// fingerprint, and leak the first ArchiveFile. Only ever active when an overlay directory
	// actually exists, and never when the directory being scanned IS the overlay - otherwise
	// pointing GENERALSX_PATCH_PATH at the game folder would filter out every archive in it.
	Bool skipOverlayFiles = FALSE;
	if (m_patchOverlayDirectory.isNotEmpty()) {
		AsciiString scanResolved;
		skipOverlayFiles = !resolveExistingPath(dir.isEmpty() ? AsciiString(".") : dir, scanResolved)
			|| comparePathNoCase(scanResolved.str(), m_patchOverlayDirectory.str()) != 0;
	}

	// GeneralsX @bugfix Claude 23/09/2026 NEVER MOUNT THE SAME ARCHIVE TWICE - see
	// StdDevice/Common/GXArchiveMount.h for the full account.
	//
	// m_archiveFileMap is keyed by the path STRING, and this function is the choke point every
	// mount goes through: loadPrimaryGameAssets, every route in loadBaseGeneralsAssetsForZH, and
	// ArchiveFileSystem::loadMods all arrive here. One archive reached by two spellings was
	// therefore mounted twice - loadIntoDirectoryTree run twice, a duplicate location left in the
	// directory tree for every path inside it, the first ArchiveFile LEAKED because operator[]
	// overwrites without deleting, and the archive counted twice in assetID.
	//
	// Measured shape it comes from: a Zero Hour root whose base-Generals folder is also reachable
	// as a differently-spelled sibling, whose `[ARCHIVES]` line counted far more mounts than there were archives
	// with 18 of them listed twice. The 27/07/2026 fix at :805-828 covers exactly one join
	// ("/../Generals" with a trailing separator); this covers the class.
	//
	// loadPatchOverlay has always had this guard (:1113) - as an exact string match, which is the
	// half that works only when the two spellings happen to be identical.
	//
	// ponytail: O(n*m) realpath calls, n archives against m already mounted. n is the archive
	// count - 65 on the worst install seen - and this runs once at startup. Keep a resolved-path
	// set on the file system if an install ever carries hundreds of archives.
	std::vector<std::string> mountedKeys;
	mountedKeys.reserve(m_archiveFileMap.size() + filenameList.size());
	for (ArchiveFileMap::const_iterator mapIt = m_archiveFileMap.begin(); mapIt != m_archiveFileMap.end(); ++mapIt) {
		mountedKeys.push_back(mapIt->first.str() != nullptr ? mapIt->first.str() : "");
	}

	Bool actuallyAdded = FALSE;
	FilenameListIter it = filenameList.begin();
	while (it != filenameList.end()) {
		if (skipOverlayFiles) {
			AsciiString fileResolved;
			if (resolveExistingPath(*it, fileResolved)
				&& isPathUnderDirectory(fileResolved, m_patchOverlayDirectory)) {
				it++;
				continue;
			}
		}

#if RTS_ZEROHOUR
		// TheSuperHackers @bugfix bobtista 18/11/2025 Skip duplicate INIZH.big in Data\INI to prevent CRC mismatches.
		// English, Chinese, and Korean SKUs shipped with two INIZH.big files (one in Run directory, one in Run\Data\INI).
		// The DeleteFile cleanup doesn't work on EA App/Origin installs because the folder is not writable, so we skip loading it instead.
		if (it->endsWithNoCase("Data\\INI\\INIZH.big") || it->endsWithNoCase("Data/INI/INIZH.big")) {
			it++;
			continue;
		}
#endif

		// Already mounted under another spelling: not reopened, still counts as loaded, and a -mod
		// load moves THAT archive to the front. Rules and reasons: GXArchiveMount::handleAlreadyMounted.
		if (GXArchiveMount::handleAlreadyMounted(mountedKeys, (*it).str(), overwrite, actuallyAdded,
				[this](const std::string& key) {
					ArchiveFileMap::iterator existing = m_archiveFileMap.find(AsciiString(key.c_str()));
					if (existing != m_archiveFileMap.end()) {
						loadIntoDirectoryTree(existing->second, TRUE);
					}
				})) {
			it++;
			continue;
		}

		ArchiveFile *archiveFile = openArchiveFile((*it).str());

		if (archiveFile != nullptr) {
			DEBUG_LOG(("StdBIGFileSystem::loadBigFilesFromDirectory - loading %s into the directory tree.", (*it).str()));
			loadIntoDirectoryTree(archiveFile, overwrite);
			m_archiveFileMap[(*it)] = archiveFile;
			mountedKeys.push_back((*it).str() != nullptr ? (*it).str() : "");
			DEBUG_LOG(("StdBIGFileSystem::loadBigFilesFromDirectory - %s inserted into the archive file map.", (*it).str()));
			actuallyAdded = TRUE;
		}

		it++;
	}

	return actuallyAdded;
}
