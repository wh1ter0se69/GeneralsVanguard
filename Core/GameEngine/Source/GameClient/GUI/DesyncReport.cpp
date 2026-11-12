/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

// FILE: DesyncReport.cpp //////////////////////////////////////////////////////////////////////////
// The desync window: MessageBox.wnd plus a per-player table drawn as one static text per cell, names in house colour.
// The plain rows are the body, so MessageBoxFit.h sizes the box; they are then blanked and the cells laid over them.
// The same report goes to stderr as [DESYNC] lines. Nothing here is in the SimID digest.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/GXTextColor.h"
#include <string>

#include "Common/GameCommon.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "GameClient/DesyncReport.h"
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/LobbySetupUi.h"
#include "GameClient/MessageBox.h"
#include "GameNetwork/NetworkInterface.h"

namespace
{

enum { COL_STATUS, COL_NAME, COL_SLOT, COL_CRC, COL_COUNT };

struct ReportLine
{
	UnicodeString cell[ COL_COUNT ];	///< as measured; a player's name cell is name + marker
	Bool isPlayer;					///< FALSE for the caption row
	Color nameColor;
	UnicodeString name;			///< the cleaned display name alone
	Bool you;								///< the viewer's own row: " (you)" follows the name
};

const WideChar *const YOU_MARKER = L" (you)";

Int prefixWidth( void *ctx, Int n )
{
	return ( (DisplayString *)ctx )->getWidth( n );
}

/// The name cell's text for a column `maxWidth` wide: the name, cut to "..." if it must be, and then
/// the marker, which is never cut (DesyncReportFitName).
UnicodeString fitNameCell( DisplayString *ds, const ReportLine &l, Int maxWidth )
{
	UnicodeString suffix( l.you ? YOU_MARKER : L"" );
	ds->setText( suffix );
	const Int suffixWidth = ds->getWidth();
	ds->setText( UnicodeString( L"..." ) );
	const Int ellipsisWidth = ds->getWidth();
	ds->setText( l.name );
	const Int keep = DesyncReportFitName( l.name.str(), l.name.getLength(), maxWidth, suffixWidth, ellipsisWidth,
		prefixWidth, ds );
	UnicodeString out = l.name;
	if( keep < l.name.getLength() )
	{
		out.truncateTo( keep );
		out.concat( L"..." );
	}
	out.concat( suffix );
	return out;
}

/// Glyph and word, never colour alone: it survives a screenshot read by someone who cannot tell red
/// from green, and a paste into a chat window as plain text.
const WideChar *verdictCell( DesyncVerdict v )
{
	switch( v )
	{
		case DESYNC_AGREES:		return L".  agrees";
		case DESYNC_DIFFERS:	return L"X  DIFFERS";
		case DESYNC_SPLIT:		return L"X  split";
		default:							return L"?  no report";
	}
}

const char *verdictWord( DesyncVerdict v )
{
	switch( v )
	{
		case DESYNC_AGREES:		return "agrees";
		case DESYNC_DIFFERS:	return "DIFFERS";
		case DESYNC_SPLIT:		return "split";
		default:							return "no-report";
	}
}

/// A display name arrives off the wire. Drop what could break a row or reorder the rest of it.
UnicodeString cleanName( const UnicodeString &name )
{
	UnicodeString out;
	for( const WideChar *c = name.str(); *c != 0; ++c )
		if( LobbyTitleCharAllowed( *c ) )
			out.concat( *c );
	return out;
}

void measure( DisplayString *ds, const UnicodeString &text, Int wrap, Int *w, Int *h )
{
	ds->setText( text );
	ds->setWordWrap( wrap );
	ds->getSize( w, h );
}

/// One cell: a static text with the body's face and drop shadow, no background, no input.
GameWindow *makeCell( GameWindow *parent, GameWindow *body, Int x, Int y, Int w, Int h,
											const UnicodeString &text, Color color )
{
	WinInstanceData instData;
	TextData textData;
	instData.init();
	memset( &textData, 0, sizeof( textData ) );		// left-aligned at the rect's own x and y
	instData.m_style = GWS_STATIC_TEXT;
	instData.m_status = WIN_STATUS_ENABLED | WIN_STATUS_NO_INPUT | WIN_STATUS_NO_FOCUS;

	// defaultVisual FALSE: every colour slot stays undefined, so only the glyphs draw - exactly
	// what the body itself draws (MessageBox.wnd gives it a zero-alpha background).
	GameWindow *cell = TheWindowManager->gogoGadgetStaticText( parent, instData.m_status, x, y, w, h,
		&instData, &textData, body->winGetFont(), FALSE );
	if( cell == nullptr )
		return nullptr;
	cell->winSetEnabledTextColors( color, body->winGetEnabledTextBorderColor() );
	GadgetStaticTextSetText( cell, text );		// every name is already cleaned and fitted
	return cell;
}

/// Lay the table over the empty lines the fit reserved for it. FALSE leaves the plain rows.
Bool layCells( GameWindow *box, const UnicodeString &header, const UnicodeString &footer,
							 const std::vector<ReportLine> &lines )
{
	if( box == nullptr || TheWindowManager == nullptr || TheDisplayStringManager == nullptr || TheNameKeyGenerator == nullptr )
		return FALSE;
	GameWindow *body = TheWindowManager->winGetWindowFromId( box,
		TheNameKeyGenerator->nameToKey( "MessageBox.wnd:StaticTextMessage" ) );
	GameWindow *parent = ( body != nullptr ) ? body->winGetParent() : nullptr;
	// The headless peer's dummy windows carry no TextData (GameWindowDummy::winGetUserData).
	TextData *tData = ( body != nullptr ) ? (TextData *)body->winGetUserData() : nullptr;
	GameFont *font = ( body != nullptr ) ? body->winGetFont() : nullptr;
	if( parent == nullptr || tData == nullptr || tData->text == nullptr || font == nullptr )
		return FALSE;

	DisplayString *ds = TheDisplayStringManager->newDisplayString();
	if( ds == nullptr )
		return FALSE;
	ds->setFont( font );

	Int bx = 0, by = 0, bw = 0, bh = 0;
	body->winGetPosition( &bx, &by );		// relative to the box, as the cells will be
	body->winGetSize( &bw, &bh );
	const Int wrap = bw - 10;							// drawStaticTextText's wrap width

	Int w = 0, charH = 0, headerH = 0;
	measure( ds, UnicodeString( L"M" ), 0, &w, &charH );
	measure( ds, header, wrap, &w, &headerH );
	if( charH <= 0 )
	{
		TheDisplayStringManager->freeDisplayString( ds );
		return FALSE;
	}
	const Int headerLines = headerH / charH;

	Int colW[ COL_COUNT ] = { 0, 0, 0, 0 };
	for( size_t r = 0; r < lines.size(); ++r )
		for( Int c = 0; c < COL_COUNT; ++c )
		{
			ds->setText( lines[r].cell[c] );
			const Int cw = ds->getWidth();
			if( cw > colW[c] )
				colW[c] = cw;
		}

	// Spread the columns over the width the box was given; when they do not fit, the name column
	// gives way and its names end in "..." (DesyncReport.h).
	Int colX[ COL_COUNT ];
	DesyncReportLayColumns( COL_COUNT, colW, COL_NAME, bw - 2 * tData->leftMargin, charH / 2, charH * 2,
		charH * 2, colX );
	for( Int c = 0; c < COL_COUNT; ++c )
		colX[c] += bx + tData->leftMargin;

	// Swap the rows for empty lines: the header, the blank under it, the table, the blank, the footer
	// keep the line count the fit measured. Then find the first line exactly as drawStaticTextText
	// will: the whole text centred in the control unless the fit anchored it to the top.
	UnicodeString text = header;
	text.concat( L"\n" );
	for( size_t r = 0; r < lines.size(); ++r )
		text.concat( L"\n" );
	text.concat( L"\n\n" );
	text.concat( footer );
	GadgetStaticTextSetText( body, text );
	Int textW = 0, textH = 0;
	tData->text->setWordWrap( wrap );
	tData->text->getSize( &textW, &textH );
	const Int top = tData->centeredVertically ? ( by + bh / 2 - textH / 2 ) : ( by + tData->topMargin );

	const Color textColor = body->winGetEnabledTextColor();
	for( size_t r = 0; r < lines.size(); ++r )
	{
		const Int y = top + ( headerLines + 1 + (Int)r ) * charH;
		if( y + charH > by + bh )
			break;		// the screen clamp cut the body short; its last lines are lost, as before
		for( Int c = 0; c < COL_COUNT; ++c )
		{
			if( lines[r].cell[c].isEmpty() )
				continue;
			// Width +10: the static wraps at its width minus 10; +2 so a cell exactly its text's width
			// is not broken by the `>=` in the wrap test. Height +2: the drop shadow is drawn a pixel
			// below the glyphs and the cell clips to its own rect.
			const Bool name = ( c == COL_NAME && lines[r].isPlayer ) ? TRUE : FALSE;
			makeCell( parent, body, colX[c], y, colW[c] + 12, charH + 2,
				name ? fitNameCell( ds, lines[r], colW[c] ) : lines[r].cell[c], name ? lines[r].nameColor : textColor );
		}
	}
	TheDisplayStringManager->freeDisplayString( ds );
	return TRUE;
}

} // namespace

//-------------------------------------------------------------------------------------------------
GameWindow *DesyncReportShow( const std::map<Int, UnsignedInt> &playerCRCs, Int noticedFrame, Int divergedFrame )
{
	// ITERATE THE SLOTS: the CRC table is keyed by the reporter's network SLOT (GameLogic::onLogicCrc).
	// (observer seats) No longer by Player index: observer slots all share
	// the ReplayObserver, so a Player-keyed table held one report for every observer and named none of them.
	std::vector<DesyncReportRow> rows;
	for( Int slot = 0; slot < MAX_SLOTS; ++slot )
	{
		std::map<Int, UnsignedInt>::const_iterator it = playerCRCs.find( slot );
		const Player *player = ThePlayerList ? ThePlayerList->getPlayerFromSlotIndex( slot ) : nullptr;
		DesyncReportRow row = { player ? player->getPlayerIndex() : -1, slot, FALSE, 0, DESYNC_NO_REPORT };
		if( it != playerCRCs.end() )
		{
			row.reported = TRUE;
			row.crc = it->second;
		}
		else if( TheNetwork == nullptr || !TheNetwork->isPlayerConnected( slot ) )
		{
			// Not expected to report: AI and empty slots never do. The same test the validator uses to
			// decide who counts.
			continue;
		}
		rows.push_back( row );
	}
	const DesyncReportSummary sum = DesyncReportJudge( rows );

	// The header. Every format() here takes numbers and literals only - never a name - because
	// UnicodeString::format is a silent no-op when vswprintf fails, and a name is what can fail it.
	const Int secs = ( noticedFrame > 0 ) ? noticedFrame / LOGICFRAMES_PER_SECOND : 0;
	UnicodeString header;
	header.format( L"Frame %d (%d:%02d game time) - the split began around frame %d.\n",
		noticedFrame, secs / 60, secs % 60, divergedFrame );
	UnicodeString line;
	if( sum.haveMajority )
	{
		const Int dissenters = sum.reporters - sum.bestCount;
		line.format( L"%d of %d players agree. %d %ls.", sum.bestCount, sum.reporters, dissenters,
			dissenters == 1 ? L"does not" : L"do not" );
	}
	else
	{
		line.format( L"No majority - the %d players that reported split into %d different games.",
			sum.reporters, sum.groups );
	}
	header.concat( line );
	if( sum.silent > 0 )
	{
		line.format( L"\n%d more had not reported yet - that is not evidence against them.", sum.silent );
		header.concat( line );
	}
	const UnicodeString footer( L"The simulations no longer agree, so this game is ending." );

	fprintf( stderr, "[DESYNC] report frame=%d split~=%d reporters=%d silent=%d games=%d majority=%d\n",
		noticedFrame, divergedFrame, sum.reporters, sum.silent, sum.groups, sum.haveMajority ? sum.bestCount : 0 );

	const Int localSlot = ( TheNetwork != nullptr ) ? (Int)TheNetwork->getLocalPlayerID() : -1;
	std::vector<ReportLine> lines;
	ReportLine caption;
	caption.cell[ COL_NAME ] = L"Player";
	caption.cell[ COL_SLOT ] = L"Slot";
	caption.cell[ COL_CRC ] = L"CRC";
	caption.isPlayer = FALSE;
	caption.nameColor = 0;
	caption.you = FALSE;
	lines.push_back( caption );

	// Plain rows for the fit to size the box by, and the report as it reads if no cell can be built.
	UnicodeString plain = header;
	plain.concat( L"\n\nPlayer   Slot   CRC" );
	for( size_t r = 0; r < rows.size(); ++r )
	{
		Player *player = ( rows[r].player >= 0 && ThePlayerList != nullptr ) ? ThePlayerList->getNthPlayer( rows[r].player ) : nullptr;
		// The SLOT's own name - an observer's, never the ReplayObserver every observer slot shares.
		const UnicodeString name = cleanName( TheNetwork != nullptr ? TheNetwork->getPlayerName( rows[r].slot )
			: ( player != nullptr ? player->getPlayerDisplayName() : UnicodeString::TheEmptyString ) );
		const Bool you = ( rows[r].slot == localSlot ) ? TRUE : FALSE;

		ReportLine l;
		l.isPlayer = TRUE;
		l.nameColor = ( player != nullptr ) ? gxPlayerTextColor( player->getPlayerColor() ) : GameMakeColor( 255, 255, 255, 255 );
		l.cell[ COL_STATUS ] = verdictCell( rows[r].verdict );
		l.name = name;
		l.you = you;
		l.cell[ COL_NAME ] = name;
		if( you )
			l.cell[ COL_NAME ].concat( YOU_MARKER );
		if( rows[r].slot >= 0 )
			l.cell[ COL_SLOT ].format( L"%d", rows[r].slot + 1 );
		else
			l.cell[ COL_SLOT ] = L"-";
		if( rows[r].reported )
			l.cell[ COL_CRC ].format( L"%08X", rows[r].crc );
		else
			l.cell[ COL_CRC ] = L"--------";
		lines.push_back( l );

		plain.concat( L"\n" );
		for( Int c = 0; c < COL_COUNT; ++c )
		{
			if( c > 0 )
				plain.concat( L"   " );
			plain.concat( l.cell[c] );
		}

		fprintf( stderr, "[DESYNC] row slot=%s player=%d verdict=%s crc=%s you=%d name=%s\n",
			DesyncReportUtf8( l.cell[ COL_SLOT ].str() ).c_str(), rows[r].player, verdictWord( rows[r].verdict ),
			DesyncReportUtf8( l.cell[ COL_CRC ].str() ).c_str(), you ? 1 : 0, DesyncReportUtf8( name.str() ).c_str() );
	}
	fflush( stderr );
	plain.concat( L"\n\n" );
	plain.concat( footer );

	// MessageBoxOk rather than Menus/CRCMismatch.wnd: that layout has a fixed body string. Default
	// size deliberately - gogoMessageBox rescales only when an explicit width AND height are given.
	GameWindow *box = MessageBoxOk( UnicodeString( L"Players are no longer in sync" ), plain, nullptr );
	layCells( box, header, footer, lines );
	return box;
}
