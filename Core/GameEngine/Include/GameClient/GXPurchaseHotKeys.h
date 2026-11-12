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
**	Which General's Points button a purchase key fires. The panel shows each power's command-bar key on EVERY tier.
**	A key fires one window: (1) the first ENABLED button carrying it, in panel order; (2) else the first one the player
**	does not own (the disabled click); (3) else nobody (all bought), leaving the key to the command bar.
**	Pure, tested by scripts/test/audit/bench/purchase_panel_hotkeys.cpp with plain c++.
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
