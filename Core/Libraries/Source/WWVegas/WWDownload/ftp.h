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

// ftp.h : Declaration of the Cftp

#pragma once

//#include "../resource.h"       // main symbols

#include <cstddef>
#ifdef _WIN32
#include <winsock.h>
// GeneralsX @bugfix Claude 27/07/2026
// The POSIX arm below gets strlcpy/strlcat, ARRAY_SIZE and socklen_t for free out of
// windows_compat.h, so FTP.cpp and Download.cpp were written against them unconditionally.
// Nothing supplied those names on the Windows arm, which left both sources referencing
// identifiers that only exist off-Windows. Mirror the same vocabulary here from the engine's
// own headers so the two arms of this header agree on what a socket TU can rely on.
#include "WWLib/stringex.h"   // strlcpy / strlcat
#include "WWLib/WWCommon.h"   // ARRAY_SIZE
// Winsock 1.1 has no socklen_t; it first appears in <ws2tcpip.h>, which cannot be used here
// because it drags winsock2.h in on top of winsock.h and the two collide. The Winsock 1.1
// getsockname()/accept() prototypes take int*, so this is the same typedef <ws2tcpip.h> makes.
typedef int socklen_t;
#else
#include "windows_compat.h"  // Includes socket_compat.h on Linux
#endif
#include <Utility/stdio_adapter.h>

#include "ftpdefs.h"

// FTP server return codes.  See RFC 959

#define FTPREPLY_SERVEROK		220
#define FTPREPLY_PASSWORD		331
#define FTPREPLY_LOGGEDIN		230
#define FTPREPLY_PORTOK			200
#define FTPREPLY_TYPEOK			200
#define FTPREPLY_RESTARTOK		350
#define FTPREPLY_CWDOK			250
#define FTPREPLY_OPENASCII		150
#define FTPREPLY_OPENBINARY		150
#define FTPREPLY_COMPLETE		226
#define FTPREPLY_CONTROLCLOSED	421

// Temporary download file name

#define FTP_TEMPFILENAME	"..\\__~DOWN_L~D"


/////////////////////////////////////////////////////////////////////////////
// Cftp
class Cftp
{
private:
	friend class CDownload;

	int		m_iCommandSocket;							// Socket for commands
	int		m_iDataSocket;								// Socket for data

	struct sockaddr_in m_CommandSockAddr;				// Address for commands
	struct sockaddr_in m_DataSockAddr;					// Address for data

	int		m_iFilePos;									// Byte offset into file
	int		m_iBytesRead;								// Number of bytes downloaded
	int		m_iFileSize;								// Total size of the file
	char	m_szRemoteFilePath[128];
	char	m_szRemoteFileName[128];
	char	m_szLocalFilePath[128];
	char	m_szLocalFileName[256];
	char	m_szServerName[128];
	char	m_szUserName[128];
	char	m_szPassword[128];
	FILE *	m_pfLocalFile;
	int		m_iStatus;

	int		m_sendNewPortStatus;
	int		m_findStart;

	int		SendData( char * pData, int iSize );
	int		RecvData( char * pData, int iSize );
	int		SendNewPort();
	int		OpenDataConnection();
	void		CloseDataConnection();
	int		AsyncGetHostByName( char * szName, struct sockaddr_in &address );

				// Convert a local filename into a temp filename to download into
	void		GetDownloadFilename( const char* localname, char* downloadname, size_t downloadname_size);

	void		CloseSockets();
	void		ZeroStuff();


public:
	Cftp();
	virtual ~Cftp();

public:
	HRESULT ConnectToServer(LPCSTR szServerName);
	HRESULT DisconnectFromServer();

	HRESULT LoginToServer( LPCSTR szUserName, LPCSTR szPassword );
	HRESULT LogoffFromServer();

	HRESULT FindFile( LPCSTR szRemoteFileName, int * piSize );

	HRESULT FileRecoveryPosition( LPCSTR szLocalFileName, LPCSTR szRegistryRoot );
	HRESULT RestartFrom( int i ) { m_iFilePos = i; return FTP_SUCCEEDED;  };

	HRESULT GetNextFileBlock( LPCSTR szLocalFileName, int * piTotalRead );

	HRESULT RecvReply( LPCSTR pReplyBuffer, int iSize, int * piRetCode );
	HRESULT SendCommand( LPCSTR pCommand, int iSize );

};
