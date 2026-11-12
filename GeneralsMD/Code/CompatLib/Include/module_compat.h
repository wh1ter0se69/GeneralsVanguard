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

class CComModule
{
  public:
    void Init(void*, HINSTANCE hInstance)
    {
      m_hInstance = hInstance;
    }
    void Term() {}

  private:
    HINSTANCE m_hInstance;
};

bool GetModuleFileName(HINSTANCE hInstance, char* buffer, int size);

typedef uintptr_t (*FARPROC)();
typedef HANDLE HMODULE;

HMODULE LoadLibrary(const char* lpFileName);
FARPROC GetProcAddress(HMODULE hModule, const char* lpProcName);
void FreeLibrary(HMODULE hModule);