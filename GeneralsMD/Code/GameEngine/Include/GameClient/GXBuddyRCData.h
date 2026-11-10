// GeneralsX: The gx right-click payload, shared by every surface that
// opens the buddy RC menus: the overlay's own lists (WOLBuddyOverlay.cpp) and the lobby roster
// (WOLLobbyMenu.cpp). One shared handler reads whatever payload the menu carries, so the two
// types must be tellable apart BEFORE any downcast:
//
//   * GXRCMenuData derives from GameSpyRCMenuData, so the handler's existing base-pointer read is
//     well-formed for both.
//   * The discriminator is m_id == GX_RC_PROFILE_SENTINEL. A real GameSpy user is m_id > 0, and
//     the GameSpy path treats anything else as "not a GameSpy user", so the sentinel is inert if
//     it ever leaks into old code. A global mode flag cannot type a heap object - the menu can
//     outlive the click that made it - so the tag lives in the payload itself.
//   * Deletion happens on the DERIVED type after the sentinel check (the base has no virtual
//     dtor, so deleting through the base pointer would be undefined for gx payloads).
//
// Ownership is take-and-null: whoever consumes the payload nulls the window's user data and
// deletes; closeRightClickMenu deletes whatever is still attached, so a menu dismissed without a
// click cannot leak - the GameSpy path's historical leak-on-dismiss is not copied.
//
// m_gxKind picks the action route. BY_ID acts on a durable id the store already holds (friend and
// ignore rows). BY_SLOT acts on a room slot - a pending request row or a roster row - and every
// BY_SLOT action revalidates m_nick (the snapshot taken at menu open) against the live roster
// before sending anything, because slots are recycled across humans.
//
// SELF (18/09/2026) is the player's OWN roster row, opened as EA's retail RCLocalPlayerMenu.wnd -
// the menu the GameSpy lobby opened for "me". Its single button (ButtonStats) is relabelled Sign
// Out rather than hidden, and it acts on nothing but this player's own sign-in, so it carries no
// id, no slot and nothing to revalidate.
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
