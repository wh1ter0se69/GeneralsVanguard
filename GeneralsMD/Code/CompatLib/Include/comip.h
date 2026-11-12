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

#pragma once

// GeneralsX @build fbraz 11/02/2026 Bender
// COM Interface Pointer header wrapper - redirects to platform-specific implementation
// Used by dx8webbrowser.cpp (DirectX web browser embedding)

#ifdef _WIN32
// Windows: Use real COM IP header from Windows SDK
#include_next <comip.h>
#else
// Linux: Use our compatibility layer (minimal stubs for compilation)
#include "comip_compat.h"
#endif
