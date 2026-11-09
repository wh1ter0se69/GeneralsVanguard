// GeneralsX @feature E · rw3-L-2 11/10/2026 GxNativeConfig.h. The line parser is DXVK's parseUserConfigLine
// (util/config/config.cpp:1270-1338 at 954c5c692) ported rule for rule, so a value means here what it means to DXVK.
#include "GxNativeConfig.h"
#include <algorithm>
#include <cctype>

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace GXRender { namespace Dev {

namespace {

bool isWhitespace(char ch) { return ch == ' ' || ch == '\x9' || ch == '\r'; }

bool isValidKeyChar(char ch)
{
	return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '.' || ch == '_';
}

size_t skipWhitespace(const std::string& line, size_t n)
{
	while (n < line.size() && isWhitespace(line[n]))
		++n;
	return n;
}

// Config::parseOptionValue(int32_t): digits with an optional leading '-', nothing else; false = DXVK keeps the default
bool parseInt(const std::string& v, int* out)
{
	if (v.empty())
		return false;
	int sign = 1;
	size_t i = 0;
	if (v[0] == '-')
		sign = -1, i = 1;
	int r = 0;
	for (; i < v.size(); ++i) {
		if (v[i] < '0' || v[i] > '9')
			return false;
		r = r * 10 + (v[i] - '0');
	}
	*out = sign * r;
	return true;
}

struct Parsed
{
	std::map<std::string, std::string> options;
	std::string section;   // the first "[...]" line seen
};

void parseLine(Parsed& p, const std::string& line, bool firstWins)
{
	size_t n = skipWhitespace(line, 0);
	if (n < line.size() && line[n] == '[') {
		// DXVK activates the following lines only when the section names the executable; v1 reproduces no section (S5)
		if (p.section.empty())
			p.section = line.substr(n);
		return;
	}
	std::string key, value;
	while (n < line.size() && isValidKeyChar(line[n]))
		key += line[n++];
	n = skipWhitespace(line, n);
	if (n >= line.size() || line[n] != '=')
		return;
	bool inside = false;
	n = skipWhitespace(line, n + 1);
	while (n < line.size()) {
		if (!inside && isWhitespace(line[n]))
			break;
		if (line[n] == '"') {
			inside = !inside;
			++n;
		} else {
			value += line[n++];
		}
	}
	if (firstWins)
		p.options.emplace(key, value);
	else
		p.options[key] = value;   // Config::setOption = insert_or_assign: the later line wins
}

} // namespace

GxNativeConfig GxNativeParseConfig(const std::string& file, bool haveFile, const std::string& env, const GxNativeConfigDebug* debug)
{
	const bool firstWins = debug && debug->firstLineWins;
	Parsed p;
	GxNativeConfig c;
	if (haveFile) {
		std::istringstream s(file);
		for (std::string line; std::getline(s, line);)
			parseLine(p, line, firstWins);
	}
	for (size_t a = 0; !env.empty() && a <= env.size();) {   // str::split(confLine, ";")
		size_t b = env.find(';', a);
		if (b == std::string::npos)
			b = env.size();
		parseLine(p, env.substr(a, b - a), firstWins);
		a = b + 1;
	}
	if (!p.section.empty()) {
		c.refused = "dxvk config: section " + p.section + " not reproduced";
		return c;
	}
	for (const auto& kv : p.options) {
		if (kv.first == "d3d9.samplerAnisotropy") {
			int v = -1;
			if (parseInt(kv.second, &v))   // getOption<int32_t>: an unparsable value leaves the default
				c.samplerAnisotropy = v;
		} else if ((kv.first == "d3d9.presentInterval" || kv.first == "dxvk.tearFree") && debug && debug->ignorePresentKeys) {
			// TEST-ONLY plant: accepted, not read
		} else if (kv.first == "d3d9.presentInterval") {
			int v = -1;
			if (parseInt(kv.second, &v))   // getOption<int32_t>: an unparsable value leaves the default
				c.presentInterval = v;
			// An interval >= 2 is also a FRAME LIMIT in DXVK (refresh / n, D3D9SwapChainEx::UpdateTargetFrameRate); the device presents
			// FIFO without that pacing, so it refuses rather than present at the full refresh (Codex nw1-present-r1 P2).
			if (c.presentInterval >= 2 && !(debug && debug->ignoreUnknownKeys)) {
				c.refused = "dxvk config: d3d9.presentInterval = " + std::to_string(c.presentInterval) + " not reproduced (frame pacing)";
				return c;
			}
		} else if (kv.first == "dxvk.tearFree") {
			std::string v = kv.second;   // getOption<Tristate>: parseStringOption lowercases; anything else leaves Auto
			std::transform(v.begin(), v.end(), v.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
			c.tearFree = v == "true" ? 1 : v == "false" ? 0 : v == "auto" ? -1 : c.tearFree;
		} else if (kv.first.empty() || kv.first == "dxvk.logLevel" || kv.first == "d3d9.forceSamplerTypeSpecConstants") {
			// no option / logging only / shader compilation only: nothing the picture depends on
		} else if (!(debug && debug->ignoreUnknownKeys)) {
			c.refused = "dxvk config: " + kv.first + " not reproduced";
			return c;
		}
	}
	return c;
}

// GeneralsX @bugfix D · asm-0968 24/10/2026 The environment and the file as DXVK reads them (src/util/util_env.cpp:24-39 getEnvVar,
// src/util/config/config.cpp:1544-1555 getUserConfig, at 954c5c692): on Windows the PROCESS environment through
// GetEnvironmentVariableW into MAX_PATH UTF-16 units, a value of MAX_PATH or more reading as unset, converted to UTF-8, and the file
// opened by its wide path - NOT the CRT's copy of the environment. W3DDisplay::init writes the player's texture filter into
// DXVK_CONFIG with SetEnvironmentVariableA (W3DDisplay.cpp, "DXVK reads GetEnvironmentVariableW, not the CRT copy"), which std::getenv
// never sees: once the shipped Windows dxvk.conf qualified (render/ws-win-present), a native Windows session ran the FILE's anisotropy
// instead of the one the player chose, while classic DXVK applied it (Codex asm0968-combo-A-render-r1 #1; the wide API and path:
// combo-r2 #1). Elsewhere getenv and a narrow path, exactly what DXVK uses there.
#if defined(_WIN32)
static std::wstring gxDxvkEnvW(const wchar_t* name)
{
	wchar_t value[MAX_PATH + 1] = {};
	const DWORD len = ::GetEnvironmentVariableW(name, value, MAX_PATH);
	if (!len || len >= MAX_PATH)
		return std::wstring();
	return std::wstring(value, len);
}

static std::string gxUtf8(const std::wstring& w)
{
	if (w.empty())
		return std::string();
	const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
	if (n <= 0)
		return std::string();
	std::string s(size_t(n), '\0');
	::WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), &s[0], n, nullptr, nullptr);
	return s;
}
#endif

GxNativeConfig GxNativeLoadConfig()
{
#if defined(_WIN32)
	const std::wstring fpw = gxDxvkEnvW(L"DXVK_CONFIG_FILE");
	const std::string ev = gxUtf8(gxDxvkEnvW(L"DXVK_CONFIG"));
	const std::wstring pathw = fpw.empty() ? std::wstring(L"dxvk.conf") : fpw;
	const std::string path = gxUtf8(pathw);
	std::ifstream f(pathw.c_str(), std::ios::binary);
#else
	const char* fp = std::getenv("DXVK_CONFIG_FILE");
	const char* evc = std::getenv("DXVK_CONFIG");
	const std::string ev = evc ? evc : "";
	const std::string path = fp && *fp ? fp : "dxvk.conf";
	std::ifstream f(path, std::ios::binary);
#endif
	std::string text;
	if (f)
		text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	GxNativeConfig c = GxNativeParseConfig(text, bool(f), ev);
	c.sources = std::string(f ? "file " + path : "no file") + (!ev.empty() ? " + env" : "");
	return c;
}

}} // namespace GXRender::Dev
