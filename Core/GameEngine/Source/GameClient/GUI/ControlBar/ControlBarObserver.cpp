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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// FILE: ControlBarObserver.cpp /////////////////////////////////////////////////
//-----------------------------------------------------------------------------
//
//                       Electronic Arts Pacific.
//
//                       Confidential Information
//                Copyright (C) 2002 - All Rights Reserved
//
//-----------------------------------------------------------------------------
//
//	created:	Aug 2002
//
//	Filename: 	ControlBarObserver.cpp
//
//	author:		Chris Huybregts
//
//	purpose:	All things related to the Observer Control bar, are in here.
//
//-----------------------------------------------------------------------------
///////////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
// SYSTEM INCLUDES ////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// USER INCLUDES //////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/GXTextColor.h"
#include <climits>

#include "Common/GameUtility.h"
#include "Common/NameKeyGenerator.h"
#include "Common/PlayerList.h"
#include "Common/Player.h"
#include "Common/PlayerTemplate.h"
#include "Common/KindOf.h"
#include "Common/Recorder.h"
#include "GameClient/ControlBar.h"
#include "GameClient/Display.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GameText.h"
#include "GameClient/GXObserverGrid.h"
#include "GameNetwork/NetworkDefs.h"
//-----------------------------------------------------------------------------
// DEFINES ////////////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
// GeneralsX: The list shows EVERY player. ControlBar.wnd has 8 buttons (WND_BUTTONS); a lobby
// seats MAX_SLOTS (14 today, 32 on the roadmap), and a 6v6 showed 8 of 12. Buttons and labels past the 8 are cloned from
// ButtonPlayer0 / StaticTextPlayer0 the first time a match needs them and laid out by GameClient/GXObserverGrid.h from
// the 8 authored rects, so retail and ControlBar Pro each keep their own look. The clones get NO name key: nameToKey on
// a new string mints a sequential id on this client alone, and an observer minting ids the players never do is a
// latent desync. Clicks are matched by window pointer instead.
enum { WND_BUTTONS = 8, MAX_BUTTONS = MAX_SLOTS };
static NameKeyType buttonPlayerID[WND_BUTTONS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID };
static NameKeyType staticTextPlayerID[WND_BUTTONS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID };
static GameWindow *ObserverPlayerInfoWindow = nullptr;
static GameWindow *ObserverPlayerListWindow = nullptr;

static GameWindow *buttonPlayer[MAX_BUTTONS] = {0};
static GameWindow *staticTextPlayer[MAX_BUTTONS] = {0};

// The authored geometry, parent-relative, measured once in initObserverControls before anything moves.
struct ObserverRect { Int x, y, w, h; };
static ObserverRect s_buttonRect[WND_BUTTONS];
static ObserverRect s_labelRect[WND_BUTTONS];
static ICoord2D s_listSize;
static GXObserverGridTemplate s_grid;
static Bool s_gridValid = FALSE;
// Retail draws each name beside its icon; ControlBar Pro parks the labels at 0,0 outside the list and shows icons only.
static Bool s_labelsInList = FALSE;
// Cells outside what the .wnd's art backs (ControlBar Pro's extra columns) sit on a plate in the layout's own label colour
// (Pro: 2,2,2,128) instead of on bare terrain. List-relative; empty when nothing grew.
static Int s_authoredRight = 0;
static IRegion2D s_plate;
static Color s_plateColor = 0;
static GameWinDrawFunc s_listDrawFunc = nullptr;

static void observerListDraw( GameWindow *window, WinInstanceData *instData )
{
	if( s_listDrawFunc != nullptr )
		s_listDrawFunc( window, instData );
	if( s_plate.hi.x <= s_plate.lo.x || TheDisplay == nullptr )
		return;
	Int x, y;
	window->winGetScreenPosition( &x, &y );
	TheDisplay->drawFillRect( x + s_plate.lo.x, y + s_plate.lo.y, s_plate.hi.x - s_plate.lo.x, s_plate.hi.y - s_plate.lo.y,
		s_plateColor );
}


static NameKeyType buttonCancelID = NAMEKEY_INVALID;

static GameWindow *winFlag = nullptr;
static GameWindow *winGeneralPortrait = nullptr;
// TheSuperHackers @tweak Allow idle worker selection for observers.
static GameWindow *buttonIdleWorker = nullptr;
static GameWindow *staticTextNumberOfUnits = nullptr;
static GameWindow *staticTextNumberOfBuildings = nullptr;
static GameWindow *staticTextNumberOfUnitsKilled = nullptr;
static GameWindow *staticTextNumberOfUnitsLost = nullptr;
static GameWindow *staticTextPlayerName = nullptr;

static NameKeyType s_replayObserverNameKey = NAMEKEY_INVALID;

//-----------------------------------------------------------------------------
// GRID ///////////////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

static Int roundToInt( Real v )
{
	return (Int)( v + 0.5f );
}

/** Measure the 8 authored buttons into s_grid. Leaves s_gridValid FALSE - the list then shows the first 8, as before -
	* for a layout this cannot read as a grid (a mod .wnd without the 8, or with them not in columns). */
static void measureObserverGrid()
{
	s_gridValid = FALSE;
	if( ObserverPlayerListWindow == nullptr )
		return;
	for( Int i = 0; i < WND_BUTTONS; ++i )
	{
		if( buttonPlayer[i] == nullptr || staticTextPlayer[i] == nullptr )
			return;
		buttonPlayer[i]->winGetPosition( &s_buttonRect[i].x, &s_buttonRect[i].y );
		buttonPlayer[i]->winGetSize( &s_buttonRect[i].w, &s_buttonRect[i].h );
		staticTextPlayer[i]->winGetPosition( &s_labelRect[i].x, &s_labelRect[i].y );
		staticTextPlayer[i]->winGetSize( &s_labelRect[i].w, &s_labelRect[i].h );
	}
	ObserverPlayerListWindow->winGetSize( &s_listSize.x, &s_listSize.y );

	const ObserverRect &b0 = s_buttonRect[0];
	const ObserverRect &l0 = s_labelRect[0];
	s_labelsInList = l0.x >= 0 && l0.y >= 0 && l0.x + l0.w <= s_listSize.x && l0.y + l0.h <= s_listSize.y;

	Int rows = 1;
	while( rows < WND_BUTTONS && abs( s_buttonRect[rows].x - b0.x ) <= 1 )
		++rows;
	s_grid.rows = rows;
	s_grid.cols = ( WND_BUTTONS + rows - 1 ) / rows;
	s_grid.cellH = b0.h;
	s_grid.cellW = s_labelsInList ? max( l0.x + l0.w, b0.x + b0.w ) - b0.x : b0.w;
	s_grid.pitchY = rows > 1 ? s_buttonRect[1].y - b0.y : 0;
	s_grid.pitchX = s_grid.cols > 1 ? s_buttonRect[rows].x - b0.x : b0.w + ( s_grid.pitchY - b0.h );

	// The panel may use what the .wnd gave the observer: the list window, and the info window that replaces it.
	Int listX, listY;
	ObserverPlayerListWindow->winGetScreenPosition( &listX, &listY );
	Int right = listX + s_listSize.x;
	if( ObserverPlayerInfoWindow != nullptr )
	{
		Int infoX, infoY, infoW, infoH;
		ObserverPlayerInfoWindow->winGetScreenPosition( &infoX, &infoY );
		ObserverPlayerInfoWindow->winGetSize( &infoW, &infoH );
		right = max( right, infoX + infoW );
	}
	s_grid.availW = right - listX - b0.x;
	s_grid.availH = ( rows - 1 ) * s_grid.pitchY + b0.h;

	s_authoredRight = 0;
	for( Int i = 0; i < WND_BUTTONS; ++i )
	{
		s_authoredRight = max( s_authoredRight, s_buttonRect[i].x + s_buttonRect[i].w );
		if( s_labelsInList )
			s_authoredRight = max( s_authoredRight, s_labelRect[i].x + s_labelRect[i].w );
	}
	s_plate.lo.x = s_plate.hi.x = 0;
	s_plateColor = staticTextPlayer[0]->winGetEnabledColor( 0 );
	if( ObserverPlayerListWindow->winGetDrawFunc() != observerListDraw )
	{
		s_listDrawFunc = ObserverPlayerListWindow->winGetDrawFunc();
		ObserverPlayerListWindow->winSetDrawFunc( observerListDraw );
	}

	s_gridValid = rows > 1 && s_grid.pitchX > 0 && s_grid.pitchY > 0 && s_grid.availW > 0;
}

/** One more button or label, the same gadget as the authored `tmpl`: art, font, colours, tooltip delay, owner (so its
	* clicks reach ControlBarObserverSystem). Created hidden, with no window id - see WND_BUTTONS. */
static GameWindow *cloneObserverWindow( GameWindow *tmpl, Bool isButton )
{
	Int x, y, w, h;
	tmpl->winGetPosition( &x, &y );
	tmpl->winGetSize( &w, &h );
	const UnsignedInt status = tmpl->winGetStatus() | WIN_STATUS_HIDDEN;
	WinInstanceData *instData = tmpl->winGetInstanceData();
	GameWindow *win = isButton
		? TheWindowManager->gogoGadgetPushButton( ObserverPlayerListWindow, status, x, y, w, h, instData,
				tmpl->winGetFont(), FALSE )
		: TheWindowManager->gogoGadgetStaticText( ObserverPlayerListWindow, status, x, y, w, h, instData,
				(TextData *)tmpl->winGetUserData(), tmpl->winGetFont(), FALSE );
	if( win != nullptr )
	{
		win->winSetWindowId( NAMEKEY_INVALID );
		BitClear( win->winGetInstanceData()->m_state, WIN_STATE_HILITED | WIN_STATE_SELECTED );
	}
	return win;
}

/** Make sure buttons exist for every player and place them: `groupSizes` are the players per team in display order
	* (see populateObserverList). Returns how many could be placed - fewer only if a window could not be created. Runs
	* when the list is populated, a context switch, never per frame. */
static Int layoutObserverList( const Int *groupSizes, Int groupCount )
{
	Int count = 0;
	for( Int g = 0; g < groupCount; ++g )
		count += groupSizes[g];
	if( !s_gridValid )
		return min( count, (Int)WND_BUTTONS );

	count = min( count, (Int)MAX_BUTTONS );
	for( Int k = WND_BUTTONS; k < count; ++k )
	{
		if( buttonPlayer[k] == nullptr )
			buttonPlayer[k] = cloneObserverWindow( buttonPlayer[0], TRUE );
		if( staticTextPlayer[k] == nullptr )
			staticTextPlayer[k] = cloneObserverWindow( staticTextPlayer[0], FALSE );
		if( buttonPlayer[k] == nullptr || staticTextPlayer[k] == nullptr )
		{
			count = k;
			break;
		}
	}

	const Bool authored = count <= WND_BUTTONS;
	const GXObserverGridLayout layout = gxObserverGridLayoutForGroups( s_grid, groupSizes, groupCount );
	const ObserverRect &b0 = s_buttonRect[0];
	const ObserverRect &l0 = s_labelRect[0];
	ICoord2D size = s_listSize;
	IRegion2D plate;
	plate.lo.x = plate.lo.y = INT_MAX;
	plate.hi.x = plate.hi.y = INT_MIN;
	Int group = 0, inGroup = 0;
	for( Int k = 0; k < count; ++k, ++inGroup )
	{
		while( group < groupCount - 1 && inGroup >= groupSizes[group] )
		{
			++group;
			inGroup = 0;
		}
		ObserverRect b, l;
		if( authored )
		{
			b = s_buttonRect[k];
			l = s_labelRect[k];
		}
		else
		{
			const GXObserverGridCell cell = layout.byGroup
				? gxObserverGridCellAt( layout, s_grid, group, inGroup )
				: gxObserverGridCell( layout, s_grid, k );
			b.x = b0.x + cell.x;
			b.y = b0.y + cell.y;
			b.w = roundToInt( b0.w * layout.scale );
			b.h = roundToInt( b0.h * layout.scale );
			l.x = b.x + roundToInt( ( l0.x - b0.x ) * layout.scale );
			l.y = b.y + roundToInt( ( l0.y - b0.y ) * layout.scale );
			l.w = roundToInt( l0.w * layout.scale );
			l.h = roundToInt( l0.h * layout.scale );
		}
		buttonPlayer[k]->winSetPosition( b.x, b.y );
		buttonPlayer[k]->winSetSize( b.w, b.h );
		// Keep the authored right-hand margin (list edge past the template's rightmost cell), so the list window - which is
		// what hit-testing descends through - covers every button it now holds and no more: in retail's two-column box a
		// margin taken from button 0 grew the input-blocking list over RightHUD (980 -> 1253 px wide).
		size.x = max( size.x, b.x + b.w + max( 0, s_listSize.x - s_authoredRight ) );
		size.y = max( size.y, b.y + b.h );
		if( b.x + b.w > s_authoredRight )
		{
			plate.lo.x = min( plate.lo.x, b.x );
			plate.lo.y = min( plate.lo.y, b.y );
			plate.hi.x = max( plate.hi.x, b.x + b.w );
			plate.hi.y = max( plate.hi.y, b.y + b.h );
		}
		if( s_labelsInList )
		{
			staticTextPlayer[k]->winSetPosition( l.x, l.y );
			staticTextPlayer[k]->winSetSize( l.w, l.h );
			size.x = max( size.x, l.x + l.w );
			size.y = max( size.y, l.y + l.h );
			if( l.x + l.w > s_authoredRight )
			{
				plate.lo.x = min( plate.lo.x, l.x );
				plate.lo.y = min( plate.lo.y, l.y );
				plate.hi.x = max( plate.hi.x, l.x + l.w );
				plate.hi.y = max( plate.hi.y, l.y + l.h );
			}
		}
	}
	ObserverPlayerListWindow->winSetSize( size.x, size.y );
	// The plate: what grew past the .wnd's own column, one row gap around it.
	const Int gap = max( 1, s_grid.pitchY - b0.h );
	// Only where the art backs a single strip (ControlBar Pro, labels parked): a retail grid shrinks inside its own art, and a
	// third retail column may poke past the authored labels by a few pixels without needing a plate.
	s_plate.lo.x = s_plate.hi.x = 0;
	if( plate.hi.x > plate.lo.x && !s_labelsInList )
	{
		s_plate.lo.x = max( 0, plate.lo.x - gap );
		s_plate.lo.y = max( 0, plate.lo.y - gap );
		s_plate.hi.x = min( size.x, plate.hi.x + gap );
		s_plate.hi.y = min( size.y, plate.hi.y + gap );
	}
	return count;
}

/** Fill cell k with player p: general icon, a 1px frame in the player's colour (the border the command buttons use),
	* the name as tooltip, and - where the layout shows names - the coloured label. */
static void showObserverButton( Int k, Player *p, const UnicodeString &label )
{
	const Color playerColor = p->getPlayerColor();
	GadgetButtonSetData( buttonPlayer[k], (void *)p );
	GadgetButtonSetEnabledImage( buttonPlayer[k], p->getPlayerTemplate()->getEnabledImage() );
	//GadgetButtonSetHiliteImage( buttonPlayer[k], p->getPlayerTemplate()->getHiliteImage() );
	//GadgetButtonSetHiliteSelectedImage( buttonPlayer[k], p->getPlayerTemplate()->getPushedImage() );
	//GadgetButtonSetDisabledImage( buttonPlayer[k], p->getPlayerTemplate()->getDisabledImage() );
	GadgetButtonSetBorder( buttonPlayer[k], playerColor );
	buttonPlayer[k]->winSetTooltip( p->getPlayerDisplayName() );
	buttonPlayer[k]->winHide( FALSE );
	buttonPlayer[k]->winSetStatus( WIN_STATUS_USE_OVERLAY_STATES );
	// A defeated player reads as defeated: EA's disabled button, which overlay-state buttons draw in grayscale.
	buttonPlayer[k]->winEnable( p->isPlayerActive() );

	staticTextPlayer[k]->winSetEnabledTextColors( gxPlayerTextColor( playerColor ), GameMakeColor( 0, 0, 0, 255 ) );
	// ControlBar Pro parks its labels outside the list; showing them drew nothing useful.
	staticTextPlayer[k]->winHide( s_gridValid && !s_labelsInList );
	// The name is another player's: fitted to the (possibly shrunk) label, with nothing that can reorder the line.
	GadgetStaticTextSetUntrustedLine( staticTextPlayer[k], label );
}

/** Grey out whoever has been defeated since the list was filled. Called every half second while the list is up; a
	* bit test per shown button unless someone was just defeated. */
static void refreshObserverListDefeats()
{
	for( Int k = 0; k < MAX_BUTTONS && buttonPlayer[k] != nullptr; ++k )
	{
		if( buttonPlayer[k]->winIsHidden() )
			continue;
		const Player *p = static_cast<const Player *>( GadgetButtonGetData( buttonPlayer[k] ) );
		const Bool active = p != nullptr && p->isPlayerActive();
		if( active != BitIsSet( buttonPlayer[k]->winGetStatus(), WIN_STATUS_ENABLED ) )
			buttonPlayer[k]->winEnable( active );
	}
}

static void hideObserverButtonsFrom( Int k )
{
	for( ; k < MAX_BUTTONS; ++k )
	{
		if( buttonPlayer[k] != nullptr )
			buttonPlayer[k]->winHide( TRUE );
		if( staticTextPlayer[k] != nullptr )
			staticTextPlayer[k]->winHide( TRUE );
	}
}

//-----------------------------------------------------------------------------
// PUBLIC FUNCTIONS ///////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------


void ControlBar::initObserverControls()
{
	ObserverPlayerInfoWindow = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:ObserverPlayerInfoWindow"));
	ObserverPlayerListWindow = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:ObserverPlayerListWindow"));

	for (Int i = 0; i < WND_BUTTONS; i++)
	{
		AsciiString tmpString;
		tmpString.format("ControlBar.wnd:ButtonPlayer%d", i);
		buttonPlayerID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		buttonPlayer[i] = TheWindowManager->winGetWindowFromId( ObserverPlayerListWindow, buttonPlayerID[i] );
		tmpString.format("ControlBar.wnd:StaticTextPlayer%d", i);
		staticTextPlayerID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		staticTextPlayer[i] = TheWindowManager->winGetWindowFromId( ObserverPlayerListWindow, staticTextPlayerID[i] );
	}
	// A new ControlBar.wnd (InGameUI::recreateControlBar, on a resolution change) took the clones down with the old one.
	for (Int i = WND_BUTTONS; i < MAX_BUTTONS; i++)
	{
		buttonPlayer[i] = nullptr;
		staticTextPlayer[i] = nullptr;
	}
	measureObserverGrid();

	staticTextNumberOfUnits = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:StaticTextNumberOfUnits"));
	staticTextNumberOfBuildings = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:StaticTextNumberOfBuildings"));
	staticTextNumberOfUnitsKilled = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:StaticTextNumberOfUnitsKilled"));
	staticTextNumberOfUnitsLost = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:StaticTextNumberOfUnitsLost"));
	staticTextPlayerName = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:StaticTextPlayerName"));
	winFlag = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:WinFlag"));
	winGeneralPortrait = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:WinGeneralPortrait"));
	buttonIdleWorker = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("ControlBar.wnd:ButtonIdleWorker"));

	buttonCancelID = TheNameKeyGenerator->nameToKey("ControlBar.wnd:ButtonCancel");

	s_replayObserverNameKey = TheNameKeyGenerator->nameToKey("ReplayObserver");
}

//-------------------------------------------------------------------------------------------------
void ControlBar::setObserverLookAtPlayer(Player *player)
{
	if (player != nullptr && player == ThePlayerList->findPlayerWithNameKey(s_replayObserverNameKey))
	{
		// Looking at the observer. Treat as not looking at player.
		m_observerLookAtPlayer = nullptr;
	}
	else
	{
		m_observerLookAtPlayer = player;
	}
}

//-------------------------------------------------------------------------------------------------
void ControlBar::setObservedPlayer(Player *player)
{
	if (player != nullptr && player == ThePlayerList->findPlayerWithNameKey(s_replayObserverNameKey))
	{
		// Looking at the observer. Treat as not observing player.
		m_observedPlayer = nullptr;
	}
	else
	{
		m_observedPlayer = player;
	}
}

//-------------------------------------------------------------------------------------------------
/** System callback for the ControlBarObserverSystem */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType ControlBarObserverSystem( GameWindow *window, UnsignedInt msg,
																			 WindowMsgData mData1, WindowMsgData mData2 )
{
	static NameKeyType buttonCommunicator = NAMEKEY_INVALID;

	switch( msg )
	{
		// --------------------------------------------------------------------------------------------
		case GWM_CREATE:
		{
				break;

		}

		//---------------------------------------------------------------------------------------------
		case GBM_MOUSE_ENTERING:
		case GBM_MOUSE_LEAVING:
		{
			break;
		}

		//---------------------------------------------------------------------------------------------
		case GBM_SELECTED:
		case GBM_SELECTED_RIGHT:
		{
			GameWindow *control = (GameWindow *)mData1;

			Int controlID = control->winGetWindowId();
			if( controlID == buttonCancelID)
			{
				rts::changeObservedPlayer(nullptr);

				ObserverPlayerInfoWindow->winHide(TRUE);
				ObserverPlayerListWindow->winHide(FALSE);
				buttonIdleWorker->winHide(TRUE);
				TheControlBar->populateObserverList();
			}

			for(Int i = 0; i <MAX_BUTTONS; ++i)
			{
				if( buttonPlayer[i] != nullptr && control == buttonPlayer[i])
				{
					Player* player = static_cast<Player*>(GadgetButtonGetData(buttonPlayer[i]));
					rts::changeObservedPlayer(player);

					ObserverPlayerInfoWindow->winHide(FALSE);
					ObserverPlayerListWindow->winHide(TRUE);

					if(TheControlBar->getObserverLookAtPlayer())
						TheControlBar->populateObserverInfoWindow();

					return MSG_HANDLED;
				}
			}

		//	if( controlID == buttonCommunicator && TheGameLogic->getGameMode() == GAME_INTERNET )
	/*
		{
				popupCommunicatorLayout = TheWindowManager->winCreateLayout( "Menus/PopupCommunicator.wnd" );
				popupCommunicatorLayout->runInit();
				popupCommunicatorLayout->hide( FALSE );
				popupCommunicatorLayout->bringForward();
			}
*/

			break;

		}

		//---------------------------------------------------------------------------------------------
		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;

}

//-----------------------------------------------------------------------------
// PRIVATE FUNCTIONS //////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

void ControlBar::populateObserverList()
{
	Player *players[MAX_BUTTONS];
	UnicodeString labels[MAX_BUTTONS];
	Int groupSizes[MAX_BUTTONS];
	Int count = 0, groupCount = 0, i;
	if(TheRecorder->isMultiplayer())
	{
		// GeneralsX: Every non-observer player, ordered by team so each team keeps together in
		// the grid (a lobby puts teams in any slots), slot order within a team; the players with no team come last.
		Int teams[MAX_BUTTONS];
		for (i = 0; i < MAX_SLOTS && count < MAX_BUTTONS; ++i)
		{
			Player *p = ThePlayerList->getPlayerFromSlotIndex(i);
			if(p == nullptr || p->isPlayerObserver())
				continue;

			const GameSlot *slot = TheGameInfo->getConstSlot(i);
			AsciiString teamStr;
			teamStr.format("Team:%d", slot->getTeamNumber() + 1);
			if (slot->isAI() && slot->getTeamNumber() == -1)
				teamStr = "Team:AI";

			UnicodeString text;
			text.format(TheGameText->fetchGameOnly("CONTROLBAR:ObsPlayerLabel"), p->getPlayerDisplayName().str(),
				TheGameText->fetch(teamStr).str());

			const Int team = slot->getTeamNumber() >= 0 ? slot->getTeamNumber() : MAX_SLOTS;
			Int at = count;
			for (; at > 0 && teams[at - 1] > team; --at)
			{
				players[at] = players[at - 1];
				labels[at] = labels[at - 1];
				teams[at] = teams[at - 1];
			}
			players[at] = p;
			labels[at] = text;
			teams[at] = team;
			++count;
		}
		for (Int k = 0; k < count; ++k)
		{
			if (k == 0 || teams[k] != teams[k - 1])
				groupSizes[groupCount++] = 0;
			++groupSizes[groupCount - 1];
		}
	}
	else
	{
		for(i =0; i < MAX_PLAYER_COUNT; ++i)
		{
			Player *p = ThePlayerList->getNthPlayer(i);
			if(p && !p->isPlayerObserver() && p->getPlayerType() == PLAYER_HUMAN)
			{
				players[0] = p;
				labels[0] = p->getPlayerDisplayName();
				groupSizes[0] = 1;
				count = groupCount = 1;
				break;
			}
		}
	}

	count = layoutObserverList(groupSizes, groupCount);
	for (Int k = 0; k < count; ++k)
		showObserverButton(k, players[k], labels[k]);
	hideObserverButtonsFrom(count);
}

void ControlBar::populateObserverInfoWindow ()
{
	if(ObserverPlayerInfoWindow->winIsHidden())
	{
		refreshObserverListDefeats();	// GeneralsX: the list is up instead
		return;
	}

	if( !m_observerLookAtPlayer )
	{
		ObserverPlayerInfoWindow->winHide(TRUE);
		ObserverPlayerListWindow->winHide(FALSE);
		buttonIdleWorker->winHide(TRUE);
		populateObserverList();
		return;
	}

	UnicodeString uString;
	KindOfMaskType mask,clearmask;
	mask.set(KINDOF_SCORE);
	clearmask.set(KINDOF_STRUCTURE);

	uString.format(L"%d",m_observerLookAtPlayer->countObjects(mask,clearmask));
	GadgetStaticTextSetText(staticTextNumberOfUnits, uString);

	Int numBuildings = 0;
	mask.clear();
	mask.set(KINDOF_SCORE);
	mask.set(KINDOF_STRUCTURE);
	clearmask.clear();
	numBuildings = m_observerLookAtPlayer->countObjects(mask,clearmask);
	mask.clear();
	mask.set(KINDOF_SCORE_CREATE);
	mask.set(KINDOF_STRUCTURE);
	numBuildings += m_observerLookAtPlayer->countObjects(mask,clearmask);
	mask.clear();
	mask.set(KINDOF_SCORE_DESTROY);
	mask.set(KINDOF_STRUCTURE);
	numBuildings += m_observerLookAtPlayer->countObjects(mask,clearmask);
	uString.format(L"%d",numBuildings);
	GadgetStaticTextSetText(staticTextNumberOfBuildings, uString);
	uString.format(L"%d",m_observerLookAtPlayer->getScoreKeeper()->getTotalUnitsDestroyed());
	GadgetStaticTextSetText(staticTextNumberOfUnitsKilled, uString);
	uString.format(L"%d",m_observerLookAtPlayer->getScoreKeeper()->getTotalUnitsLost());
	GadgetStaticTextSetText(staticTextNumberOfUnitsLost, uString);
	GadgetStaticTextSetText(staticTextPlayerName, m_observerLookAtPlayer->getPlayerDisplayName());
	Color color = m_observerLookAtPlayer->getPlayerColor();
	staticTextPlayerName->winSetEnabledTextColors(gxPlayerTextColor( color ), GameMakeColor(0,0,0,255));
	winFlag->winSetEnabledImage(0, m_observerLookAtPlayer->getPlayerTemplate()->getFlagWaterMarkImage());
	winGeneralPortrait->winHide(FALSE);
	buttonIdleWorker->winHide(FALSE);
}
