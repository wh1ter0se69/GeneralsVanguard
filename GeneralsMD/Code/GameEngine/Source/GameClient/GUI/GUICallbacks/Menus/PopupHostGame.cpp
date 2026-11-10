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

// FILE: PopupHostGame.cpp /////////////////////////////////////////////////
//-----------------------------------------------------------------------------
//
//                       Electronic Arts Pacific.
//
//                       Confidential Information
//                Copyright (C) 2002 - All Rights Reserved
//
//-----------------------------------------------------------------------------
//
//	created:	Jul 2002
//
//	Filename: 	PopupHostGame.cpp
//
//	author:		Chris Huybregts
//
//	purpose:	Contains the Callbacks for the Host Game Popus
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

#include "Common/GlobalData.h"
#include "Common/NameKeyGenerator.h"
#include "Common/version.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/Gadget.h"
#include "GameClient/GameText.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GadgetCheckBox.h"
#include "GameClient/GadgetComboBox.h"
#include "GameClient/GadgetListBox.h"
#include "GameNetwork/GameSpy/GSConfig.h"
#include "GameNetwork/GameSpy/PeerDefs.h"
#include "GameNetwork/GameSpy/PeerThread.h"
#include "GameNetwork/GameSpyOverlay.h"

#include "GameNetwork/GameSpy/LadderDefs.h"
#include "Common/CustomMatchPreferences.h"
#include "Common/LadderPreferences.h"
#include "Common/UserPreferences.h"		// LANPreferences: the relay room reads Limit Armies from it
#include "GameClient/GUICallbacks.h"		// GXRelayLobbyIsActive / GXRelayHostGameWithOptions
#include "GameClient/GadgetStaticText.h"	// the confirm-password label is set from code

//-----------------------------------------------------------------------------
// DEFINES ////////////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

static NameKeyType parentPopupID = NAMEKEY_INVALID;
static NameKeyType textEntryGameNameID = NAMEKEY_INVALID;
static NameKeyType buttonCreateGameID = NAMEKEY_INVALID;
static NameKeyType checkBoxAllowObserversID = NAMEKEY_INVALID;
static NameKeyType textEntryGameDescriptionID = NAMEKEY_INVALID;
static NameKeyType buttonCancelID = NAMEKEY_INVALID;
static NameKeyType textEntryLadderPasswordID = NAMEKEY_INVALID;
static NameKeyType comboBoxLadderNameID = NAMEKEY_INVALID;
static NameKeyType textEntryGamePasswordID = NAMEKEY_INVALID;
// GeneralsX: Added by !GXUI.big; absent on a stock archive, so every use
// is guarded and their absence means "no confirmation required" rather than "cannot host".
static NameKeyType textEntryGamePasswordConfirmID = NAMEKEY_INVALID;
static NameKeyType staticTextConfirmPasswordID = NAMEKEY_INVALID;
static NameKeyType checkBoxLimitArmiesID = NAMEKEY_INVALID;
static NameKeyType checkBoxUseStatsID = NAMEKEY_INVALID;

static GameWindow *parentPopup = nullptr;
static GameWindow *textEntryGameName = nullptr;
static GameWindow *buttonCreateGame = nullptr;
static GameWindow *checkBoxAllowObservers = nullptr;
static GameWindow *textEntryGameDescription = nullptr;
static GameWindow *buttonCancel = nullptr;
static GameWindow *comboBoxLadderName = nullptr;
static GameWindow *textEntryLadderPassword = nullptr;
static GameWindow *textEntryGamePassword = nullptr;
static GameWindow *textEntryGamePasswordConfirm = nullptr;
static GameWindow *staticTextConfirmPassword = nullptr;
static GameWindow *checkBoxLimitArmies = nullptr;
static GameWindow *checkBoxUseStats = nullptr;


void createGame();

//-----------------------------------------------------------------------------
// PUBLIC FUNCTIONS ///////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

// Ladders --------------------------------------------------------------------------------

static bool isPopulatingLadderBox = false;

void CustomMatchHideHostPopup(Bool hide)
{
	if (!parentPopup)
		return;

	parentPopup->winHide( hide );
}

void HandleCustomLadderSelection(Int ladderID)
{
	if (!parentPopup)
		return;

	CustomMatchPreferences pref;

	if (ladderID == 0)
	{
		pref.setLastLadder(AsciiString::TheEmptyString, 0);
		pref.write();
		return;
	}

	const LadderInfo *info = TheLadderList->findLadderByIndex(ladderID);
	if (!info)
	{
		pref.setLastLadder(AsciiString::TheEmptyString, 0);
	}
	else
	{
		pref.setLastLadder(info->address, info->port);
	}

	pref.write();
}

void PopulateCustomLadderListBox( GameWindow *win )
{
	if (!parentPopup || !win)
		return;

	isPopulatingLadderBox = true;

	CustomMatchPreferences pref;

	Color specialColor = GameSpyColor[GSCOLOR_MAP_SELECTED];
	Color normalColor = GameSpyColor[GSCOLOR_MAP_UNSELECTED];
	Color favoriteColor = GameSpyColor[GSCOLOR_MAP_UNSELECTED];
	Color localColor = GameSpyColor[GSCOLOR_MAP_UNSELECTED];
	Int index;
	GadgetListBoxReset( win );

	std::set<const LadderInfo *> usedLadders;

	// start with "No Ladder"
	index = GadgetListBoxAddEntryText( win, TheGameText->fetch("GUI:NoLadder"), normalColor, -1 );
	GadgetListBoxSetItemData( win, nullptr, index );

	// add the last ladder
	Int selectedPos = 0;
	AsciiString lastLadderAddr = pref.getLastLadderAddr();
	UnsignedShort lastLadderPort = pref.getLastLadderPort();
	const LadderInfo *info = TheLadderList->findLadder( lastLadderAddr, lastLadderPort );
	if (info && info->index > 0 && info->validCustom)
	{
		usedLadders.insert(info);
		index = GadgetListBoxAddEntryText( win, info->name, favoriteColor, -1 );
		GadgetListBoxSetItemData( win, (void *)(info->index), index );
		selectedPos = index;
	}

	// our recent ladders
	LadderPreferences ladPref;
	ladPref.loadProfile( TheGameSpyInfo->getLocalProfileID() );
	const LadderPrefMap recentLadders = ladPref.getRecentLadders();
	for (LadderPrefMap::const_iterator cit = recentLadders.begin(); cit != recentLadders.end(); ++cit)
	{
		AsciiString addr = cit->second.address;
		UnsignedShort port = cit->second.port;
		if (addr == lastLadderAddr && port == lastLadderPort)
			continue;
		const LadderInfo *info = TheLadderList->findLadder( addr, port );
		if (info && info->index > 0 && info->validCustom && usedLadders.find(info) == usedLadders.end())
		{
			usedLadders.insert(info);
			index = GadgetListBoxAddEntryText( win, info->name, favoriteColor, -1 );
			GadgetListBoxSetItemData( win, (void *)(info->index), index );
		}
	}

	// local ladders
	const LadderInfoList *lil = TheLadderList->getLocalLadders();
	LadderInfoList::const_iterator lit;
	for (lit = lil->begin(); lit != lil->end(); ++lit)
	{
		const LadderInfo *info = *lit;
		if (info && info->index < 0 && info->validCustom && usedLadders.find(info) == usedLadders.end())
		{
			usedLadders.insert(info);
			index = GadgetListBoxAddEntryText( win, info->name, localColor, -1 );
			GadgetListBoxSetItemData( win, (void *)(info->index), index );
		}
	}

	// special ladders
	lil = TheLadderList->getSpecialLadders();
	for (lit = lil->begin(); lit != lil->end(); ++lit)
	{
		const LadderInfo *info = *lit;
		if (info && info->index > 0 && info->validCustom && usedLadders.find(info) == usedLadders.end())
		{
			usedLadders.insert(info);
			index = GadgetListBoxAddEntryText( win, info->name, specialColor, -1 );
			GadgetListBoxSetItemData( win, (void *)(info->index), index );
		}
	}

	// standard ladders
	lil = TheLadderList->getStandardLadders();
	for (lit = lil->begin(); lit != lil->end(); ++lit)
	{
		const LadderInfo *info = *lit;
		if (info && info->index > 0 && info->validCustom && usedLadders.find(info) == usedLadders.end())
		{
			usedLadders.insert(info);
			index = GadgetListBoxAddEntryText( win, info->name, normalColor, -1 );
			GadgetListBoxSetItemData( win, (void *)(info->index), index );
		}
	}

	GadgetListBoxSetSelected( win, selectedPos );
	isPopulatingLadderBox = false;
}

void PopulateCustomLadderComboBox()
{
	if (!parentPopup || !comboBoxLadderName)
		return;

	isPopulatingLadderBox = true;

	CustomMatchPreferences pref;
	AsciiString userPrefFilename;
	Int localProfile = TheGameSpyInfo->getLocalProfileID();
	userPrefFilename.format("GeneralsOnline\\CustomPref%d.ini", localProfile);
	pref.load(userPrefFilename);

	std::set<const LadderInfo *> usedLadders;

	Color specialColor = GameSpyColor[GSCOLOR_MAP_SELECTED];
	Color normalColor = GameSpyColor[GSCOLOR_MAP_UNSELECTED];
	Int index;
	GadgetComboBoxReset( comboBoxLadderName );
	index = GadgetComboBoxAddEntry( comboBoxLadderName, TheGameText->fetch("GUI:NoLadder"), normalColor );
	GadgetComboBoxSetItemData( comboBoxLadderName, index, nullptr );

	Int selectedPos = 0;
	AsciiString lastLadderAddr = pref.getLastLadderAddr();
	UnsignedShort lastLadderPort = pref.getLastLadderPort();
	const LadderInfo *info = TheLadderList->findLadder( lastLadderAddr, lastLadderPort );
	if (info && info->validCustom)
	{
		usedLadders.insert(info);
		index = GadgetComboBoxAddEntry( comboBoxLadderName, info->name, specialColor );
		GadgetComboBoxSetItemData( comboBoxLadderName, index, (void *)(info->index) );
		selectedPos = index;
	}

	LadderPreferences ladPref;
	ladPref.loadProfile( localProfile );
	const LadderPrefMap recentLadders = ladPref.getRecentLadders();
	for (LadderPrefMap::const_iterator cit = recentLadders.begin(); cit != recentLadders.end(); ++cit)
	{
		AsciiString addr = cit->second.address;
		UnsignedShort port = cit->second.port;
		if (addr == lastLadderAddr && port == lastLadderPort)
			continue;
		const LadderInfo *info = TheLadderList->findLadder( addr, port );
		if (info && info->validCustom && usedLadders.find(info) == usedLadders.end())
		{
			usedLadders.insert(info);
			index = GadgetComboBoxAddEntry( comboBoxLadderName, info->name, normalColor );
			GadgetComboBoxSetItemData( comboBoxLadderName, index, (void *)(info->index) );
		}
	}

	index = GadgetComboBoxAddEntry( comboBoxLadderName, TheGameText->fetch("GUI:ChooseLadder"), normalColor );
	GadgetComboBoxSetItemData( comboBoxLadderName, index, (void *)-1 );

	GadgetComboBoxSetSelectedPos( comboBoxLadderName, selectedPos );
	isPopulatingLadderBox = false;
}

// Window Functions -----------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------
/** Initialize the PopupHostGameInit menu */
//-------------------------------------------------------------------------------------------------
void PopupHostGameInit( WindowLayout *layout, void *userData )
{
	parentPopupID = TheNameKeyGenerator->nameToKey("PopupHostGame.wnd:ParentHostPopUp");
	parentPopup = TheWindowManager->winGetWindowFromId(nullptr, parentPopupID);

	textEntryGameNameID = TheNameKeyGenerator->nameToKey("PopupHostGame.wnd:TextEntryGameName");
	textEntryGameName = TheWindowManager->winGetWindowFromId(parentPopup, textEntryGameNameID);
	UnicodeString name;
	name.translate(TheGameSpyInfo->getLocalName());
	GadgetTextEntrySetText(textEntryGameName, name);

	textEntryGameDescriptionID = TheNameKeyGenerator->nameToKey("PopupHostGame.wnd:TextEntryGameDescription");
	textEntryGameDescription = TheWindowManager->winGetWindowFromId(parentPopup, textEntryGameDescriptionID);
	GadgetTextEntrySetText(textEntryGameDescription, UnicodeString::TheEmptyString);

	textEntryLadderPasswordID = TheNameKeyGenerator->nameToKey("PopupHostGame.wnd:TextEntryLadderPassword");
	textEntryLadderPassword = TheWindowManager->winGetWindowFromId(parentPopup, textEntryLadderPasswordID);
	GadgetTextEntrySetText(textEntryLadderPassword, UnicodeString::TheEmptyString);

	textEntryGamePasswordID = TheNameKeyGenerator->nameToKey("PopupHostGame.wnd:TextEntryGamePassword");
	textEntryGamePassword = TheWindowManager->winGetWindowFromId(parentPopup, textEntryGamePasswordID);
	GadgetTextEntrySetText(textEntryGamePassword, UnicodeString::TheEmptyString);

	// GeneralsX: The confirm field, and its label.
	//
	// Both are added by !GXUI.big, which overrides Window\Menus\PopupHostGame.wnd. That is safe and
	// was MEASURED rather than assumed: probe-dataid.sh over a modified copy of this very layout
	// leaves dataID at AB9092CC, identical to baseline, while the asset fingerprint moves - so the
	// archive is genuinely mounted and dataID genuinely ignores it. The engine's identity code says
	// the same thing in words ("a skinned UI has no effect on the simulation and must not be a
	// compatibility barrier").
	//
	//
	//
	// EVERY LOOKUP IS NULL-TOLERANT. A player running the stock archive without !GXUI.big gets a
	// layout with neither control, and must still be able to host - so an absent confirm field
	// means "no confirmation required", not a dead button. That is the only reason this feature can
	// ship ahead of the archive reaching every install.
	textEntryGamePasswordConfirmID = TheNameKeyGenerator->nameToKey("PopupHostGame.wnd:TextEntryGamePasswordConfirm");
	textEntryGamePasswordConfirm = TheWindowManager->winGetWindowFromId(parentPopup, textEntryGamePasswordConfirmID);
	if (textEntryGamePasswordConfirm != nullptr)
	{
		// The layout template this was cloned from carries TEXT = "Entry"; clear it, exactly as the
		// three fields above do, or the box opens pre-filled with a word the player did not type.
		GadgetTextEntrySetText(textEntryGamePasswordConfirm, UnicodeString::TheEmptyString);
		EntryData *confirmEntry = (EntryData *)textEntryGamePasswordConfirm->winGetUserData();
		if (confirmEntry != nullptr)
			confirmEntry->secretText = TRUE;
	}

	// The label is cloned from the password label, so it arrives saying "Password" - the CSF key
	// came with it. Set from code through FETCH_OR_SUBSTITUTE, which is the house pattern and means
	// no .csf has to be edited to add one line of English.
	staticTextConfirmPasswordID = TheNameKeyGenerator->nameToKey("PopupHostGame.wnd:StaticTextConfirmPassword");
	staticTextConfirmPassword = TheWindowManager->winGetWindowFromId(parentPopup, staticTextConfirmPasswordID);
	if (staticTextConfirmPassword != nullptr)
	{
		GadgetStaticTextSetText(staticTextConfirmPassword,
			TheGameText->FETCH_OR_SUBSTITUTE("GUI:ConfirmPassword", L"Confirm password"));
	}

	// GeneralsX: Asterisks, not the password.
	//
	// The .wnd authors this box as ordinary text, so it rendered what was typed in the clear - on a
	// screen people stream and screenshot, and this is the one field on it worth reading over
	// somebody's shoulder. The game-options screen one click later already masks its own password
	// box, so showing it here was also the two screens disagreeing about the same secret.
	//
	// secretText is the engine's own flag for this (Gadget.h: "If TRUE text appears as asterisks"),
	// which is display-only - GadgetTextEntryGetText still returns the real string, so the create
	// path below is unaffected. Set here rather than in the layout because editing the .wnd would
	// move dataID and every peer would refuse every join; this is why the mask has to be code.
	//
	// Unconditional, not relay-only: the GameSpy path has exactly the same shoulder, and the flag
	// changes nothing it reads. Generals Online masks theirs the same way and for the same stated
	// reason ("hide password for streams").
	if (textEntryGamePassword != nullptr)
	{
		EntryData *pwEntry = (EntryData *)textEntryGamePassword->winGetUserData();
		if (pwEntry != nullptr)
			pwEntry->secretText = TRUE;
	}

	buttonCreateGameID = TheNameKeyGenerator->nameToKey("PopupHostGame.wnd:ButtonCreateGame");
	buttonCreateGame = TheWindowManager->winGetWindowFromId(parentPopup, buttonCreateGameID);

	buttonCancelID = TheNameKeyGenerator->nameToKey("PopupHostGame.wnd:ButtonCancel");
	buttonCancel = TheWindowManager->winGetWindowFromId(parentPopup, buttonCancelID);

	checkBoxAllowObserversID = TheNameKeyGenerator->nameToKey("PopupHostGame.wnd:CheckBoxAllowObservers");
	checkBoxAllowObservers = TheWindowManager->winGetWindowFromId(parentPopup, checkBoxAllowObserversID);
	CustomMatchPreferences customPref;
	GadgetCheckBoxSetChecked(checkBoxAllowObservers, customPref.allowsObservers());

	comboBoxLadderNameID = TheNameKeyGenerator->nameToKey("PopupHostGame.wnd:ComboBoxLadderName");
	comboBoxLadderName = TheWindowManager->winGetWindowFromId(parentPopup, comboBoxLadderNameID);
	if (comboBoxLadderName)
		GadgetComboBoxReset(comboBoxLadderName);
	PopulateCustomLadderComboBox();

  checkBoxUseStatsID = TheNameKeyGenerator->nameToKey("PopupHostGame.wnd:CheckBoxUseStats");
  checkBoxUseStats = TheWindowManager->winGetWindowFromId(parentPopup, checkBoxUseStatsID);
	Bool usingStats = customPref.getUseStats();
  GadgetCheckBoxSetChecked( checkBoxUseStats, usingStats );

	// limit armies is disallowed in "use stats" games
  checkBoxLimitArmiesID = TheNameKeyGenerator->nameToKey("PopupHostGame.wnd:CheckBoxLimitArmies");
  checkBoxLimitArmies = TheWindowManager->winGetWindowFromId(parentPopup, checkBoxLimitArmiesID);
	// GeneralsX: In the relay lobby Use Stats locks nothing: the relay records no
	// stats and its room (the LAN options screen) has no such rule, so this popup must not force off a
	// Limit Armies the room kept - it would write that forced "off" back on create. The room keeps it in
	// LANPreferences (it cannot write CustomMatchPreferences: TheGameSpyInfo is torn down while a room is
	// open), so pre-tick from there: a choice changed in the last room is the one offered here.
	Bool factionsLimited = customPref.getFactionsLimited();
	if (GXRelayLobbyIsActive())
	{
		LANPreferences lanPref;
		factionsLimited = lanPref.getBool( "LimitArmies", FALSE );
	}
	const Bool statsLockArmies = usingStats && !GXRelayLobbyIsActive();
	checkBoxLimitArmies->winEnable(! statsLockArmies );
  GadgetCheckBoxSetChecked( checkBoxLimitArmies, statsLockArmies? FALSE : factionsLimited );

	TheWindowManager->winSetFocus( parentPopup );
	TheWindowManager->winSetModal( parentPopup );

}


//-------------------------------------------------------------------------------------------------
/** PopupHostGameUpdate callback */
//-------------------------------------------------------------------------------------------------
void PopupHostGameUpdate( WindowLayout * layout, void *userData)
{
	if (GadgetCheckBoxIsChecked( checkBoxUseStats ) && !GXRelayLobbyIsActive())	// GeneralsX: see Init
	{
		checkBoxLimitArmies->winEnable( FALSE );
		GadgetCheckBoxSetChecked( checkBoxLimitArmies, FALSE );
	}
	else
	{
		checkBoxLimitArmies->winEnable( TRUE );
	}
}


//-------------------------------------------------------------------------------------------------
/** PopupHostGameInput callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType PopupHostGameInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 )
{
	switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CHAR:
		{
			UnsignedByte key = mData1;
			UnsignedByte state = mData2;
//			if (buttonPushed)
//				break;

			switch( key )
			{

				// ----------------------------------------------------------------------------------------
				case KEY_ESC:
				{

					//
					// send a simulated selected event to the parent window of the
					// back/exit button
					//
					if( BitIsSet( state, KEY_STATE_UP ) )
					{
						TheWindowManager->winSendSystemMsg( window, GBM_SELECTED,
																							(WindowMsgData)buttonCancel, buttonCancelID );

					}

					// don't let key fall through anywhere else
					return MSG_HANDLED;

				}

			}

		}

	}

	return MSG_IGNORED;

}

//-------------------------------------------------------------------------------------------------
/** PopupHostGameSystem callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType PopupHostGameSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 )
{
  switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CREATE:
		{

			break;

		}
    //---------------------------------------------------------------------------------------------
		case GWM_DESTROY:
		{
			parentPopup = nullptr;

			break;

		}

    //----------------------------------------------------------------------------------------------
    case GWM_INPUT_FOCUS:
		{

			// if we're givin the opportunity to take the keyboard focus we must say we want it
			if( mData1 == TRUE )
				*(Bool *)mData2 = TRUE;

			break;

		}

    //----------------------------------------------------------------------------------------------
		case GEM_UPDATE_TEXT:
			{
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();

				if ( controlID == textEntryGameNameID )
				{
					UnicodeString txtInput;

					// grab the game's name
					txtInput.set(GadgetTextEntryGetText( textEntryGameName ));

					// Clean up the text (remove leading/trailing chars, etc)
					const WideChar *c = txtInput.str();
					while (c && (iswspace(*c)))
						c++;

					if (c)
						txtInput = UnicodeString(c);
					else
						txtInput = UnicodeString::TheEmptyString;

					// Put the whitespace-free version in the box
					GadgetTextEntrySetText( textEntryGameName, txtInput );

				}
				break;
			}
    //---------------------------------------------------------------------------------------------
		case GCM_SELECTED:
			{
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				Int pos = -1;
				GadgetComboBoxGetSelectedPos(control, &pos);

				if (controlID == comboBoxLadderNameID && !isPopulatingLadderBox)
				{
					if (pos >= 0)
					{
						Int ladderID = static_cast<Int>(reinterpret_cast<intptr_t>(GadgetComboBoxGetItemData(control, pos)));
						if (ladderID < 0)
						{
							// "Choose a ladder" selected - open overlay
							PopulateCustomLadderComboBox(); // this restores the non-"Choose a ladder" selection
							GameSpyOpenOverlay( GSOVERLAY_LADDERSELECT );
						}
					}
				}
				break;
			}

    //---------------------------------------------------------------------------------------------
		case GBM_SELECTED:
		{
			GameWindow *control = (GameWindow *)mData1;
			Int controlID = control->winGetWindowId();

      if( controlID == buttonCancelID )
			{
				parentPopup = nullptr;
				GameSpyCloseOverlay(GSOVERLAY_GAMEOPTIONS);
				SetLobbyAttemptHostJoin( FALSE );
			}
			else if( controlID == buttonCreateGameID)
			{
				UnicodeString name;
				name = GadgetTextEntryGetText(textEntryGameName);
				name.trim();

				// GeneralsX: THE RELAY LOBBY BRANCHES BEFORE createGame().
				//
				// createGame() ends at TheGameSpyPeerMessageQueue->addRequest(req), and on the relay
				// path that pointer is NULL by construction - it is the switch the whole relay lobby
				// is built on, which is what lets these retail screens be reused without touching
				// their GameSpy half. Falling through would be a null call on every host.
				//
				// It also fills a PeerRequest with ladder addresses, an exeCRC/iniCRC pair and a
				// staging-room description that mean nothing without a GameSpy backend, so there is
				// nothing here to salvage for us beyond the two fields the player actually typed.
				//
				// The password is handed over as typed and is NOT trimmed: leading or trailing
				// spaces are legitimate in a password, and silently eating them would make a lobby
				// refuse the very string its host believes they set. The game NAME is trimmed,
				// because that one is displayed.
				if (GXRelayLobbyIsActive())
				{
					// GeneralsX: A typo here locks the host out of their
					// own lobby, and silently: the password is stored as a salted verifier, so
					// nothing can recover or even display what was actually set, and the first
					// symptom is a friend being refused with a password the host is certain is
					// right. Confirming costs one comparison and removes that whole class of
					// report. Refused BEFORE anything is created, so a mistyped game is never
					// advertised at all.
					//
					// Compared on the WIDE strings, before any translate(): AsciiString::translate
					// casts each WideChar to char - its own @todo says "will only work for 7-bit
					// ascii" - so two different non-ASCII passwords can flatten to the same bytes
					// and compare equal. The check has to be at least as strict as the thing it is
					// guarding.
					//
					// Skipped entirely when the field is absent, which is a stock archive without
					// !GXUI.big. Such a player simply gets the previous behaviour rather than a
					// button that cannot work.
					const UnicodeString typed = GadgetTextEntryGetText(textEntryGamePassword);
					if (textEntryGamePasswordConfirm != nullptr)
					{
						const UnicodeString again = GadgetTextEntryGetText(textEntryGamePasswordConfirm);
						if (typed.compare(again) != 0)
						{
							// REPORTED IN THE LABEL, NOT IN A MESSAGE BOX. GSMessageBoxOk was tried
							// first and is unusable here: PopupHostGameInit calls winSetModal on
							// this popup, so the box is created and dims the screen but never draws
							// above it - the player gets a greyed-out screen with no message and no
							// way to tell what happened. Observed by driving the dialog; it
							// compiles and "works" in every sense except being visible.
							//
							// The label is already ours and already positioned next to the field
							// the player has to fix, which is where the eye is. Clearing the
							// confirm entry and taking focus makes the retry a single action.
							if (staticTextConfirmPassword != nullptr)
							{
								// Kept to one line on purpose. The label is a fixed 26px band; the
								// first wording ("... - type it again") wrapped to two and the
								// second line was clipped by the entry below it, which looked like
								// a rendering fault rather than a message. Anything put here has to
								// fit the band at Arial 10.
								GadgetStaticTextSetText(staticTextConfirmPassword,
									TheGameText->FETCH_OR_SUBSTITUTE("GUI:ConfirmPasswordMismatch",
										L"Passwords do not match"));
							}
							GadgetTextEntrySetText(textEntryGamePasswordConfirm, UnicodeString::TheEmptyString);
							TheWindowManager->winSetFocus(textEntryGamePasswordConfirm);
							break;			// nothing created, and the popup stays open
						}
					}

					// GeneralsX: This popup's own retail Limit Armies checkbox. The relay
					// room is the LAN options screen, which takes its starting state from LANPreferences, so the
					// choice is handed over there - and read back from there by Init next time; the room's own
					// checkbox keeps it editable afterwards.
					if (checkBoxLimitArmies != nullptr)
					{
						LANPreferences lanPref;
						lanPref.setBool("LimitArmies", GadgetCheckBoxIsChecked(checkBoxLimitArmies));
						lanPref.write();
					}

					AsciiString passwd;
					passwd.translate(typed);
					GXRelayHostGameWithOptions(name, passwd);
					parentPopup = nullptr;
					GameSpyCloseOverlay(GSOVERLAY_GAMEOPTIONS);
					break;
				}

				if(name.isEmpty())
				{
					name.translate(TheGameSpyInfo->getLocalName());
					GadgetTextEntrySetText(textEntryGameName, name);
				}
				createGame();
				parentPopup = nullptr;
				GameSpyCloseOverlay(GSOVERLAY_GAMEOPTIONS);
			}
			break;
		}
		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;

}


//-----------------------------------------------------------------------------
// PRIVATE FUNCTIONS //////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

void createGame()
{
	TheGameSpyInfo->setCurrentGroupRoom(0);
	PeerRequest req;
	UnicodeString gameName = GadgetTextEntryGetText(textEntryGameName);
	req.peerRequestType = PeerRequest::PEERREQUEST_CREATESTAGINGROOM;
	req.text = gameName.str();
	TheGameSpyGame->setGameName(gameName);
	AsciiString passwd;
	passwd.translate(GadgetTextEntryGetText(textEntryGamePassword));
	req.password = passwd.str();
	CustomMatchPreferences customPref;
	Bool aO = GadgetCheckBoxIsChecked(checkBoxAllowObservers);
  Bool limitArmies = GadgetCheckBoxIsChecked( checkBoxLimitArmies );
  Bool useStats = GadgetCheckBoxIsChecked( checkBoxUseStats );
	customPref.setAllowsObserver(aO);
  customPref.setFactionsLimited( limitArmies );
  customPref.setUseStats( useStats );
	customPref.write();
	req.stagingRoomCreation.allowObservers = aO;
  req.stagingRoomCreation.useStats = useStats;
	TheGameSpyGame->setAllowObservers(aO);
  TheGameSpyGame->setOldFactionsOnly( limitArmies );
  TheGameSpyGame->setUseStats( useStats );
	req.stagingRoomCreation.exeCRC = TheGlobalData->m_exeCRC;
	req.stagingRoomCreation.iniCRC = TheGlobalData->m_iniCRC;
	req.stagingRoomCreation.gameVersion = TheGameSpyInfo->getInternalIP();
	req.stagingRoomCreation.restrictGameList = TheGameSpyConfig->restrictGamesToLobby();

	Int ladderSelectPos = -1, ladderID = -1;
	GadgetComboBoxGetSelectedPos(comboBoxLadderName, &ladderSelectPos);
	req.ladderIP = "localhost";
	req.stagingRoomCreation.ladPort = 0;
	if (ladderSelectPos >= 0)
	{
		// GeneralsX: 64-bit safe pointer cast
		ladderID = static_cast<Int>(reinterpret_cast<intptr_t>(GadgetComboBoxGetItemData(comboBoxLadderName, ladderSelectPos)));
		if (ladderID != 0)
		{
			// actual ladder
			const LadderInfo *info = TheLadderList->findLadderByIndex(ladderID);
			if (info)
			{
				req.ladderIP = info->address.str();
				req.stagingRoomCreation.ladPort = info->port;
			}
		}
	}
	TheGameSpyGame->setLadderIP(req.ladderIP.c_str());
	TheGameSpyGame->setLadderPort(req.stagingRoomCreation.ladPort);
	req.hostPingStr = TheGameSpyInfo->getPingString().str();

	TheGameSpyPeerMessageQueue->addRequest(req);
}
