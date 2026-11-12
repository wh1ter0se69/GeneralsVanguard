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

// L1: the EFFECTIVE DXVK configuration a native session must reproduce (
// §5.2, row S5). DXVK reads $DXVK_CONFIG_FILE (else ./dxvk.conf), then the $DXVK_CONFIG string split at ';', later lines winning
// (references/fbraz3-dxvk 954c5c692 util/config/config.cpp getUserConfig + parseUserConfigLine). The game writes DXVK_CONFIG itself
// before the device (GeneralsMD W3DDisplay.cpp:1804-1838). Render-only.
#pragma once

#include <string>

namespace GXRender { namespace Dev {

struct GxNativeConfig
{
	int samplerAnisotropy = -1;   // d3d9.samplerAnisotropy (DXVK's default -1: no override)
	// the present keys the SHIPPED Windows dxvk.conf sets (setup-run-win64.ps1: presentInterval 0 +
	// tearFree True = MAILBOX). presentInterval >= 0 replaces the app's sync interval on every present (d3d9_swapchain.cpp:139-140);
	// tearFree is DXVK's Tristate (dxvk_options.cpp:14): -1 Auto, 0 False, 1 True, applied by dxvk_presenter.cpp pickPresentMode.
	int presentInterval = -1;     // d3d9.presentInterval (DXVK's default -1: the app's interval)
	int tearFree = -1;            // dxvk.tearFree
	std::string refused;          // non-empty: the session must be CLASSIC (S5), the text names the key
	std::string sources;          // "file <path>", "env" - for the session line
};

struct GxNativeConfigDebug   // TEST-ONLY plants (gx_native_test); all false in the product
{
	bool firstLineWins = false;     // an earlier line keeps its value over a later one
	bool ignoreUnknownKeys = false; // a key the device does not reproduce is not refused
	bool ignorePresentKeys = false; // d3d9.presentInterval / dxvk.tearFree are accepted but not read
};

// The parser alone: `file` is the config file's text (haveFile false = no file), `env` the DXVK_CONFIG string.
GxNativeConfig GxNativeParseConfig(const std::string& file, bool haveFile, const std::string& env,
	const GxNativeConfigDebug* debug = nullptr);
// The process's effective configuration, read the way DXVK reads it (environment + working directory at the call).
GxNativeConfig GxNativeLoadConfig();

}} // namespace GXRender::Dev
