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

// One INI number parser for every platform, so every peer turns the same text into the same bits. Apple libc++
// lacks float from_chars before macOS/iOS 26 and sscanf differs at the edges, so floats use fast_float everywhere.
// inf and nan are rejected as in retail; integers use std::from_chars. Engine-free: scripts/test/ini-numbers.

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
