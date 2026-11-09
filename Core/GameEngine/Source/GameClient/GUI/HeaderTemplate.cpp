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

// FILE: HeaderTemplate.cpp /////////////////////////////////////////////////
//-----------------------------------------------------------------------------
//
//                       Electronic Arts Pacific.
//
//                       Confidential Information
//                Copyright (C) 2002 - All Rights Reserved
//
//-----------------------------------------------------------------------------
//
//	created:	Aug 2002
//
//	Filename: 	HeaderTemplate.cpp
//
//	author:		Chris Huybregts
//
//	purpose:	The header template system is used to maintain a unified look across
//						windows.  It also allows Localization to customize the looks based
//						on language fonts.
//
//-----------------------------------------------------------------------------
///////////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
// SYSTEM INCLUDES ////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// USER INCLUDES //////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
#include "PreRTS.h"

#include "Common/INI.h"
#include "Common/FileSystem.h"
#include "Common/Registry.h"
#include "GameClient/HeaderTemplate.h"
#include "GameClient/GameFont.h"
#include "GameClient/GlobalLanguage.h"
#include "GameClient/Display.h"
//-----------------------------------------------------------------------------
// DEFINES ////////////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
const FieldParse HeaderTemplateManager::m_headerFieldParseTable[] =
{
	{ "Font",								INI::parseQuotedAsciiString,						nullptr, offsetof( HeaderTemplate, m_fontName ) },
	{ "Point",							INI::parseInt,										nullptr, offsetof( HeaderTemplate, m_point) },
	{ "Bold",								INI::parseBool,										nullptr, offsetof( HeaderTemplate, m_bold ) },
	{ nullptr, nullptr, nullptr, 0 },
};

HeaderTemplateManager *TheHeaderTemplateManager = nullptr;
//-----------------------------------------------------------------------------
// PUBLIC FUNCTIONS ///////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
void INI::parseHeaderTemplateDefinition( INI *ini )
{
	AsciiString name;
	HeaderTemplate *hTemplate;

	// read the name
	const char* c = ini->getNextToken();
	name.set( c );

	// find existing item if present
	hTemplate = TheHeaderTemplateManager->findHeaderTemplate( name );
	if( hTemplate == nullptr )
	{

		// allocate a new item
		hTemplate = TheHeaderTemplateManager->newHeaderTemplate( name );

	}
	else
	{
		DEBUG_CRASH(( "[LINE: %d in '%s'] Duplicate header Template %s found!", ini->getLineNum(), ini->getFilename().str(), name.str() ));
	}
	// parse the ini definition
	ini->initFromINI( hTemplate, TheHeaderTemplateManager->getFieldParse() );

}

HeaderTemplate::HeaderTemplate() :
m_font(nullptr),
m_point(0),
m_bold(FALSE)
{
	m_fontName.clear();
	m_name.clear();
}

HeaderTemplate::~HeaderTemplate(){}

HeaderTemplateManager::HeaderTemplateManager()
{}

HeaderTemplateManager::~HeaderTemplateManager()
{
	HeaderTemplateListIt it = m_headerTemplateList.begin();
	while(it != m_headerTemplateList.end())
	{
		HeaderTemplate *hTemplate = *it;
		delete hTemplate;
		it = m_headerTemplateList.erase(it);

	}
}

void HeaderTemplateManager::init()
{
	{
		AsciiString fname;
		fname.format("Data\\%s\\HeaderTemplate", GetRegistryLanguage().str());

		INI ini;
		ini.loadFileDirectory( fname, INI_LOAD_OVERWRITE, nullptr );
	}

	populateGameFonts();
}

HeaderTemplate *HeaderTemplateManager::findHeaderTemplate( AsciiString name )
{
	HeaderTemplateListIt it = m_headerTemplateList.begin();
	while(it != m_headerTemplateList.end())
	{
		HeaderTemplate *hTemplate = *it;
		if(hTemplate->m_name.compare(name) == 0)
			return hTemplate;
		++it;
	}
	return nullptr;
}

HeaderTemplate *HeaderTemplateManager::newHeaderTemplate( AsciiString name )
{
	HeaderTemplate *newHTemplate = NEW HeaderTemplate;
	DEBUG_ASSERTCRASH(newHTemplate, ("Unable to create a new Header Template in HeaderTemplateManager::newHeaderTemplate"));
	if(!newHTemplate)
		return nullptr;

	newHTemplate->m_name = name;
	m_headerTemplateList.push_front(newHTemplate);
	return newHTemplate;

}

GameFont *HeaderTemplateManager::getFontFromTemplate( AsciiString name )
{
	HeaderTemplate *ht = findHeaderTemplate( name );
	if(!ht)
	{
		//DEBUG_LOG(("HeaderTemplateManager::getFontFromTemplate - Could not find header %s", name.str()));
		return nullptr;
	}

	return ht->m_font;
}

HeaderTemplate *HeaderTemplateManager::getFirstHeader()
{
	HeaderTemplateListIt it = m_headerTemplateList.begin();
	if( it == m_headerTemplateList.end())
		return nullptr;

	return *it;
}

HeaderTemplate *HeaderTemplateManager::getNextHeader( HeaderTemplate *ht )
{
	HeaderTemplateListIt it = m_headerTemplateList.begin();
	while(it != m_headerTemplateList.end())
	{
		if(*it == ht)
		{
			++it;
			if( it == m_headerTemplateList.end())
				return nullptr;
			return *it;
		}
		++it;
	}
	return nullptr;

}

void HeaderTemplateManager::onResolutionChanged()
{
	populateGameFonts();
}
//-----------------------------------------------------------------------------
// PRIVATE FUNCTIONS //////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

void HeaderTemplateManager::populateGameFonts()
{
	// GeneralsX @probe Claude 20/08/2026 GX_FONT=1 - the size every header-template control on every
	// screen ends up at, and the single factor that produced it.
	//
	// This is where the menu furniture's size is actually decided. A .wnd's own FONT is parsed and
	// then discarded for any control carrying a HEADERTEMPLATE (GameWindowManagerScript.cpp, the END
	// branch), which is 134 of 290 controls across the layouts loaded at startup and EVERY control
	// on the main menu that draws text - so this table, not the layout, is what "the menu is too big"
	// is about.
	//
	// The factor is read out as adjustFontSize(100) rather than recomputed here: it is derived from a
	// scaling method and a scaler that both live behind GlobalLanguage's private members, and a
	// second implementation of that formula in a diagnostic is a second thing that can be wrong.
	{
		static Int s_on = -1;
		if( s_on < 0 )
		{
			const char *env = getenv("GX_FONT");
			s_on = (env != NULL && atoi(env) > 0) ? 1 : 0;
		}
		if( s_on && TheGlobalLanguageData != NULL && TheDisplay != NULL )
		{
			fprintf(stderr, "[GXFONT] headerTemplates display=%dx%d adjustFontSize(100)=%d (factor %.2f)\n",
				TheDisplay->getWidth(), TheDisplay->getHeight(),
				TheGlobalLanguageData->adjustFontSize(100),
				(Real)TheGlobalLanguageData->adjustFontSize(100) / 100.0f);
			fflush(stderr);
		}
	}

	HeaderTemplateListIt it = m_headerTemplateList.begin();
	while(it != m_headerTemplateList.end())
	{
		HeaderTemplate *hTemplate = *it;
		Real pointSize = TheGlobalLanguageData->adjustFontSize(hTemplate->m_point);
		{
			static Int s_on = -1;
			if( s_on < 0 )
			{
				const char *env = getenv("GX_FONT");
				s_on = (env != NULL && atoi(env) > 0) ? 1 : 0;
			}
			if( s_on )
			{
				fprintf(stderr, "[GXFONT] headerTemplate '%s' font='%s' authored=%d -> %d bold=%d\n",
					hTemplate->m_name.str(), hTemplate->m_fontName.str(),
					hTemplate->m_point, (Int)pointSize, (Int)hTemplate->m_bold);
				fflush(stderr);
			}
		}
		GameFont *font = TheFontLibrary->getFont(hTemplate->m_fontName, pointSize,hTemplate->m_bold);
		DEBUG_ASSERTCRASH(font,("HeaderTemplateManager::populateGameFonts - Could not find font %s %d",hTemplate->m_fontName.str(), hTemplate->m_point));

		hTemplate->m_font = font;

		++it;
	}
}
