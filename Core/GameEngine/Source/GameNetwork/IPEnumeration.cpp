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

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameNetwork/IPEnumeration.h"
#include "GameNetwork/networkutil.h"
#include "GameClient/ClientInstance.h"

#include <string.h>

#ifndef _WIN32
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#endif

#ifndef _WIN32
// GeneralsX @bugfix Darwin answers a limited broadcast (255.255.255.255) with EHOSTUNREACH no
// matter what socket options are set, and Linux binds broadcast reception to the address the
// datagram was actually sent to, so LAN discovery needs the subnet-directed broadcast of the
// interface it is speaking from. getifaddrs already carries it; only ifa_addr was being read.
// Returns 0 when this interface has no meaningful broadcast address - a point-to-point or /32
// VPN link, where ifa_broadaddr aliases ifa_dstaddr and holds the peer, not a broadcast - which
// tells the caller to fall back to INADDR_BROADCAST rather than send to a bogus destination.
static UnsignedInt getInterfaceBroadcast( const struct ifaddrs *ifa, UnsignedInt hostAddr )
{
	UnsignedInt broadcastAddr = 0;

	if ((ifa->ifa_flags & IFF_BROADCAST) != 0
		&& (ifa->ifa_flags & IFF_POINTOPOINT) == 0
		&& ifa->ifa_broadaddr != nullptr
		&& ifa->ifa_broadaddr->sa_family == AF_INET)
	{
		const sockaddr_in *bcast = reinterpret_cast<const sockaddr_in *>(ifa->ifa_broadaddr);
		broadcastAddr = ntohl(bcast->sin_addr.s_addr);
	}
	else if (ifa->ifa_netmask != nullptr && ifa->ifa_netmask->sa_family == AF_INET)
	{
		const sockaddr_in *mask = reinterpret_cast<const sockaddr_in *>(ifa->ifa_netmask);
		broadcastAddr = hostAddr | ~ntohl(mask->sin_addr.s_addr);
	}

	// A /32 mask leaves the host address unchanged, and 0.0.0.0 is no address at all.
	if (broadcastAddr == hostAddr)
	{
		broadcastAddr = 0;
	}

	return broadcastAddr;
}

// GeneralsX: An interface with no LAN segment behind it: point-to-point
// (utun, tun, wg, tailscale0, a cellular pdp_ip) or a /32 mask. Nothing else can answer a LAN
// broadcast there, so it must never be elected the LAN address while a real segment exists.
static Bool isTunnelInterface( const struct ifaddrs *ifa )
{
	if ((ifa->ifa_flags & IFF_POINTOPOINT) != 0)
	{
		return TRUE;
	}
	if (ifa->ifa_netmask != nullptr && ifa->ifa_netmask->sa_family == AF_INET)
	{
		const sockaddr_in *mask = reinterpret_cast<const sockaddr_in *>(ifa->ifa_netmask);
		return mask->sin_addr.s_addr == 0xFFFFFFFF;
	}
	return FALSE;
}

// GeneralsX: iOS names its cellular data links pdp_ip0..N. They are point-to-
// point, so isTunnelInterface counts them - right for keeping them off the LAN - but they are no VPN, and
// "Use my VPN for LAN" must not prefer one: a carrier 10.x sorts below a tailnet 100.x.
// Trade-off: iOS only; a Linux ppp0/wwan modem or Windows mobile broadband reads as a plain tunnel or LAN.
static Bool isCellularInterface( const struct ifaddrs *ifa )
{
	return ifa->ifa_name != nullptr && strncmp(ifa->ifa_name, "pdp_ip", 6) == 0;
}
#endif

#ifdef _WIN32
// GeneralsX: The same fact on Windows, where the address list itself comes
// from the hostname lookup and carries no interface at all. SIO_GET_INTERFACE_LIST returns one
// INTERFACE_INFO per IPv4 address with its flags and mask. Neither the struct nor the code is
// declared by the Winsock 1.1 header this engine builds against (<ws2ipdef.h> drags winsock2.h in
// on top of winsock.h - see udp.h), so both are mirrored here, ABI-stable since NT 4, the same way
// udp.cpp mirrors SIO_UDP_CONNRESET: ULONG iiFlags, then three sockaddr_gen unions. sockaddr_gen
// is { sockaddr (16), sockaddr_in (16), sockaddr_in6_old (short, u_short, u_long, in6_addr = 24) }
// - the OLD in6 form without sin6_scope_id, so 24 bytes, not sockaddr_in6's 28 - on Win32 and Win64
// alike (checked against the SDK metadata as windows-sys generates it: INTERFACE_INFO, sockaddr_gen,
// sockaddr_in6_old). The parse below still refuses any reply whose records do not all read as
// AF_INET, so a layout mismatch degrades to retail order instead of marking the wrong address.
// Measured 28/09/2026 on the owner's PC: the Tailscale adapter holds its 100.x address as /32.
struct GxInterfaceInfo
{
	UnsignedInt flags;
	unsigned char address[24];
	unsigned char broadcast[24];
	unsigned char netmask[24];
};
static_assert(sizeof(GxInterfaceInfo) == 76, "INTERFACE_INFO is 76 bytes");
static const unsigned long GX_SIO_GET_INTERFACE_LIST = 0x4004747F;	// _IOR('t', 127, u_long)
static const unsigned long GX_IFF_POINTTOPOINT = 0x8;
static const unsigned long GX_IFF_UP = 0x1;

static Bool gxSockaddrIsIPv4( const unsigned char *gen )
{
	sockaddr_in in;
	memcpy(&in, gen, sizeof(in));
	return in.sin_family == AF_INET;
}

static UnsignedInt gxSockaddrIPv4( const unsigned char *gen )
{
	sockaddr_in in;
	memcpy(&in, gen, sizeof(in));
	return ntohl(in.sin_addr.s_addr);
}

// Fills 'tunnels' with the host-order addresses that sit on a point-to-point or /32 interface, and 'up'
// with whether that interface is up (IFF_UP). Returns how many; 0 when the ioctl fails, which leaves every
// address in plain retail order. 'up' feeds the "unpreferred" label and the VPN extras, never the tunnel label.
static Int getWindowsTunnelAddresses( UnsignedInt *tunnels, Bool *up, Int maxTunnels )
{
	GxInterfaceInfo infos[64];
	DWORD bytes = 0;
	Int count = 0;

	SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s == INVALID_SOCKET)
	{
		return 0;
	}
	if (WSAIoctl(s, GX_SIO_GET_INTERFACE_LIST, nullptr, 0, infos, sizeof(infos), &bytes, nullptr, nullptr) == 0)
	{
		const Int n = (Int)(bytes / sizeof(GxInterfaceInfo));
		Bool valid = (bytes % sizeof(GxInterfaceInfo)) == 0;
		for (Int i = 0; i < n && valid; ++i)
		{
			valid = gxSockaddrIsIPv4(infos[i].address);	// a misread stride lands this on other bytes
		}
		for (Int i = 0; valid && i < n && count < maxTunnels; ++i)
		{
			if ((infos[i].flags & GX_IFF_POINTTOPOINT) != 0 || gxSockaddrIPv4(infos[i].netmask) == 0xFFFFFFFF)
			{
				up[count] = (infos[i].flags & GX_IFF_UP) != 0;
				tunnels[count++] = gxSockaddrIPv4(infos[i].address);
			}
		}
	}
	closesocket(s);
	return count;
}

// GeneralsX: How a hostname-lookup address is labelled on Windows: a tunnel when the
// interface list has it on a point-to-point or /32 interface, as before; and "unpreferred" too when every such
// interface is down - a disconnected VPN the lookup still returns must not be what the switch prefers.
// "Unpreferred" ranks exactly as a tunnel with the switch off, so that list is unchanged.
static void gxWindowsTunnelLabel( UnsignedInt ip, const UnsignedInt *tunnels, const Bool *up, Int tunnelCount,
	Bool *tunnel, Bool *unpreferred )
{
	// The same address on an adapter that is up and one that is down is live: ANY matching interface up
	// keeps it preferred, whatever order the list gives them - as gxWindowsVPNExtras reads it (q7-macupd-r5).
	Bool anyUp = FALSE;
	*tunnel = FALSE;
	for (Int t = 0; t < tunnelCount; ++t)
	{
		if (tunnels[t] == ip)
		{
			*tunnel = TRUE;
			anyUp = anyUp || up[t];
		}
	}
	*unpreferred = *tunnel && !anyUp;
}

// GeneralsX: The Windows list comes from gethostbyname(hostname),
// which can leave a VPN adapter's address out; SIO_GET_INTERFACE_LIST still finds it, but was only used to
// label the hostname entries, so "Use my VPN for LAN" could not pick it. With the switch ON, every tunnel
// address the hostname lookup did not return joins the list: loopback (127/8) and a down interface (a
// disconnected VPN keeping its /32) excluded, each address once.
// With it OFF nothing is added, so the list - the Options menu's included - and the address it elects stay
// exactly the hostname list's. Only tunnels, because they are what the switch prefers: adding a plain
// adapter would let it change the LAN pick with the switch on and no VPN up. Pure, so
// scripts/test/win-vpn-enum-test.sh drives it on any host. Fills 'out', returns how many.
static Int gxWindowsVPNExtras( const UnsignedInt *listed, Int listedCount, const UnsignedInt *tunnels, const Bool *up,
	Int tunnelCount, Bool useVPN, UnsignedInt *out, Int maxOut )
{
	Int count = 0;
	for (Int t = 0; useVPN && t < tunnelCount && count < maxOut; ++t)
	{
		Bool skip = !up[t] || (tunnels[t] >> 24) == 127 || tunnels[t] == 0;
		for (Int i = 0; !skip && i < listedCount; ++i)
		{
			skip = listed[i] == tunnels[t];
		}
		for (Int i = 0; !skip && i < count; ++i)
		{
			skip = out[i] == tunnels[t];
		}
		if (!skip)
		{
			out[count++] = tunnels[t];
		}
	}
	return count;
}
#endif

IPEnumeration::IPEnumeration()
{
	m_IPlist = nullptr;
	m_isWinsockInitialized = false;
}

IPEnumeration::~IPEnumeration()
{
	if (m_isWinsockInitialized)
	{
		WSACleanup();
		m_isWinsockInitialized = false;
	}

	EnumeratedIP *ip = m_IPlist;
	while (ip)
	{
		ip = ip->getNext();
		deleteInstance(m_IPlist);
		m_IPlist = ip;
	}
}

EnumeratedIP * IPEnumeration::getAddresses()
{
	if (m_IPlist)
		return m_IPlist;

	if (!m_isWinsockInitialized)
	{
		WORD verReq = MAKEWORD(2, 2);
		WSADATA wsadata;

		int err = WSAStartup(verReq, &wsadata);
		if (err != 0) {
			return nullptr;
		}

		if ((LOBYTE(wsadata.wVersion) != 2) || (HIBYTE(wsadata.wVersion) !=2)) {
			WSACleanup();
			return nullptr;
		}
		m_isWinsockInitialized = true;
	}

	// TheSuperHackers @feature Add one unique local host IP address for each multi client instance.
	if (rts::ClientInstance::isMultiInstance())
	{
		const UnsignedInt id = rts::ClientInstance::getInstanceId();
		addNewIP(
			127,
			(UnsignedByte)(id >> 16),
			(UnsignedByte)(id >> 8),
			(UnsignedByte)(id));
	}

#ifndef _WIN32
	// GeneralsX: Enumerate active IPv4 interfaces on non-Windows (POSIX) platforms instead of hostname resolution.
	struct ifaddrs *ifaddr = nullptr;
	if (getifaddrs(&ifaddr) == 0)
	{
		for (struct ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next)
		{
			if (ifa->ifa_addr == nullptr)
			{
				continue;
			}

			if (ifa->ifa_addr->sa_family != AF_INET)
			{
				continue;
			}

			if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_LOOPBACK) != 0)
			{
				continue;
			}

			const sockaddr_in *addr = reinterpret_cast<const sockaddr_in *>(ifa->ifa_addr);
			// GeneralsX: Use ntohl to convert from network byte order before extracting octets;
			// reading s_addr byte-by-byte on little-endian platforms reverses the IPv4 octets.
			const UnsignedInt hostAddr = ntohl(addr->sin_addr.s_addr);
			addNewIP(
				(UnsignedByte)((hostAddr >> 24) & 0xFF),
				(UnsignedByte)((hostAddr >> 16) & 0xFF),
				(UnsignedByte)((hostAddr >> 8) & 0xFF),
				(UnsignedByte)(hostAddr & 0xFF),
				getInterfaceBroadcast(ifa, hostAddr),
				isTunnelInterface(ifa),
				isCellularInterface(ifa));
		}
		freeifaddrs(ifaddr);

		if (m_IPlist)
		{
			return m_IPlist;
		}
	}
	else
	{
		DEBUG_LOG(("Failed call to getifaddrs; errno returned %d", errno));
	}
#endif

#ifdef _WIN32
	UnsignedInt tunnels[64];
	Bool tunnelUp[64];
	const Int tunnelCount = getWindowsTunnelAddresses(tunnels, tunnelUp, 64);
	UnsignedInt listed[64];
	Int listedCount = 0;
#endif
	// Every exit of the hostname path goes through here: on Windows, with the switch on, the VPN addresses
	// the hostname lookup did not return join the list - also when that lookup failed outright, a VPN-only
	// machine's case. Elsewhere, and with the switch off, it adds nothing.
	auto finish = [&]() -> EnumeratedIP *
	{
#ifdef _WIN32
		UnsignedInt extras[64];
		const Int extraCount = gxWindowsVPNExtras(listed, listedCount, tunnels, tunnelUp, tunnelCount, IPEnumeration::getUseVPNForLAN(), extras, 64);
		for (Int e = 0; e < extraCount; ++e)
		{
			addNewIP((UnsignedByte)(extras[e] >> 24), (UnsignedByte)(extras[e] >> 16), (UnsignedByte)(extras[e] >> 8),
				(UnsignedByte)extras[e], 0, TRUE);
		}
#endif
		return m_IPlist;
	};

	// get the local machine's host name
	char hostname[256];
	if (gethostname(hostname, sizeof(hostname)))
	{
		DEBUG_LOG(("Failed call to gethostname; WSAGetLastError returned %d", WSAGetLastError()));
		return finish();
	}
	DEBUG_LOG(("Hostname is '%s'", hostname));

	// get host information from the host name
	hostent* hostEnt = gethostbyname(hostname);
	if (hostEnt == nullptr)
	{
		DEBUG_LOG(("Failed call to gethostbyname; WSAGetLastError returned %d", WSAGetLastError()));
		return finish();
	}

	// sanity-check the length of the IP adress
	if (hostEnt->h_length != 4)
	{
		DEBUG_LOG(("gethostbyname returns oddly-sized IP addresses!"));
		return finish();
	}

	// construct a list of addresses
	int numAddresses = 0;
	char *entry;
	while ( (entry = hostEnt->h_addr_list[numAddresses++]) != nullptr )
	{
		Bool tunnel = FALSE;
		Bool unpreferred = FALSE;
#ifdef _WIN32
		const UnsignedInt ip = AssembleIp((UnsignedByte)entry[0], (UnsignedByte)entry[1], (UnsignedByte)entry[2], (UnsignedByte)entry[3]);
		gxWindowsTunnelLabel(ip, tunnels, tunnelUp, tunnelCount, &tunnel, &unpreferred);
		if (listedCount < 64)
		{
			listed[listedCount++] = ip;	// past 64, addNewIP's own duplicate check still holds
		}
#endif
		addNewIP(
			(UnsignedByte)entry[0],
			(UnsignedByte)entry[1],
			(UnsignedByte)entry[2],
			(UnsignedByte)entry[3],
			0,
			tunnel,
			unpreferred);
	}

	return finish();
}

// GeneralsX: A link-local (APIPA, 169.254.0.0/16) address must never win
// the default interface election. The list is kept in ascending numeric order and LanLobbyMenuInit
// takes the HEAD of it whenever Options.ini's IPAddress does not match a live interface, but
// 169.254.x.x (0xA9FE....) sorts BELOW every private range a real LAN uses - 172.16 (0xAC10....),
// 192.168 (0xC0A8....) - so any machine holding an idle second NIC announced an unroutable address
// and broadcast to 169.254.255.255. The lobby then stayed empty with no diagnostic of any kind,
// because every LAN log line in this path is DEBUG_LOG and compiles away in a shipping build.
// Measured on the Mac mini: en10 held 169.254.77.30 while the real LAN was en0 <mini-ip>.
// Rank by class first, then numerically within the class, so routable addresses always precede
// link-local ones while a link-local host with nothing else still enumerates normally.
//
// GeneralsX: A tunnel must not win it either. Measured 27/09/2026 in a
// Windows + Mac mini LAN lobby: both engines advertised their Tailscale address as their LAN address,
// because 100.x (0x64......) sorts below 192.168 (0xC0A8....) exactly as 169.254 did. The lobby
// joined and both peers then dropped at game start. So a middle class for an address on an interface
// with no LAN segment (point-to-point or /32: isTunnelInterface, getWindowsTunnelAddresses). It is
// decided by what the interface IS, never by the address range: 100.64.0.0/10 is also a real LAN on
// some networks, and a VPN can hold any RFC 1918 block. Class 0 keeps retail's plain ascending
// order, so a box without a tunnel elects exactly what it did before, and Options.ini IPAddress
// still wins over all of this, which keeps LAN-over-VPN possible on purpose.
// Trade-off: a VPN adapter that holds a real subnet mask and no point-to-point flag (NordLynx /16,
// an OpenVPN TAP) still sorts by number; the upgrade is the adapter type from GetAdaptersAddresses
// (iphlpapi) on Windows.
//
// GeneralsX: "Use my VPN for LAN" (UseVPNForLAN) swaps the first two
// classes, for players who really do play over Tailscale. What it buys is DIRECT CONNECT by IP over
// the tunnel (NetworkDirectConnect takes this election, and on Windows the socket is bound to it, so
// a LAN-bound socket could not talk to a 100.x peer at all). It does NOT make the LAN game LIST work
// over Tailscale: discovery is broadcast, and a point-to-point /32 has no broadcast address
// (getInterfaceBroadcast -> 0 -> LANAPI falls back to 255.255.255.255, which a tailnet does not
// carry). The launcher's hint says "join by IP" for that reason. An L2 VPN (ZeroTier) has a real
// mask and broadcast, is not a tunnel here, and already behaves like a LAN either way.
// Trade-off: directed (unicast) discovery to tailnet peers is the upgrade if the list must work too.
static Bool s_useVPNForLAN = FALSE;

// This client's own synthetic loopback address in multi-instance mode (getAddresses adds it first), else 0.
static UnsignedInt gxOwnInstanceIP()
{
	if (!rts::ClientInstance::isMultiInstance())
	{
		return 0;
	}
	return (127u << 24) | (rts::ClientInstance::getInstanceId() & 0xFFFFFFu);
}

void IPEnumeration::setUseVPNForLAN( Bool use )
{
	s_useVPNForLAN = use;
}

Bool IPEnumeration::getUseVPNForLAN()
{
	return s_useVPNForLAN;
}

// A saved IPAddress is, for most players, not a choice: OptionsMenu saves the list head on any Accept,
// so an older build left the tunnel it elected on its own. Off: a saved tunnel counts as unset and a
// saved non-tunnel address is honoured exactly as before. On: the mirror image - a saved non-tunnel
// address counts as unset while a tunnel exists (the head is one, since tunnels rank first), so the
// switch always takes effect; with no tunnel up, the saved address is honoured as before. On, in
// multi-instance mode: no saved address at all - every instance keeps its own 127.x, the head, or two
// instances would bind one saved address.
Bool IPEnumeration::isSavedAddressUsable( Bool tunnel, Bool cellular, Bool headIsTunnel )
{
	if (cellular)
	{
		return FALSE;	// a tunnel when off (unset, as above); never the VPN it was saved as when on
	}
	if (s_useVPNForLAN)
	{
		return !rts::ClientInstance::isMultiInstance() && (tunnel || !headIsTunnel);
	}
	return !tunnel;
}

static UnsignedInt ipSortClass( UnsignedInt ip, Bool tunnel, Bool cellular, Bool vpn )
{
	if ((ip & 0xFFFF0000) == 0xA9FE0000)
	{
		return 4;	// 169.254.0.0/16 link-local
	}
	if (!vpn)
	{
		return (tunnel || cellular) ? 1 : 0;	// off: tunnels (cellular with them) after the LAN
	}
	// On: a multi-instance client's OWN 127.x first - what keeps two instances on one machine from binding
	// the same address. Only that one: any other loopback, a 127.0.0.1 the hostname lookup
	// returned included, ranks as a plain address (w49vpn-r4, q7-macupd-r9) - then the VPN, the LAN, an
	// unpreferred tunnel.
	if (ip != 0 && ip == gxOwnInstanceIP())
	{
		return 0;
	}
	return cellular ? 3 : (tunnel ? 1 : 2);
}

static Bool ipSortsBefore( UnsignedInt lhs, Bool lhsTunnel, Bool lhsCellular, UnsignedInt rhs, Bool rhsTunnel, Bool rhsCellular )
{
	const UnsignedInt lhsClass = ipSortClass(lhs, lhsTunnel, lhsCellular, s_useVPNForLAN);
	const UnsignedInt rhsClass = ipSortClass(rhs, rhsTunnel, rhsCellular, s_useVPNForLAN);

	if (lhsClass != rhsClass)
	{
		return lhsClass < rhsClass;
	}

	return lhs < rhs;
}

// GeneralsX: The order with "Use my VPN for LAN" OFF, whatever
// the switch says: GameSpy's default address (OptionPreferences::getOnlineIPSelection) takes the first entry
// in it, so the LAN-only switch never decides where chat binds. Reads no global, so any thread may call it.
Bool IPEnumeration::sortsBeforeWithoutVPN( UnsignedInt lhs, Bool lhsTunnel, Bool lhsCellular, UnsignedInt rhs, Bool rhsTunnel, Bool rhsCellular )
{
	const UnsignedInt lhsClass = ipSortClass(lhs, lhsTunnel, lhsCellular, FALSE);
	const UnsignedInt rhsClass = ipSortClass(rhs, rhsTunnel, rhsCellular, FALSE);
	return lhsClass != rhsClass ? lhsClass < rhsClass : lhs < rhs;
}

void IPEnumeration::addNewIP( UnsignedByte a, UnsignedByte b, UnsignedByte c, UnsignedByte d, UnsignedInt broadcastIP, Bool tunnel, Bool cellular )
{
	UnsignedInt ip = AssembleIp(a, b, c, d);
	for (EnumeratedIP *current = m_IPlist; current != nullptr; current = current->getNext())
	{
		if (current->getIP() == ip)
		{
			return;
		}
	}

	EnumeratedIP *newIP = newInstance(EnumeratedIP);

	AsciiString str;
	str.format("%d.%d.%d.%d", (int)a, (int)b, (int)c, (int)d);

	newIP->setIPstring(str);
	newIP->setIP(ip);
	newIP->setBroadcastIP(broadcastIP);
	newIP->setTunnel(tunnel);
	newIP->setCellular(cellular);

	DEBUG_LOG(("IP: 0x%8.8X (%s) broadcast 0x%8.8X", ip, str.str(), broadcastIP));

	// Add the IP to the list in ascending order, routable addresses ahead of tunnels and link-local ones.
	if (!m_IPlist)
	{
		m_IPlist = newIP;
		newIP->setNext(nullptr);
	}
	else
	{
		if (ipSortsBefore(newIP->getIP(), tunnel, cellular, m_IPlist->getIP(), m_IPlist->isTunnel(), m_IPlist->isCellular()))
		{
			newIP->setNext(m_IPlist);
			m_IPlist = newIP;
		}
		else
		{
			EnumeratedIP *p = m_IPlist;
			while (p->getNext() && ipSortsBefore(p->getNext()->getIP(), p->getNext()->isTunnel(), p->getNext()->isCellular(), newIP->getIP(), tunnel, cellular))
			{
				p = p->getNext();
			}
			newIP->setNext(p->getNext());
			p->setNext(newIP);
		}
	}

	// GeneralsX: Release-visible. DEBUG_LOG above is ((void)0) in every
	// shipping preset, so without this the chosen interface is unobservable and a dead lobby looks
	// identical to a firewall drop.
	fprintf(stderr, "[LAN] enumerated %s broadcast=%d.%d.%d.%d class=%s\n",
		str.str(), PRINTF_IP_AS_4_INTS(broadcastIP),
		ipSortClass(ip, tunnel, cellular, s_useVPNForLAN) == 4 ? "link-local" : (cellular ? "tunnel-unpreferred" : (tunnel ? "tunnel" : "routable")));
	fflush(stderr);
}

AsciiString IPEnumeration::getMachineName()
{
	if (!m_isWinsockInitialized)
	{
		WORD verReq = MAKEWORD(2, 2);
		WSADATA wsadata;

		int err = WSAStartup(verReq, &wsadata);
		if (err != 0) {
			return "";
		}

		if ((LOBYTE(wsadata.wVersion) != 2) || (HIBYTE(wsadata.wVersion) !=2)) {
			WSACleanup();
			return "";
		}
		m_isWinsockInitialized = true;
	}

	// get the local machine's host name
	char hostname[256];
	if (gethostname(hostname, sizeof(hostname)))
	{
		DEBUG_LOG(("Failed call to gethostname; WSAGetLastError returned %d", WSAGetLastError()));
		return "";
	}

	return AsciiString(hostname);
}


