/*
**	Command & Conquer Generals(tm)
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

#define _tcslen strlen
#define _tcscmp strcmp
#define _tcsicmp strcasecmp
#define _tcsclen strlen
#define _tcscpy strcpy

// These need to match windows_base.h from DXVK, as long as it's being used
typedef char TCHAR;
typedef const TCHAR* LPCTSTR;
typedef TCHAR* LPTSTR;