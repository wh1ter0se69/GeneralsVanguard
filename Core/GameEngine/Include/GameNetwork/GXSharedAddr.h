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

// Which slots' peers share an address with another slot's peer:
// the slots ConnectionManager::arrivedFrom must match by PORT as well as address. Pure, so
// scripts/test/shared-addr-test.cpp can drive it - ConnectionManager.cpp cannot be compiled alone.
//
// Taken ONCE, from the slot list, in parseUserList - never from the live connection table. Two
// peers behind one NAT are told apart only by port; the old check asked whether ANOTHER CONNECTED
// peer had the address, so the moment one of the pair was disconnected locally its connection was
// deleted, the other matched by address alone, and the dropped peer - still sending from that
// address - passed as it for every disconnect command.
//
// addr[i] is slot i's remote address; 0 for the local slot and for a slot with no human.
#pragma once

// scale32: slot masks are 64 bits (RelayMaskType), a 32-bit holder cannot name slots 32-33.
// Self-contained (no NetworkDefs.h): unsigned long long is RelayMaskType, 1ULL the slot bit.
inline unsigned long long GXSharedAddrSlots(const unsigned int *addr, int n)
{
	unsigned long long mask = 0;
	for (int i = 0; i < n; ++i)
		for (int j = i + 1; j < n; ++j)
			if ((addr[i] != 0) && (addr[i] == addr[j]))
				mask |= (1ULL << i) | (1ULL << j);
	return mask;
}
