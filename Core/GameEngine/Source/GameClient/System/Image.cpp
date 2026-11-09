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

// FILE: Image.cpp ////////////////////////////////////////////////////////////////////////////////
// Created:   Colin Day, June 2001
// Desc:      High level representation of images, this is currently being
//						written so we have a way to refer to images in the windows
//						GUI, this system should be replaced with something that can
//						handle real image management or written to accommodate
//						all parts of the engine that need images.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#define DEFINE_IMAGE_STATUS_NAMES
#include "Lib/BaseType.h"
#include "Common/Debug.h"
#include "Common/INI.h"
#include "Common/GlobalData.h"
#include "GameClient/Image.h"
#include "Common/NameKeyGenerator.h"
#include "Common/FileSystem.h"
#include "GameClient/GXSpectatorHud.h"

#include <string>

// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////
const FieldParse Image::m_imageFieldParseTable[] =
{

	{ "Texture",				Image::parseImageTexture,						nullptr, 		offsetof( Image, m_filename ) },
	{ "TextureWidth",		Image::parseImageInt,								nullptr, 		offsetof( Image, m_textureSize.x ) },
	{ "TextureHeight",	Image::parseImageInt,								nullptr, 		offsetof( Image, m_textureSize.y ) },
	{ "Coords",					Image::parseImageCoords,						nullptr, 		offsetof( Image, m_UVCoords ) },
	{ "Status",					Image::parseImageStatus,						nullptr, 		offsetof( Image, m_status ) },

	{ nullptr,							nullptr,																nullptr, 		0 }

};

// PRIVATE FUNCTIONS //////////////////////////////////////////////////////////////////////////////
// GeneralsX @feature Claude 22/10/2026 Every field a MappedImage definition writes goes through this table, so the image
// collection hears of every definition parsed after start-up - a map's map.ini / solo.ini - by the parser itself, not by
// comparing values afterwards (a restatement equal to a value we hold changes nothing a comparison can see; Codex lui-r5,
// lui-r6). At start-up, and into the scratch collection of gxKeepOriginals, it is a no-op (nothing is swapped yet).
static void gxNoteRedefined( void *instance )
{
	if( TheMappedImageCollection != nullptr )
		TheMappedImageCollection->gxNoteRedefined( (const Image *)instance );
}

void Image::parseImageTexture( INI* ini, void *instance, void *store, const void* userData )
{
	INI::parseAsciiString( ini, instance, store, userData );
	gxNoteRedefined( instance );
}

void Image::parseImageInt( INI* ini, void *instance, void *store, const void* userData )
{
	INI::parseInt( ini, instance, store, userData );
	gxNoteRedefined( instance );
}

//-------------------------------------------------------------------------------------------------
/** Parse an image coordinates in the form of
	*
	* COORDS = Left:AAA Top:BBB Right:CCC Bottom:DDD */
//-------------------------------------------------------------------------------------------------
void Image::parseImageCoords( INI* ini, void *instance, void *store, const void* /*userData*/ )
{
	Int left = INI::scanInt(ini->getNextSubToken("Left"));
	Int top = INI::scanInt(ini->getNextSubToken("Top"));
	Int right = INI::scanInt(ini->getNextSubToken("Right"));
	Int bottom = INI::scanInt(ini->getNextSubToken("Bottom"));

	// get the image we're storing in
	Image *theImage = (Image *)instance;

	//
	// store the UV coords based on what we've read in and the texture size
	// defined for this image
	//
	Region2D uvCoords;

	uvCoords.lo.x = (Real)left;
	uvCoords.lo.y = (Real)top;
	uvCoords.hi.x = (Real)right;
	uvCoords.hi.y = (Real)bottom;

	// adjust the coords by texture size
	const ICoord2D *textureSize = theImage->getTextureSize();
	if( textureSize->x )
	{
		uvCoords.lo.x /= (Real)textureSize->x;
		uvCoords.hi.x /= (Real)textureSize->x;
	}
	if( textureSize->y )
	{
		uvCoords.lo.y /= (Real)textureSize->y;
		uvCoords.hi.y /= (Real)textureSize->y;
	}

	// store the uv coords
	theImage->setUV( &uvCoords );

	// compute the image size based on the coords we read and store
	ICoord2D imageSize;
	imageSize.x = right - left;
	imageSize.y = bottom - top;
	theImage->setImageSize( &imageSize );
	gxNoteRedefined( instance );

}

//-------------------------------------------------------------------------------------------------
/** Parse the image status line */
//-------------------------------------------------------------------------------------------------
void Image::parseImageStatus( INI* ini, void *instance, void *store, const void* /*userData*/)
{
	// use existing INI parsing for the bit strings
	INI::parseBitString32(ini, instance, store, imageStatusNames);

	//
	// if we are rotated 90 degrees clockwise we need to swap our width and height as
	// they were computed from the page location rect, which was for the rotated image
	// (see ImagePacker tool for more details)
	//
	UnsignedInt *theStatusBits = (UnsignedInt *)store;
	if( BitIsSet( *theStatusBits, IMAGE_STATUS_ROTATED_90_CLOCKWISE ) )
	{
		Image *theImage = (Image *)instance;
		ICoord2D imageSize;

		imageSize.x = theImage->getImageHeight();  // note it's height not width
		imageSize.y = theImage->getImageWidth();   // note it's width not height
		theImage->setImageSize( &imageSize );

	}
	gxNoteRedefined( instance );

}

// PUBLIC DATA ////////////////////////////////////////////////////////////////////////////////////
ImageCollection *TheMappedImageCollection = nullptr;  ///< mapped images

// PUBLIC FUNCTIONS////////////////////////////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
Image::Image()
{

	m_name.clear();
	m_filename.clear();
	m_textureSize.x = 0;
	m_textureSize.y = 0;
	m_UVCoords.lo.x = 0.0f;
	m_UVCoords.lo.y = 0.0f;
	m_UVCoords.hi.x = 1.0f;
	m_UVCoords.hi.y = 1.0f;
	m_imageSize.x = 0;
	m_imageSize.y = 0;
	m_rawTextureData = nullptr;
	m_status = IMAGE_STATUS_NONE;

}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
Image::~Image()
{

}

//-------------------------------------------------------------------------------------------------
/** Set a status bit into the existing status, return the previous status
	* bit collection from before the set */
//-------------------------------------------------------------------------------------------------
UnsignedInt Image::setStatus( UnsignedInt bit )
{
	UnsignedInt prevStatus = m_status;

	BitSet( m_status, bit );
	return prevStatus;

}

//-------------------------------------------------------------------------------------------------
/** Clear a status bit from the existing status, return the previous
	* status bit collection from before the clear */
//-------------------------------------------------------------------------------------------------
UnsignedInt Image::clearStatus( UnsignedInt bit )
{
	UnsignedInt prevStatus = m_status;

	BitClear( m_status, bit );
	return prevStatus;

}

///////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
ImageCollection::ImageCollection()
{
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
ImageCollection::~ImageCollection()
{
  for (ImageMap::iterator i=m_imageMap.begin();i!=m_imageMap.end();++i)
    deleteInstance(i->second);
}

//-------------------------------------------------------------------------------------------------
/** adds the given image to the collection, transfers ownership to this object */
//-------------------------------------------------------------------------------------------------
void ImageCollection::addImage( Image *image )
{
  m_imageMap[TheNameKeyGenerator->nameToLowercaseKey(image->getName())]=image;
}

//-------------------------------------------------------------------------------------------------
const Image *ImageCollection::findImage( NameKeyType namekey ) const
{
	ImageMap::const_iterator i = m_imageMap.find(namekey);
	return i == m_imageMap.end() ? nullptr : i->second;
}

//-------------------------------------------------------------------------------------------------
/** Find an image given the image name */
//-------------------------------------------------------------------------------------------------
const Image *ImageCollection::findImageByName( const AsciiString& name ) const
{
	return findImage(TheNameKeyGenerator->nameToLowercaseKey(name));
}

//-------------------------------------------------------------------------------------------------
/** Find an image given the image name */
//-------------------------------------------------------------------------------------------------
const Image *ImageCollection::findImageByName( const char* name ) const
{
	return findImage(TheNameKeyGenerator->nameToLowercaseKey(name));
}

//-------------------------------------------------------------------------------------------------
/** GeneralsX @fix Claude 05/10/2026 Find an image by name WITHOUT minting a NameKey. findImageByName goes through
	* nameToLowercaseKey, which creates a key for a name it has never seen, and NameKeys are numbered in one sequence the
	* simulation shares (a science travels as its key's number), so a lookup only one peer makes can renumber what a later
	* map defines on that peer alone (Codex codex-batch-1005 p1-spectator; memory namekey-minting-is-a-desync-hazard).
	* addImage interns every image's name, so a name without a key holds no image and the answer is the same. Linear in
	* the collection: for rare, cached lookups only. */
//-------------------------------------------------------------------------------------------------
const Image *ImageCollection::findImageByNameNoMint( const char* name ) const
{
	for (ImageMap::const_iterator i = m_imageMap.begin(); i != m_imageMap.end(); ++i)
		if (_stricmp(i->second->getName().str(), name) == 0)
			return i->second;
	return nullptr;
}

//-------------------------------------------------------------------------------------------------
/** GeneralsX @feature Claude 22/10/2026 A GeneralsX LOOK LAYER is a HandCreated MappedImage file named zzGX*.ini
	* (!GXUISide.big's zzGXSidebar.ini): it REDEFINES retail and Control Bar Pro images in place - the control bar's side
	* buttons, the General's star button and his experience ladder - so the GeneralsX UI's bar matches its HUD. Being data,
	* it used to reach the legacy look too (owner 21/10/2026, a legacy screenshot: "These shouldn't change if the UI is
	* legacy"). The layer cannot carry its art under new names instead: every MappedImage name is interned as a NameKey,
	* and a name only some peers intern renumbers the keys minted after it on those peers (memory
	* namekey-minting-is-a-desync-hazard). So the art keeps its names and the collection keeps BOTH definitions. */
static Bool gxIsLookLayer( const AsciiString &path )
{
	const char *base = path.str();
	for (const char *p = base; *p != '\0'; ++p)
		if (*p == '/' || *p == '\\')
			base = p + 1;
	return _strnicmp( base, "zzGX", 4 ) == 0;
}

/** The files INI::loadDirectory( dirName ) reads, in the order it reads them: the same listing with its '\' -> '/'
	* canonicalisation, the directory's own files first, then those in subdirectories (INI.cpp, loadDirectory). */
static void gxIniFilesInLoadOrder( AsciiString dirName, std::vector<AsciiString> &out )
{
	FilenameList raw, canon;
	dirName.concat( '\\' );
	TheFileSystem->getFileListInDirectory( dirName, "*.ini", raw, TRUE );
	for (FilenameList::const_iterator it = raw.begin(); it != raw.end(); ++it)
	{
		std::string tmp( (*it).str() );
		for (std::string::size_type i = 0; i < tmp.size(); ++i)
			if (tmp[i] == '\\')
				tmp[i] = '/';
		canon.insert( AsciiString( tmp.c_str() ) );
	}
	for (Int pass = 0; pass < 2; ++pass)
		for (FilenameList::const_iterator it = canon.begin(); it != canon.end(); ++it)
		{
			const char *rest = (*it).str() + dirName.getLength();
			const Bool inSubdir = strchr( rest, '\\' ) != nullptr || strchr( rest, '/' ) != nullptr;
			if (inSubdir == (pass == 1))
				out.push_back( *it );
		}
}

/** What load() reads, in its order. withLookLayers = FALSE reads the same files minus every look layer: the definitions
	* the images had before any layer. */
static void gxLoadMappedImages( Int textureSize, Bool withLookLayers )
{
	char buffer[ _MAX_PATH ];
	INI ini;
	// first load in the user created mapped image files if we have them.
	WIN32_FIND_DATA findData;
	AsciiString userDataPath;
	if(TheGlobalData)
	{
		// GeneralsX @bugfix copilot 12/03/2026 Use platform-correct separators for user data mapped image paths.
#ifdef _WIN32
		userDataPath.format("%sINI\\MappedImages\\*.ini",TheGlobalData->getPath_UserData().str());
		HANDLE findHandle = FindFirstFile(userDataPath.str(), &findData);
		if(findHandle != INVALID_HANDLE_VALUE)
		{
			FindClose(findHandle);
			userDataPath.format("%sINI\\MappedImages",TheGlobalData->getPath_UserData().str());
			ini.loadDirectory(userDataPath, INI_LOAD_OVERWRITE, nullptr );
		}
#else
		userDataPath.format("%sINI/MappedImages/*.ini",TheGlobalData->getPath_UserData().str());
		HANDLE findHandle = FindFirstFile(userDataPath.str(), &findData);
		if(findHandle != INVALID_HANDLE_VALUE)
		{
			FindClose(findHandle);
			userDataPath.format("%sINI/MappedImages",TheGlobalData->getPath_UserData().str());
			ini.loadDirectory(userDataPath, INI_LOAD_OVERWRITE, nullptr );
		}
#endif
	}

	// construct path to the mapped images folder of the correct texture size
	sprintf( buffer, "Data\\INI\\MappedImages\\TextureSize_%d", textureSize );

	// load all the ine files in that directory

	ini.loadDirectory( AsciiString( buffer ), INI_LOAD_OVERWRITE, nullptr );

	if( withLookLayers )
	{
		ini.loadDirectory("Data\\INI\\MappedImages\\HandCreated", INI_LOAD_OVERWRITE, nullptr );
		return;
	}
	std::vector<AsciiString> files;
	gxIniFilesInLoadOrder( "Data\\INI\\MappedImages\\HandCreated", files );
	for( size_t i = 0; i < files.size(); ++i )
		if( !gxIsLookLayer( files[i] ) )
			ini.load( files[i], INI_LOAD_OVERWRITE, nullptr );
}

//-------------------------------------------------------------------------------------------------
/** Load this image collection with all the images specified in the INI files
	* for the proper texture size directory */
//-------------------------------------------------------------------------------------------------
void ImageCollection::load( Int textureSize )
{
	gxLoadMappedImages( textureSize, TRUE );
	gxKeepOriginals( textureSize );
}

/** An image swapped only while it still holds the definition this collection last gave it. A map's map.ini may redefine any
	* MappedImage after start-up (INI.cpp: MappedImage is a map-rules block, and its leftover outlives the match), and that
	* definition must survive as it does without the layer - not be put back to a start-up one (Codex lui-r2 P2). */
static Bool gxHoldsDef( const Image *im, const AsciiString &filename, const ICoord2D &textureSize, const Region2D &uv,
												const ICoord2D &imageSize, UnsignedInt status )
{
	return im->getFilename().compareNoCase( filename ) == 0 && im->getTextureSize()->x == textureSize.x &&
				 im->getTextureSize()->y == textureSize.y && im->getUV()->lo.x == uv.lo.x && im->getUV()->lo.y == uv.lo.y &&
				 im->getUV()->hi.x == uv.hi.x && im->getUV()->hi.y == uv.hi.y && im->getImageSize()->x == imageSize.x &&
				 im->getImageSize()->y == imageSize.y && im->getStatus() == status;
}

/** GeneralsX @feature Claude 22/10/2026 With a look layer installed, parse the same files once more WITHOUT the layers into
	* a scratch collection, and keep, for every image whose definition differs, the definition from before the layers next to
	* the one it was loaded with. The same parser on the same files, so an original is exactly what the game drew before the
	* layer existed; every name was interned by the load just done, so the second parse mints no NameKey. At start-up only,
	* where every INI load already happens. */
void ImageCollection::gxKeepOriginals( Int textureSize )
{
	m_gxLook.clear();
	std::vector<AsciiString> hand;
	gxIniFilesInLoadOrder( "Data\\INI\\MappedImages\\HandCreated", hand );
	Bool layer = FALSE;
	for( size_t i = 0; i < hand.size() && !layer; ++i )
		layer = gxIsLookLayer( hand[i] );
	if( !layer || TheMappedImageCollection != this )		// the parser fills TheMappedImageCollection
		return;

	ImageCollection originals;
	TheMappedImageCollection = &originals;
	try
	{
		gxLoadMappedImages( textureSize, FALSE );
	}
	catch( ... )
	{
		TheMappedImageCollection = this;
		DEBUG_CRASH(( "ImageCollection: the look layers' originals did not parse; the layers' art stays in every look" ));
		return;
	}
	TheMappedImageCollection = this;

	for( ImageMap::const_iterator it = originals.m_imageMap.begin(); it != originals.m_imageMap.end(); ++it )
	{
		ImageMap::iterator live = m_imageMap.find( it->first );
		if( live == m_imageMap.end() )
			continue;
		const Image *o = it->second;
		Image *im = live->second;
		if( gxHoldsDef( im, o->m_filename, o->m_textureSize, o->m_UVCoords, o->m_imageSize, o->m_status ) )
			continue;
		if( o->m_rawTextureData != nullptr || im->m_rawTextureData != nullptr )
			continue;		// a raw-texture image draws its own texture object, not a page; never swapped
		GxLookSwap s;
		s.image = im;
		s.showsGeneralsX = TRUE;
		s.foreign = FALSE;
		s.original.filename = o->m_filename;
		s.original.textureSize = o->m_textureSize;
		s.original.uv = o->m_UVCoords;
		s.original.imageSize = o->m_imageSize;
		s.original.status = o->m_status;
		s.generalsx.filename = im->m_filename;
		s.generalsx.textureSize = im->m_textureSize;
		s.generalsx.uv = im->m_UVCoords;
		s.generalsx.imageSize = im->m_imageSize;
		s.generalsx.status = im->m_status;
		m_gxLook.push_back( s );
	}
	// The look this session starts in, from the same GXHud.ini the HUD reads (the launcher writes it before a launch), applied
	// NOW: a map's map.ini may redefine these images later, field by field, on top of whatever they hold then - so they must
	// already hold the right look's art (Codex lui-r3 P2). The HUD keeps it current every frame of a match.
	Bool startGeneralsX = FALSE;
	if( TheGlobalData )
	{
		AsciiString path = TheGlobalData->getPath_UserData();
		path.concat( "GXHud.ini" );
		std::string text;
		if( FILE *f = fopen( path.str(), "rb" ) )
		{
			char buf[ 4096 ];
			size_t n;
			while( ( n = fread( buf, 1, sizeof( buf ), f ) ) > 0 )
				text.append( buf, n );
			fclose( f );
		}
		startGeneralsX = GXSpectatorHud::parseSettings( text ).generalsx ? TRUE : FALSE;
	}
	gxShowGeneralsXLook( startGeneralsX );
	fprintf( stderr, "[GXLOOK] look layers redefine %d images; their originals are kept for the legacy look; starting %s\n",
		(Int)m_gxLook.size(), startGeneralsX ? "generalsx" : "legacy" );
	fflush( stderr );
}

void ImageCollection::gxNoteRedefined( const Image *image )
{
	for( size_t i = 0; i < m_gxLook.size(); ++i )
		if( m_gxLook[i].image == image )
			m_gxLook[i].foreign = TRUE;
}

/** Called every frame of a match (GXSpectatorHud.inl underWindows). Ownership comes from the field parsers (gxNoteRedefined);
	* the value check below is the second line, for anything that writes an image without parsing a definition. About 100
	* short compares per frame. */
void ImageCollection::gxShowGeneralsXLook( Bool on )
{
	for( size_t i = 0; i < m_gxLook.size(); ++i )
	{
		GxLookSwap &e = m_gxLook[i];
		if( e.foreign )
			continue;
		const GxImageDef &was = e.showsGeneralsX ? e.generalsx : e.original;
		Image *im = e.image;
		// Redefined since this collection last wrote it (a map): theirs from now on, in every look, never reclaimed - even
		// if a later definition happens to equal one of ours (Codex lui-r3 P2).
		if( !gxHoldsDef( im, was.filename, was.textureSize, was.uv, was.imageSize, was.status ) )
		{
			e.foreign = TRUE;
			continue;
		}
		if( e.showsGeneralsX == on )
			continue;
		const GxImageDef &d = on ? e.generalsx : e.original;
		e.showsGeneralsX = on;
		im->m_filename = d.filename;
		im->m_textureSize = d.textureSize;
		im->m_UVCoords = d.uv;
		im->m_imageSize = d.imageSize;
		im->m_status = d.status;
	}
}
