// GeneralsX @security Claude 19/09/2026 Which slots' peers share an address with another slot's peer:
// the slots ConnectionManager::arrivedFrom must match by PORT as well as address. Pure, so
// scripts/test/shared-addr-test.cpp can drive it - ConnectionManager.cpp cannot be compiled alone.
//
// Taken ONCE, from the slot list, in parseUserList - never from the live connection table. Two
// peers behind one NAT are told apart only by port; the old check asked whether ANOTHER CONNECTED
// peer had the address, so the moment one of the pair was disconnected locally its connection was
// deleted, the other matched by address alone, and the dropped peer - still sending from that
// address - passed as it for every disconnect command (Codex, 19/09/2026).
//
// addr[i] is slot i's remote address; 0 for the local slot and for a slot with no human.
#pragma once

// GeneralsX @feature Claude 06/11/2026 scale32: slot masks are 64 bits (RelayMaskType), a 32-bit holder cannot name slots 32-33.
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
