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

//////// Win32BIGFileSystem.h ///////////////////////////
// Bryan Cleveland, August 2002
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

#include "Win32Device/Common/Win32BIGFile.h"
#include "Win32Device/Common/Win32BIGFileSystem.h"
// GeneralsX: Shared with the StdDevice twin on purpose: both file
// systems key m_archiveFileMap by the path string and both need the same by-file test. The
// header lives under StdDevice/Common alongside StdAssetMarker.h because that is where this
// fork's dependency-free, directly-testable helpers already live; Core/GameEngineDevice/Include
// is one include root, so the path resolves identically from here.
#include "StdDevice/Common/GXArchiveMount.h"
#include "Utility/endian_compat.h"

#include <climits>
#include <cstdlib>
#include <cstdio>
#include <cstring>

// GeneralsX @bugfix wh1ter0se 12/08/2026 Directory walk for the base-Generals sibling scan below.
// Same arrangement GameEngine.cpp:63 already uses for the save-directory walk: the shim is entirely
// inside #ifdef _WIN32 and reimplements opendir/readdir/closedir on _findfirst/_findnext, which the
// MSVC CRT has always had. Reusing it beats adding a raw <dirent.h> that would be a hard C1083 on
// Windows. NOTE: the shim provides d_name ONLY - there is no d_type, so nothing below may use it.
#ifdef _WIN32
#include "Utility/dirent_compat.h"
#else
#include <dirent.h>
#endif

#if !defined(_WIN32)
// strcasecmp/strncasecmp. Unconditional on every non-Windows target rather than gated on
// _UNIX: this file is in the unconditional GameEngineDevice source list, so it is compiled
// on macOS and iOS too, and the gate happened to work only because <strings.h> arrived
// transitively.
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


// GeneralsX: Reject malformed BIG archives. Ports upstream PR #3588 (c7b7814e9) by hand:
// upstream's loader is Core/GameEngine/Source/Common/System/BIGFileSystem.cpp, which the fork does not build - the same
// directory reader lives here and in its twin (StdBIGFileSystem.cpp / Win32BIGFileSystem.cpp), so the two stay mirrored.
// The header and every directory entry are read whole and the read is checked; the counts are unsigned (a count with the
// top bit set would read as negative and mount an EMPTY archive); a short header, a directory that runs past the end of
// the file or a path with no terminator inside the buffer rejects the whole archive. In an anonymous namespace because
// Windows compiles both twins into one library.
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

// GeneralsX: Community-patch overlay directory.
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

// GeneralsX: Community-patch overlay directory - path helpers.

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

// _fullpath()/realpath() collapse "..", "." and mixed separators, so two spellings of one
// directory compare equal - which is what the "is the overlay the asset root?" and "did
// the recursive game-folder scan already reach this file?" tests both need. On Windows that
// canonicalisation also matters for the separator itself: the game-folder scan produces
// backslash paths while a configured GENERALSX_PATCH_PATH may well use forward slashes.
// Failure is also the existence test: an overlay directory that is not there resolves to
// nothing and every code path below it is skipped.
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

	// The one place this arm has to do more than its realpath() twin. Every directory that
	// reaches here from loadBigFilesFromDirectory carries a trailing separator, because
	// tryLoadBigFiles appends one so FindFirstFile gets "...\Zero Hour\*.big" - and _fullpath
	// keeps it where realpath() strips it. Both callers then compare the result against
	// m_patchOverlayDirectory, which has none, so without this a GENERALSX_PATCH_PATH pointing
	// at the game folder would fail the "is this scan the overlay?" test and filter out every
	// archive in it. Normalise to realpath()'s shape. A bare drive root ("C:\") keeps its
	// separator, because there the separator IS the directory.
	size_t resolvedLen = strlen(buffer);
	while (resolvedLen > 1
		&& (buffer[resolvedLen - 1] == '\\' || buffer[resolvedLen - 1] == '/')
		&& !(resolvedLen == 3 && buffer[1] == ':')) {
		buffer[--resolvedLen] = '\0';
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

// Strict containment on a separator boundary, so "C:\games\ZeroHourExtra" is never treated
// as living inside "C:\games\ZeroHour".
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
	candidate.concat('\\');
	candidate.concat(kPatchOverlayDefaultSubdir);
	return resolveExistingPath(candidate, resolvedOverlayDirectory);
}

template <typename TBigFileSystem>
static Bool tryLoadBigFiles(TBigFileSystem* fileSystem, const AsciiString& directory, const char* sourceTag, Bool overwrite = FALSE)
{
	if (directory.isEmpty()) {
		return FALSE;
	}

	// GeneralsX: Guarantee the trailing separator.
	//
	// Win32LocalFileSystem::getFileListInDirectory builds its search string as
	// originalDirectory + currentDirectory + searchName with NO separator inserted
	// (Win32LocalFileSystem.cpp:131-137). A configured asset root without a trailing
	// backslash therefore produces the mask "...Zero Hour*.big", FindFirstFile matches
	// nothing, and the engine reports "did not provide BIG files" and later dies loading
	// INI. Nothing in that chain mentions a separator, so it reads as missing game data.
	// Costing a user hours over one absent character is not an acceptable failure mode.
	AsciiString dirWithSep = directory;
	const char lastCh = dirWithSep.getCharAt(dirWithSep.getLength() - 1);
	if (lastCh != '\\' && lastCh != '/') {
		dirWithSep.concat('\\');
	}

	DEBUG_LOG(("Win32BIGFileSystem::init - trying '%s' assets directory: %s", sourceTag, directory.str()));
	const Bool loaded = fileSystem->loadBigFilesFromDirectory(dirWithSep, "*.big", overwrite, FALSE);
	if (loaded) {
		DEBUG_LOG(("Win32BIGFileSystem::init - loaded BIG files from %s (%s)", directory.str(), sourceTag));
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

	// GeneralsX: Resolve primary asset directory by ENV > INI > default > current.
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

	DEBUG_LOG(("Win32BIGFileSystem::init - trying current working directory as last-resort assets path"));
	if (fileSystem->loadBigFilesFromDirectory("", "*.big", FALSE, FALSE)) {
		fprintf(stderr, "[ASSET_ROOT] Selected source=current-working-directory\n");
		if (loadedDirectory != nullptr) {
			*loadedDirectory = AsciiString::TheEmptyString;
		}
		return TRUE;
	}

	return FALSE;
}

#if RTS_ZEROHOUR
// GeneralsX @bugfix wh1ter0se 12/08/2026 THE SIBLING GUESS ONLY EVER MATCHED ONE LAYOUT.
// Zero Hour takes its WORLD art from the base Generals archives; if they are not mounted the scene renders magenta
// while the UI looks fine. Candidates are gated on a marker (base Generals has INI.big and no INIZH.big) because
// tryLoadBigFiles mounts any .big, including a second Zero Hour install's.
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
	// Lower-case spellings are tried too: the archives ship upper-case, but case matters on Linux
	// and this same helper compiles into the Std build.
	if (!fileExistsAt(dir, sep, "INI.big") && !fileExistsAt(dir, sep, "ini.big")) {
		return FALSE;
	}
	if (fileExistsAt(dir, sep, "INIZH.big") || fileExistsAt(dir, sep, "inizh.big")) {
		return FALSE;
	}
	return TRUE;
}

// GeneralsX @bugfix wh1ter0se 12/08/2026 STOP GUESSING HOW THE FOLDER IS SPELLED.
// Enumerating the parent and testing each entry for the MARKER, not the NAME, also covers case-sensitive volumes,
// trailing spaces and localised names. ASCII-only fold on purpose: strcasecmp follows LC_CTYPE, so under a Turkish
// locale 'I' folds to a dotless i and INI.big stops matching itself.
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

// Walks the asset root's parent ONE level. Never recurses: the parent can be C:\ or /, and the
// engine's own getFileListInDirectory recurses without bound, which is why it is not used here.
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

// GeneralsX: Steam keeps base Generals in <game folder>\ZH_Generals, which
// the old recursive game-folder scan mounted right after the top-level archives and BEFORE the
// overlay and every base-Generals resolver. Mounted here, by name, at that same position, so a Steam
// install resolves every contested path as it did (the resolvers below find it already mounted).
// No case-insensitive lookup needed: Windows paths are case-insensitive. Absent folder = no-op.
template <typename TBigFileSystem>
static void mountSteamBaseGenerals(TBigFileSystem* fileSystem, const AsciiString& zhAssetDirectory)
{
	AsciiString subdir = zhAssetDirectory;
	if (subdir.isNotEmpty()) {
		const char c = subdir.getCharAt(subdir.getLength() - 1);
		if (c != '\\' && c != '/') {
			subdir.concat('\\');
		}
	}
	subdir.concat("ZH_Generals");
	if (tryLoadBigFiles(fileSystem, subdir, "steam-zh-generals")) {
		fprintf(stderr, "[ASSET_ROOT] Steam base Generals mounted from '%s'\n", subdir.str());
	}
}

template <typename TBigFileSystem>
static void loadBaseGeneralsAssetsForZH(TBigFileSystem* fileSystem, const AsciiString& zhAssetDirectory)
{
	// GeneralsX: Resolve base Generals asset directory for ZH by ENV > INI > default.
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
		// GeneralsX: Join on a separator-free base. A configured root often ends with a separator, and appending
		// "/ZH_Generals" then spells the path differently from the recursive scan's; m_archiveFileMap is keyed by that
		// string, so the same archives mount twice and assetID moves.
		AsciiString base = zhAssetDirectory;
		while (base.getLength() > 0) {
			const char c = base.getCharAt(base.getLength() - 1);
			if (c != '/' && c != '\\')
				break;
			base.truncateBy(1);
		}

		AsciiString siblingGenerals = base;
		siblingGenerals.concat("\\..\\Generals");
		if (tryLoadBigFiles(fileSystem, siblingGenerals, "default-sibling-generals")) {
			return;
		}

		AsciiString steamGenerals = base;
		steamGenerals.concat("\\ZH_Generals");
		if (tryLoadBigFiles(fileSystem, steamGenerals, "default-zh-generals")) {
			return;
		}

		// The names every non-Steam layout actually uses. Ordered most-specific first; "Generals"
		// is repeated from the unguarded attempt above deliberately, so that if this list ever
		// becomes the only path it still covers that case.
		static const char* const baseGeneralsSiblingNames[] = {
			"Command and Conquer Generals",
			"Command & Conquer Generals",
			"C&C Generals",
			"Generals",
		};
		for (size_t i = 0; i < sizeof(baseGeneralsSiblingNames) / sizeof(baseGeneralsSiblingNames[0]); ++i) {
			AsciiString candidate = base;
			candidate.concat("\\..\\");
			candidate.concat(baseGeneralsSiblingNames[i]);
			if (!looksLikeBaseGenerals(candidate, "\\")) {
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
		if (findBaseGeneralsSibling(base, "\\", scanned)) {
			if (tryLoadBigFiles(fileSystem, scanned, "sibling-scan-generals")) {
				fprintf(stderr, "[ASSET_ROOT] base Generals found by scanning for INI.big: '%s'\n",
					scanned.str());
				return;
			}
		}

		// GeneralsX: A NOTE, NOT THE WARNING. Whether this install can play
		// the same game as everybody else is decided later and directly - can every bridge model load?
		// (TerrainVisual::isEveryBridgeModelAvailable) - because the base archives can also arrive
		// another way: in the Zero Hour folder itself, through a -mod, or split across two sources.
		// That check prints the "[ASSET_ROOT] WARNING" the cross-platform harnesses fail a peer on;
		// printing it here too would fail a merged install that is complete.
		fprintf(stderr, "[ASSET_ROOT] note: no base Generals folder found next to '%s'. Zero Hour "
			"needs Textures.big/W3D.big/Terrain.big from the BASE game; unless they are mounted some "
			"other way the world renders magenta and tree and bridge models fail to load. Looked for ZH_Generals\\ "
			"inside it and for a sibling folder containing INI.big.\n", base.str());
	}
}
#endif

}

Win32BIGFileSystem::Win32BIGFileSystem() : ArchiveFileSystem() {
}

Win32BIGFileSystem::~Win32BIGFileSystem() {
}

void Win32BIGFileSystem::init() {
	DEBUG_ASSERTCRASH(TheLocalFileSystem != nullptr, ("TheLocalFileSystem must be initialized before TheArchiveFileSystem."));
	if (TheLocalFileSystem == nullptr) {
		return;
	}

	// GeneralsX: Resolve the overlay BEFORE anything is mounted.
	// loadBigFilesFromDirectory has to know about it: a recursive scan (a -mod folder) would otherwise mount an overlay
	// living inside it twice - once at path-sort precedence by that scan and once at filename precedence below.
	if (resolvePatchOverlayDirectory(m_patchOverlayDirectory)) {
		fprintf(stderr, "[PATCH_OVERLAY] Overlay directory '%s'\n", m_patchOverlayDirectory.str());
	}

	AsciiString primaryAssetsDirectory;
	const Bool loadedPrimaryAssets = loadPrimaryGameAssets(this, &primaryAssetsDirectory);
	// GeneralsX: Only ever read by the DEBUG_ASSERTCRASH below,
	// which compiles to nothing in release - so the variable is unused there and MSVC's
	// warnings-as-errors rejects it (C4189). Keep the call, silence the release warning.
	(void)loadedPrimaryAssets;
	DEBUG_ASSERTCRASH(loadedPrimaryAssets, ("No BIG files were loaded for the primary game assets."));

	// GeneralsX: Propagate the resolved asset root to the local file
	// system, mirroring StdBIGFileSystem. Windows already resolved CNC_GENERALS_ZH_PATH here and
	// then discarded it, so every loose data file was cwd-relative only. That is not merely a
	// convenience: it is the root cause of the AI-only frame-0 desync (Data\Scripts\*.scb missing
	// from the run folder), the cursor bug, and built-in maps absent from the map cache.
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

	// GeneralsX: One neutral fact for a "wrong menu" report: the ARCHIVE the
	// default shell-map path resolves to at this point. Not reflected: loose-file overrides (the
	// local file system is asked first), a ShellMapName/-shellMap override, and -mod (loaded later).
	// Precedence is deliberately left alone: dataID, a join-refusal field, depends on which archive
	// won each contested path.
	ArchiveFile *shellMapArchive = getArchiveFile(AsciiString("Maps\\ShellMap1\\ShellMap1.map"));
	fprintf(stderr, "[ARCHPATH] shell map archive fallback (default Maps\\ShellMap1\\ShellMap1.map, before -mod): %s\n",
		shellMapArchive != nullptr ? shellMapArchive->getName().str() : "(none)");
}

// GeneralsX: Mirror of StdBIGFileSystem::loadPatchOverlay (keep in step; only separators differ). Loads an extra .big
// from GENERALSX_PATCH_PATH, else <exe dir>\GeneralsXPatch, with the SAME dataID as the game-folder file.
// Neither overwrite flag works: FALSE loses every contested file, TRUE wins ones it must not (empty CommandButton.ini).
// Archives resolve in FILENAME order, first match wins; the overlay is inserted where its filename sorts.
void Win32BIGFileSystem::loadPatchOverlay(const AsciiString& primaryAssetsDirectory)
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
	overlayWithSep.concat('\\');

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

// GeneralsX: Filename-ordered insertion into the shared tree.
//
// The directory walk is a verbatim copy of ArchiveFileSystem::loadIntoDirectoryTree, and it
// has to be: that file is inside the simulation source digest, so extending it would move
// sourceID and force a lockstep-atomic rebuild of all four peers for a change that only ever
// touches asset mounting. The ONLY difference is the insertion position - instead of the
// front (overwrite=TRUE) or the back (FALSE), we walk the existing range and stop at the
// first archive whose filename sorts after ours, which is exactly where the game folder's
// own path-sorted mount order would have put us.
void Win32BIGFileSystem::loadIntoDirectoryTreeAtPriority(ArchiveFile *archiveFile, const AsciiString& priorityName)
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

		// GeneralsX: TheSuperHackers #3374 (3f3ed9fc8): walk directories until the path is used up;
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

void Win32BIGFileSystem::reset() {
}

void Win32BIGFileSystem::update() {
}

void Win32BIGFileSystem::postProcessLoad() {
}

ArchiveFile * Win32BIGFileSystem::openArchiveFile(const Char *filename) {
	File *fp = TheLocalFileSystem->openFile(filename, File::READ | File::BINARY);
	AsciiString archiveFileName;
	archiveFileName = filename;
	archiveFileName.toLower();
	UnsignedInt archiveFileSize = 0;
	UnsignedInt numLittleFiles = 0;
	BIGFileHeader header;

	DEBUG_LOG(("Win32BIGFileSystem::openArchiveFile - opening BIG file %s", filename));

	if (fp == nullptr) {
		DEBUG_CRASH(("Could not open archive file %s for parsing", filename));
		return nullptr;
	}

	AsciiString asciibuf;
	char buffer[_MAX_PATH];
	if (fp->read(&header, sizeof(header)) != sizeof(header) || memcmp(header.identifier, BIGFileIdentifier, sizeof(BIGFileIdentifier)) != 0) {
		DEBUG_CRASH(("Error reading BIG file header in file %s", filename));
		// GeneralsX: Said in release builds too, where DEBUG_CRASH is empty: a refused archive must
		// not read as a missing one.
		fprintf(stderr, "[BIGFS] refused %s: not a BIG archive, or its 16-byte header is cut short\n", filename);
		fp->close();
		fp = nullptr;
		return nullptr;
	}

	archiveFileSize = letoh(header.archiveFileSize);
	numLittleFiles = betoh(header.numFiles);

	DEBUG_LOG(("Win32BIGFileSystem::openArchiveFile - size of archive file is %u bytes", archiveFileSize));

//	char t;

	DEBUG_LOG(("Win32BIGFileSystem::openArchiveFile - %u are contained in archive", numLittleFiles));
//	for (Int i = 0; i < 2; ++i) {
//		t = buffer[i];
//		buffer[i] = buffer[(4-i)-1];
//		buffer[(4-i)-1] = t;
//	}

	// read in each directory listing. The header read above leaves the file at 0x10, where the directory starts.
	ArchivedFileInfo fileInfo;
	// TheSuperHackers @fix Mauller 23/04/2025 Create new file handle when necessary to prevent memory leak
	ArchiveFile *archiveFile = NEW Win32BIGFile(filename, AsciiString::TheEmptyString);

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

		// read in the path name of the file. GeneralsX: had already bounded this loop
		// (a name longer than the buffer, or a truncated archive, ran it past the stack buffer); #3588's helper replaces it.
		const Int pathIndex = readNullTerminatedString(fp, buffer, sizeof(buffer));
		if (pathIndex < 0) {
			corrupt = TRUE;
			break;
		}

		// GeneralsX: Fork hardening, not part of upstream #3588: an entry whose offset or size
		// does not fit the Int that RAMFile and StreamingArchiveFile take them as (either >= 2^31, or offset + size past
		// INT_MAX) is left out of the directory. Kept, a size of 0xFFFFFFFF reached RAMFile::openFromArchive as -1 (new[]
		// of a negative length) and StreamingArchiveFile as -1, whose read clamp then handed fread SIZE_MAX, and a sum
		// past INT_MAX overflowed their Int arithmetic. No archive the game can read has
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
//		DEBUG_LOG(("Win32BIGFileSystem::openArchiveFile - adding file %s to archive file %s, file number %u", debugpath.str(), fileInfo.m_archiveFilename.str(), i));

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

void Win32BIGFileSystem::closeArchiveFile(const Char *filename) {
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

void Win32BIGFileSystem::closeAllArchiveFiles() {
}

void Win32BIGFileSystem::closeAllFiles() {
}

// GeneralsX: The single-file -mod half of the guard below: loadMods opens
// that archive itself and never reaches loadBigFilesFromDirectory. Same rule, same helper.
// GeneralsX: See ArchiveFileSystem::isModArchive.
Bool Win32BIGFileSystem::isModArchive(const Char *archiveName) const {
	return TheGlobalData != nullptr && GXArchiveMount::isModArchive(archiveName, TheGlobalData->m_modBIG.str(), TheGlobalData->m_modDir.str());
}

Bool Win32BIGFileSystem::promoteIfAlreadyMounted(const Char *filename) {
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

Bool Win32BIGFileSystem::loadBigFilesFromDirectory(AsciiString dir, AsciiString fileMask, Bool overwrite) {
	return loadBigFilesFromDirectory(dir, fileMask, overwrite, TRUE);
}

// GeneralsX: THE GAME FOLDER IS SCANNED AT THE TOP LEVEL ONLY - twin of the
// same change in StdBIGFileSystem.cpp. GeneralsX loads only its
// own files. Retail walked every subfolder, so a GeneralsOnline `launcher\MapsZH.big` mounted and,
// because "...\launcher\MapsZH.big" sorts before "...\MapsZH.big" and the first archive mounted
// keeps a contested path, served its own ShellMap1.map. Every game-folder, base-Generals and cwd
// scan in this file passes FALSE; Steam's ZH_Generals is mounted by name (mountSteamBaseGenerals).
// A -mod folder keeps retail's recursion through the three-argument virtual above.
Bool Win32BIGFileSystem::loadBigFilesFromDirectory(AsciiString dir, AsciiString fileMask, Bool overwrite, Bool searchSubdirectories) {

	FilenameList filenameList;
	TheLocalFileSystem->getFileListInDirectory(dir, "", fileMask, filenameList, searchSubdirectories);

	// GeneralsX: Leave the overlay's archives to loadPatchOverlay.
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

	// GeneralsX: NEVER MOUNT THE SAME ARCHIVE TWICE - twin of the guard
	// in StdBIGFileSystem.cpp, full account in StdDevice/Common/GXArchiveMount.h.
	//
	// m_archiveFileMap is keyed by the path STRING and this function is the choke point every
	// mount goes through, so one archive reached by two spellings mounted twice: the directory
	// tree carried a duplicate location for every path inside it, the first ArchiveFile leaked
	// because operator[] overwrites without deleting, and assetID counted the archive twice.
	//
	// Trade-off: O(n*m) _fullpath calls, once at startup, n = the archive count. Keep a resolved
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
			DEBUG_LOG(("Win32BIGFileSystem::loadBigFilesFromDirectory - loading %s into the directory tree.", (*it).str()));
			loadIntoDirectoryTree(archiveFile, overwrite);
			m_archiveFileMap[(*it)] = archiveFile;
			mountedKeys.push_back((*it).str() != nullptr ? (*it).str() : "");
			DEBUG_LOG(("Win32BIGFileSystem::loadBigFilesFromDirectory - %s inserted into the archive file map.", (*it).str()));
			actuallyAdded = TRUE;
		}

		it++;
	}

	return actuallyAdded;
}
