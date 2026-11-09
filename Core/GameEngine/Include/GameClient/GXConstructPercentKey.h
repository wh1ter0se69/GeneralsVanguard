// GeneralsX @bugfix Claude 26/09/2026 (backlog #72, Codex) The key Drawable::drawConstructPercent rebuilds its
// "under construction" text on, derived from the CSF format that text is drawn with.
//
// The TSH-perf port keyed the text on lrintf(percent) - right for retail's "%.0f%%", stale for a localization or
// mod whose CONTROLBAR:UnderConstructionDesc shows decimals ("%.1f%%" changed text ten times per whole percent
// while the key moved once). Here the key is the number exactly as printf draws it: its digits at the format's own
// precision, so the text is rebuilt when, and only when, the number drawn changes (and on an exact tie). A format this
// cannot read that way (%e, %g, '*', no number...) gets no key and is rebuilt every frame, retail's cost, never stale.
//
// Header-only and pure: both games' Drawable.cpp and scripts/test/construct-percent-key-test.cpp use it.

#pragma once

#include <cmath>
#include <wchar.h>

/// The decimals the number in `fmt` is drawn with, when its first conversion is %f or %F (an optional 'l', no '*',
/// no positional '$') with a precision of at most 6 - 6 when none is given. -1 for anything else.
inline int gxFixedDecimals( const wchar_t *fmt )
{
	for (const wchar_t *p = fmt; p != NULL && *p != L'\0'; ++p)
	{
		if (*p != L'%')
			continue;
		++p;
		if (*p == L'%')
			continue;					// a literal '%'
		while (*p == L'-' || *p == L'+' || *p == L' ' || *p == L'#' || *p == L'0' || *p == L'\'')
			++p;						// flags
		while (*p >= L'0' && *p <= L'9')
			++p;						// width
		int decimals = 6;
		if (*p == L'.')
		{
			decimals = 0;
			for (++p; *p >= L'0' && *p <= L'9'; ++p)
				if ((decimals = decimals * 10 + (*p - L'0')) > 6)
					return -1;
		}
		if (*p == L'l')
			++p;
		return (*p == L'f' || *p == L'F') ? decimals : -1;
	}
	return -1;
}

/// The number `percent` draws as at `decimals` places, as a key: its digits times 8, plus `decimals`, so keys made at two
/// precisions never compare equal. -1 - "no key, rebuild" - for decimals outside 0..6, a negative, -0 or non-finite
/// value, one too large to hold, and an EXACT tie.
///
/// Arithmetic, never a format (Codex r2: this runs every frame for every building under construction in view, and the
/// rebuild it guards is the only place that formats). Exact for a float x = M * 2^e: x * 10^d (d <= 6) is either
/// exactly k + 0.5 - a double holds that, and the product rounds to it - or off every such tie by a nonzero multiple
/// of min(2^e, 1/2), so by more than x * 2^-24, while the double product is off by at most x * 10^d * 2^-53 - over 500 times
/// less even at 6 decimals. So the double lands on the same side of every tie as printf's exact conversion. The tie itself is left to the C library (IEEE ties-to-even; older Windows CRTs round it up), so it
/// gets no key and is rebuilt: never stale, and a frame on an exact tie is rare.
inline int gxConstructPercentKey( float percent, int decimals )
{
	static const double scale[7] = { 1.0, 10.0, 100.0, 1e3, 1e4, 1e5, 1e6 };
	if (decimals < 0 || decimals > 6 || !(percent >= 0.0f) || percent > 200.0f || std::signbit(percent))
		return -1;					// NaN fails >=; -0 draws as "-0"
	const double t = (double)percent * scale[decimals];
	const double whole = std::floor(t);
	const double frac = t - whole;	// exact
	if (frac == 0.5)
		return -1;
	return (int)(((long long)whole + (frac > 0.5 ? 1 : 0)) * 8 + decimals);
}
