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

// gx right-click payload shared by the buddy RC menus (WOLBuddyOverlay.cpp lists, WOLLobbyMenu.cpp roster).
// GXRCMenuData derives from GameSpyRCMenuData; m_id == GX_RC_PROFILE_SENTINEL identifies it before any downcast.
// Delete the DERIVED type after that check (no virtual dtor). The consumer nulls the window's user data;
// closeRightClickMenu deletes whatever is still attached, so a dismissed menu cannot leak.
// BY_SLOT actions revalidate m_nick against the live roster first, because slots are recycled.
// SELF is the player's own roster row (RCLocalPlayerMenu.wnd, its one button relabelled Sign Out): no id or slot.
#pragma once

#include "GameNetwork/GameSpy/PeerDefs.h"	// GameSpyRCMenuData, RCItemType, GPProfile
#include "GameClient/GXPlayerIdentity.h"	// GX_PLAYER_ID_LEN

enum { GX_RC_PROFILE_SENTINEL = -2 };

class GXRCMenuData : public GameSpyRCMenuData
{
public:
	enum Kind { BY_ID = 0, BY_SLOT = 1, SELF = 2 };

	Kind m_gxKind;
	char m_gxId[GX_PLAYER_ID_LEN + 1];	///< BY_ID: the durable id, copied - store pointers die on mutation
	Int  m_gxSlot;						///< BY_SLOT: the room slot the click landed on

	GXRCMenuData()
	{
		m_id = (GPProfile)GX_RC_PROFILE_SENTINEL;
		m_itemType = ITEM_NONE;
		m_gxKind = BY_ID;
		m_gxId[0] = '\0';
		m_gxSlot = 0;
	}
};
