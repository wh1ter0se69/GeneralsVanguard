// GeneralsX @probe Claude 02/10/2026 GX_TEXT_CONTRAST's curve, pure so scripts/test/text-shadow-test.cpp can check it.
// DirectWrite-style "enhanced contrast" on 8-bit glyph coverage: a' = a(k+1) / (ak+1). Partial-coverage edge pixels get
// stronger, 0 and 255 stay put. k outside (0, 100] - off, NaN, infinite, absurd - is the identity (review P3: k = inf gave
// NaN for every value and a huge k turned every edge pixel solid).

#pragma once

inline unsigned char gxTextContrast( float k, unsigned char coverage )
{
	if (!(k > 0.0f && k <= 100.0f))
		return coverage;
	const float a = coverage / 255.0f;
	return (unsigned char)(a * (k + 1.0f) / (a * k + 1.0f) * 255.0f + 0.5f);
}
