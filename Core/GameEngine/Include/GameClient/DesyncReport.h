/*
** GeneralsX: who the desync report says agrees, disagrees or was silent,
** and the order it lists them in, as a pure function (scripts/test/desync-report-test.cpp).
**
** The window that uses it is DesyncReportShow (DesyncReport.cpp). This half holds the decisions
** that make the report true or false, so they can be checked with no engine and no display.
**
** EVERY PLAYER IS LISTED, not only the ones who disagree: Generals Online prints the minority alone
** (references/generals-online-client, GameLogic.cpp:2795-2805), which cannot tell "eleven agree and
** one machine is broken" from "the lobby split six against six" - two situations that call for
** opposite actions. The counts ARE the diagnosis.
**
** NO MAJORITY, NO CULPRIT. Generals Online takes the most frequent CRC with a bare `>`, so an even
** split blames whichever half std::map walked first. A tie for the largest group is reported as a
** split, with nobody named.
**
** A SILENT PEER IS NOT A GUILTY ONE. A CRC that has not arrived within the interval is the ordinary
** condition of a lockstep peer (GameLogic.cpp's shortfall note), so it gets its own verdict and is
** counted apart from the players that disagree.
**
** EVERY COUNT COMES FROM THE ROWS, so the header can never disagree with the table under it (quoting
** "2 of 4 agree" over a table with a silent row invites reading the silent one as a dissenter).
**
** ROW ORDER groups the players that are in the same game: the smallest group first (the likeliest
** broken machine is at the top), ties by lowest slot, lobby order inside a group, silent rows last.
** The Slot column keeps every row findable against the lobby.
*/

#pragma once

#include "Lib/BaseType.h"
#include "GameClient/LobbySetupUi.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

enum DesyncVerdict
{
	DESYNC_AGREES,			///< reported the majority's CRC
	DESYNC_DIFFERS,			///< reported, and not the majority's CRC
	DESYNC_SPLIT,				///< reported, and there is no unique majority to measure it against
	DESYNC_NO_REPORT		///< connected, but its CRC had not arrived this interval
};

struct DesyncReportRow
{
	Int player;								///< ThePlayerList index of the slot's Player (every observer slot shares one); -1 = none
	Int slot;									///< lobby slot, 0-based - the key of the CRC table and the [CRC] log
	Bool reported;
	UnsignedInt crc;					///< meaningful only when reported
	DesyncVerdict verdict;		///< set by DesyncReportJudge
};

struct DesyncReportSummary
{
	Int reporters;						///< rows that reported
	Int silent;								///< rows that did not
	Int groups;								///< distinct CRCs among the reporters
	Int bestCount;						///< size of the largest group
	Bool haveMajority;				///< the largest group is the only one that size
};

/// Give every row its verdict, sort the rows into report order, and count them.
inline DesyncReportSummary DesyncReportJudge( std::vector<DesyncReportRow> &rows )
{
	struct Group { Int count; Int firstSlot; };
	std::map<UnsignedInt, Group> groups;
	DesyncReportSummary s = { 0, 0, 0, 0, FALSE };

	for( size_t i = 0; i < rows.size(); ++i )
	{
		if( !rows[i].reported )
		{
			++s.silent;
			continue;
		}
		++s.reporters;
		std::map<UnsignedInt, Group>::iterator g = groups.find( rows[i].crc );
		if( g == groups.end() )
		{
			Group fresh = { 1, rows[i].slot };
			groups[ rows[i].crc ] = fresh;
		}
		else
		{
			++g->second.count;
			if( rows[i].slot < g->second.firstSlot )
				g->second.firstSlot = rows[i].slot;
		}
	}

	Int tiedAtBest = 0;
	UnsignedInt bestCRC = 0;
	for( std::map<UnsignedInt, Group>::const_iterator g = groups.begin(); g != groups.end(); ++g )
	{
		if( g->second.count > s.bestCount )
		{
			s.bestCount = g->second.count;
			bestCRC = g->first;
			tiedAtBest = 1;
		}
		else if( g->second.count == s.bestCount )
		{
			++tiedAtBest;
		}
	}
	s.groups = (Int)groups.size();
	s.haveMajority = ( tiedAtBest == 1 ) ? TRUE : FALSE;

	for( size_t i = 0; i < rows.size(); ++i )
	{
		if( !rows[i].reported )
			rows[i].verdict = DESYNC_NO_REPORT;
		else if( !s.haveMajority )
			rows[i].verdict = DESYNC_SPLIT;
		else
			rows[i].verdict = ( rows[i].crc == bestCRC ) ? DESYNC_AGREES : DESYNC_DIFFERS;
	}

	// A strict weak order on (silent last, group size, group's first slot, slot). Two groups can
	// not share a first slot - a slot reports one CRC - so equal keys only ever mean the same group.
	struct Order
	{
		const std::map<UnsignedInt, Group> *groups;
		bool operator()( const DesyncReportRow &a, const DesyncReportRow &b ) const
		{
			if( a.reported != b.reported )
				return a.reported ? true : false;
			if( a.reported )
			{
				const Group &ga = groups->find( a.crc )->second;
				const Group &gb = groups->find( b.crc )->second;
				if( ga.count != gb.count )
					return ga.count < gb.count;
				if( ga.firstSlot != gb.firstSlot )
					return ga.firstSlot < gb.firstSlot;
			}
			return a.slot < b.slot;
		}
	};
	Order order = { &groups };
	std::stable_sort( rows.begin(), rows.end(), order );
	return s;
}

/// Put `count` columns of widths `w` side by side, centred in `avail` pixels, and write each one's x
/// (from the left of `avail`) to `x`. The gap spreads the columns over the room there is, between
/// minGap and maxGap - so a long header line widening the box does not pull the table apart. Below
/// minGap, column `give` narrows by exactly the overflow (its text then ends in "..."), to no less
/// than floorW; past that the table overhangs both edges equally. Returns the gap used.
inline Int DesyncReportLayColumns( Int count, Int *w, Int give, Int avail, Int minGap, Int maxGap, Int floorW, Int *x )
{
	if( count <= 0 )
		return 0;
	Int natural = 0;
	for( Int c = 0; c < count; ++c )
		natural += w[c];
	const Int gaps = count - 1;
	Int gap = maxGap;
	if( natural + gaps * minGap > avail )
	{
		gap = minGap;
		w[ give ] -= natural + gaps * minGap - avail;
		if( w[ give ] < floorW )
			w[ give ] = floorW;
	}
	else if( gaps > 0 && ( avail - natural ) / gaps < maxGap )
	{
		gap = ( avail - natural ) / gaps;
	}
	Int total = gaps * gap;
	for( Int c = 0; c < count; ++c )
		total += w[c];
	x[0] = ( avail - total ) / 2;
	for( Int c = 1; c < count; ++c )
		x[c] = x[c - 1] + w[c - 1] + gap;
	return gap;
}

/// How many characters of a name to show in a column `maxWidth` wide when `suffixWidth` pixels of
/// marker - the viewer's " (you)" - follow it. The marker is never cut: the name gives way and ends
/// in "..." (`ellipsisWidth`), as LobbyTitleFitLength fits any other untrusted line.
inline Int DesyncReportFitName( const WideChar *name, Int len, Int maxWidth, Int suffixWidth, Int ellipsisWidth,
																LobbyTitlePrefixWidthFunc prefixWidth, void *ctx )
{
	return LobbyTitleFitLength( name, len, maxWidth - suffixWidth, ellipsisWidth, prefixWidth, ctx );
}

/// UTF-8 for a stderr line, where a %ls conversion depends on the C locale and drops what it cannot
/// convert. WideChar is 16 bits on Windows: a surrogate pair becomes one 4-byte character, and a
/// lone surrogate (or anything past U+10FFFF) becomes U+FFFD rather than invalid UTF-8.
inline std::string DesyncReportUtf8( const WideChar *s )
{
	std::string out;
	for( ; s != nullptr && *s != 0; ++s )
	{
		UnsignedInt u = (UnsignedInt)*s;
		if( u >= 0xD800 && u <= 0xDBFF && (UnsignedInt)s[1] >= 0xDC00 && (UnsignedInt)s[1] <= 0xDFFF )
		{
			u = 0x10000 + ( ( u - 0xD800 ) << 10 ) + ( (UnsignedInt)s[1] - 0xDC00 );
			++s;
		}
		else if( ( u >= 0xD800 && u <= 0xDFFF ) || u > 0x10FFFF )
		{
			u = 0xFFFD;
		}
		if( u < 0x80 )
			out += (char)u;
		else if( u < 0x800 )
		{
			out += (char)( 0xC0 | ( u >> 6 ) );
			out += (char)( 0x80 | ( u & 0x3F ) );
		}
		else if( u < 0x10000 )
		{
			out += (char)( 0xE0 | ( u >> 12 ) );
			out += (char)( 0x80 | ( ( u >> 6 ) & 0x3F ) );
			out += (char)( 0x80 | ( u & 0x3F ) );
		}
		else
		{
			out += (char)( 0xF0 | ( u >> 18 ) );
			out += (char)( 0x80 | ( ( u >> 12 ) & 0x3F ) );
			out += (char)( 0x80 | ( ( u >> 6 ) & 0x3F ) );
			out += (char)( 0x80 | ( u & 0x3F ) );
		}
	}
	return out;
}

class GameWindow;

/// The desync window: MessageBox.wnd with the report laid out as a table (DesyncReport.cpp), and the
/// same report on stderr as [DESYNC] lines. Returns the box, as MessageBoxOk does.
GameWindow *DesyncReportShow( const std::map<Int, UnsignedInt> &playerCRCs, Int noticedFrame, Int divergedFrame );
