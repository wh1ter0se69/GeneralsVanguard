// GeneralsX: GX_RENDER_NATIVE=OFF side of GxNativeInstall.h. Always built.
#ifndef GX_RENDER_NATIVE
#include "W3DDevice/GameClient/GxNativeInstall.h"

#include <cstdio>

#include "GameClient/GXGraphicsSettings.h"	// D · render-line-0967: the renderer switch (Options.ini GXRenderer, GX_RENDERER over it)

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
