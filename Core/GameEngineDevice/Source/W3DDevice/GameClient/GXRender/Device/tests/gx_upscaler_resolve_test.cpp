// B · mac-upscaler-u2 18/10/2026: GXGraphics::resolveUpscaler - every GXUpscaler value x {Mac (no Streamline), Windows without
// Streamline, Windows with Streamline} -> the value upscaler() will return. Build:
//   c++ -std=c++17 -I u2stub -I <repo>/Core/GameEngine/Include gx_upscaler_resolve_test.cpp -o gx_upscaler_resolve_test
// Mac and Windows-without-Streamline are the same build flag (streamlineBuild=false); both are listed so a platform split can't hide.
#include "GameClient/GXGraphicsSettings.h"
#include <cstdio>

int main()
{
	using namespace GXGraphics;
	struct Row { Int in; Int mac, winNoSl, winSl; };
	const Row rows[] = {
		{ UPSCALER_OFF,              UPSCALER_OFF,      UPSCALER_OFF,      UPSCALER_OFF },
		{ UPSCALER_BILINEAR,         UPSCALER_BILINEAR, UPSCALER_BILINEAR, UPSCALER_BILINEAR },
		{ UPSCALER_FSR1,             UPSCALER_FSR1,     UPSCALER_FSR1,     UPSCALER_FSR1 },
		{ UPSCALER_FSR3,             UPSCALER_OFF,      UPSCALER_OFF,      UPSCALER_OFF },
		{ UPSCALER_DLSS,             UPSCALER_OFF,      UPSCALER_OFF,      UPSCALER_DLSS },
		{ UPSCALER_METALFX,          UPSCALER_OFF,      UPSCALER_OFF,      UPSCALER_OFF },
		{ UPSCALER_METALFX_TEMPORAL, UPSCALER_OFF,      UPSCALER_OFF,      UPSCALER_OFF },
		{ UPSCALER_XESS,             UPSCALER_OFF,      UPSCALER_OFF,      UPSCALER_OFF },
		{ -1,                        UPSCALER_OFF,      UPSCALER_OFF,      UPSCALER_OFF },
		{ UPSCALER_COUNT,            UPSCALER_OFF,      UPSCALER_OFF,      UPSCALER_OFF },
	};
	int bad = 0;
	for (const Row &r : rows) {
		const char *why = nullptr;
		const Int got[3] = { resolveUpscaler(r.in, false, &why), resolveUpscaler(r.in, false, nullptr), resolveUpscaler(r.in, true, &why) };
		const Int want[3] = { r.mac, r.winNoSl, r.winSl };
		for (int i = 0; i < 3; ++i)
			if (got[i] != want[i]) { std::printf("FAIL in=%d build=%d got=%d want=%d\n", (int)r.in, i, (int)got[i], (int)want[i]); ++bad; }
	}
	const char *why = nullptr;
	resolveUpscaler(UPSCALER_METALFX_TEMPORAL, false, &why);
	if (why == nullptr || *why == '\0') { std::printf("FAIL: a changed request gave no reason\n"); ++bad; }
	resolveUpscaler(UPSCALER_FSR1, false, &why);
	if (why == nullptr || *why != '\0') { std::printf("FAIL: an unchanged request gave a reason\n"); ++bad; }
	std::printf(bad ? "gx_upscaler_resolve_test: %d FAILED\n" : "gx_upscaler_resolve_test: all rows pass\n", bad);
	return bad != 0;
}
