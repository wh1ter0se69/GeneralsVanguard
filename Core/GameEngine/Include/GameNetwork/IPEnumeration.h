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

// IPEnumeration.h ///////////////////////////////////////////////////////////////
// Class for enumerating IP addresses
// Author: Matthew D. Campbell, October 2001

#pragma once

#include "GameNetwork/Transport.h"

/**
 * IP wrapper class.
 */
class EnumeratedIP : public MemoryPoolObject
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(EnumeratedIP, "EnumeratedIP")
public:
	EnumeratedIP() { m_IPstring = ""; m_next = nullptr; m_IP = 0; m_broadcastIP = 0; m_tunnel = FALSE; m_cellular = FALSE; }

	// Access functions
	AsciiString getIPstring() { return m_IPstring; }
	void setIPstring( AsciiString name ) { m_IPstring = name; }
	UnsignedInt getIP() { return m_IP; }
	void setIP( UnsignedInt IP ) { m_IP = IP; }
	// GeneralsX: The subnet-directed broadcast address of the interface this address
	// lives on. Darwin rejects a limited broadcast (255.255.255.255) with EHOSTUNREACH, so LAN
	// discovery has to aim at the subnet instead. 0 means the platform enumeration could not
	// supply one and the caller should fall back to INADDR_BROADCAST.
	UnsignedInt getBroadcastIP() { return m_broadcastIP; }
	void setBroadcastIP( UnsignedInt IP ) { m_broadcastIP = IP; }
	// GeneralsX: The address lives on an interface with no LAN segment behind it - point-to-
	// point (utun, tun, wg, tailscale0, a cellular pdp_ip) or a /32 mask: a VPN or overlay tunnel.
	// FALSE when the platform could not say, which keeps that address in retail order.
	Bool isTunnel() { return m_tunnel; }
	void setTunnel( Bool tunnel ) { m_tunnel = tunnel; }
	// GeneralsX: A tunnel "Use my VPN for LAN" must never prefer: a carrier link
	// (iOS pdp_ip), or a Windows VPN address on no interface that is up.
	// With the switch off it ranks exactly as a tunnel.
	Bool isCellular() { return m_cellular; }
	void setCellular( Bool cellular ) { m_cellular = cellular; }
	EnumeratedIP *getNext() { return m_next; }
	void setNext( EnumeratedIP *next ) { m_next = next; }

protected:
	AsciiString m_IPstring;
	UnsignedInt m_IP;
	UnsignedInt m_broadcastIP;
	Bool m_tunnel;
	Bool m_cellular;
	EnumeratedIP *m_next;
};
EMPTY_DTOR(EnumeratedIP)


/**
 * The IPEnumeration class is used to obtain a list of IP addresses on the
 * local machine.
 */
class IPEnumeration
{
public:

	IPEnumeration();
	~IPEnumeration();

	EnumeratedIP * getAddresses();		///< Return a linked list of local IP addresses
	AsciiString getMachineName();			///< Return the Network Neighborhood machine name

	// GeneralsX: "Use my VPN for LAN" (Options.ini UseVPNForLAN, default
	// off, set once at startup from GlobalData). Off: a tunnel ranks after the real LAN and a saved
	// tunnel IPAddress counts as unset. On: a tunnel ranks first, and while one exists a saved
	// non-tunnel IPAddress counts as unset. A cellular link is never the preferred side, nor a saved pick.
	// On in multi-instance mode, no saved IPAddress is used: each instance keeps its own 127.x.
	static void setUseVPNForLAN( Bool use );
	static Bool getUseVPNForLAN();
	static Bool isSavedAddressUsable( Bool tunnel, Bool cellular, Bool headIsTunnel );
	static Bool sortsBeforeWithoutVPN( UnsignedInt lhs, Bool lhsTunnel, Bool lhsCellular, UnsignedInt rhs, Bool rhsTunnel, Bool rhsCellular );	///< the order with the switch off	///< may a saved IPAddress on this interface win?

protected:
	void addNewIP( UnsignedByte a, UnsignedByte b, UnsignedByte c, UnsignedByte d, UnsignedInt broadcastIP = 0, Bool tunnel = FALSE, Bool cellular = FALSE );

	EnumeratedIP *m_IPlist;
	Bool m_isWinsockInitialized;
};
