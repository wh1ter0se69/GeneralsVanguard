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

// GeneralsX @bugfix Claude 24/09/2026 One number parser for INI data on every platform.
//
// INI values feed the simulation, so every peer must turn the same text into the same bits. INI.cpp used
// std::from_chars on Windows and Linux but sscanf on Apple, because Apple's libc++ only ships float
// from_chars from macOS/iOS 26 (we target 15 and 16). The two agree on every number in the shipped data,
// but not on the grammar at the edges: sscanf reads "0x1p3" as 8 where from_chars reads 0, accepts
// out-of-range values that from_chars rejects (INI.cpp then throws on one platform only), and rejects
// "+-1" that the '+' strip lets from_chars read. A custom map's map.ini is enough to hit any of these.
//
// Floats now go through fast_float (header-only, the same code libstdc++'s from_chars is built on) on
// every platform, so grammar and rounding are identical by construction. inf and nan are rejected, as retail did. Integers use std::from_chars,
// which is header-only and available on every target. scripts/test/ini-numbers checks this function
// against a golden corpus of every number in the retail and GeneralsX INI data.
//
// Kept free of engine headers so that test can compile it alone.

#pragma once

#include <fast_float/fast_float.h>

#include <charconv>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <type_traits>

// Parses token as INI.cpp's scanReal/scanInt/scanUnsignedInt do. Returns false where they throw
// INI_INVALID_DATA. A leading '+' is dropped first: neither parser accepts it, and custom INI files use it.
template <typename Type>
inline bool gxIniScanNumber(std::string_view token, Type &out)
{
	if (!token.empty() && token[0] == '+')
	{
		token.remove_prefix(1);
	}

	if constexpr (std::is_floating_point_v<Type>)
	{
		// no_infnan: retail's VC6 sscanf("%f") rejected "inf"/"nan" (INI_INVALID_DATA), and an accepted one
		// can turn into a NaN whose sign bit differs between x86 and ARM, which splits the CRC.
		const auto result = fast_float::from_chars(token.data(), token.data() + token.size(), out,
			fast_float::chars_format::general | fast_float::chars_format::no_infnan);
		return result.ec == std::errc{};
	}
	else
	{
		// TheSuperHackers @info std::from_chars cannot parse "-1" as uint32, so integers go through int64.
		std::int64_t value{};
		const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
		out = static_cast<Type>(value);
		return result.ec == std::errc{};
	}
}
