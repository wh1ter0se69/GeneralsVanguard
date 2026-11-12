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

#include "types_compat.h"
#include "wnd_compat.h"
// GeneralsX: SDL3 only for windowing builds (Phase 2)
#ifdef SAGE_USE_SDL3
#include <SDL3/SDL.h>
#endif

DWORD GetWindowLong(HWND hWnd, int nIndex)
{
  return 0;
}

void AdjustWindowRect(RECT *pRect, DWORD dwStyle, BOOL bMenu)
{
  // This is left blank, since the SDL3 window is always going to be the in-game resolution size
  // Windows seems to need to take into account the decoration size, which SDL3 does not need to
}

BOOL ShowWindow(HWND hWnd, int nCmdShow)
{
  return TRUE;
}

void SetWindowPos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags)
{
#ifdef SAGE_USE_SDL3
  SDL_Window *window = (SDL_Window *)hWnd;
  if (!window) return;

  if (!(uFlags & SWP_NOMOVE)) {
    SDL_SetWindowPosition(window, X, Y);
  }

  if (!(uFlags & SWP_NOSIZE)) {
    SDL_SetWindowSize(window, cx, cy);
  }
#endif
}

void GetWindowRect(HWND hWnd, RECT *pRect)
{

}

void GetClientRect(HWND hWnd, RECT *pRect)
{

}

HWND GetDesktopWindow() {
  return NULL;
}

HDC GetDC(HWND hWnd) {
  return NULL;
}

int ReleaseDC(HWND hWnd, HDC hDC) 
{
  return 0;
}

void SetDeviceGammaRamp(HDC hDC, LPVOID lpRamp)
{

}

int MessageBox(HWND hWnd, LPCTSTR lpText, LPCTSTR lpCaption, UINT uType)
{
  return 0;
}

void SetCursor(void *hCursor)
{

}

void GetCursorPos(struct POINT *ptCursor) {

}

void ScreenToClient(HWND hWnd, struct POINT *ptCursor)
{

}

void ClientToScreen(HWND hWnd, struct POINT *ptCursor)
{

}
