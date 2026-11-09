/*
**	GeneralsX @feature Claude 17/10/2026 - ONE rule for a player's colour used as TEXT.
**
**	Colour set R1 includes Black (40,40,40). A swatch, a unit or a house keeps that exact colour, but the same
**	colour as text on a dark panel or over terrain is close to unreadable. The players bar already lifts every
**	name 35% toward white (InGameUI.cpp brightenForText: Black 40 -> 115 grey); this header holds that blend and
**	the near-black test, so the bar, in-game chat, the score screen, diplomacy, the observer list, squad numbers,
**	the lobby game-info list and the desync report all follow one rule.
**
**	Only NEAR-BLACK colours are lifted by gxPlayerTextColor: the other eleven R1 colours (darkest: Purple 150,0,200)
**	read fine and stay exactly as the palette defines them. Header-only so a test can drive it with
**	`c++ -std=c++17 -I Core/GameEngine/Include`.
*/

#pragma once

#ifndef GX_TEXT_COLOR_H
#define GX_TEXT_COLOR_H

#include "GameClient/Color.h"

/// Blend toward white; 0 = unchanged, 1 = white. Alpha passes through (banners fade with it).
inline Color gxLiftTowardWhite( Color c, float k )
{
	UnsignedByte r = 0, g = 0, b = 0, a = 255;
	GameGetColorComponents( c, &r, &g, &b, &a );
	return GameMakeColor( (Int)( r + ( 255 - r ) * k ), (Int)( g + ( 255 - g ) * k ), (Int)( b + ( 255 - b ) * k ), a );
}

/// The players bar's lift, for any colour.
inline Color gxBrightenForText( Color c ) { return gxLiftTowardWhite( c, 0.35f ); }

/// True when every channel is below this: Black (40) is in, Purple (200 blue), Blue, Raspberry are out.
static const int GX_NEAR_BLACK_MAX_CHANNEL = 96;

inline bool gxIsNearBlack( Color c )
{
	UnsignedByte r = 0, g = 0, b = 0, a = 255;
	GameGetColorComponents( c, &r, &g, &b, &a );
	return r < GX_NEAR_BLACK_MAX_CHANNEL && g < GX_NEAR_BLACK_MAX_CHANNEL && b < GX_NEAR_BLACK_MAX_CHANNEL;
}

/// How far a NEAR-BLACK player colour is lifted as text. 0.75 takes Black (40) to 201: a light grey that still reads as
/// Black's colour (pure white would read as a white player) and holds up against bright terrain and roads, where the
/// bar's 0.35 (115) was grey-on-grey.
static const float GX_NEAR_BLACK_TEXT_LIFT = 0.75f;

/// A player's colour as TEXT colour: lifted hard when near-black, the colour itself otherwise. (The players bar keeps
/// its own gentler gxBrightenForText for every name, so it is unchanged.)
inline Color gxPlayerTextColor( Color c ) { return gxIsNearBlack( c ) ? gxLiftTowardWhite( c, GX_NEAR_BLACK_TEXT_LIFT ) : c; }

#endif
