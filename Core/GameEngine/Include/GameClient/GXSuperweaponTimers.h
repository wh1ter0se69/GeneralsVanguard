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

/*
**	Which superweapon timers a player sees. Client-only: two Options.ini keys, which
**	the launcher's Settings page writes (gxio::options KEY_SUPERWEAPON_TIMERS*), read by InGameUI::postDraw once per match.
**	The simulation never reads them, so players may differ.
**
**	  SuperweaponTimers = all | soon | off     absent or anything else = all (every timer, as before)
**	  SuperweaponTimerSoonSeconds = N          in "soon", a timer shows only when it fires in N seconds or less (a ready one
**	                                           too); absent or not a whole number = 60, clamped to 10..600
**
**	"off" hides the timers only: Eva's "superweapon ready" lines and the spectator launch toast stay. Pure, so
**	scripts/test/superweapon-timers-setting-test.cpp drives these very functions.
*/

#pragma once

#ifndef GX_SUPERWEAPON_TIMERS_H
#define GX_SUPERWEAPON_TIMERS_H

enum GxSwTimersMode { GX_SW_TIMERS_ALL = 0, GX_SW_TIMERS_SOON = 1, GX_SW_TIMERS_OFF = 2 };

static const int kGxSwTimerSoonDefault = 60;
static const int kGxSwTimerSoonMin = 10;
static const int kGxSwTimerSoonMax = 600;	///< 10 minutes: longer than any stock superweapon's countdown, so "soon" = "all" above it

/// `value` without surrounding spaces or tabs, as [*first, *last); FALSE when nothing is left.
inline bool gxSwTimersTrim( const char *value, const char **first, const char **last )
{
	if( value == nullptr )
		return false;
	while( *value == ' ' || *value == '\t' )
		++value;
	const char *end = value;
	while( *end != '\0' )
		++end;
	while( end > value && ( end[ -1 ] == ' ' || end[ -1 ] == '\t' || end[ -1 ] == '\r' ) )
		--end;
	*first = value;
	*last = end;
	return end > value;
}

/// The SuperweaponTimers value, ignoring case and surrounding spaces.
inline GxSwTimersMode gxSwTimersModeParse( const char *value )
{
	const char *a = nullptr, *b = nullptr;
	if( !gxSwTimersTrim( value, &a, &b ) )
		return GX_SW_TIMERS_ALL;
	static const struct { const char *word; GxSwTimersMode mode; } kWords[] = { { "soon", GX_SW_TIMERS_SOON }, { "off", GX_SW_TIMERS_OFF } };
	for( const auto &w : kWords )
	{
		const char *p = a;
		const char *q = w.word;
		for( ; p < b && *q != '\0'; ++p, ++q )
		{
			char c = *p;
			if( c >= 'A' && c <= 'Z' )
				c = (char)( c - 'A' + 'a' );
			if( c != *q )
				break;
		}
		if( p == b && *q == '\0' )
			return w.mode;
	}
	return GX_SW_TIMERS_ALL;
}

/// The SuperweaponTimerSoonSeconds value: a whole number of seconds, clamped; anything else is the default.
inline int gxSwTimerSoonParse( const char *value )
{
	const char *a = nullptr, *b = nullptr;
	if( !gxSwTimersTrim( value, &a, &b ) || b - a > 6 )
		return kGxSwTimerSoonDefault;
	int n = 0;
	for( const char *p = a; p < b; ++p )
	{
		if( *p < '0' || *p > '9' )
			return kGxSwTimerSoonDefault;
		n = n * 10 + ( *p - '0' );
	}
	return n < kGxSwTimerSoonMin ? kGxSwTimerSoonMin : n > kGxSwTimerSoonMax ? kGxSwTimerSoonMax : n;
}

/// Whether one launcher's time is drawn: every one in "all", none in "off", in "soon" a ready one or one firing within soonSeconds.
inline bool gxSwTimerShown( GxSwTimersMode mode, bool ready, int secondsLeft, int soonSeconds )
{
	if( mode == GX_SW_TIMERS_OFF )
		return false;
	if( mode == GX_SW_TIMERS_SOON )
		return ready || secondsLeft <= soonSeconds;
	return true;
}

#endif
