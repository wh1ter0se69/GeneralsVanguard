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

// GX_RENDER_NATIVE=OFF side of GxNativeInstall.h. Always built.
#ifndef GX_RENDER_NATIVE
#include "W3DDevice/GameClient/GxNativeInstall.h"

#include <cstdio>

#include "GameClient/GXGraphicsSettings.h"	// the renderer switch (Options.ini GXRenderer, GX_RENDERER over it)

namespace GxNative
{
void Install_Unavailable()
{
	if (GXGraphics::nativeRendererRequested()) {
		fprintf(stderr, "[GXNATIVE] session=classic reason=\"build has no GX_RENDER_NATIVE\"\n");
		fflush(stderr);
	}
}
}
#endif
