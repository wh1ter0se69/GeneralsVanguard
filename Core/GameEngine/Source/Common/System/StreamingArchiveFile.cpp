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

//----------------------------------------------------------------------------
//
//                       Westwood Studios Pacific.
//
//                       Confidential Information
//                Copyright(C) 2001 - All Rights Reserved
//
//----------------------------------------------------------------------------
//
// Project:   RTS
//
// Module:    IO
//
// File name: StreamingArchiveFile.cpp
//
// Created:   12/06/02
//
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------
//         Includes
//----------------------------------------------------------------------------

#include "PreRTS.h"

#include <fcntl.h>
// GeneralsX @build BenderAI 12/02/2026 io.h is Windows-specific (file descriptors)
// Linux: Uses POSIX unistd.h for read/write/close/lseek instead
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include <sys/stat.h>

#include "Common/AsciiString.h"
#include "Common/FileSystem.h"
#include "Common/StreamingArchiveFile.h"
#include "Common/PerfTimer.h"


//----------------------------------------------------------------------------
//         Externals
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Defines
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Private Types
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Private Data
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Public Data
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Private Prototypes
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Private Functions
//----------------------------------------------------------------------------

//=================================================================
// StreamingArchiveFile::StreamingArchiveFile
//=================================================================

StreamingArchiveFile::StreamingArchiveFile()
: m_file(nullptr),
	m_startingPos(0),
	m_size(0),
	m_curPos(0)
{

}


//----------------------------------------------------------------------------
//         Public Functions
//----------------------------------------------------------------------------


//=================================================================
// StreamingArchiveFile::~StreamingArchiveFile
//=================================================================

StreamingArchiveFile::~StreamingArchiveFile()
{
}

//=================================================================
// StreamingArchiveFile::open
//=================================================================
/**
	* This function opens a file using the file system. Access flags
	* are mapped to the appropriate open flags. Returns true if file
	* was opened successfully.
	*/
//=================================================================

//DECLARE_PERF_TIMER(StreamingArchiveFile)
Bool StreamingArchiveFile::open( const Char *filename, Int access, size_t bufferSize )
{
	//USE_PERF_TIMER(StreamingArchiveFile)
	File *file = TheFileSystem->openFile( filename, access, bufferSize );

	if ( file == nullptr )
	{
		return FALSE;
	}

	return open( file );
}

//============================================================================
// StreamingArchiveFile::open
//============================================================================

Bool StreamingArchiveFile::open( File *file )
{
	return TRUE;
}

//============================================================================
// StreamingArchiveFile::openFromArchive
//============================================================================
Bool StreamingArchiveFile::openFromArchive(File *archiveFile, const AsciiString& filename, Int offset, Int size)
{
	//USE_PERF_TIMER(StreamingArchiveFile)
	if (archiveFile == nullptr) {
		return FALSE;
	}

	if (File::open(filename.str(), File::READ | File::BINARY | File::STREAMING) == FALSE) {
		return FALSE;
	}

	m_file = archiveFile;
	m_startingPos = offset;
	m_size = size;
	m_curPos = 0;

	if (m_file->seek(offset, File::START) != offset) {
		return FALSE;
	}

	if (m_file->seek(size) != m_startingPos + size) {
		return FALSE;
	}

	// We know this will succeed.
	m_file->seek(offset, File::START);

	m_nameStr = filename;

	return TRUE;
}

//=================================================================
// StreamingArchiveFile::close
//=================================================================
/**
	* Closes the current file if it is open.
  * Must call StreamingArchiveFile::close() for each successful StreamingArchiveFile::open() call.
	*/
//=================================================================

void StreamingArchiveFile::close()
{
	File::close();
}

//=================================================================
// StreamingArchiveFile::read
//=================================================================
// if buffer is null, just advance the current position by 'bytes'
Int StreamingArchiveFile::read( void *buffer, Int bytes )
{
	if (!m_file) {
		return 0;
	}

	// There shouldn't be a way that this can fail, because we've already verified that the file
	// contains at least this many bits.
	m_file->seek(m_startingPos + m_curPos, File::START);

	// GeneralsX @bugfix Claude 29/10/2026 Clamp without the sum: `bytes + m_curPos` overflowed Int for an entry ending near
	// INT_MAX once m_curPos was near its end (after seek(END)), and the read then ran past the entry into the archive's next
	// bytes (Codex tsh1028-3588-sec-r2 F2, batch tsh-1029). m_curPos stays in [0, m_size] (seek clamps it, a read advances
	// it by at most what is left), so m_size - m_curPos cannot overflow. A negative count reads nothing.
	if (bytes < 0)
		bytes = 0;
	if (bytes > m_size - m_curPos)
		bytes = m_size - m_curPos;

	Int bytesRead = m_file->read(buffer, bytes);

	if (bytesRead > 0)	// GeneralsX: a failed read (-1) leaves the position, keeping m_curPos in [0, m_size]
		m_curPos += bytesRead;

	return bytesRead;
}

//=================================================================
// StreamingArchiveFile::write
//=================================================================

Int StreamingArchiveFile::write( const void *buffer, Int bytes )
{
	DEBUG_CRASH(("Cannot write to streaming files."));
	return -1;
}

//=================================================================
// StreamingArchiveFile::seek
//=================================================================

Int StreamingArchiveFile::seek( Int pos, seekMode mode)
{
	// GeneralsX @bugfix Claude 29/10/2026 In 64 bits: `m_curPos + pos` and `m_size + pos` overflowed Int before the clamp
	// below for an entry near INT_MAX, so a seek past the end could land at 0 (batch tsh-1029 F2).
	Int64 newPos;

	switch( mode )
	{
		case START:
			newPos = pos;
			break;
		case CURRENT:
			newPos = (Int64)m_curPos + pos;
			break;
		case END:
			DEBUG_ASSERTCRASH(pos <= 0, ("StreamingArchiveFile::seek - position should be <= 0 for a seek starting from the end."));
			newPos = (Int64)m_size + pos;
			break;
		default:
			// bad seek mode
			return -1;
	}

	if ( newPos < 0 )
	{
		newPos = 0;
	}
	else if ( newPos > m_size )
	{
		newPos = m_size;
	}

	m_curPos = (Int)newPos;

	return m_curPos;

}

