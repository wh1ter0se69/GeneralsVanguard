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
// Project:   RTS3
//
// File name: GameText.cpp
//
// Created:   11/07/01
//
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------
//         Includes
//----------------------------------------------------------------------------

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/GameText.h"
#include "Common/Language.h"
#include "Common/Registry.h"
#include "GameClient/LanguageFilter.h"
#include "Common/Debug.h"
#include "Common/UnicodeString.h"
#include "Common/AsciiString.h"
#include "Common/GlobalData.h"
#include "Common/file.h"
#include "Common/FileSystem.h"
#include "Common/LocalFileSystem.h"
#include "Common/version.h"




//----------------------------------------------------------------------------
//         Externals
//----------------------------------------------------------------------------

#if defined(RTS_DEBUG)
Bool g_useStringFile = TRUE;
#endif


//----------------------------------------------------------------------------
//         Defines
//----------------------------------------------------------------------------

#define CSF_ID ( ('C'<<24) | ('S'<<16) | ('F'<<8) | (' ') )
#define CSF_LABEL ( ('L'<<24) | ('B'<<16) | ('L'<<8) | (' ') )
#define CSF_STRING ( ('S'<<24) | ('T'<<16) | ('R'<<8) | (' ') )
#define CSF_STRINGWITHWAVE ( ('S'<<24) | ('T'<<16) | ('R'<<8) | ('W') )
#define CSF_VERSION 3

#define STRING_FILE 0
#define CSF_FILE 1
#define MAX_UITEXT_LENGTH (10*1024)
//----------------------------------------------------------------------------
//         Private Types
//----------------------------------------------------------------------------

//===============================
// StringInfo
//===============================

struct StringInfo
{
	AsciiString			label;
	UnicodeString		text;
	AsciiString			speech;
};

struct StringLookUp
{
	AsciiString		*label;
	StringInfo		*info;
};

//===============================
// CSFHeader
//===============================

struct CSFHeader
{
	Int id;
	Int version;
	Int num_labels;
	Int num_strings;
	Int skip;
	Int langid;

};

//===============================
// struct NoString
//===============================

struct NoString
{
	struct NoString *next;
	UnicodeString text;
};


//===============================
// GameTextManager
//===============================

class GameTextManager : public GameTextInterface
{
	public:

		GameTextManager();
		virtual ~GameTextManager() override;

		virtual void					init() override;						///< Initializes the text system
		virtual void					deinit();					///< Shuts down the text system
		virtual void					update() override {};			///< update text manager
		virtual void					reset() override;					///< Resets the text system

		virtual UnicodeString fetch( const Char *label, Bool *exists = nullptr ) override;		///< Returns the associated labeled unicode text
		virtual UnicodeString fetch( AsciiString label, Bool *exists = nullptr ) override;		///< Returns the associated labeled unicode text
		virtual UnicodeString fetchFormat( const Char *label, ... ) override;
		virtual UnicodeString fetchGameOnly( const Char *label, Bool *exists = nullptr ) override { return fetchFrom( label, exists, FALSE ); }
		virtual UnicodeString fetchOrSubstitute( const Char *label, const WideChar *substituteText ) override;
		virtual UnicodeString fetchOrSubstituteFormat( const Char *label, const WideChar *substituteFormat, ... ) override;
		virtual UnicodeString fetchOrSubstituteFormatVA( const Char *label, const WideChar *substituteFormat, va_list args ) override;

		virtual AsciiStringVec& getStringsWithLabelPrefix(AsciiString label) override;

		virtual void					initMapStringFile( const AsciiString& filename ) override;

	protected:

		Int							m_textCount;
		Int							m_maxLabelLen;
		Char						m_buffer[MAX_UITEXT_LENGTH];
		Char						m_buffer2[MAX_UITEXT_LENGTH];
		Char						m_buffer3[MAX_UITEXT_LENGTH];
		WideChar				m_tbuffer[MAX_UITEXT_LENGTH*2];

		StringInfo			*m_stringInfo;
		StringLookUp		*m_stringLUT;
		StringInfo			*m_fallbackStringInfo;
		StringLookUp		*m_fallbackStringLUT;
		Int						m_fallbackTextCount;
		Bool						m_initialized;
#if defined(RTS_DEBUG)
		Bool						m_jabberWockie;
		Bool						m_munkee;
#endif
		NoString				*m_noStringList;
		Int							m_useStringFile;
		LanguageID			m_language;
		UnicodeString		m_failed;

		StringInfo			*m_mapStringInfo;
		StringLookUp		*m_mapStringLUT;
		Int							m_mapTextCount;

		/// m_asciiStringVec will be altered every time that getStringsWithLabelPrefix is called,
		/// so don't simply store a pointer to it.
		AsciiStringVec			m_asciiStringVec;

		void						stripSpaces ( WideChar *string );
		void						removeLeadingAndTrailing ( Char *m_buffer );
		void						readToEndOfQuote( File *file, Char *in, Char *out, Char *wavefile, Int maxBufLen );
		void						reverseWord ( Char *file, Char *lp );
		void						translateCopy( WideChar *outbuf, Char *inbuf );
		Bool						getStringCount( const Char *filename, Int& textCount );
		Bool						getCSFInfo ( const Char *filename, Int& textCount, LanguageID& language, FileInstance instance = 0 );
		Bool						parseCSF(  const Char *filename, StringInfo *stringInfo, Int textCount, Int& maxLabelLen, FileInstance instance = 0 );
		Bool						parseStringFile( const char *filename );
		Bool						parseMapStringFile( const char *filename );
		Bool						readLine( char *buffer, Int max, File *file );
		UnicodeString				fetchFrom( const Char *label, Bool *exists, Bool allowMapText );
};

static int __cdecl			compareLUT ( const void *,  const void*);
//----------------------------------------------------------------------------
//         Private Data
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Public Data
//----------------------------------------------------------------------------

GameTextInterface *TheGameText = nullptr;

//----------------------------------------------------------------------------
//         Private Prototypes
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Private Functions
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Public Functions
//----------------------------------------------------------------------------

//============================================================================
// CreateGameTextInterface
//============================================================================

GameTextInterface* CreateGameTextInterface()
{
	return NEW GameTextManager;
}


//============================================================================
// GameTextManager::GameTextManager
//============================================================================

GameTextManager::GameTextManager()
:	m_textCount(0),
	m_maxLabelLen(0),
	m_stringInfo(nullptr),
	m_stringLUT(nullptr),
	m_fallbackStringInfo(nullptr),
	m_fallbackStringLUT(nullptr),
	m_fallbackTextCount(0),
	m_initialized(FALSE),
	m_noStringList(nullptr),
#if defined(RTS_DEBUG)
	m_jabberWockie(FALSE),
	m_munkee(FALSE),
	m_useStringFile(g_useStringFile),
#else
	m_useStringFile(TRUE),
#endif
	m_mapStringInfo(nullptr),
	m_mapStringLUT(nullptr),
	m_failed(L"***FATAL*** String Manager failed to initialize properly")
{
	for(Int i=0; i < MAX_UITEXT_LENGTH; i++)
	{
		m_buffer[i] = 0;
		m_buffer2[i] = 0;
		m_buffer3[i] = 0;
	}
}

//============================================================================
// GameTextManager::~GameTextManager
//============================================================================

GameTextManager::~GameTextManager()
{
	deinit();
}

//============================================================================
// GameTextManager::init
//============================================================================

extern const Char *g_strFile;
extern const Char *g_csfFile;

void GameTextManager::init()
{
	AsciiString csfFile;
	csfFile.format(g_csfFile, GetRegistryLanguage().str());
	Int format;

	// GeneralsX: Debug CSF init
	fprintf(stderr, "[CSF] init() - START\n");

	if ( m_initialized )
	{
		return;
	}

	m_initialized = TRUE;

	m_maxLabelLen = 0;
#if defined(RTS_DEBUG)
	if(TheGlobalData)
	{
		m_jabberWockie = TheGlobalData->m_jabberOn;
		m_munkee = 	TheGlobalData->m_munkeeOn;
	}
#endif

	if ( m_useStringFile && getStringCount( g_strFile, m_textCount ) )
	{
		format = STRING_FILE;
	}
	else if ( getCSFInfo ( csfFile.str(), m_textCount, m_language ) )
	{
		fprintf(stderr, "[CSF] init() - getCSFInfo OK, textCount=%d\n", m_textCount);
		format = CSF_FILE;
	}
	else
	{
		fprintf(stderr, "[CSF] init() - getCSFInfo FAILED\n");
		return;
	}

	if( m_textCount == 0 )
	{
		return;
	}

	//Allocate StringInfo Array

	m_stringInfo = NEW StringInfo[m_textCount];

	if( m_stringInfo == nullptr )
	{
		deinit();
		return;
	}

	if ( format == STRING_FILE )
	{
		if( parseStringFile( g_strFile ) == FALSE )
		{
			deinit();
			return;
		}
	}
	else
	{
		fprintf(stderr, "[CSF] init() - Calling parseCSF()...\n");
		if ( !parseCSF ( csfFile.str(), m_stringInfo, m_textCount, m_maxLabelLen ) )
		{
			fprintf(stderr, "[CSF] init() - parseCSF FAILED\n");
			deinit();
			return;
		}
		fprintf(stderr, "[CSF] init() - parseCSF SUCCESS\n");
	}

	m_stringLUT = NEW StringLookUp[m_textCount];

	StringLookUp *lut = m_stringLUT;
	StringInfo *info = m_stringInfo;

	for ( Int i = 0; i < m_textCount; i++ )
	{
		lut->info = info;
		lut->label = &info->label;
		lut++;
		info++;
	}

	qsort( m_stringLUT, m_textCount, sizeof(StringLookUp), compareLUT  );

	// GeneralsX: Load fallback CSF instance when a mod provides an incomplete table.
	// GeneralsX: A string FILE (Generals.str) gets the CSF as its fallback too.
	// Without it a label an incomplete .str lacks fell through to the loaded map's map.str, and the engine uses ~120 such labels as
	// printf formats (format(fetch("CONTROLBAR:PowerDescription"), ...) and the like), so a custom map could supply the format.
	// Which CSF: instance 0 is the loose file when one exists, else the first archive (FileSystem::openFile). GeneralsX ships its overlay
	// as the loose Data/<lang>/Generals.csf, so with it the retail table is instance 1; without it the retail table is instance 0 -
	// instance 1 would then be W3DEnglishZH.big's older CSF, with stale text. CSF mode keeps instance 1.
	{
		Int fallbackCount = 0;
		LanguageID originalLanguage = m_language;
		FileInstance fallbackInstance = 1;
		if ( format == STRING_FILE )
		{
			// A lone loose CSF (no archive copy behind it) is itself the fallback.
			const Bool loose = TheLocalFileSystem && TheLocalFileSystem->doesFileExist(csfFile.str());
			if ( !loose || !getCSFInfo(csfFile.str(), fallbackCount, m_language, 1) )
				fallbackInstance = 0;
			fallbackCount = 0;
		}

		if ( getCSFInfo(csfFile.str(), fallbackCount, m_language, fallbackInstance) && fallbackCount > 0 )
		{
			m_fallbackStringInfo = NEW StringInfo[fallbackCount];

			if ( m_fallbackStringInfo != nullptr )
			{
				Int fallbackMaxLabelLen = m_maxLabelLen;
				if ( parseCSF(csfFile.str(), m_fallbackStringInfo, fallbackCount, fallbackMaxLabelLen, fallbackInstance) )
				{
					m_fallbackTextCount = fallbackCount;
					m_maxLabelLen = max(m_maxLabelLen, fallbackMaxLabelLen);

					m_fallbackStringLUT = NEW StringLookUp[m_fallbackTextCount];

					if ( m_fallbackStringLUT != nullptr )
					{
						StringLookUp *fallbackLut = m_fallbackStringLUT;
						StringInfo *fallbackInfo = m_fallbackStringInfo;

						for ( Int i = 0; i < m_fallbackTextCount; i++ )
						{
							fallbackLut->info = fallbackInfo;
							fallbackLut->label = &fallbackInfo->label;
							fallbackLut++;
							fallbackInfo++;
						}

						qsort( m_fallbackStringLUT, m_fallbackTextCount, sizeof(StringLookUp), compareLUT );
					}
					else
					{
						delete [] m_fallbackStringInfo;
						m_fallbackStringInfo = nullptr;
						m_fallbackTextCount = 0;
					}
				}
				else
				{
					delete [] m_fallbackStringInfo;
					m_fallbackStringInfo = nullptr;
				}
			}
		}

		m_language = originalLanguage;
	}

}

//============================================================================
// GameTextManager::deinit
//============================================================================

void GameTextManager::deinit()
{

	delete [] m_stringInfo;
	m_stringInfo = nullptr;

	delete [] m_stringLUT;
	m_stringLUT = nullptr;

	delete [] m_fallbackStringInfo;
	m_fallbackStringInfo = nullptr;

	delete [] m_fallbackStringLUT;
	m_fallbackStringLUT = nullptr;

	m_textCount = 0;
	m_fallbackTextCount = 0;

	NoString *noString = m_noStringList;

	DEBUG_LOG_RAW(("\n"));
	DEBUG_LOG(("*** Missing strings ***"));
	while ( noString )
	{
		DEBUG_LOG(("*** %ls ***", noString->text.str()));
		NoString *next = noString->next;
		delete noString;
		noString = next;
	}
	DEBUG_LOG(("*** End missing strings ***"));
	DEBUG_LOG_RAW(("\n"));

	m_noStringList = nullptr;

	m_initialized = FALSE;
}

//============================================================================
// GameTextManager::reset
//============================================================================

void GameTextManager::reset()
{
	delete [] m_mapStringInfo;
	m_mapStringInfo = nullptr;

	delete [] m_mapStringLUT;
	m_mapStringLUT = nullptr;
}


//============================================================================
// GameTextManager::stripSpaces
//============================================================================

void GameTextManager::stripSpaces ( WideChar *string )
{
	WideChar *str, *ptr;
	WideChar ch, last = 0;
	Int skipall = TRUE;

	str = ptr = string;

	while ( (ch = *ptr++) != 0 )
	{
		if ( ch == ' '  )
		{
			if ( last == ' ' || skipall )
			{
				continue;
			}
		}

		if ( ch == '\n' || ch == '\t' )
		{
				// remove last space
				if ( last == ' ' )
				{
					str--;
				}

				skipall = TRUE;		// skip all spaces
				last = *str++ = ch;
				continue;
		}

		last = *str++ = ch;
		skipall = FALSE;
	}

	if ( last == ' ' )
	{
		str--;
	}

	*str = 0;
}

//============================================================================
// GameTextManager::removeLeadingAndTrailing
//============================================================================

void GameTextManager::removeLeadingAndTrailing ( Char *buffer )
{
	Char *first, *ptr;
	Char ch;

	ptr = first = buffer;

	while ( (ch = *first) != 0 && iswspace ( ch ))
	{
			first++;
	}

	while ( (*ptr++ = *first++) != 0 );

	ptr -= 2;

	while ( (ptr > buffer) && (ch = *ptr) != 0 && iswspace ( ch ) )
	{
		ptr--;
	}

	ptr++;
	*ptr = 0;
}

//============================================================================
// GameTextManager::readToEndOfQuote
//============================================================================

void GameTextManager::readToEndOfQuote( File *file, Char *in, Char *out, Char *wavefile, Int maxBufLen )
{
	Int slash = FALSE;
	Int state = 0;
	Int line_start = FALSE;
	Char ch;
	Int ccount = 0;
	Int len = 0;
	Int done = FALSE;

	// GeneralsX: A damaged map.str (sent to every lobby peer with the map) must not write past
	// m_buffer2/m_buffer3 or hang the loader. The text loop now keeps the last byte for its terminator (it still reads the
	// closing quote there, so a string that fits exactly keeps its wave name), the wave-name loop below never wrote with any
	// bound at all, and end of file was never seen: readChar() returned 0 there, not EOF, so an unclosed quote at the end
	// of the file spun the wave-name loop forever (and a raw 0xFF byte, equal to EOF as a signed Char, ended the text
	// early). End of file is now the read failing, nothing else. Well-formed files read as before.
	const Int bufLen = maxBufLen;
	Bool atEnd = FALSE;
	*wavefile = 0;	// an entry that ends early must not keep the previous entry's wave name
	auto nextChar = [&]() -> Char
	{
		if ( in )
		{
			Char c = *in++;
			if ( c != 0 )
			{
				return c;
			}
			in = nullptr; // have exhausted the input m_buffer
		}
		Char c;
		if ( file->read( &c, 1 ) == 1 )
		{
			return c;
		}
		atEnd = TRUE;
		return 0;
	};

	while ( maxBufLen )
	{
		// get next Char
		ch = nextChar();

		if ( atEnd )
		{
			*out = 0;
			return ;
		}

		if ( ch == '\n' )
		{
			line_start = TRUE;
			slash = FALSE;
			ccount = 0;
			ch = ' ';
		}
		else if ( ch == '\\' && !slash)
		{
			slash = TRUE;
		}
		else if ( ch == '\\' && slash)
		{
			slash = FALSE;
		}
		else if ( ch == '"' && !slash )
		{
			break; // done
		}
		else
		{
			slash = FALSE;
		}

		if ( iswspace ( ch ))
		{
			ch = ' ';
		}

		if ( maxBufLen == 1 )
		{
			break; // the last byte is the terminator's
		}

		*out++ = ch;
		maxBufLen--;
	}

	*out = 0;

	while ( !done )
	{
		// get next Char
		ch = nextChar();

		if ( atEnd || ch == '\n' )
		{
			break;
		}

		switch ( state )
		{

			case 0:
				if ( iswspace ( ch ) || ch == '=' )
				{
					break;
				}

				state = 1;
				FALLTHROUGH;
			case 1:
				if ( ( ch >= 'a' && ch <= 'z') || ( ch >= 'A' && ch <='Z') || (ch >= '0' && ch <= '9') || ch == '_' )
				{
					if ( len < bufLen - 1 ) // room for the terminator
					{
						*wavefile++ = ch;
						len++;
					}
					break;
				}
				state = 2;
				FALLTHROUGH;
			case 2:
				break;
		}
	}

	*wavefile = 0;

	if ( len )
	{
		if ( ( ch = *(wavefile-1)) >= '0' && ch <= '9' && len < bufLen - 1 ) // the 'e' only when it still fits
		{
			*wavefile++ = 'e';
			*wavefile = 0;
		}
	}

}


//============================================================================
// GameTextManager::reverseWord
//============================================================================

void GameTextManager::reverseWord ( Char *file, Char *lp )
{
	Int first = TRUE;
	Char f, l;
	Int ok = TRUE	;

	while ( ok )
	{
		if ( file >= lp )
		{
			return;
		}

		f = *file;
		l = *lp;

		if ( first )
		{
			if ( f >= 'A' && f <= 'Z' )
			{
				if ( l >= 'a' && l <= 'z' )
				{
					f = (f - 'A') + 'a';
					l = (l - 'a') + 'A';
				}
			}

			first = FALSE;
		}

		*lp-- = f;
		*file++ = l;

	}

}

//============================================================================
// GameTextManager::translateCopy
//============================================================================

void GameTextManager::translateCopy( WideChar *outbuf, Char *inbuf )
{
	Int slash = FALSE;

#if defined(RTS_DEBUG)
	if ( m_jabberWockie )
	{
		static Char buffer[MAX_UITEXT_LENGTH*2];
		Char *firstLetter = nullptr, *lastLetter;
		Char *b = buffer;
		Int formatWord = FALSE;
		Char ch;

		while ( (ch = *inbuf++) != 0 )
		{
			if ( ! (( ch >= 'a' && ch <= 'z') || ( ch >= 'A' && ch <= 'Z' )))
			{
				if ( firstLetter )
				{
					if ( !formatWord )
					{
						lastLetter = b-1;
						reverseWord ( firstLetter, lastLetter );
					}
					firstLetter = nullptr;
					formatWord = FALSE;
				}
				*b++ = ch;
				// GeneralsX: A string ending in '\\' or '%' made both copies below step over the
				// terminator, and the outer loop then read past the end of the text (m_buffer2 for a map.str entry).
				if ( ch == '\\' && *inbuf != 0 )
				{
					*b++ = *inbuf++;
				}
				if ( ch == '%' )
				{
					while ( (ch = *inbuf) != 0 && !( (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')))
					{
						*b++ = ch;
						inbuf++;
					}
					if ( ch != 0 )
					{
						*b++ = ch;
						inbuf++;
					}
				}
			}
			else
			{
				if ( !firstLetter )
				{
					firstLetter = b;
				}

				*b++ = ch;

			}
		}

		if ( firstLetter )
		{
			lastLetter = b-1;
			reverseWord ( firstLetter, lastLetter );
		}

		*b++ = 0;
		inbuf = buffer;
	}
	else if( m_munkee )
	{
		wcscpy(outbuf, L"Munkee");
		return;
	}
#endif

	while( *inbuf != '\0' )
	{
		if( slash == TRUE )
		{
			slash = FALSE;

			switch( *inbuf )
			{
				// in case end of string is reached
				// should never happen!!!
				case '\0':
					return;

				case '\\':
					*outbuf++ = '\\';
					break;

				case '\'':
					*outbuf++ = '\'';
					break;

				case '\"':
					*outbuf++ = '\"';
					break;

				case '\?':
					*outbuf++ = '\?';
					break;

				case 't':
					*outbuf++ = '\t';
					break;

				case 'n':
					*outbuf++ = '\n';
					break;

				default:
					*outbuf++ = *inbuf & 0x00FF;
					break;
			}
		}
		else if( *inbuf != '\\' )
		{
			*outbuf++ = *inbuf & 0x00FF;
		}
		else
			slash = TRUE;

		inbuf++;
	}
	*outbuf= 0;
}

//============================================================================
// GameTextManager::getStringCount
//============================================================================

Bool GameTextManager::getStringCount( const char *filename, Int& textCount )
{
	Int ok = TRUE;

	textCount = 0;

	File *file;
	file = TheFileSystem->openFile(filename, File::READ | File::TEXT);
	DEBUG_LOG(("Looking in %s for string file", filename));

	if ( file == nullptr )
	{
		return FALSE;
	}

	while(ok)
	{
		if( !readLine( m_buffer, sizeof(m_buffer) - 1, file ) )
			break;
		removeLeadingAndTrailing ( m_buffer );

		if( m_buffer[0] == '"' )
		{
				Int len = strlen(m_buffer);
				if ( len + 1 < (Int)sizeof(m_buffer) ) // a line that filled m_buffer still has its newline in the file
				{
					m_buffer[ len ] = '\n';
					m_buffer[ len+1] = 0;
				}
			readToEndOfQuote( file, &m_buffer[1], m_buffer2, m_buffer3, MAX_UITEXT_LENGTH );
		}
		else if( stricmp( m_buffer, "END") == 0 )
		{
			textCount++;
		}
	}

	textCount += 500;
	file->close();
	file = nullptr;
	return TRUE;
}

//============================================================================
// GameTextManager::getCSFInfo
//============================================================================

Bool GameTextManager::getCSFInfo ( const Char *filename, Int& textCount, LanguageID& language, FileInstance instance )
{
	CSFHeader header;
	Int ok = FALSE;
	File *file = TheFileSystem->openFile(filename, File::READ | File::BINARY, File::BUFFERSIZE, instance);
	DEBUG_LOG(("Looking in %s for compiled string file", filename));

	if ( file != nullptr )
	{
		if ( file->read( &header, sizeof ( header )) == sizeof ( header ) )
		{
			if ( header.id == CSF_ID )
			{
				textCount = header.num_labels;

				if ( header.version >= 2 )
				{
					language = (LanguageID) header.langid;
				}
				else
				{
					language = LANGUAGE_ID_US;
				}

				ok = TRUE;
			}
		}

		file->close();
		file = nullptr;
	}

	return ok;
}

//============================================================================
// GameTextManager::parseCSF
//============================================================================

Bool GameTextManager::parseCSF( const Char *filename, StringInfo *stringInfo, Int textCount, Int& maxLabelLen, FileInstance instance )
{
	File *file;
	Int id;
	Int len;
	Int listCount = 0;
	Bool ok = FALSE;
	CSFHeader header;

	// GeneralsX: Debug parseCSF
	fprintf(stderr, "[CSF] parseCSF() - START filename='%s'\n", filename);

	file = TheFileSystem->openFile(filename, File::READ | File::BINARY, File::BUFFERSIZE, instance);

	if ( file == nullptr )
	{
		fprintf(stderr, "[CSF] parseCSF() - openFile FAILED\n");
		return FALSE;
	}

	fprintf(stderr, "[CSF] parseCSF() - File opened\n");

	if (  file->read ( &header, sizeof ( CSFHeader)) != sizeof ( CSFHeader) )
	{
		fprintf(stderr, "[CSF] parseCSF() - read header FAILED\n");
		return FALSE;
	}

	// GeneralsX: Log header for debugging
	fprintf(stderr, "[CSF] parseCSF() - Header: id=%#x version=%d num_labels=%d num_strings=%d skip=%d langid=%d\n",
		header.id, header.version, header.num_labels, header.num_strings, header.skip, header.langid);

	// GeneralsX: Skip extra bytes after header
	// The 'skip' field indicates how many bytes to advance before reading first label
	if (header.skip > 0) {
		fprintf(stderr, "[CSF] parseCSF() - Skipping %d bytes as indicated by header.skip\n", header.skip);
		file->seek(header.skip, File::CURRENT);
	}

	fprintf(stderr, "[CSF] parseCSF() - Starting main loop (textCount=%d)...\n", textCount);

	while( file->read ( &id, sizeof (id)) == sizeof ( id) )
	{
		Int num;
		Int num_strings;

		if ( id != CSF_LABEL )
		{
			fprintf(stderr, "[CSF] parseCSF() - ERROR: Expected CSF_LABEL (%#x), got %#x at label %d\n", CSF_LABEL, id, listCount);
			goto quit;
		}

		file->read ( &num_strings, sizeof ( Int ));

		file->read ( &len, sizeof ( Int ) );

		if ( len )
		{
			file->read ( m_buffer, len );
		}

		m_buffer[len] = 0;

		stringInfo[listCount].label = m_buffer;


		if ( len > maxLabelLen )
		{
			maxLabelLen = len;
		}

		num = 0;

		while ( num < num_strings )
		{
		 	file->read ( &id, sizeof ( Int ) );

			if ( id != CSF_STRING && id != CSF_STRINGWITHWAVE )
			{
				fprintf(stderr, "[CSF] parseCSF() - ERROR: Expected CSF_STRING (%#x) or CSF_STRINGWITHWAVE (%#x), got %#x at label %d, string %d\n", 
					CSF_STRING, CSF_STRINGWITHWAVE, id, listCount, num);
				goto quit;
			}

		 	file->read ( &len, sizeof ( Int ) );

			if ( len )
			{
				// GeneralsX: Linux wchar_t compatibility
				// CSF files store 16-bit wide chars (UCS-2), but Linux wchar_t is 32-bit (UCS-4)
#ifdef _WIN32
				file->read ( m_tbuffer, len*sizeof(WideChar) );
#else
				uint16_t convert_buffer[MAX_UITEXT_LENGTH*2];
				file->read ( convert_buffer, len*sizeof(uint16_t) );
				for (int i = 0; i < len; i++)
				{
					m_tbuffer[i] = convert_buffer[i];
				}
#endif
			}

			if ( num == 0 )
			{
				// only use the first string found
				m_tbuffer[len] = 0;

				{
					WideChar *ptr;

					ptr = m_tbuffer;

					while ( *ptr )
					{
						// GeneralsX: Bitwise NOT for 16-bit values only
#ifdef _WIN32
						*ptr = ~*ptr;
#else
						// only negate the lower 16 bits (32-bit widechar on Linux)
						*ptr = ~*ptr & 0x0000FFFF;
#endif
						ptr++;
					}
				}

				stripSpaces ( m_tbuffer );
				stringInfo[listCount].text = m_tbuffer;
			}

			if ( id == CSF_STRINGWITHWAVE )
			{
			 	file->read ( &len, sizeof ( Int ) );
				if ( len )
				{
					file->read ( m_buffer, len );
				}
				m_buffer[len] = 0;

				if ( num == 0 && len )
				{
					// only use the first string found
					stringInfo[listCount].speech = m_buffer;
				}

			}

			num++;
		}

		listCount++;
		
		// GeneralsX: Progress logging every 500 labels
		if (listCount % 500 == 0) {
			fprintf(stderr, "[CSF] parseCSF() - Progress: %d/%d labels processed\n", listCount, textCount);
		}
	}

	fprintf(stderr, "[CSF] parseCSF() - Main loop complete! Processed %d/%d labels\n", listCount, textCount);
	ok = TRUE;

quit:

	fprintf(stderr, "[CSF] parseCSF() - Reached quit label: ok=%s, listCount=%d/%d\n", 
		ok ? "TRUE" : "FALSE", listCount, textCount);

	file->close();
	file = nullptr;

	return ok;
}


//============================================================================
// GameTextManager::parseStringFile
//============================================================================

Bool GameTextManager::parseStringFile( const char *filename )
{
	Int listCount = 0;
	Int ok = TRUE;

	File *file = TheFileSystem->openFile(filename, File::READ | File::TEXT);

	if ( file == nullptr )
	{
		return FALSE;
	}

	while( ok )
	{
		Int len;
		if( !readLine( m_buffer, sizeof(m_buffer) - 1, file ))
		{
			break;
		}

		removeLeadingAndTrailing ( m_buffer );

		if( ( *(unsigned short *)m_buffer == 0x2F2F) || !m_buffer[0])			//	0x2F2F is Hex for //
			continue;

		// GeneralsX: The table was sized by getStringCount(), which reads a line that starts with
		// a quote as a string (and swallows the END lines inside it) where this loop reads it as a label, so a damaged
		// file yields more labels than the table holds. Stop at the table's end instead of writing past it.
		if ( listCount >= m_textCount )
		{
			DEBUG_CRASH(( "String file '%s' has more labels than its table holds (%d)", filename, m_textCount ));
			break;
		}

		// make sure label is unique

		for ( Int i = 0; i < listCount; i++ )
		{
			if ( stricmp ( m_stringInfo[i].label.str(), m_buffer ) == 0)
			{
				DEBUG_CRASH ( ("String label '%s' multiply defined!", m_buffer ));
			}
		}

		m_stringInfo[listCount].label = m_buffer;
		len = strlen ( m_buffer );


		if ( len > m_maxLabelLen )
		{
			m_maxLabelLen = len;
		}

		Bool readString = FALSE;
		while( ok )
		{
			if (!readLine( m_buffer, sizeof(m_buffer) - 1, file ))
			{
				DEBUG_CRASH (("Unexpected end of string file"));
				ok = FALSE;
				goto quit;
			}

			removeLeadingAndTrailing ( m_buffer );

			if( m_buffer[0] == '"' )
			{
				len = strlen(m_buffer);
				if ( len + 1 < (Int)sizeof(m_buffer) ) // a line that filled m_buffer still has its newline in the file
				{
					m_buffer[ len ] = '\n';
					m_buffer[ len+1] = 0;
				}
				readToEndOfQuote( file, &m_buffer[1], m_buffer2, m_buffer3, MAX_UITEXT_LENGTH );


				if ( readString )
				{
					// only one string per label allows
						DEBUG_CRASH ( ("String label '%s' has more than one string defined!", m_stringInfo[listCount].label.str()));
				}
				else
				{
					// Copy string into new home
					translateCopy( m_tbuffer, m_buffer2 );
					stripSpaces ( m_tbuffer );

					m_stringInfo[listCount].text = m_tbuffer ;
					m_stringInfo[listCount].speech = m_buffer3;
					readString = TRUE;
				}
			}
			else if ( stricmp ( m_buffer, "END" ) == 0)
			{
				break;
			}
		}

		listCount++;
	}

quit:

	file->close();
	file = nullptr;

	return ok;
}

//============================================================================
// GameTextManager::initMapStringFile
//============================================================================

void GameTextManager::initMapStringFile( const AsciiString& filename )
{
	m_mapTextCount = 0;
	getStringCount( filename.str(), m_mapTextCount );

	m_mapStringInfo = NEW StringInfo[m_mapTextCount];

	parseMapStringFile( filename.str() );

	m_mapStringLUT = NEW StringLookUp[m_mapTextCount];

	StringLookUp *lut = m_mapStringLUT;
	StringInfo *info = m_mapStringInfo;

	for ( Int i = 0; i < m_mapTextCount; i++ )
	{
		lut->info = info;
		lut->label = &info->label;
		lut++;
		info++;
	}

	qsort( m_mapStringLUT, m_mapTextCount, sizeof(StringLookUp), compareLUT  );
}

//============================================================================
// GameTextManager::parseMapStringFile
//============================================================================

Bool GameTextManager::parseMapStringFile( const char *filename )
{
	Int listCount = 0;
	Int ok = TRUE;

	File *file;

	file = TheFileSystem->openFile(filename, File::READ | File::TEXT);
	if ( file == nullptr )
	{
		return FALSE;
	}

	while( ok )
	{
		Int len;
		if( !readLine( m_buffer, sizeof(m_buffer) - 1, file ))
		{
			break;
		}

		removeLeadingAndTrailing ( m_buffer );

		if( ( *(unsigned short *)m_buffer == 0x2F2F) || !m_buffer[0])			//	0x2F2F is Hex for //
			continue;

		// GeneralsX: Same table bound as parseStringFile; a map.str comes from the lobby host.
		if ( listCount >= m_mapTextCount )
		{
			DEBUG_CRASH(( "Map string file '%s' has more labels than its table holds (%d)", filename, m_mapTextCount ));
			break;
		}

		// make sure label is unique

		for ( Int i = 0; i < listCount; i++ )
		{
			if ( stricmp ( m_mapStringInfo[i].label.str(), m_buffer ) == 0)
			{
				DEBUG_CRASH ( ("String label '%s' multiply defined!", m_buffer ));
			}
		}

		m_mapStringInfo[listCount].label = m_buffer;
		len = strlen ( m_buffer );


		if ( len > m_maxLabelLen )
		{
			m_maxLabelLen = len;
		}

		Bool readString = FALSE;
		while( ok )
		{
			if (!readLine( m_buffer, sizeof(m_buffer) - 1, file ))
			{
				DEBUG_CRASH (("Unexpected end of string file"));
				ok = FALSE;
				goto quit;
			}

			removeLeadingAndTrailing ( m_buffer );

			if( m_buffer[0] == '"' )
			{
				len = strlen(m_buffer);
				if ( len + 1 < (Int)sizeof(m_buffer) ) // a line that filled m_buffer still has its newline in the file
				{
					m_buffer[ len ] = '\n';
					m_buffer[ len+1] = 0;
				}
				readToEndOfQuote( file, &m_buffer[1], m_buffer2, m_buffer3, MAX_UITEXT_LENGTH );


				if ( readString )
				{
					// only one string per label allowed
						DEBUG_CRASH ( ("String label '%s' has more than one string defined!", m_mapStringInfo[listCount].label.str()));
				}
				else
				{
					// Copy string into new home
					translateCopy( m_tbuffer, m_buffer2 );
					stripSpaces ( m_tbuffer );

					UnicodeString text = UnicodeString(m_tbuffer);
					if (TheLanguageFilter)
						TheLanguageFilter->filterLine(text);

					m_mapStringInfo[listCount].text = text;
					m_mapStringInfo[listCount].speech = m_buffer3;
					readString = TRUE;
				}
			}
			else if ( stricmp ( m_buffer, "END" ) == 0)
			{
				break;
			}
		}

		listCount++;
	}

quit:

	file->close();
	file = nullptr;

	return ok;
}

//============================================================================
// *GameTextManager::fetch
//============================================================================

UnicodeString GameTextManager::fetch( const Char *label, Bool *exists )
{
	return fetchFrom( label, exists, TRUE );
}

// GeneralsX: fetchFormat, fetchOrSubstituteFormat and fetchOrSubstitute (whose result callers
// format too) use the text they find as a printf format. A label missing from the game's own tables - every new
// GeneralsX string behind FETCH_OR_SUBSTITUTE starts that way - used to fall through to the map's map.str, which arrives
// with the map from the lobby host, so a map could supply "%n" or "%s" for it and crash or corrupt every client that
// formatted it. They now look in the game's tables only (allowMapText FALSE) and take their substitute instead; plain
// fetch() still serves the map's own labels for display.
UnicodeString GameTextManager::fetchFrom( const Char *label, Bool *exists, Bool allowMapText )
{
	DEBUG_ASSERTCRASH ( m_initialized, ("String Manager has not been m_initialized") );

	if( m_stringInfo == nullptr )
	{
		if( exists )
			*exists = FALSE;
		return m_failed;
	}

	StringLookUp *lookUp;
	StringLookUp key;
	AsciiString lb;
	lb = label;
	key.info = nullptr;
	key.label = &lb;

	lookUp = (StringLookUp *) bsearch( &key, (void*) m_stringLUT, m_textCount, sizeof(StringLookUp), compareLUT );

	// GeneralsX: Fallback to lower-priority CSF when override tables are incomplete.
	if ( lookUp == nullptr && m_fallbackStringLUT && m_fallbackTextCount )
	{
		lookUp = (StringLookUp *) bsearch( &key, (void*) m_fallbackStringLUT, m_fallbackTextCount, sizeof(StringLookUp), compareLUT );
	}

	// GeneralsX: The map's map.str comes LAST, after the fallback CSF. It was searched before the
	// fallback, and the deployed Generals.csf is an overlay of a few labels with the retail CSF as that fallback, so a map
	// could replace almost every retail string - including the ~120 that the engine uses as printf formats (a map's
	// "%n" for CONTROLBAR:PowerDescription crashed the client). In retail the game's table always wins over the map's.
	if ( lookUp == nullptr && allowMapText && m_mapStringLUT && m_mapTextCount )
	{
		lookUp = (StringLookUp *) bsearch( &key, (void*) m_mapStringLUT, m_mapTextCount, sizeof(StringLookUp), compareLUT );
	}

	if( lookUp == nullptr )
	{

		// string not found
		if( exists )
			*exists = FALSE;

		// See if we already have the missing string
		UnicodeString missingString;
		missingString.format(L"MISSING: '%hs'", label);

		NoString *noString = m_noStringList;

		while ( noString )
		{
			if (noString->text == missingString)
				return missingString;

			noString = noString->next;
		}

		//DEBUG_LOG(("*** MISSING:'%s' ***", label));
		// Remember file could have been altered at this point.
		noString = NEW NoString;
		noString->text = missingString;
		noString->next = m_noStringList;
		m_noStringList = noString;
		return noString->text;
	}
	if( exists )
		*exists = TRUE;
	return lookUp->info->text;
}

//============================================================================
// *GameTextManager::fetch
//============================================================================

UnicodeString GameTextManager::fetch( AsciiString label, Bool *exists )
{
	return fetch(label.str(), exists);
}

//============================================================================
// *GameTextManager::fetchFormat
//============================================================================

UnicodeString GameTextManager::fetchFormat( const Char *label, ... )
{
	Bool exists;
	UnicodeString str = fetchFrom(label, &exists, FALSE);
	if (exists)
	{
		UnicodeString strFormat;

		va_list args;
		va_start(args, label);
		strFormat.format_va(str.str(), args);
		va_end(args);

		str = strFormat;
	}
	return str;
}

//============================================================================
// GameTextManager::fetchOrSubstitute
//============================================================================

UnicodeString GameTextManager::fetchOrSubstitute( const Char *label, const WideChar *substituteText )
{
	Bool exists;
	UnicodeString str = fetchFrom(label, &exists, FALSE);	// callers format this one too (GXLobbyCommands)
	if (!exists)
		str = substituteText;
	return str;
}

//============================================================================
// GameTextManager::fetchOrSubstituteFormat
//============================================================================

UnicodeString GameTextManager::fetchOrSubstituteFormat( const Char *label, const WideChar *substituteFormat, ... )
{
	va_list args;
	va_start(args, substituteFormat);
	UnicodeString str = fetchOrSubstituteFormatVA(label, substituteFormat, args);
	va_end(args);

	return str;
}

//============================================================================
// GameTextManager::fetchOrSubstituteFormatVA
//============================================================================

UnicodeString GameTextManager::fetchOrSubstituteFormatVA( const Char *label, const WideChar *substituteFormat, va_list args )
{
	Bool exists;
	UnicodeString str = fetchFrom(label, &exists, FALSE);
	if (exists)
	{
		UnicodeString strFormat;
		// GeneralsX: The format source was strFormat.str() - the freshly default-constructed
		// destination, whose str() is the empty-string sentinel. vswprintf therefore formatted "" and
		// succeeded, so str was overwritten with an empty string: the behaviour was exactly inverted,
		// with the localized text rendering blank whenever the label EXISTS and only the substitute
		// path (below) rendering correctly. Format from the fetched string, as fetchFormat() does.
		strFormat.format_va(str.str(), args);
		str = strFormat;
	}
	else
	{
		str.format_va(substituteFormat, args);
	}

	return str;
}

//============================================================================
// GameTextManager::getStringsWithLabelPrefix
//============================================================================

AsciiStringVec& GameTextManager::getStringsWithLabelPrefix(AsciiString label)
{
	m_asciiStringVec.clear();
	if (m_stringLUT) {
		for (int i = 0; i < m_textCount; ++i) {
			if (strstr(m_stringLUT[i].label->str(), label.str()) == m_stringLUT[i].label->str()) {
				m_asciiStringVec.push_back(*m_stringLUT[i].label);
			}
		}
	}
	if (m_mapStringLUT) {
		for (int i = 0; i < m_mapTextCount; ++i) {
			if (strstr(m_mapStringLUT[i].label->str(), label.str()) == m_mapStringLUT[i].label->str()) {
				m_asciiStringVec.push_back(*m_mapStringLUT[i].label);
			}
		}
	}
	return m_asciiStringVec;
}

//============================================================================
// GameTextManager::readLine
//============================================================================

// GeneralsX: Reads at most max chars and then writes the terminator, so the buffer must hold
// max+1: every caller passes sizeof(m_buffer) - 1 (two used to pass the whole buffer, so a 10 KB line in a map.str wrote
// past m_buffer), and the '\n' appended after a line that opens a quote is added only when it still fits.
Bool	GameTextManager::readLine( char *buffer, Int max, File *file )
{
	Int ok = FALSE;

	while ( max && file->read( buffer, 1 ) == 1 )
	{
		ok = TRUE;

		if ( *buffer == '\n' )
		{
			break;
		}

		buffer++;
		max--;
	}

	*buffer = 0;

	return ok;
}

//============================================================================
// compareLUT
//============================================================================

static int __cdecl compareLUT ( const void *i1,  const void*i2)
{
	StringLookUp *lut1 = (StringLookUp*) i1;
	StringLookUp *lut2 = (StringLookUp*) i2;

	return stricmp( lut1->label->str(), lut2->label->str());
}
