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

#include "registryini.h"

#include <ctype.h>
#include <errno.h>
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>	// MoveFileExA, SaveRegistryIni's replace
#else
#include <unistd.h>
#endif

namespace {
	typedef std::map<std::string, std::string> RegistryIniSection;
	typedef std::map<std::string, RegistryIniSection> RegistryIniData;

	const char *kCurrentUserRoot = "HKEY_CURRENT_USER";
	const char *kLocalMachineRoot = "HKEY_LOCAL_MACHINE";
	const char *kDefaultValueKey = "@";

	std::string TrimCopy(const std::string &value)
	{
		std::string::size_type start = 0;
		while (start < value.length() && isspace(static_cast<unsigned char>(value[start]))) {
			++start;
		}

		std::string::size_type end = value.length();
		while (end > start && isspace(static_cast<unsigned char>(value[end - 1]))) {
			--end;
		}

		return value.substr(start, end - start);
	}

	std::string ToLowerAscii(std::string value)
	{
		for (std::string::size_type i = 0; i < value.length(); ++i) {
			value[i] = static_cast<char>(tolower(static_cast<unsigned char>(value[i])));
		}

		return value;
	}

	bool StartsWith(const std::string &value, const char *prefix)
	{
		const std::string::size_type prefixLength = strlen(prefix);
		return value.length() >= prefixLength && value.compare(0, prefixLength, prefix) == 0;
	}

	bool EnsureDirectoryRecursive(const std::string &directory);

	std::string GetRegistryIniDirectory()
	{
	#ifdef _WIN32
		const char *appData = getenv("APPDATA");
		if (appData != nullptr && appData[0] != '\0') {
			return std::string(appData) + "\\GeneralsX";
		}

		const char *userProfile = getenv("USERPROFILE");
		if (userProfile != nullptr && userProfile[0] != '\0') {
			return std::string(userProfile) + "\\AppData\\Roaming\\GeneralsX";
		}
	#else
		const char *home = getenv("HOME");
		#ifdef __APPLE__
			if (home != nullptr && home[0] != '\0') {
				return std::string(home) + "/Library/Application Support/GeneralsX";
			}
		#else
			const char *xdgConfigHome = getenv("XDG_CONFIG_HOME");
			if (xdgConfigHome != nullptr && xdgConfigHome[0] != '\0') {
				return std::string(xdgConfigHome) + "/GeneralsX";
			}

			if (home != nullptr && home[0] != '\0') {
				return std::string(home) + "/.config/GeneralsX";
			}
		#endif
	#endif

		return "GeneralsX";
	}

	std::string GetRegistryIniPath()
	{
	#ifdef _WIN32
		return GetRegistryIniDirectory() + "\\registry.ini";
	#else
		return GetRegistryIniDirectory() + "/registry.ini";
	#endif
	}

	bool EnsureRegistryStorageExists()
	{
		const std::string directory = GetRegistryIniDirectory();
		if (!EnsureDirectoryRecursive(directory)) {
			return false;
		}

	#ifndef _WIN32
		// A registry.ini that is a link to nothing (its volume not mounted) is not a
		// file to create: the "a" below would make an empty shadow behind the link, the read would then succeed on it,
		// and the save would replace the link. Nothing is read or written then.
		struct stat linkInfo;
		struct stat targetInfo;
		if (lstat(GetRegistryIniPath().c_str(), &linkInfo) == 0 && stat(GetRegistryIniPath().c_str(), &targetInfo) != 0) {
			return false;
		}
	#endif

		FILE *file = fopen(GetRegistryIniPath().c_str(), "a");
		if (file == nullptr) {
			return false;
		}

		fclose(file);
		return true;
	}

	int MakeDirectory(const char *path)
	{
	#ifdef _WIN32
		return _mkdir(path);
	#else
		return mkdir(path, 0755);
	#endif
	}

	bool EnsureDirectoryRecursive(const std::string &directory)
	{
		if (directory.empty()) {
			return false;
		}

		std::string current;
		for (std::string::size_type i = 0; i < directory.length(); ++i) {
			const char currentChar = directory[i];
			current += currentChar;

			if (currentChar != '/' && currentChar != '\\') {
				continue;
			}

			if (current.length() <= 1) {
				continue;
			}

			if (current.length() == 3 && current[1] == ':') {
				continue;
			}

			if (MakeDirectory(current.c_str()) != 0 && errno != EEXIST) {
				return false;
			}
		}

		if (MakeDirectory(directory.c_str()) != 0 && errno != EEXIST) {
			return false;
		}

		return true;
	}

	std::string NormalizeRoot(const char *root)
	{
		const std::string lowered = ToLowerAscii(TrimCopy(root == nullptr ? "" : root));
		if (lowered == "hkey_local_machine" || lowered == "hklm") {
			return ToLowerAscii(kLocalMachineRoot);
		}

		if (lowered == "hkey_current_user" || lowered == "hkcu" || lowered.empty()) {
			return ToLowerAscii(kCurrentUserRoot);
		}

		return lowered;
	}

	void StripKnownRootPrefix(std::string &path, std::string &root)
	{
		const std::string lowered = ToLowerAscii(path);
		if (StartsWith(lowered, "hkey_local_machine\\")) {
			root = ToLowerAscii(kLocalMachineRoot);
			path.erase(0, strlen("HKEY_LOCAL_MACHINE\\"));
		} else if (StartsWith(lowered, "hklm\\")) {
			root = ToLowerAscii(kLocalMachineRoot);
			path.erase(0, strlen("HKLM\\"));
		} else if (StartsWith(lowered, "hkey_current_user\\")) {
			root = ToLowerAscii(kCurrentUserRoot);
			path.erase(0, strlen("HKEY_CURRENT_USER\\"));
		} else if (StartsWith(lowered, "hkcu\\")) {
			root = ToLowerAscii(kCurrentUserRoot);
			path.erase(0, strlen("HKCU\\"));
		}
	}

	std::string NormalizeRegistryPath(const char *path)
	{
		std::string normalized = TrimCopy(path == nullptr ? "" : path);
		for (std::string::size_type i = 0; i < normalized.length(); ++i) {
			if (normalized[i] == '/') {
				normalized[i] = '\\';
			}
		}

		normalized = ToLowerAscii(normalized);
		while (!normalized.empty() && normalized[0] == '\\') {
			normalized.erase(0, 1);
		}
		while (!normalized.empty() && normalized[normalized.length() - 1] == '\\') {
			normalized.erase(normalized.length() - 1);
		}

		std::vector<std::string> parts;
		std::string current;
		for (std::string::size_type i = 0; i < normalized.length(); ++i) {
			if (normalized[i] == '\\') {
				if (!current.empty()) {
					if (current != "wow6432node") {
						parts.push_back(current);
					}
					current.clear();
				}
				continue;
			}

			current += normalized[i];
		}

		if (!current.empty() && current != "wow6432node") {
			parts.push_back(current);
		}

		 // GeneralsX:
		 // Apply known product name aliases to improve compatibility with mods
		 // and variants that write/read multiple product registry paths.
		 if (!parts.empty()) {
			 static const struct Alias { const char *from; const char *to; } aliases[] = {
				 {"zero hour", "command and conquer generals zero hour"},
				 {"generals zero hour", "command and conquer generals zero hour"},
				 {"command & conquer generals zero hour", "command and conquer generals zero hour"},
				 {"command and conquer generals", "command and conquer generals"},
				 {"command and conquer generals zh", "command and conquer generals zero hour"},
				 {"cnc_generals_zh", "command and conquer generals zero hour"},
			 };

			 for (std::size_t i = 0; i < parts.size(); ++i) {
				 // Try to match multi-part aliases (up to 6 parts) by joining parts with spaces
				 for (std::size_t aliasTry = 0; aliasTry < 6 && i + aliasTry < parts.size(); ++aliasTry) {
					 std::string combined = parts[i];
					 for (std::size_t k = 1; k <= aliasTry; ++k) {
						 combined += " ";
						 combined += parts[i + k];
					 }

					 for (std::size_t a = 0; a < sizeof(aliases)/sizeof(aliases[0]); ++a) {
						 if (combined == aliases[a].from) {
							 // replace sequence [i .. i+aliasTry] with single canonical part
							 parts[i] = std::string(aliases[a].to);
							 // erase the extra parts
							 if (aliasTry > 0) {
								 parts.erase(parts.begin() + i + 1, parts.begin() + i + 1 + aliasTry);
							 }
							 break;
						 }
					 }
					 // if we matched, stop attempting longer joins at this index
					 if (parts[i] != combined) {
						 break;
					 }
				 }
			 }
		 }

		 normalized.clear();
		 for (std::vector<std::string>::const_iterator it = parts.begin(); it != parts.end(); ++it) {
			 if (!normalized.empty()) {
				 normalized += "\\";
			 }
			 normalized += *it;
		 }

		return normalized;
	}

	std::string NormalizeSectionName(const char *root, const char *path)
	{
		std::string normalizedRoot = NormalizeRoot(root);
		std::string rawPath = TrimCopy(path == nullptr ? "" : path);
		StripKnownRootPrefix(rawPath, normalizedRoot);
		const std::string normalizedPath = NormalizeRegistryPath(rawPath.c_str());

		if (normalizedPath.empty()) {
			return normalizedRoot;
		}

		return normalizedRoot + "\\" + normalizedPath;
	}

	std::string NormalizeStoredSectionName(const std::string &section)
	{
		return NormalizeSectionName(nullptr, section.c_str());
	}

	std::string BuildSectionName(const char *root, const char *path)
	{
		return NormalizeSectionName(root, path);
	}

	std::string BuildKeyName(const char *key)
	{
		const std::string normalized = ToLowerAscii(TrimCopy(key == nullptr ? "" : key));
		if (normalized.empty() || normalized == "@" || normalized == "@default" || normalized == "(default)" || normalized == "default") {
			return kDefaultValueKey;
		}

		return normalized;
	}

	// `unreadable`, when asked for, is set when registry.ini exists and could not be
	// read: any fopen failure but "not there", or a read error part way. The writers below refuse then - they used to
	// ignore this function's result, so an unreadable registry.ini loaded as an empty registry and the next write
	// replaced the file with that one value (the file-wipe class, sweep). What is loaded and returned is
	// unchanged.
	bool LoadRegistryIni(RegistryIniData &data, bool *unreadable = nullptr)
	{
		if (unreadable != nullptr) {
			*unreadable = false;
		}
		if (!EnsureRegistryStorageExists()) {
			// Nothing was read, so a writer must not go on as if the registry were empty: a one-off failure here
			// followed by a save that succeeds replaced the file with one value.
			if (unreadable != nullptr) {
				*unreadable = true;
			}
			return false;
		}

		errno = 0;
		FILE *file = fopen(GetRegistryIniPath().c_str(), "r");
		if (file == nullptr) {
			if (unreadable != nullptr) {
				*unreadable = errno != ENOENT;
			}
			return false;
		}

		char line[4096];
		std::string currentSection;
		bool firstLine = true;

		while (fgets(line, sizeof(line), file) != nullptr) {
			std::string currentLine = line;
			if (firstLine && currentLine.length() >= 3 &&
				static_cast<unsigned char>(currentLine[0]) == 0xEF &&
				static_cast<unsigned char>(currentLine[1]) == 0xBB &&
				static_cast<unsigned char>(currentLine[2]) == 0xBF) {
				currentLine.erase(0, 3);
			}
			firstLine = false;

			while (!currentLine.empty() && (currentLine[currentLine.length() - 1] == '\n' || currentLine[currentLine.length() - 1] == '\r')) {
				currentLine.erase(currentLine.length() - 1);
			}

			const std::string trimmedLine = TrimCopy(currentLine);
			if (trimmedLine.empty() || trimmedLine[0] == ';' || trimmedLine[0] == '#') {
				continue;
			}

			if (trimmedLine[0] == '[' && trimmedLine[trimmedLine.length() - 1] == ']') {
				currentSection = NormalizeStoredSectionName(trimmedLine.substr(1, trimmedLine.length() - 2));
				continue;
			}

			const std::string::size_type separator = currentLine.find('=');
			if (separator == std::string::npos || currentSection.empty()) {
				continue;
			}

			const std::string key = BuildKeyName(currentLine.substr(0, separator).c_str());
			const std::string value = TrimCopy(currentLine.substr(separator + 1));
			data[currentSection][key] = value;
		}

		if (unreadable != nullptr && ferror(file)) {
			*unreadable = true;
		}
		fclose(file);
		return true;
	}

	// The writers' load: false, after one line in the log, when registry.ini could not be read and must not be replaced.
	bool LoadRegistryIniToRewrite(RegistryIniData &data)
	{
		bool unreadable = false;
		LoadRegistryIni(data, &unreadable);
		if (unreadable) {
			static bool s_noted = false;
			if (!s_noted) {
				s_noted = true;
				fprintf(stderr, "[GXREG] %s could not be read: it is left exactly as it is, and registry writes are "
					"refused while it cannot be read.\n", GetRegistryIniPath().c_str());
			}
			return false;
		}
		return true;
	}

	bool SaveRegistryIni(const RegistryIniData &data)
	{
		if (!EnsureRegistryStorageExists()) {
			return false;
		}

		// Written beside the file and swapped in, never truncated in place: a crash or
		// a full disk between the fopen("w") and the last line used to leave an empty or half registry.
		// A fresh temp made exclusively ("x"): a link left at the temp's name is never followed, so nothing else is
		// truncated through it, and two writers never share one.
		const std::string path = GetRegistryIniPath();
	#ifdef _WIN32
		const int pid = _getpid();
	#else
		const int pid = (int)getpid();
	#endif
		std::string tmp;
		FILE *file = nullptr;
		for (int n = 0; n < 16 && file == nullptr; ++n) {
			char suffix[48];
			snprintf(suffix, sizeof(suffix), ".%d.%d.tmp", pid, n);
			tmp = path + suffix;
			errno = 0;
			file = fopen(tmp.c_str(), "wx");
			if (file == nullptr && errno != EEXIST) {
				return false;
			}
		}
		if (file == nullptr) {
			return false;
		}

		for (RegistryIniData::const_iterator sectionIt = data.begin(); sectionIt != data.end(); ++sectionIt) {
			fprintf(file, "[%s]\n", sectionIt->first.c_str());
			for (RegistryIniSection::const_iterator valueIt = sectionIt->second.begin(); valueIt != sectionIt->second.end(); ++valueIt) {
				fprintf(file, "%s=%s\n", valueIt->first == kDefaultValueKey ? "@" : valueIt->first.c_str(), valueIt->second.c_str());
			}
			fprintf(file, "\n");
		}

		bool ok = fflush(file) == 0 && !ferror(file);
	#ifdef _WIN32
		ok = ok && _commit(_fileno(file)) == 0;
	#else
		ok = ok && fsync(fileno(file)) == 0;
	#endif
		ok = (fclose(file) == 0) && ok;
	#ifdef _WIN32
		// MSVCRT's rename() refuses an existing destination (no Windows caller today: the engine's registry is the
		// real one there).
		ok = ok && MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
	#else
		ok = ok && rename(tmp.c_str(), path.c_str()) == 0;
	#endif
		if (!ok) {
			remove(tmp.c_str());
		}
		return ok;
	}
}

namespace RegistryIni {
	const char *CurrentUserRoot()
	{
		return kCurrentUserRoot;
	}

	const char *LocalMachineRoot()
	{
		return kLocalMachineRoot;
	}

	// Persist non-Windows registry reads and writes in registry.ini.
	bool ReadString(const char *root, const char *path, const char *key, std::string &value)
	{
		RegistryIniData data;
		LoadRegistryIni(data);

		const RegistryIniData::const_iterator sectionIt = data.find(BuildSectionName(root, path));
		if (sectionIt == data.end()) {
			return false;
		}

		const RegistryIniSection::const_iterator valueIt = sectionIt->second.find(BuildKeyName(key));
		if (valueIt == sectionIt->second.end()) {
			return false;
		}

		value = valueIt->second;
		return true;
	}

	bool ReadUnsignedInt(const char *root, const char *path, const char *key, unsigned int &value)
	{
		std::string storedValue;
		if (!ReadString(root, path, key, storedValue)) {
			return false;
		}

		char *end = nullptr;
		const unsigned long parsedValue = strtoul(storedValue.c_str(), &end, 0);
		if (end == storedValue.c_str() || *end != '\0') {
			return false;
		}

		value = static_cast<unsigned int>(parsedValue);
		return true;
	}

	bool WriteString(const char *root, const char *path, const char *key, const char *value)
	{
		RegistryIniData data;
		if (!LoadRegistryIniToRewrite(data)) {
			return false;
		}
		data[BuildSectionName(root, path)][BuildKeyName(key)] = value == nullptr ? "" : value;
		return SaveRegistryIni(data);
	}

	bool WriteUnsignedInt(const char *root, const char *path, const char *key, unsigned int value)
	{
		char buffer[32];
		sprintf(buffer, "%u", value);
		return WriteString(root, path, key, buffer);
	}

	bool SectionExists(const char *root, const char *path)
	{
		RegistryIniData data;
		LoadRegistryIni(data);
		return data.find(BuildSectionName(root, path)) != data.end();
	}

	bool DeleteValue(const char *root, const char *path, const char *key)
	{
		RegistryIniData data;
		if (!LoadRegistryIniToRewrite(data)) {
			return false;
		}

		RegistryIniData::iterator sectionIt = data.find(BuildSectionName(root, path));
		if (sectionIt == data.end()) {
			return false;
		}

		sectionIt->second.erase(BuildKeyName(key));
		if (sectionIt->second.empty()) {
			data.erase(sectionIt);
		}

		return SaveRegistryIni(data);
	}

	bool ClearSection(const char *root, const char *path)
	{
		RegistryIniData data;
		if (!LoadRegistryIniToRewrite(data)) {
			return false;
		}
		data.erase(BuildSectionName(root, path));
		return SaveRegistryIni(data);
	}

	bool ListValues(const char *root, const char *path, std::vector<std::string> &keys)
	{
		keys.clear();
		RegistryIniData data;
		LoadRegistryIni(data);

		const RegistryIniData::const_iterator sectionIt = data.find(BuildSectionName(root, path));
		if (sectionIt == data.end()) {
			return false;
		}

		for (RegistryIniSection::const_iterator valueIt = sectionIt->second.begin(); valueIt != sectionIt->second.end(); ++valueIt) {
			keys.push_back(valueIt->first == kDefaultValueKey ? "" : valueIt->first);
		}

		return true;
	}
}