/*
**	GeneralsX @feature Claude 27/09/2026 - which General's Points button a purchase key fires.
**
**	Pure on purpose, like GXSkillAnnouncePolicy.h next door: the choice is testable without an engine.
**	scripts/test/audit/bench/purchase_panel_hotkeys.cpp compiles this header with plain c++.
**
**	The panel shows one letter per power on EVERY tier - Paradrop 1, 2 and 3 all show the Paradrop key - because the
**	letter is the key of the power's own command-bar button (ControlBar::resolvePurchaseScienceSource). A key fires one
**	window, so each key goes to:
**	  1. the first ENABLED button carrying it, in panel order: the next tier the player can buy right now. When two
**	     powers share a key, the one that can be bought wins over one that cannot;
**	  2. else the first button carrying it that the player does not own: pressing it gives the disabled click, and the
**	     panel still owns the key while it is open;
**	  3. else nobody: every button carrying it is bought, so the key is left to the command bar.
**	Every other button carrying the key keeps its letter, greyed (W3DPushButton.cpp drawHotKeyLabel asks
**	HotKeyManager::isHotKeyBoundTo), exactly as a refused command-bar letter does.
*/

#pragma once

#ifndef GX_PURCHASE_HOTKEYS_H
#define GX_PURCHASE_HOTKEYS_H

namespace GXPurchaseHotKeys
{

struct Slot
{
	int key;				///< the letter, lower case; 0 = this button has no key
	bool enabled;		///< purchasable right now
	bool owned;			///< the player already has this science
};

/// bindOut[i] is set when slots[i] should receive its key. Slots are in panel order.
inline void chooseBindings( const Slot *slots, int count, bool *bindOut )
{
	for( int i = 0; i < count; ++i )
		bindOut[ i ] = false;

	// Pass 0 takes the buyable buttons, pass 1 what is left for keys nobody buyable wanted.
	for( int pass = 0; pass < 2; ++pass )
	{
		for( int i = 0; i < count; ++i )
		{
			if( slots[ i ].key == 0 || ( pass == 0 ? !slots[ i ].enabled : slots[ i ].owned ) )
				continue;

			bool taken = false;
			for( int j = 0; j < count && !taken; ++j )
				taken = bindOut[ j ] && slots[ j ].key == slots[ i ].key;
			if( !taken )
				bindOut[ i ] = true;
		}
	}
}

} // namespace GXPurchaseHotKeys

#endif // GX_PURCHASE_HOTKEYS_H
