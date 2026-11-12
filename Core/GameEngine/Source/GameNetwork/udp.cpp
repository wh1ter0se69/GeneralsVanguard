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

// FILE: Udp.cpp //////////////////////////////////////////////////////////////
// Implementation of UDP socket wrapper class (taken from wnet lib)
// Author: Matthew D. Campbell, July 2001
///////////////////////////////////////////////////////////////////////////////

// SYSTEM INCLUDES ////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#ifdef _WIN32
// mstcpip.h declares SIO_UDP_CONNRESET only behind a _WIN32_WINNT gate this translation unit
// does not necessarily meet; the value is ABI-stable, so define it the way gxlaunch.c does.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET	_WSAIOW(IOC_VENDOR, 12)
#endif
#endif

// USER INCLUDES //////////////////////////////////////////////////////////////
#include "Common/GameEngine.h"
//#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/udp.h"


//-------------------------------------------------------------------------

#ifdef DEBUG_LOGGING

#define CASE(x) case (x): return #x;

AsciiString GetWSAErrorString( Int error )
{
	switch (error)
	{
		CASE(WSABASEERR)
		CASE(WSAEINTR)
		CASE(WSAEBADF)
		CASE(WSAEACCES)
		CASE(WSAEFAULT)
		CASE(WSAEINVAL)
		CASE(WSAEMFILE)
		CASE(WSAEWOULDBLOCK)
		CASE(WSAEINPROGRESS)
		CASE(WSAEALREADY)
		CASE(WSAENOTSOCK)
		CASE(WSAEDESTADDRREQ)
		CASE(WSAEMSGSIZE)
		CASE(WSAEPROTOTYPE)
		CASE(WSAENOPROTOOPT)
		CASE(WSAEPROTONOSUPPORT)
		CASE(WSAESOCKTNOSUPPORT)
		CASE(WSAEOPNOTSUPP)
		CASE(WSAEPFNOSUPPORT)
		CASE(WSAEAFNOSUPPORT)
		CASE(WSAEADDRINUSE)
		CASE(WSAEADDRNOTAVAIL)
		CASE(WSAENETDOWN)
		CASE(WSAENETUNREACH)
		CASE(WSAENETRESET)
		CASE(WSAECONNABORTED)
		CASE(WSAECONNRESET)
		CASE(WSAENOBUFS)
		CASE(WSAEISCONN)
		CASE(WSAENOTCONN)
		CASE(WSAESHUTDOWN)
		CASE(WSAETOOMANYREFS)
		CASE(WSAETIMEDOUT)
		CASE(WSAECONNREFUSED)
		CASE(WSAELOOP)
		CASE(WSAENAMETOOLONG)
		CASE(WSAEHOSTDOWN)
		CASE(WSAEHOSTUNREACH)
		CASE(WSAENOTEMPTY)
		CASE(WSAEPROCLIM)
		CASE(WSAEUSERS)
		CASE(WSAEDQUOT)
		CASE(WSAESTALE)
		CASE(WSAEREMOTE)
		CASE(WSAEDISCON)
		CASE(WSASYSNOTREADY)
		CASE(WSAVERNOTSUPPORTED)
		CASE(WSANOTINITIALISED)
		CASE(WSAHOST_NOT_FOUND)
		CASE(WSATRY_AGAIN)
		CASE(WSANO_RECOVERY)
		CASE(WSANO_DATA)
		default:
		{
			AsciiString ret;
			ret.format("Not a Winsock error (%d)", error);
			return ret;
		}
	}
	return AsciiString::TheEmptyString; // will not be hit, ever.
}

#undef CASE

#endif // defined(RTS_DEBUG)

//-------------------------------------------------------------------------

UDP::UDP()
{
  fd=0;
}

UDP::~UDP()
{
	if (fd)
		closesocket(fd);
}

Int UDP::Bind(const char *Host,UnsignedShort port)
{
  struct hostent *hostStruct;
  struct in_addr *hostNode;

  if (isdigit(Host[0]))
    return ( Bind( ntohl(inet_addr(Host)), port) );

  hostStruct = gethostbyname(Host);
  if (hostStruct == nullptr)
    return (0);
  hostNode = (struct in_addr *) hostStruct->h_addr;
  return ( Bind(ntohl(hostNode->s_addr),port) );
}

// You must call bind, implicit binding is for sissies
//   Well... you can get implicit binding if you pass 0 for either arg
Int UDP::Bind(UnsignedInt IP,UnsignedShort Port)
{
  int retval;
  int status;

  IP=htonl(IP);
  Port=htons(Port);

  addr.sin_family=AF_INET;
  addr.sin_port=Port;
  addr.sin_addr.s_addr=IP;
  fd=socket(AF_INET,SOCK_DGRAM,DEFAULT_PROTOCOL);
  #ifdef _WIN32
  if (fd==SOCKET_ERROR)
    fd=-1;
  #endif
  if (fd==-1)
    return(UNKNOWN);

  // GeneralsX: Put the socket in non-blocking mode BEFORE the bind rather than after.
  // The old order returned on bind failure at the line below, skipping SetBlocking entirely,
  // so a socket that failed to bind stayed blocking - and because GetStatus() was pinned to OK
  // on POSIX (see mapPosixReadError above) the caller took it as a working socket and later
  // parked the main thread in recvfrom(). bind() does not block, so hoisting this is free.
  if (SetBlocking(FALSE)==UNKNOWN)
    fprintf(stderr,"Couldn't set nonblocking mode!\n");

  #ifdef _WIN32
  // GeneralsX: SIO_UDP_CONNRESET. On Windows a closed peer socket sends ICMP port-unreachable, which makes the NEXT
  // recvfrom fail WSAECONNRESET; UDP::Read returns -1 and Transport::doRecv's drain loop aborts, so one departed
  // peer throttles a live peer's receive path. A failed ioctl is not worth failing the bind over.
  {
    DWORD off = 0, ret = 0;
    WSAIoctl(fd, SIO_UDP_CONNRESET, &off, sizeof(off), NULL, 0, &ret, NULL, NULL);
  }
  #endif

  retval=bind(fd,(struct sockaddr *)&addr,sizeof(addr));

  #ifdef _WIN32
  if (retval==SOCKET_ERROR)
	{
    retval=-1;
		m_lastError = WSAGetLastError();
	}
  #else
  // GeneralsX: Record errno so GetStatus() can report the real failure. Without this
  // it returned OK and a failed bind looked like a successful one.
  if (retval==-1)
		m_lastError = errno;
  #endif
  if (retval==-1)
  {
    status=GetStatus();
    // GeneralsX: GetStatus() maps 0 to OK, so an errno this switch does not know
    // (EADDRNOTAVAIL is the common one - a stale Options.ini IPAddress naming an interface
    // that no longer exists) must still surface as a failure rather than as OK.
    if (status==OK)
      status=UNKNOWN;
    fprintf(stderr,"UDP::Bind failed (status %d) for IP %u.%u.%u.%u port %u\n",
            (int)status,
            (unsigned)((ntohl(IP)>>24)&0xff), (unsigned)((ntohl(IP)>>16)&0xff),
            (unsigned)((ntohl(IP)>>8)&0xff),  (unsigned)(ntohl(IP)&0xff),
            (unsigned)ntohs(Port));
    fflush(stderr);
    // GeneralsX: Close the socket a failed bind leaves behind. Transport::init
    // retries Bind on the same object for a whole second, and every attempt overwrote fd with a new
    // socket: with the port taken, macOS leaked ~61400 descriptors (kern.maxfilesperproc) in that
    // second. The process then could not open Options.ini, so the next initRelay read an ABSENT
    // RelayAddress - which means the bootstrap production relay - and could not resolve it either.
    closesocket(fd);
    fd=0;
    return(status);
  }

  return(OK);
}

Int UDP::getLocalAddr(UnsignedInt &ip, UnsignedShort &port)
{
  ip=myIP;
  port=myPort;
  return(OK);
}


// private function
Int UDP::SetBlocking(Int block)
{
  #ifdef _WIN32
   unsigned long flag=1;
   if (block)
     flag=0;
   int retval;
   retval=ioctlsocket(fd,FIONBIO,&flag);
   if (retval==SOCKET_ERROR)
     return(UNKNOWN);
   else
     return(OK);
  #else  // UNIX
   int flags = fcntl(fd, F_GETFL, 0);
   if (block==FALSE)          // set nonblocking
     flags |= O_NONBLOCK;
   else                       // set blocking
     flags &= ~(O_NONBLOCK);

   if (fcntl(fd, F_SETFL, flags) < 0)
   {
     return(UNKNOWN);
   }
   return(OK);
  #endif
}


Int UDP::Write(const unsigned char *msg,UnsignedInt len,UnsignedInt IP,UnsignedShort port)
{
  Int retval;
  struct sockaddr_in to;

  // This happens frequently
  if ((IP==0)||(port==0)) return(ADDRNOTAVAIL);

#ifdef _UNIX
  errno=0;
#endif
  to.sin_port=htons(port);
  to.sin_addr.s_addr=htonl(IP);
  to.sin_family=AF_INET;

  ClearStatus();
  retval=sendto(fd,(const char *)msg,len,0,(struct sockaddr *)&to,sizeof(to));
  #ifdef _WIN32
  if (retval==SOCKET_ERROR)
	{
    retval=-1;
		m_lastError = WSAGetLastError();
#ifdef DEBUG_LOGGING
		static Int errCount = 0;
#endif
		DEBUG_ASSERTLOG(errCount++ > 100, ("UDP::Write() - WSA error is %s", GetWSAErrorString(WSAGetLastError()).str()));
	}
  #else
  // GeneralsX: Record errno on POSIX so Transport::doSend can distinguish a transient
  // EWOULDBLOCK (retry) from a hard EHOSTUNREACH/EACCES (drop). Without it both read as OK
  // and the failed message was retained forever, leaking one of MAX_MESSAGES slots per failure.
  if (retval==-1)
		m_lastError = errno;
  #endif

  return(retval);
}

#ifndef _WIN32
// GeneralsX: errno is recorded on POSIX too, so GetStatus() and Bind report real failures; an unreported failed Bind
// left a BLOCKING socket that froze Transport::doRecv. POSIX recvfrom reports an idle socket as -1/EAGAIN, so that
// drain case is filtered here before any error is recorded, or doRecv would fail on every idle tick.
Int UDP::mapPosixReadError()
{
	const int err = errno;
	if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR)
		return 0;			// nothing queued (or interrupted); not an error
	m_lastError = err;
	return -1;
}
#endif

Int UDP::Read(unsigned char *msg,UnsignedInt len,sockaddr_in *from)
{
  Int retval;
  // GeneralsX: Use socklen_t for POSIX socket functions (fighter19 pattern)
  socklen_t alen=sizeof(sockaddr_in);

  if (from!=nullptr)
  {
    retval=recvfrom(fd,(char *)msg,len,0,(struct sockaddr *)from,&alen);
    #ifdef _WIN32
    if (retval == SOCKET_ERROR)
		{
			if (WSAGetLastError() != WSAEWOULDBLOCK)
			{
				// failing because of a blocking error isn't really such a bad thing.
				m_lastError = WSAGetLastError();
#ifdef DEBUG_LOGGING
				static Int errCount = 0;
#endif
				DEBUG_ASSERTLOG(errCount++ > 100, ("UDP::Read() - WSA error is %s", GetWSAErrorString(WSAGetLastError()).str()));
				retval = -1;
			} else {
				retval = 0;
			}
		}
    #else
		if (retval < 0)
			retval = mapPosixReadError();
    #endif
  }
  else
  {
    retval=recvfrom(fd,(char *)msg,len,0,nullptr,nullptr);
    #ifdef _WIN32
    if (retval==SOCKET_ERROR)
		{
			if (WSAGetLastError() != WSAEWOULDBLOCK)
			{
				// failing because of a blocking error isn't really such a bad thing.
				m_lastError = WSAGetLastError();
#ifdef DEBUG_LOGGING
				static Int errCount = 0;
#endif
				DEBUG_ASSERTLOG(errCount++ > 100, ("UDP::Read() - WSA error is %s", GetWSAErrorString(WSAGetLastError()).str()));
				retval = -1;
			} else {
				retval = 0;
			}
		}
    #else
		if (retval < 0)
			retval = mapPosixReadError();
    #endif
  }
  return(retval);
}


void UDP::ClearStatus()
{
  #ifndef _WIN32
  errno=0;
  #endif

	m_lastError = 0;
}

UDP::sockStat UDP::GetStatus()
{
	Int status = m_lastError;
 #ifdef _WIN32
  //int status=WSAGetLastError();
  switch (status) {
    case NO_ERROR:
      return OK;
    case WSAEINTR:
      return INTR;
    case WSAEINPROGRESS:
      return INPROGRESS;
    case WSAECONNREFUSED:
      return CONNREFUSED;
    case WSAEINVAL:
      return INVAL;
    case WSAEISCONN:
      return ISCONN;
    case WSAENOTSOCK:
      return NOTSOCK;
    case WSAETIMEDOUT:
      return TIMEDOUT;
    case WSAEALREADY:
      return ALREADY;
    case WSAEWOULDBLOCK:
      return WOULDBLOCK;
    case WSAEBADF:
      return BADF;
    default:
      return (UDP::sockStat)status;
  }
 #else
  //int status=errno;
  switch (status) {
    case 0:
      return OK;
    case EINTR:
      return INTR;
    case EINPROGRESS:
      return INPROGRESS;
    case ECONNREFUSED:
      return CONNREFUSED;
    case EINVAL:
      return INVAL;
    case EISCONN:
      return ISCONN;
    case ENOTSOCK:
      return NOTSOCK;
    case ETIMEDOUT:
      return TIMEDOUT;
    case EALREADY:
      return ALREADY;
    case EAGAIN:
      return AGAIN;
    // GeneralsX: EWOULDBLOCK == EAGAIN on Linux (duplicate case)
    #if EAGAIN != EWOULDBLOCK
    case EWOULDBLOCK:
      return WOULDBLOCK;
    #endif
    case EBADF:
      return BADF;
    // GeneralsX: Map the errnos this port actually hits. EADDRNOTAVAIL is a stale
    // Options.ini IPAddress naming an interface that no longer exists; EADDRINUSE is a
    // second instance already holding 8086; EHOSTUNREACH is what Darwin returns for a
    // limited broadcast to 255.255.255.255, which is how LAN discovery fails here.
    case EADDRNOTAVAIL:
      return ADDRNOTAVAIL;
    case EADDRINUSE:
      return ADDRINUSE;
    case EPIPE:
      return PIPE;
    default:
      return UNKNOWN;
  }
 #endif
}



/*
//
// Wait for net activity on this socket
//
int UDP::Wait(Int sec,Int usec,fd_set &returnSet)
{
  fd_set inputSet;

  FD_ZERO(&inputSet);
  FD_SET(fd,&inputSet);

  return(Wait(sec,usec,inputSet,returnSet));
}
*/

/*
//
// Wait for net activity on a list of sockets
//
int UDP::Wait(Int sec,Int usec,fd_set &givenSet,fd_set &returnSet)
{
  Wtime        timeout,timenow,timethen;
  fd_set       backupSet;
  int          retval=0,done,givenMax;
  Bool         noTimeout=FALSE;
  timeval      tv;

  returnSet=givenSet;
  backupSet=returnSet;

  if ((sec==-1)&&(usec==-1))
    noTimeout=TRUE;

  timeout.SetSec(sec);
  timeout.SetUsec(usec);
  timethen+=timeout;

  givenMax=fd;
  for (UnsignedInt i=0; i<(sizeof(fd_set)*8); i++)   // i=maxFD+1
  {
    if (FD_ISSET(i,&givenSet))
      givenMax=i;
  }
  ///DBGMSG("WAIT  fd="<<fd<<"  givenMax="<<givenMax);

  done=0;
  while( ! done)
  {
    if (noTimeout)
      retval=select(givenMax+1,&returnSet,0,0,nullptr);
    else
    {
      timeout.GetTimevalMT(tv);
      retval=select(givenMax+1,&returnSet,0,0,&tv);
    }

    if (retval>=0)
      done=1;

    else if ((retval==-1)&&(errno==EINTR))  // in case of signal
    {
      if (noTimeout==FALSE)
      {
        timenow.Update();
        timeout=timethen-timenow;
      }
      if ((noTimeout==FALSE)&&(timenow.GetSec()==0)&&(timenow.GetUsec()==0))
        done=1;
      else
        returnSet=backupSet;
    }
    else  // maybe out of memory?
    {
      done=1;
    }
  }
  ///DBGMSG("Wait retval: "<<retval);
  return(retval);
}
*/




// Set the kernel buffer sizes for incoming, and outgoing packets
//
// Linux seems to have a buffer max of 32767 bytes for this,
//  (which is the default). If you try and set the size to
//  greater than the default it just sets it to 32767.

Int UDP::SetInputBuffer(UnsignedInt bytes)
{
   int retval,arg=bytes;

   retval=setsockopt(fd,SOL_SOCKET,SO_RCVBUF,
     (char *)&arg,sizeof(int));
   if (retval==0)
     return(TRUE);
   else
     return(FALSE);
}

// Same note goes for the output buffer

Int UDP::SetOutputBuffer(UnsignedInt bytes)
{
   int retval,arg=bytes;

   retval=setsockopt(fd,SOL_SOCKET,SO_SNDBUF,
     (char *)&arg,sizeof(int));
   if (retval==0)
     return(TRUE);
   else
     return(FALSE);
}

// Get the system buffer sizes

int UDP::GetInputBuffer()
{
   int retval,arg=0;
   // GeneralsX: Use socklen_t for POSIX socket functions (fighter19 pattern)
   socklen_t len=sizeof(int);

   retval=getsockopt(fd,SOL_SOCKET,SO_RCVBUF,
     (char *)&arg,&len);
   return(arg);
}


int UDP::GetOutputBuffer()
{
   int retval,arg=0;
   // GeneralsX: Use socklen_t for POSIX socket functions (fighter19 pattern)
   socklen_t len=sizeof(int);

   retval=getsockopt(fd,SOL_SOCKET,SO_SNDBUF,
     (char *)&arg,&len);
   return(arg);
}

Int UDP::AllowBroadcasts(Bool status)
{
	int retval;
	BOOL val = status;
	retval = setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (char *)&val, sizeof(BOOL));
	if (retval == 0)
		return TRUE;

	// GeneralsX: No caller has ever looked at this return value, and a socket missing
	// SO_BROADCAST refuses every discovery send with EACCES - at the LAN screen that is
	// indistinguishable from an empty network, which is the hardest kind of failure to chase.
	DEBUG_LOG(("UDP::AllowBroadcasts - setsockopt(SO_BROADCAST, %d) failed with error %d",
		(Int)status, WSAGetLastError()));
	return FALSE;
}
