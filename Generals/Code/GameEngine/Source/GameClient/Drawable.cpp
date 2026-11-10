/*
**	Command & Conquer Generals(tm)
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

// Drawable.cpp ///////////////////////////////////////////////////////////////////////////////////
// "Drawables" - graphical GameClient entities bound to GameLogic objects
// Author: Michael S. Booth, March 2001
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <Utility/intrin_compat.h>	// For _isnan compatibility
#include "Common/AudioEventInfo.h"
#include "Common/AudioSettings.h"
#include "Common/BitFlagsIO.h"
#include "Common/BuildAssistant.h"
#include "Common/ClientUpdateModule.h"
#include "Common/DrawModule.h"
#include "Common/FramePacer.h"
#include "Common/GameAudio.h"
#include "Common/GameLOD.h"
#include "Common/GameState.h"
#include "Common/GameUtility.h"
#include "Common/GlobalData.h"
#include "Common/ModuleFactory.h"
#include "Common/PerfTimer.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "Common/Xfer.h"

#include "GameLogic/ExperienceTracker.h"
#include "GameLogic/GameLogic.h"		// for logic frame count
#include "GameLogic/Object.h"
#include "GameLogic/Locomotor.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Module/ContainModule.h"
#include "GameLogic/Module/PhysicsUpdate.h"
#include "GameLogic/Module/StealthUpdate.h"
#include "GameLogic/Module/StickyBombUpdate.h"
#include "GameLogic/Module/BattlePlanUpdate.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/Weapon.h"

#include "GameClient/Anim2D.h"
#include "GameClient/ControlBar.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/Drawable.h"
#include "GameClient/GXConstructPercentKey.h"
#include "GameClient/GXWorldLabelScale.h"
#include "GameClient/GXImageVariant.h"
#include "GameClient/DrawGroupInfo.h"
#include "GameClient/GameFont.h"
#include "GameClient/GameClient.h"
#include "GameClient/GlobalLanguage.h"
#include "GameClient/InGameUI.h"
#include "GameClient/Image.h"
#include "GameClient/ParticleSys.h"
#include "GameClient/LanguageFilter.h"
#include "GameClient/Shadow.h"
#include "GameClient/GameText.h"

#include "WW3D2/ww3d.h"

#define VERY_TRANSPARENT_HEATVISION (0.001f)
#define HEATVISION_FADE_SCALAR (0.8f)

static const char *const TheDrawableIconNames[] =
{
	"DefaultHeal",
	"StructureHeal",
	"VehicleHeal",
#ifdef ALLOW_DEMORALIZE
	"Demoralized",
#else
	"Demoralized_OBSOLETE",
#endif
	"BombTimed",
	"BombRemote",
	"Disabled",
	"BattlePlanIcon_Bombard",
	"BattlePlanIcon_HoldTheLine",
	"BattlePlanIcon_SeekAndDestroy",
  "Emoticon",
	"Enthusiastic",//a red cross? // soon to replace?
	"Subliminal",  //with the gold border! replace?
	"CarBomb",
	nullptr
};
static_assert(ARRAY_SIZE(TheDrawableIconNames) == MAX_ICONS + 1, "Incorrect array size");




// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
DrawableIconInfo::DrawableIconInfo()
{
	for (int i = 0; i < MAX_ICONS; ++i)
	{
		m_icon[i] = nullptr;
		m_keepTillFrame[i] = 0;
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
DrawableIconInfo::~DrawableIconInfo()
{
	clear();
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void DrawableIconInfo::clear()
{
	for (int i = 0; i < MAX_ICONS; ++i)
	{
		deleteInstance(m_icon[i]);
		m_icon[i] = nullptr;
		m_keepTillFrame[i] = 0;
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void DrawableIconInfo::killIcon(DrawableIconType t)
{
	if (m_icon[t])
	{
		deleteInstance(m_icon[t]);
		m_icon[t] = nullptr;
		m_keepTillFrame[t] = 0;
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
DrawableLocoInfo::DrawableLocoInfo()
{
	m_pitch = 0.0f;
	m_pitchRate = 0.0f;
	m_roll = 0.0f;
	m_rollRate = 0.0f;
	m_yaw = 0.0f;
	m_accelerationPitch = 0.0f;
	m_accelerationPitchRate = 0.0f;
	m_accelerationRoll = 0.0f;
	m_accelerationRollRate = 0.0f;
	m_overlapZVel = 0.0f;
	m_overlapZ = 0.0f;
	m_wobble = 1.0f;

	m_wheelInfo.m_frontLeftHeightOffset = 0;
	m_wheelInfo.m_frontRightHeightOffset = 0;
	m_wheelInfo.m_rearLeftHeightOffset = 0;
	m_wheelInfo.m_rearRightHeightOffset = 0;
	m_wheelInfo.m_framesAirborneCounter = 0;
	m_wheelInfo.m_framesAirborne = 0;
	m_wheelInfo.m_wheelAngle = 0;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
DrawableLocoInfo::~DrawableLocoInfo()
{
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
static const char *drawableIconIndexToName( DrawableIconType iconIndex )
{

	DEBUG_ASSERTCRASH( iconIndex >= ICON_FIRST && iconIndex < MAX_ICONS,
										 ("drawableIconIndexToName - Illegal index '%d'", iconIndex) );

	return TheDrawableIconNames[ iconIndex ];

}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
static DrawableIconType drawableIconNameToIndex( const char *iconName )
{

	DEBUG_ASSERTCRASH( iconName != nullptr, ("drawableIconNameToIndex - Illegal name") );

	for( Int i = ICON_FIRST; i < MAX_ICONS; ++i )
		if( stricmp( TheDrawableIconNames[ i ], iconName ) == 0 )
			return (DrawableIconType)i;

	return ICON_INVALID;

}

// ------------------------------------------------------------------------------------------------
// constants
const UnsignedInt HEALING_ICON_DISPLAY_TIME	= LOGICFRAMES_PER_SECOND * 3;
const UnsignedInt DEFAULT_HEAL_ICON_WIDTH		= 32;
const UnsignedInt DEFAULT_HEAL_ICON_HEIGHT	= 32;
const RGBColor SICKLY_GREEN_POISONED_COLOR	= {-1.0f,  1.0f, -1.0f};
const RGBColor DARK_GRAY_DISABLED_COLOR			= {-0.5f, -0.5f, -0.5f};
const RGBColor RED_IRRADIATED_COLOR					= { 1.0f, -1.0f, -1.0f};
const Int MAX_ENABLED_MODULES								= 16;

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

/*static*/ Bool							Drawable::s_staticImagesInited = false;
/*static*/ const Image*			Drawable::s_veterancyImage[LEVEL_COUNT]	= { nullptr };
/*static*/ const Image*			Drawable::s_fullAmmo = nullptr;
/*static*/ const Image*			Drawable::s_emptyAmmo = nullptr;
/*static*/ const Image*			Drawable::s_fullContainer = nullptr;
/*static*/ const Image*			Drawable::s_emptyContainer = nullptr;
/*static*/ const Image*			Drawable::s_fullContainerHD[2] = { nullptr, nullptr };
/*static*/ const Image*			Drawable::s_emptyContainerHD[2] = { nullptr, nullptr };
// GeneralsX: file-static, not Drawable members - Drawable.h is digested whole into the SimID.
static const Image* s_fullAmmoHD[2] = { nullptr, nullptr };
static const Image* s_veterancyHD[2][4] = { { nullptr } };   // SCVeter1..3@133 / @200 (!GXUIHD.big)
static const Image* s_emptyAmmoHD[2] = { nullptr, nullptr };
static_assert(GX_PIP_VARIANTS == 2, "Drawable.h sizes s_fullContainerHD / s_emptyContainerHD for two pip variants");
/*static*/ Anim2DTemplate**	Drawable::s_animationTemplates = nullptr;
#ifdef DIRTY_CONDITION_FLAGS
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
/*static*/ Int							Drawable::s_modelLockCount = 0;
/*static*/ Bool							Drawable::s_anyModelDirty = TRUE;
// GX-SIMID-END
#endif

// ------------------------------------------------------------------------------------------------
/*static*/ void Drawable::initStaticImages()
{
	if (s_staticImagesInited)
		return;

	s_veterancyImage[0] = nullptr;
 	s_veterancyImage[1] = TheMappedImageCollection->findImageByName("SCVeter1");
	s_veterancyImage[2] = TheMappedImageCollection->findImageByName("SCVeter2");
	s_veterancyImage[3] = TheMappedImageCollection->findImageByName("SCVeter3");

	s_fullAmmo	= TheMappedImageCollection->findImageByName("SCPAmmoFull");
	s_emptyAmmo	= TheMappedImageCollection->findImageByName("SCPAmmoEmpty");
	s_fullContainer	= TheMappedImageCollection->findImageByName("SCPPipFull");
	s_emptyContainer	= TheMappedImageCollection->findImageByName("SCPPipEmpty");
	// GeneralsX: The hand-drawn 7x16 / 10x24 pips (!GXUIHD.big, GXImageVariant.h). Absent art
	// stays null and drawContained falls back to the next size down, ultimately the retail pair above.
	for (Int v = 0; v < GX_PIP_VARIANTS; ++v)
	{
		AsciiString name;
		name.format("SCPPipFull@%d", GX_PIP_VARIANT_PCT[v]);
		s_fullContainerHD[v] = TheMappedImageCollection->findImageByName(name);
		name.format("SCPPipEmpty@%d", GX_PIP_VARIANT_PCT[v]);
		s_emptyContainerHD[v] = TheMappedImageCollection->findImageByName(name);
		name.format("SCPAmmoFull@%d", GX_PIP_VARIANT_PCT[v]);
		s_fullAmmoHD[v] = TheMappedImageCollection->findImageByName(name);
		name.format("SCPAmmoEmpty@%d", GX_PIP_VARIANT_PCT[v]);
		s_emptyAmmoHD[v] = TheMappedImageCollection->findImageByName(name);
		for (Int lv = 1; lv <= 3; ++lv)
		{
			name.format("SCVeter%d@%d", lv, GX_PIP_VARIANT_PCT[v]);
			s_veterancyHD[v][lv] = TheMappedImageCollection->findImageByName(name);
		}
	}

	s_animationTemplates = NEW Anim2DTemplate* [ MAX_ICONS ];

	s_animationTemplates[ICON_DEFAULT_HEAL]			= TheAnim2DCollection->findTemplate(TheDrawableIconNames[ICON_DEFAULT_HEAL]);
	s_animationTemplates[ICON_STRUCTURE_HEAL]		= TheAnim2DCollection->findTemplate(TheDrawableIconNames[ICON_STRUCTURE_HEAL]);
	s_animationTemplates[ICON_VEHICLE_HEAL]			= TheAnim2DCollection->findTemplate(TheDrawableIconNames[ICON_VEHICLE_HEAL]);
#ifdef ALLOW_DEMORALIZE
	s_animationTemplates[ICON_DEMORALIZED]			= TheAnim2DCollection->findTemplate(TheDrawableIconNames[ICON_DEMORALIZED]);
#endif
	s_animationTemplates[ICON_BOMB_TIMED]				= TheAnim2DCollection->findTemplate(TheDrawableIconNames[ICON_BOMB_TIMED]);
	s_animationTemplates[ICON_BOMB_REMOTE]			= TheAnim2DCollection->findTemplate(TheDrawableIconNames[ICON_BOMB_REMOTE]);
	s_animationTemplates[ICON_DISABLED]					= TheAnim2DCollection->findTemplate(TheDrawableIconNames[ICON_DISABLED]);
	s_animationTemplates[ICON_BATTLEPLAN_BOMBARD]						= TheAnim2DCollection->findTemplate(TheDrawableIconNames[ICON_BATTLEPLAN_BOMBARD]);
	s_animationTemplates[ICON_BATTLEPLAN_HOLDTHELINE]				= TheAnim2DCollection->findTemplate(TheDrawableIconNames[ICON_BATTLEPLAN_HOLDTHELINE]);
	s_animationTemplates[ICON_BATTLEPLAN_SEARCHANDDESTROY]	= TheAnim2DCollection->findTemplate(TheDrawableIconNames[ICON_BATTLEPLAN_SEARCHANDDESTROY]);
	s_animationTemplates[ICON_EMOTICON]					= nullptr; //Emoticons can be anything, so we'll need to handle it dynamically.
	s_animationTemplates[ICON_ENTHUSIASTIC]			= TheAnim2DCollection->findTemplate(TheDrawableIconNames[ICON_ENTHUSIASTIC]);
	s_animationTemplates[ICON_ENTHUSIASTIC_SUBLIMINAL]			= TheAnim2DCollection->findTemplate(TheDrawableIconNames[ICON_ENTHUSIASTIC_SUBLIMINAL]);
	s_animationTemplates[ICON_CARBOMB]			= TheAnim2DCollection->findTemplate(TheDrawableIconNames[ICON_CARBOMB]);

	s_staticImagesInited = true;

}

//-------------------------------------------------------------------------------------------------
/*static*/ void Drawable::killStaticImages()
{
	delete[] s_animationTemplates;
	s_animationTemplates = nullptr;
}

//-------------------------------------------------------------------------------------------------
void Drawable::saturateRGB(RGBColor& color, Real factor)
{
	color.red *= factor;
	color.green *= factor;
	color.blue *= factor;

	Real halfFactor = factor * 0.5f;

	color.red -= halfFactor;
	color.green -= halfFactor;
	color.blue -= halfFactor;

}


//--- A MACRO TO APPLY TO TheTacticalView->getZoom() ------ To Clamp the return to a visually pleasing size
//--- so that icons, emoticons, health bars, pips, etc, look reasonably solid and don't shimmer or tweed
//#define CLAMP_ICON_ZOOM_FACTOR(n) (MAX(0.80f, MIN(1.00f, n)))

// GeneralsX: THE ICON CLUSTER SHRINKS WITH THE BAR, ONLY ABOVE THE STOCK CAMERA CEILING.
// Normal builds draw pips, the veterancy chevron and the status icons at a FIXED pixel size (their zoom sizing
// is inside SCALE_ICONS_WITH_ZOOM_ML, never defined), while the health bar is sized by
// View::getRetailReferenceZoom(). Above the ceiling the stock MaxCameraHeight would give this view the bar shrinks
// with the unit, so the icons must too or they dwarf it. 1.0 up to that ceiling - the retail look, unchanged, which
// is NOT the same as zoom <= 1: a widescreen view's stock ceiling already takes zoom past 1.0.
static Real gxOverlayIconShrinkRaw( void )
{
	Real iconScale = 1.0f;
	if ( TheTacticalView != nullptr )
		TheTacticalView->getRetailReferenceZoom( &iconScale );
	return iconScale;
}

// GeneralsX:
// Generals Online draws these pips at a fixed 1.0 scale at every zoom
// (references/generals-online-client Drawable.cpp drawAmmo/drawContained: scale = 1.0f), so the occupancy and ammo pips here
// never shrink with the zoom-out either. ONE mechanism for every over-unit marker (pips, rank chevrons, status icons, emoticons,
// battle-plan and bomb icons): gxOverlayIconShrink() is the constant-size scale they all call; only the zoom-label text keeps
// gxOverlayIconShrinkRaw(). At the farthest zoom-out these were 1-2 px otherwise (0.10). GX_PIPS_FLOOR=0 (test only) restores the old behaviour for the "before" picture.
// GeneralsX: The ammo pips are drawn last-but-one
// and the rank chevron last, both hanging off the same health bar, and at the constant 1.0 scale (HD 10x20 art from 1440p) a 4-5 pip
// row is wider than the bar, so it ran under the chevron and dwarfed a parked jet. drawAmmo leaves the rect it drew here; drawVeterancy
// reads it (same drawable, same frame) and steps the chevron right of the row only when the two rects actually intersect.
static DrawableID s_ammoRectOwner = INVALID_DRAWABLE_ID;
static IRegion2D s_ammoRect;

static Real gxOverlayIconShrink( void )
{
	static Real s_floor = -1.0f;
	if (s_floor < 0.0f)
	{
		const char *v = getenv("GX_PIPS_FLOOR");
		s_floor = (v != nullptr && strcmp(v, "0") == 0) ? 0.0f : 1.0f;
	}
	const Real s = gxOverlayIconShrinkRaw();
	return s < s_floor ? s_floor : s;
}

// GeneralsX: THE "UNDER CONSTRUCTION" TEXT SHRINKS GENTLY ABOVE THE STOCK CAMERA CEILING (owner,
// 02/10/2026: at max zoom-out the "Building: x%" labels stayed full size and covered the map). Same size as retail up to
// the ceiling; above it the caption font steps down with gxWorldLabelScale (GXWorldLabelScale.h). Every label shares one
// size, so the font is looked up only when that size steps - a camera at rest asks the library nothing, and setFont()
// returns at once for the font a string already has. Fonts live until the game exits (FontLibrary::reset runs only in
// GameClient's destructor), so keeping the pointer is safe.
static GameFont *gxConstructLabelFont( void )
{
	static Int s_basePoints = 0;
	static Int s_points = 0;
	static GameFont *s_font = nullptr;
	if (TheInGameUI == nullptr || TheFontLibrary == nullptr || TheGlobalLanguageData == nullptr)
		return nullptr;
	const Int basePoints = TheGlobalLanguageData->adjustFontSize(TheInGameUI->getDrawableCaptionPointSize());
	const Int points = gxWorldLabelPoints( basePoints, gxWorldLabelScale( gxOverlayIconShrinkRaw() ),
		basePoints == s_basePoints ? s_points : 0 );
	if (s_font == nullptr || basePoints != s_basePoints || points != s_points)
	{
		s_font = TheFontLibrary->getFont( TheInGameUI->getDrawableCaptionFontName(), points, TheInGameUI->isDrawableCaptionBold() );
		s_basePoints = basePoints;
		s_points = points;
	}
	return s_font;
}

// GeneralsX: THE GROUP NUMERAL AND FORMATION LETTER SHRINK WITH THE CLUSTER TOO.
// drawUIText() draws both from shared DisplayStrings in TheDrawGroupInfo's font at its fixed point size, so above
// retail's camera ceiling they stayed full-size beside the shrunken bar. Re-point the string at the same font scaled
// by the icon shrink. The font library interns fonts and setFont() ignores the font a string already has, so the text
// is only re-rendered when the size actually changes; a shrink of 1.0 asks for retail's own font, i.e. no change.
static void gxShrinkOverlayText( DisplayString *str, Real shrink )
{
	if( str == nullptr || TheFontLibrary == nullptr || TheDrawGroupInfo->m_fontSize < 1 )
		return;
	const Int pointSize = MAX( 1, REAL_TO_INT( TheDrawGroupInfo->m_fontSize * shrink + 0.5f ) );
	str->setFont( TheFontLibrary->getFont( TheDrawGroupInfo->m_fontName, pointSize, TheDrawGroupInfo->m_fontIsBold ) );
}
#define CLAMP_ICON_ZOOM_FACTOR(n) (n)//nothing


//-------------------------------------------------------------------------------------------------
/** Drawables are lightweight, graphical entities which live on the GameClient,
 * and are usually bound to GameLogic objects.  In other words, they are the
 * graphical side of a logical object, whereas GameLogic objects encapsulate
 * behaviors and physics.  */
//-------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
Drawable::Drawable( const ThingTemplate *thingTemplate, DrawableStatusBits statusBits )
				: Thing( thingTemplate )
{

	// assign status bits before anything else can be done
	m_status = statusBits;

	m_nextDrawable = nullptr;
	m_prevDrawable = nullptr;

	// register drawable with the GameClient ... do this first before we start doing anything
	// complex that uses any of the drawable data so that we have and ID!!  It's ok to initialize
	// members of the drawable before this registration happens
	//
	TheGameClient->registerDrawable( this );

	Int i;

	m_flashColor = 0;
	m_selected = '\0';

	m_expirationDate = 0;  // 0 == never expires

	m_lastConstructDisplayed = -1;
	// GeneralsX: No construct-percent string up front: every Drawable
	// allocated one (plus a font lookup) though only under-construction buildings ever draw it.
	// drawConstructPercent() now allocates it on first use, with the caption font that used to be set here.
	m_constructDisplayString = nullptr;

	m_ambientSound = nullptr;

	m_decalOpacityFadeTarget = 0;
	m_decalOpacityFadeRate = 0;
	m_decalOpacity = 0;

	m_explicitOpacity = 1.0f;
	m_stealthOpacity = 1.0f;
	m_effectiveStealthOpacity = 1.0f;
	m_terrainDecalType = TERRAIN_DECAL_NONE;

  m_fadeMode = FADING_NONE;
	m_timeElapsedFade = 0;
	m_timeToFade = 0;
	// GeneralsX: Sentinel, so the first updateDrawable() call steps.
	m_lastLogicFrameUpdated = InvalidShroudClearFrame;
	m_interpFrame = InvalidShroudClearFrame;
	memset(m_interpTurretPrev, 0, sizeof(m_interpTurretPrev));
	memset(m_interpTurretCur, 0, sizeof(m_interpTurretCur));

	m_shroudClearFrame = InvalidShroudClearFrame;

	for (i = 0; i < NUM_DRAWABLE_MODULE_TYPES; ++i)
		m_modules[i] = nullptr;

	m_stealthLook = STEALTHLOOK_NONE;

	m_flashCount = 0;

	m_locoInfo = nullptr;
	m_physicsXform = nullptr;

	// sanity
	if( TheGameClient == nullptr || thingTemplate == nullptr )
	{

		assert( 0 );
		return;

	}

	m_instance.Make_Identity();
	m_instanceIsIdentity = true;

	//Real scaleFuzziness = thingTemplate->getInstanceScaleFuzziness();
	//Real fuzzyScale = ( 1.0f + GameClientRandomValueReal( -scaleFuzziness, scaleFuzziness ));
	m_instanceScale = thingTemplate->getAssetScale();// * fuzzyScale;

	// initially not bound to an object
	m_object = nullptr;

	// tintStatusTracking
	m_tintStatus = 0;
	m_prevTintStatus = 0;

#ifdef DIRTY_CONDITION_FLAGS
	m_isModelDirty = true;
	s_anyModelDirty = TRUE;
#endif

	m_hidden = false;
	m_hiddenByStealth = false;
	m_heatVisionOpacity = 0.0f;
	m_drawableFullyObscuredByShroud = false;

	m_ambientSoundEnabled = TRUE;

  m_receivesDynamicLights = TRUE; // a good default... overridden by one of my draw modules if at all

	// allocate any modules we need to, we should keep
	// this at or near the end of the drawable construction so that we have
	// all the valid data about the thing when we create the module
	//

	//Filter out drawable modules which have been disabled because of game LOD.
	Int modIdx;
	Module** m;

	const ModuleInfo& drawMI = thingTemplate->getDrawModuleInfo();
	m_modules[MODULETYPE_DRAW - FIRST_DRAWABLE_MODULE_TYPE] = MSGNEW("ModulePtrs") Module*[drawMI.getCount()+1];	// pool[]ify
	m = m_modules[MODULETYPE_DRAW - FIRST_DRAWABLE_MODULE_TYPE];
	for (modIdx = 0; modIdx < drawMI.getCount(); ++modIdx)
	{
		const ModuleData* newModData = drawMI.getNthData(modIdx);
		if (TheGlobalData->m_useDrawModuleLOD &&
				newModData->getMinimumRequiredGameLOD() > TheGameLODManager->getStaticLODLevel())
			continue;
		*m++ = TheModuleFactory->newModule(this, drawMI.getNthName(modIdx), newModData, MODULETYPE_DRAW);
	}
	*m = nullptr;

	const ModuleInfo& cuMI = thingTemplate->getClientUpdateModuleInfo();
	if (cuMI.getCount())
	{
		// since most things don't have CU modules, we allow this to be null!
		m_modules[MODULETYPE_CLIENT_UPDATE - FIRST_DRAWABLE_MODULE_TYPE] = MSGNEW("ModulePtrs") Module*[cuMI.getCount()+1];	// pool[]ify
		m = m_modules[MODULETYPE_CLIENT_UPDATE - FIRST_DRAWABLE_MODULE_TYPE];
		for (modIdx = 0; modIdx < cuMI.getCount(); ++modIdx)
		{
			const ModuleData* newModData = cuMI.getNthData(modIdx);

	/// @todo srj -- this is evil, we shouldn't look at the module name directly!
			if (thingTemplate->isKindOf(KINDOF_SHRUBBERY) &&
					!TheGlobalData->m_useTreeSway &&
					cuMI.getNthName(modIdx).compareNoCase("SwayClientUpdate") == 0)
				continue;

			*m++ = TheModuleFactory->newModule(this, cuMI.getNthName(modIdx), newModData, MODULETYPE_CLIENT_UPDATE);
		}
		*m = nullptr;
	}

	/// allow for inter-Module resolution
	for (i = 0; i < NUM_DRAWABLE_MODULE_TYPES; ++i)
	{
		for (Module** m = m_modules[i]; m && *m; ++m)
			(*m)->onObjectCreated();
	}

	const Bool shadowsEnabled = getShadowsEnabled();

	// TheSuperHackers @fix xezon 14/09/2025 Match the shadow states of all draw modules with this drawable.
	for (DrawModule** dm = (DrawModule**)getModuleList(MODULETYPE_DRAW); *dm; ++dm)
	{
		(*dm)->setShadowsEnabled(shadowsEnabled);
	}

	m_groupNumber = nullptr;
	m_captionDisplayString = nullptr;
	m_drawableInfo.m_drawable = this;
	m_drawableInfo.m_ghostObject = nullptr;

	m_iconInfo = nullptr;								// lazily allocate!
	m_selectionFlashEnvelope = nullptr;	// lazily allocate!
	m_colorTintEnvelope = nullptr;				// lazily allocate!

	initStaticImages();

	startAmbientSound();

}
// GX-SIMID-END

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
Drawable::~Drawable()
{
	Int i;

	if( m_constructDisplayString )
		TheDisplayStringManager->freeDisplayString( m_constructDisplayString );
	m_constructDisplayString = nullptr;

	if ( m_captionDisplayString )
		TheDisplayStringManager->freeDisplayString( m_captionDisplayString );
	m_captionDisplayString = nullptr;

	m_groupNumber = nullptr;

	// delete any modules callbacks
	for (i = 0; i < NUM_DRAWABLE_MODULE_TYPES; ++i)
	{
		for (Module** m = m_modules[i]; m && *m; ++m)
		{
			deleteInstance(*m);
			*m = nullptr;	// in case other modules call findModule from their dtor!
		}
		delete [] m_modules[i];
		m_modules[i] = nullptr;
	}

	stopAmbientSound();

	m_ambientSound.Clear();

	/// @todo this is nasty, we need a real general effects system
	// remove any entries that might be present from the ray effect system
	TheGameClient->removeFromRayEffects( this );

	// reset object to nullptr so we never mistaken grab "dead" objects
	m_object = nullptr;

	// delete any icons present
	deleteInstance(m_iconInfo);
	m_iconInfo = nullptr;

	deleteInstance(m_selectionFlashEnvelope);
	m_selectionFlashEnvelope = nullptr;

	deleteInstance(m_colorTintEnvelope);
	m_colorTintEnvelope = nullptr;

	deleteInstance(m_locoInfo);
	m_locoInfo = nullptr;

	delete m_physicsXform;
}

//-------------------------------------------------------------------------------------------------
/** Run from GameClient::destroyDrawable */
//-------------------------------------------------------------------------------------------------
void Drawable::onDestroy()
{

	//
	// run the onDelete on all modules present so they each have an opportunity to cleanup
	// anything they need to ... including talking to any other modules
	//
	for( Int i = 0; i < NUM_DRAWABLE_MODULE_TYPES; i++ )
	{

		for( Module** m = m_modules[ i ]; m && *m; ++m )
			(*m)->onDelete();

	}

}

//-------------------------------------------------------------------------------------------------
Bool Drawable::isVisible()
{
	for (DrawModule** dm = getDrawModules(); *dm; ++dm)
	{
		if ((*dm)->isVisible())
		{
			return TRUE;
		}
	}
	return FALSE;
}

//-------------------------------------------------------------------------------------------------
Bool Drawable::getShouldAnimate( Bool considerPower ) const
{
	const Object *obj = getObject();

	if (obj)
	{
		if (considerPower && obj->testScriptStatusBit(OBJECT_STATUS_SCRIPT_UNPOWERED))
			return FALSE;

		if (obj->isDisabled())
		{
			if( obj->isDisabledByType( DISABLED_HACKED )
				|| obj->isDisabledByType( DISABLED_PARALYZED )
				|| obj->isDisabledByType( DISABLED_EMP )
				// srj sez: unmanned things also should not animate. (eg, gattling tanks,
				// which have a slight barrel animation even when at rest). if this causes
				// a problem, we will need to fix gattling tanks in another way.
				|| obj->isDisabledByType( DISABLED_UNMANNED )
				)
				return FALSE;

			if (considerPower && obj->isDisabledByType(DISABLED_UNDERPOWERED))
			{
				// We only pause animations if this draw module says so
				// By checking for the others first, we prevent underpower from allowing a True on an addition disable type
				return FALSE;
			}
		}
	}

	return TRUE;
}

//-------------------------------------------------------------------------------------------------
// this method must ONLY be called from the client, NEVER From the logic, not even indirectly.
Bool Drawable::clientOnly_getFirstRenderObjInfo(Coord3D* pos, Real* boundingSphereRadius, Matrix3D* transform)
{
	DrawModule** dm = getDrawModules();
	const ObjectDrawInterface* di = (dm && *dm) ? (*dm)->getObjectDrawInterface() : nullptr;
	if (di)
	{
		return di->clientOnly_getRenderObjInfo(pos, boundingSphereRadius, transform);
	}
	return false;
}

//-------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
Bool Drawable::getProjectileLaunchOffset(WeaponSlotType wslot, Int specificBarrelToUse, Matrix3D* launchPos, WhichTurretType tur, Coord3D* turretRotPos, Coord3D* turretPitchPos) const
{
	for (const DrawModule** dm = getDrawModules(); *dm; ++dm)
	{
		const ObjectDrawInterface* di = (*dm)->getObjectDrawInterface();
		if (di && di->getProjectileLaunchOffset(m_conditionState, wslot, specificBarrelToUse, launchPos, tur, turretRotPos, turretPitchPos))
			return true;
	}
	return false;
}
// GX-SIMID-END

//-------------------------------------------------------------------------------------------------
void Drawable::setAnimationLoopDuration(UnsignedInt numFrames)
{
	for (DrawModule** dm = getDrawModules(); *dm; ++dm)
	{
		ObjectDrawInterface* di = (*dm)->getObjectDrawInterface();
		if (di)
			di->setAnimationLoopDuration(numFrames);
	}
}

//-------------------------------------------------------------------------------------------------
void Drawable::setAnimationCompletionTime(UnsignedInt numFrames)
{
	for (DrawModule** dm = getDrawModules(); *dm; ++dm)
	{
		ObjectDrawInterface* di = (*dm)->getObjectDrawInterface();
		if (di)
			di->setAnimationCompletionTime(numFrames);
	}
}

//-------------------------------------------------------------------------------------------------
void Drawable::updateSubObjects()
{
	for (DrawModule** dm = getDrawModules(); *dm; ++dm)
	{
		ObjectDrawInterface* di = (*dm)->getObjectDrawInterface();
		if (di)
			di->updateSubObjects();
	}
}

//-------------------------------------------------------------------------------------------------
void Drawable::showSubObject( const AsciiString& name, Bool show )
{
	for (DrawModule** dm = getDrawModules(); *dm; ++dm)
	{
		ObjectDrawInterface* di = (*dm)->getObjectDrawInterface();
		if (di)
		{
			di->showSubObject( name, show );
		}
	}
}

#ifdef ALLOW_ANIM_INQUIRIES
// srj sez: not sure if this is a good idea, for net sync reasons...
//-------------------------------------------------------------------------------------------------
/**
	This call asks, "In the current animation (if any) how far along are you, from 0.0f to 1.0f".
*/
Real Drawable::getAnimationScrubScalar() const // lorenzen
{
	for (const DrawModule** dm = getDrawModules(); *dm; ++dm)
	{
		const ObjectDrawInterface* di = (*dm)->getObjectDrawInterface();
		if (di)
		{
			return di->getAnimationScrubScalar();
		}
	}

	return 0.0f;

}
#endif

//-------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
Int Drawable::getPristineBonePositions(const char* boneNamePrefix, Int startIndex, Coord3D* positions, Matrix3D* transforms, Int maxBones) const
{
	Int count = 0;
	for (const DrawModule** dm = getDrawModules(); *dm; ++dm)
	{
		if (maxBones <= 0)
			break;

		const ObjectDrawInterface* di = (*dm)->getObjectDrawInterface();
		if (di)
		{
			Int subcount =
				di->getPristineBonePositionsForConditionState(m_conditionState, boneNamePrefix, startIndex, positions, transforms, maxBones);

			if (subcount > 0)
			{
				count += subcount;
				if (positions)
					positions += subcount;
				if (transforms)
					transforms += subcount;
				maxBones -= subcount;
			}
		}
	}
	return count;
}
// GX-SIMID-END

//-------------------------------------------------------------------------------------------------
Int Drawable::getCurrentClientBonePositions(const char* boneNamePrefix, Int startIndex, Coord3D* positions, Matrix3D* transforms, Int maxBones) const
{
	Int count = 0;
	for (const DrawModule** dm = getDrawModules(); *dm; ++dm)
	{
		if (maxBones <= 0)
			break;

		const ObjectDrawInterface* di = (*dm)->getObjectDrawInterface();
		if (di)
		{
			Int subcount =
				di->getCurrentBonePositions(boneNamePrefix, startIndex, positions, transforms, maxBones);

			if (subcount > 0)
			{
				count += subcount;
				if (positions)
					positions += subcount;
				if (transforms)
					transforms += subcount;
				maxBones -= subcount;
			}
		}
	}
	return count;
}

//-------------------------------------------------------------------------------------------------
Bool Drawable::getCurrentWorldspaceClientBonePositions(const char* boneName, Matrix3D& transform) const
{
	for (const DrawModule** dm = getDrawModules(); *dm; ++dm)
	{
		const ObjectDrawInterface* di = (*dm)->getObjectDrawInterface();
		if (di && di->getCurrentWorldspaceClientBonePositions(boneName, transform))
		{
			return true;
		}
	}
	return false;
}

//-------------------------------------------------------------------------------------------------
void Drawable::setTerrainDecal(TerrainDecalType type)
{
	if (m_terrainDecalType == type)
		return;

	m_terrainDecalType=type;

	DrawModule** dm = getDrawModules();

	//Only the first draw module gets a decal to prevent stacking.
	//Should be okay as long as we keep the primary object in the
	//first module.
	if (*dm)
		(*dm)->setTerrainDecal(type);

}

//-------------------------------------------------------------------------------------------------
void Drawable::setTerrainDecalSize(Real x, Real y)
{
	DrawModule** dm = getDrawModules();

	if (*dm)
		(*dm)->setTerrainDecalSize(x,y);
}

//-------------------------------------------------------------------------------------------------
void Drawable::setTerrainDecalFadeTarget(Real target, Real rate)
{
	if (m_decalOpacityFadeTarget != target)
	{
		m_decalOpacityFadeTarget = target;
		m_decalOpacityFadeRate = rate;
	}
	//else
	//	m_decalOpacityFadeRate = 0;

}

//-------------------------------------------------------------------------------------------------
void Drawable::setShadowsEnabled(Bool enable)
{
	// set status bit
	if( enable )
		setDrawableStatus( DRAWABLE_STATUS_SHADOWS );
	else
		clearDrawableStatus( DRAWABLE_STATUS_SHADOWS );

	for (DrawModule** dm = getDrawModules(); *dm; ++dm)
	{
		(*dm)->setShadowsEnabled(enable);
	}
}

//-------------------------------------------------------------------------------------------------
/**frees all shadow resources used by this module - used by Options screen.*/
void Drawable::releaseShadows()
{
	for (DrawModule** dm = getDrawModules(); *dm; ++dm)
	{
		(*dm)->releaseShadows();
	}
}

//-------------------------------------------------------------------------------------------------
/**create shadow resources if not already present. Used by Options screen.*/
void Drawable::allocateShadows()
{
	for (DrawModule** dm = getDrawModules(); *dm; ++dm)
	{
		(*dm)->allocateShadows();
	}
}

//-------------------------------------------------------------------------------------------------
void Drawable::setFullyObscuredByShroud(Bool fullyObscured)
{
	if (m_drawableFullyObscuredByShroud != fullyObscured)
	{
		for (DrawModule** dm = getDrawModules(); *dm; ++dm)
		{
			(*dm)->setFullyObscuredByShroud(fullyObscured);
		}
		m_drawableFullyObscuredByShroud = fullyObscured;
	}
}

//-------------------------------------------------------------------------------------------------
/** Set drawable's "selected" status, if not already set.  Also update running
 * total count of selected drawables. */
//-------------------------------------------------------------------------------------------------
void Drawable::friend_setSelected()
{
	if(isSelected() == false)
	{
		m_selected = TRUE;
		onSelected();
	}

}

//-------------------------------------------------------------------------------------------------
/** Clear drawable's "selected" status, if not already clear.  Also update running
 * total count of selected drawables. */
//-------------------------------------------------------------------------------------------------
void Drawable::friend_clearSelected()
{
	if(isSelected())
	{
		m_selected = FALSE;
		onUnselected();
	}
}

// ------------------------------------------------------------------------------------------------
/** Flash the drawable with the color */
// ------------------------------------------------------------------------------------------------
void Drawable::colorFlash( const RGBColor* color, UnsignedInt decayFrames, UnsignedInt attackFrames, UnsignedInt sustainAtPeak )
{
	if (m_colorTintEnvelope == nullptr)
		m_colorTintEnvelope = newInstance(TintEnvelope);

	if( color )
	{
		m_colorTintEnvelope->play( color, attackFrames, decayFrames, sustainAtPeak);
	}
	else
	{
		RGBColor white;
		white.setFromInt(0xffffffff);
		m_colorTintEnvelope->play( &white );
	}
}

// ------------------------------------------------------------------------------------------------
/** Tint a drawable a specified color */
// ------------------------------------------------------------------------------------------------
void Drawable::colorTint( const RGBColor* color )
{
	if( color )
	{
		// set the color via color flash
		colorFlash( color, 0, 0, ~0u );
	}
	else
	{
		if (m_colorTintEnvelope == nullptr)
			m_colorTintEnvelope = newInstance(TintEnvelope);

		// remove the tint applied to the object
		m_colorTintEnvelope->rest();
	}

}

//-------------------------------------------------------------------------------------------------
/** Gathering point for all things besides actual selection that must happen on selection */
//-------------------------------------------------------------------------------------------------
void Drawable::onSelected()
{

	flashAsSelected();//much simpler

	Object* obj = getObject();
	if ( obj )
	{
		ContainModuleInterface* contain = obj->getContain();
		if ( contain )
		{
			contain->clientVisibleContainedFlashAsSelected();
		}
	}

}

//-------------------------------------------------------------------------------------------------
/** Gathering point for all things besides actual selection that must happen on deselection */
//-------------------------------------------------------------------------------------------------
void Drawable::onUnselected()
{
	// nothing
}

//-------------------------------------------------------------------------------------------------
/** get FX color value to add to ALL LIGHTS when drawing */
//-------------------------------------------------------------------------------------------------
const Vector3 * Drawable::getTintColor() const
{
	if ( m_colorTintEnvelope )
	{
		if (m_colorTintEnvelope->isEffective())
		{
			return m_colorTintEnvelope->getColor();
		}
	}

	return nullptr;

}
//-------------------------------------------------------------------------------------------------
/** get SELECTION color value to add to ALL LIGHTS when drawing */
//-------------------------------------------------------------------------------------------------
const Vector3 * Drawable::getSelectionColor()	const
{
	if (m_selectionFlashEnvelope)
	{
		if (m_selectionFlashEnvelope->isEffective())
		{
			return m_selectionFlashEnvelope->getColor();
		}
	}

	return nullptr;

}


//-------------------------------------------------------------------------------------------------
/** fades the object out gradually...how gradually is determined by number of frames */
//-------------------------------------------------------------------------------------------------
void Drawable::fadeOut( UnsignedInt frames )		///< cloak object
{
	setDrawableOpacity(1.0);
	m_fadeMode = FADING_OUT;
	m_timeToFade = frames;
	m_timeElapsedFade = 0;
}

//-------------------------------------------------------------------------------------------------
/** fades the object in gradually...how gradually is determined by number of frames */
//-------------------------------------------------------------------------------------------------
void Drawable::fadeIn( UnsignedInt frames )		///< decloak object
{
	setDrawableOpacity(0.0);
	m_fadeMode = FADING_IN;
	m_timeToFade = frames;
	m_timeElapsedFade = 0;
}


//-------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
Real Drawable::getScale () const
{
	return m_instanceScale;
//	return getTemplate()->getAssetScale();
}
// GX-SIMID-END

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
void Drawable::reactToBodyDamageStateChange(BodyDamageType newState)
{
	static const ModelConditionFlagType TheDamageMap[BODYDAMAGETYPE_COUNT] =
	{
		MODELCONDITION_INVALID,
		MODELCONDITION_DAMAGED,
		MODELCONDITION_REALLY_DAMAGED,
		MODELCONDITION_RUBBLE,
	};

	ModelConditionFlags newDamage;
	if (TheDamageMap[newState] != MODELCONDITION_INVALID)
		newDamage.set(TheDamageMap[newState]);

	clearAndSetModelConditionFlags(
		MAKE_MODELCONDITION_MASK3(MODELCONDITION_DAMAGED, MODELCONDITION_REALLY_DAMAGED, MODELCONDITION_RUBBLE),
		newDamage);

	startAmbientSound(newState, TheGlobalData->m_timeOfDay);
}
// GX-SIMID-END

//-------------------------------------------------------------------------------------------------
void Drawable::setEffectiveOpacity( Real pulseFactor, Real explicitOpacity /* = -1.0f */)
{
	if( explicitOpacity != -1.0f )
	{
		m_stealthOpacity = MIN( 1.0f, MAX( 0.0f, explicitOpacity ) );
	}

	Real pf = MIN(1.0f, MAX(0.0f, pulseFactor));

	Real pulseMargin = (1.0f - m_stealthOpacity);
	Real pulseAmount = pulseMargin * pf;

	m_effectiveStealthOpacity = m_stealthOpacity + pulseAmount;
}

//-------------------------------------------------------------------------------------------------
void Drawable::imitateStealthLook( Drawable& otherDraw )
{
}

//-------------------------------------------------------------------------------------------------
/** update is called once per frame */
//-------------------------------------------------------------------------------------------------
//DECLARE_PERF_TIMER(updateDrawable)
void Drawable::updateDrawable()
{
	//USE_PERF_TIMER(updateDrawable)

	UnsignedInt now = TheGameLogic->getFrame();
	Object *obj = getObject();

	// GeneralsX: The class-C animation gate. See m_lastLogicFrameUpdated
	// in Drawable.h for why this is per-object and why it compares rather than subtracts.
	//
	// This gates ONLY the state that is authored in logic frames. It deliberately does NOT gate the
	// ClientUpdateModule loop below (SwayClientUpdate is already scaled by
	// getActualLogicTimeScaleOverFpsRatio and is meant to be per-render), the expiration test
	// (an absolute compare against `now`, correct at any call rate), or the tint/selection-flash
	// envelopes (their update() is already timeScale-corrected - gating them would double-compensate
	// and make tints run 8x SLOW).
	//
	// At most one logic frame can retire per iteration today: GameEngine::update() calls
	// TheGameLogic->UPDATE() at most once with no catch-up loop. If that ever gains one, this
	// under-runs at one step per call - the safe direction, slow rather than bursty - and the fix is
	// to step by min(now - m_lastLogicFrameUpdated, someCap) here.
	const Bool logicFrameAdvanced = (now != m_lastLogicFrameUpdated);
	m_lastLogicFrameUpdated = now;

	{
		for (ClientUpdateModule** cu = getClientUpdateModules(); cu && *cu; ++cu)
		{
			(*cu)->clientUpdate();
		}
	}

	{

		// handle fading in or out
		if (m_fadeMode != FADING_NONE)
		{
			Real numer = (m_fadeMode == FADING_IN) ? (m_timeElapsedFade) : (m_timeToFade-m_timeElapsedFade);

			setDrawableOpacity(numer/(Real)m_timeToFade);
			// The opacity is still SET every iteration; only the clock advances on a tick.
			if (logicFrameAdvanced)
				++m_timeElapsedFade;

			if (m_timeElapsedFade > m_timeToFade)
				m_fadeMode = FADING_NONE;
		}
	}


	if ( getTerrainDecalType() != TERRAIN_DECAL_NONE )
	{
		DrawModule** dm = getDrawModules();

		if (*dm)
		{
			if (m_decalOpacityFadeRate != 0)
			{
				//LERP
				(*dm)->setTerrainDecalOpacity(m_decalOpacity);
				if (logicFrameAdvanced)
					m_decalOpacity += m_decalOpacityFadeRate;
			}
			//---------------

			if (m_decalOpacityFadeRate < 0 && m_decalOpacity <= 0 )
			{
				m_decalOpacityFadeRate = 0.0f;
				m_decalOpacity = 0.0f;
				this->setTerrainDecal(TERRAIN_DECAL_NONE);
			}
			else if (m_decalOpacityFadeRate > 0 && m_decalOpacity >= 1.0f)
			{
				m_decalOpacity = 1.0f;
				m_decalOpacityFadeRate = 0.0f;
				(*dm)->setTerrainDecalOpacity(m_decalOpacity);
			}

		}
	}
	else
		m_decalOpacity = 0;


	{

		if (m_expirationDate != 0 && now >= m_expirationDate)
		{
			DEBUG_ASSERTCRASH(obj == nullptr, ("Drawables with Objects should not have expiration dates!"));
			TheGameClient->destroyDrawable(this);
			return;
		}
	}

	{

		// The modulo was never a gate on its own: an eligible frame number stays eligible for every
		// iteration inside that frame, so one flash burned as many counts as the loop spun.
		if (m_flashCount > 0 && logicFrameAdvanced
		    && (TheGameClient->getFrame() % DRAWABLE_FRAMES_PER_FLASH) == 0)
		{
			RGBColor tmp;
			tmp.setFromInt(m_flashColor);
			colorFlash(&tmp);
			m_flashCount--;
		}
	}

	//Lets figure out whether we should be changing colors right about now
	// we'll use an ifelseif ladder since we are scanning bits
	if( m_prevTintStatus != m_tintStatus )// edge test
	{
		if ( testTintStatus( TINT_STATUS_DISABLED ) )
		{
			if (m_colorTintEnvelope == nullptr)
				m_colorTintEnvelope = newInstance(TintEnvelope);
			m_colorTintEnvelope->play( &DARK_GRAY_DISABLED_COLOR, 30, 30, SUSTAIN_INDEFINITELY);
		}
//		else if ( testTintStatus( TINT_STATUS_POISONED) )
//		{
//			if (m_colorTintEnvelope == nullptr)
//				m_colorTintEnvelope = newInstance(TintEnvelope);
//			m_colorTintEnvelope->play( &SICKLY_GREEN_POISONED_COLOR, 30, 30, SUSTAIN_INDEFINITELY);
//		}
//		else if ( testTintStatus( TINT_STATUS_IRRADIATED) )
//		{
//			if (m_colorTintEnvelope == nullptr)
//				m_colorTintEnvelope = newInstance(TintEnvelope);
//			m_colorTintEnvelope->play( &RED_IRRADIATED_COLOR, 30, 30, SUSTAIN_INDEFINITELY);
//		}
		else
		{
			// NO TINTING SHOULD BE PRESENT
			if (m_colorTintEnvelope == nullptr)
				m_colorTintEnvelope = newInstance(TintEnvelope);
			m_colorTintEnvelope->release(); // head on back to normal, now
		}

	}

	m_prevTintStatus = m_tintStatus;//for next frame

	if ( obj )
	{
		if ( ! obj->isEffectivelyDead() )
			clearTintStatus( TINT_STATUS_IRRADIATED); // so the res glow stops when not exposed
	}

	if (m_colorTintEnvelope)
	  m_colorTintEnvelope->update(); // defector fx, disable fx, etc...

	if (m_selectionFlashEnvelope)
		m_selectionFlashEnvelope->update(); // selection flashing

	//If we have an ambient sound, and we aren't currently playing it, attempt to play it now
	if( m_ambientSound && m_ambientSoundEnabled && !m_ambientSound->getEventName().isEmpty() && !m_ambientSound->isCurrentlyPlaying() )
	{
		startAmbientSound();
	}
}

//-------------------------------------------------------------------------------------------------
void Drawable::flashAsSelected( const RGBColor *color ) ///< drawable takes care of the details if you spec no color
{
	if (m_selectionFlashEnvelope == nullptr)
		m_selectionFlashEnvelope = newInstance(TintEnvelope);

	if ( color )
	{
		m_selectionFlashEnvelope->play( color, 0, 4 );
	}
	else
	{
		Object *obj = getObject();
		if (obj)
		{
			RGBColor tempColor;
			if (TheGlobalData->m_selectionFlashHouseColor)
				tempColor.setFromInt(obj->getIndicatorColor());
			else
				tempColor.setFromInt(0xffffffff);//white

			Real saturation = TheGlobalData->m_selectionFlashSaturationFactor;
			saturateRGB( tempColor, saturation );
			m_selectionFlashEnvelope->play( &tempColor, 0, 4 );

		}
	}
}

//-------------------------------------------------------------------------------------------------
// GeneralsX: Render interpolation of the drawable's own transform (graphics research
// a2-07, g-05). mtx comes in as the logic transform and leaves as the one to draw: lerp(the previous logic frame,
// the current one, Display::getLogicPhase()), position lerped and rotation slerped. Only this local copy changes.
// Thing::m_transform stays the logic transform, because logic converts pristine bone positions through it
// (Thing::convertBonePosToWorldPos, from Object::getSingle/MultiLogicalBonePosition, DeliverPayloadAIUpdate,
// HelicopterSlowDeathUpdate): blending that would desync, and no fingerprint would see it.
//
// It draws the logic transform unchanged, byte for byte, when the phase is exactly 1 (every draw gets a tick, a
// headless peer, GX_INTERP=0) or nothing moved, so those frames are what they always were and an unchanged static
// HLOD keeps W3DModelDraw's Set_Transform skip. It snaps (draws the logic transform and starts over from it) for a
// drawable with no object (client-driven, such as the placement preview), on the first refresh, after any logic
// frame nothing refreshed it on (culled, inside a garrison or transport, shrouded: it must not slide from where it
// was last seen), when the transform changed without a tick (a script teleport in frozen time), for a jump of more
// than 200 units in one tick (a teleport), and for the camera-lock object (the camera moves to its logic position
// on every draw, so an interpolated body would wobble on screen).
//
// The snapshot is refreshed on demand while the client draws, so every consumer of "where is it drawn this frame" -
// draw(), the icon UI, bone positions (laser beams, riders on a container's bone) - gets the same answer whatever
// order the drawables are drawn in. A query made during a logic update never refreshes it (drawnPhase).
//
// The turrets' angle and pitch are kept with it, under the same rules: they are relative to the hull, so a turret
// drawn on a blended hull takes the same phase (getDrawnTurretRotAndPitch).
static void gxReadTurrets(const Object* obj, Real turrets[MAX_TURRETS][2])
{
	const AIUpdateInterface* ai = obj->getAIUpdateInterface();
	for (Int t = 0; t < MAX_TURRETS; ++t)
	{
		turrets[t][0] = turrets[t][1] = 0.0f;
		if (ai != nullptr)
			ai->getTurretRotAndPitch((WhichTurretType)t, &turrets[t][0], &turrets[t][1]);
	}
}

void Drawable::refreshInterpolation() const
{
	const Matrix3D& logic = *getTransformMatrix();
	const UnsignedInt frame = TheGameLogic->getFrame();
	if (frame != m_interpFrame)
	{
		Bool continues = (m_interpFrame != InvalidShroudClearFrame && frame == m_interpFrame + 1);
		if (continues)
		{
			const Real dx = logic.Get_X_Translation() - m_interpCur.Get_X_Translation();
			const Real dy = logic.Get_Y_Translation() - m_interpCur.Get_Y_Translation();
			const Real dz = logic.Get_Z_Translation() - m_interpCur.Get_Z_Translation();
			continues = (dx * dx + dy * dy + dz * dz <= 200.0f * 200.0f);
		}
		m_interpPrev = continues ? m_interpCur : logic;
		m_interpCur = logic;
		m_interpFrame = frame;
		Real turrets[MAX_TURRETS][2];
		gxReadTurrets(getObject(), turrets);
		memcpy(m_interpTurretPrev, continues ? m_interpTurretCur : turrets, sizeof(turrets));
		memcpy(m_interpTurretCur, turrets, sizeof(turrets));
	}
	else if (memcmp(&m_interpCur, &logic, sizeof(Matrix3D)) != 0)
	{
		m_interpPrev = logic;
		m_interpCur = logic;
		memcpy(m_interpTurretPrev, m_interpTurretCur, sizeof(m_interpTurretCur));
	}
}

// The phase this drawable is drawn at this frame, its snapshot refreshed first. 1 draws the logic pose exactly.
Real Drawable::drawnPhase() const
{
	// Asked outside the client's draw - in the middle of a logic update, where LaserUpdate::initLaser reads the muzzle
	// bone of a unit that may just have moved (before m_frame++) - it is the logic pose, always. The snapshot belongs to
	// the next draw, which takes this move as a tick: refreshing it here would count the move as "moved without a tick"
	// and drop the previous pose, and the last draw's phase says nothing about now (interp-r11 P2).
	if (getObject() == nullptr || !TheDisplay->isRenderPhase())
		return 1.0f;
	// Nothing to blend while the phase is pinned to 1 (every draw gets a tick) or GX_INTERP=0: no snapshot either, so those
	// paths cost what they did before interpolation. After the pin releases, a drawable draws
	// its logic pose until two consecutive frames have been snapshotted.
	if (!TheDisplay->isInterpolationActive())
		return 1.0f;
	refreshInterpolation();
	const Real alpha = TheDisplay->getLogicPhase();
	if (alpha == 1.0f)
		return 1.0f;
	if (TheTacticalView != nullptr
		&& (TheTacticalView->getCameraLock() == getObject()->getID() || TheTacticalView->getCameraLockDrawable() == this))
		return 1.0f;
	return alpha;
}

// TRUE when this frame draws a blend, with the phase in alpha. FALSE draws the logic transform exactly.
Bool Drawable::isInterpolating(Real& alpha) const
{
	alpha = drawnPhase();
	return alpha != 1.0f && memcmp(&m_interpPrev, &m_interpCur, sizeof(Matrix3D)) != 0;
}

Coord3D Drawable::getInterpolationOffset() const
{
	Coord3D offset;
	offset.zero();
	Real alpha;
	if (isInterpolating(alpha))
	{
		// lerp(prev, cur, alpha) - cur, the translation Matrix3D::Lerp draws
		const Real k = 1.0f - alpha;
		offset.x = (m_interpPrev.Get_X_Translation() - m_interpCur.Get_X_Translation()) * k;
		offset.y = (m_interpPrev.Get_Y_Translation() - m_interpCur.Get_Y_Translation()) * k;
		offset.z = (m_interpPrev.Get_Z_Translation() - m_interpCur.Get_Z_Translation()) * k;
	}
	return offset;
}

// The base transform this drawable is drawn at this frame: the blend (TRUE), or the logic transform exactly (FALSE).
Bool Drawable::getDrawnBase(Matrix3D& mtx) const
{
	Real alpha;
	if (isInterpolating(alpha))
	{
		Matrix3D::Lerp(m_interpPrev, m_interpCur, alpha, mtx);
		return TRUE;
	}
	mtx = *getTransformMatrix();
	return FALSE;
}

// GeneralsX: A turret's angle and pitch are relative to the hull. On a blended hull the logic
// ones aim it off by the hull's own blend and snap back at every tick, so they take the same phase:
// a turret tracking a target while the hull turns keeps pointing at it, and one turning on a still hull turns smoothly.
// Untouched - the logic pose, exactly - when this frame draws the logic pose or the turret did not move.
void Drawable::getDrawnTurretRotAndPitch(WhichTurretType tur, Real* angle, Real* pitch) const
{
	const Real alpha = drawnPhase();
	const Real* prev = m_interpTurretPrev[tur];
	const Real* cur = m_interpTurretCur[tur];
	if (alpha == 1.0f || (prev[0] == cur[0] && prev[1] == cur[1]))
		return;
	*angle = prev[0] + WWMath::Normalize_Angle(cur[0] - prev[0]) * alpha;
	*pitch = prev[1] + WWMath::Normalize_Angle(cur[1] - prev[1]) * alpha;
}

// GeneralsX: Effects a logic update spawns from a bone - weapon-fire FX at the muzzle - stay
// where they are put, while the body is drawn from its previous logic pose towards this one over the next tick: spawned
// at the logic pose they sit up to a tick ahead of the drawn muzzle. The module carries them
// (W3DModelDraw::gxCarryBone) to the pose the next draw starts from, the previous logic pose, when that draw will blend by refreshInterpolation's and drawnPhase's
// rules: a logic update (not a draw), the phase not pinned, the snapshot at the frame shown last, not the camera-lock
// object, no teleport. Otherwise untouched: today's.
Bool Drawable::getFirstDrawnPose(Matrix3D& pose) const
{
	// Whether the coming draw blends, not whether the last one did: a draw just before a late tick reaches alpha 1 exactly
	// while interpolating, and the next one blends from the previous pose again.
	if (getObject() == nullptr || TheDisplay->isRenderPhase() || !TheDisplay->isInterpolationActive()
		|| m_interpFrame != TheGameLogic->getFrame())
		return FALSE;
	if (TheTacticalView != nullptr
		&& (TheTacticalView->getCameraLock() == getObject()->getID() || TheTacticalView->getCameraLockDrawable() == this))
		return FALSE;
	const Matrix3D& logic = *getTransformMatrix();
	const Real dx = logic.Get_X_Translation() - m_interpCur.Get_X_Translation();
	const Real dy = logic.Get_Y_Translation() - m_interpCur.Get_Y_Translation();
	const Real dz = logic.Get_Z_Translation() - m_interpCur.Get_Z_Translation();
	if (dx * dx + dy * dy + dz * dz > 200.0f * 200.0f)
		return FALSE;
	pose = m_interpCur;
	composeDrawnPose(pose, 1.0f);	// the physics the next draw blends from: the current totals
	return TRUE;
}

// GeneralsX: Render interpolation: a render object still holds the pose it was last placed from
// (its module's last draw, or the logic transform after a move), which is not this frame's while it interpolates. Carry a
// transform read from it - a bone - to the full pose this frame draws (the module carries, W3DModelDraw::gxCarryBone): base, instance matrix and client physics,
// rotation included so a bone far from the pivot of a turning body stays on it (interp-r2). So a laser start,
// a rider on a container's bone, an emitter on an animated bone or a module attached to another module's bone meets the
// body as it is drawn this frame, whatever order they run in. The carry is per render object, from where it actually
// sits: one module can query another's bone before that one has drawn this frame.
// Outside the draw (a logic update) the target is the logic pose. Untouched, today's, while pinned to 1 with the render
// object placed at 1 - not this frame's alpha alone: one that reaches 1 without a tick still finds the render object at
// the last draw's fraction.
Bool Drawable::getCarryPose(Matrix3D& pose, Real placedPhase) const
{
	if (!TheDisplay->isInterpolationActive() && placedPhase == 1.0f)
		return FALSE;
	const Bool drawing = TheDisplay->isRenderPhase();
	if (drawing && showsClientPhysics())
	{
		// The client physics steps once per sync, in whichever of this and draw() comes first, and only where draw() would
		// step it too (it was drawn at the previous sync): render-local state, advanced early, hence the cast.
		const_cast<Drawable*>(this)->advancePhysicsXform(TRUE);
	}
	getDrawnBase(pose);
	composeDrawnPose(pose, drawing ? TheDisplay->getLogicPhase() : 1.0f);
	return TRUE;
}

// mtx comes in as the logic transform and leaves as the one to draw.
void Drawable::interpolateTransform(Matrix3D& mtx)
{
	Real alpha;
	if (isInterpolating(alpha))
		Matrix3D::Lerp(m_interpPrev, m_interpCur, alpha, mtx);
}

//-------------------------------------------------------------------------------------------------
Bool Drawable::showsClientPhysics() const
{
	return TheGlobalData->m_showClientPhysics && getObject() && !getObject()->isDisabledByType( DISABLED_HELD );
}

// GeneralsX: The client physics - bank, pitch, suspension bounce, wobble - is tuned per RETAIL frame:
// every stiffness, damping and coefficient calcPhysicsXform* applies is a per-step value for a 30 Hz step (TSH: "catered to
// a 30 fps logic step"). At GX_LOGIC_60HZ a WW3D sync is half a retail frame, so stepping once per sync rang every spring
// twice as fast and fed it velocities per 60 Hz frame (half) and accelerations per 60 Hz frame^2 (a quarter): a jet in a
// turn banked about half as far as at 30 Hz (wh1ter0se 04/10, King Raptor and MiG "rotation"; Generals Online does the same).
// So the client physics steps once per retail frame, reads velocity and acceleration per retail frame, and the draw
// interpolates across the whole retail frame (gxRetailPhysicsPhase). Picture only: calcPhysicsXform is outside the
// simulation (resources/gitinfo/simdigest_exclusions.txt). At 30 Hz a retail frame is one logic frame: unchanged.
static const UnsignedInt GX_RETAIL_FRAME = (UnsignedInt)GX_FRAMES30(1);

static Coord3D gxPerRetailFrame(const Coord3D *perLogicFrame, Real scale)
{
	Coord3D v = *perLogicFrame;
	v.scale(scale);
	return v;
}
static Coord3D gxVelocityPerRetailFrame(const PhysicsBehavior *physics)
{
	return gxPerRetailFrame(physics->getVelocity(), (Real)GX_RETAIL_FRAME);
}
static Coord3D gxAccelerationPerRetailFrame(const PhysicsBehavior *physics)
{
	return gxPerRetailFrame(physics->getAcceleration(), (Real)(GX_RETAIL_FRAME * GX_RETAIL_FRAME));
}

#if GX_LOGIC_60HZ
// TRUE when a retail frame began between the previous WW3D sync and this one: the step every drawable that was advanced at
// the previous sync takes now. Kept per sync, not per drawable: a drawable that was advanced at the previous sync saw the
// same previous sync, so one record of the logic frame at each sync answers for all of them.
static Bool gxRetailFrameBegan()
{
	static UnsignedInt s_syncTime = ~0u;
	static UnsignedInt s_frame = 0;
	static UnsignedInt s_prevFrame = 0;
	if (s_syncTime != WW3D::Get_Sync_Time())
	{
		s_syncTime = WW3D::Get_Sync_Time();
		s_prevFrame = s_frame;
		s_frame = TheGameLogic->getFrame();
	}
	return s_frame / GX_RETAIL_FRAME != s_prevFrame / GX_RETAIL_FRAME;
}
#endif

// The draw's phase across the retail frame the last step began: prev -> total spans GX_RETAIL_FRAME logic frames.
static Real gxRetailPhysicsPhase(Real logicPhase)
{
#if GX_LOGIC_60HZ
	return ((Real)(TheGameLogic->getFrame() % GX_RETAIL_FRAME) + logicPhase) / (Real)GX_RETAIL_FRAME;
#else
	return logicPhase;
#endif
}

// GeneralsX: Split out of applyPhysicsXform, and stepped at most once per sync: a bone query
// before this frame's draw composes the pose the draw will (getCarryPose), and must not step it a second time.
// continuingOnly: step only a drawable that was stepped at the previous sync, as the draw's first branch does.
void Drawable::advancePhysicsXform(Bool continuingOnly)
{
	if (m_physicsXform != nullptr)
	{
		// TheSuperHackers @tweak Advance physics only on WW Sync frames.
		// All calculations are originally catered to a 30 fps logic step.
		if (WW3D::Get_Sync_Frame_Time() != 0 && m_physicsXform->m_syncTime != WW3D::Get_Sync_Time()
			&& (!continuingOnly || m_physicsXform->m_syncTime == WW3D::Get_Previous_Sync_Time()))
		{
			const Bool continuing = (m_physicsXform->m_syncTime == WW3D::Get_Previous_Sync_Time());
#if GX_LOGIC_60HZ
			// gxRetailFrameBegan() first, always evaluated: it records this sync for every drawable advanced at it, new ones
			// included, and a drawable that continues next sync relies on that record.
			const Bool step = gxRetailFrameBegan() || !continuing;
#else
			const Bool step = TRUE;
#endif
			if (step)
			{
				m_physicsXform->setPrevTotals();
				calcPhysicsXform(*m_physicsXform);

				// New or previously undrawn objects have no result from the previous logic step.
				if (!continuing)
				{
					m_physicsXform->setPrevTotals();
				}
			}
			m_physicsXform->m_syncTime = WW3D::Get_Sync_Time();
		}
	}
}

// GeneralsX: mtx comes in as the base (the logic transform, or its blend) and leaves as the pose
// the draw modules place this drawable at: the instance matrix and the client physics at physicsPhase on top. One
// composition for draw() and for a bone query made before it (getCarryPose), so the two agree to the bit.
void Drawable::composeDrawnPose(Matrix3D& mtx, Real physicsPhase) const
{
	if (!isInstanceIdentity())
	{
#ifdef ALLOW_TEMPORARIES
		mtx = mtx * (*getInstanceMatrix());
#else
		mtx.postMul(*getInstanceMatrix());
#endif
	}

	if (showsClientPhysics())
	{
		applyPhysicsXform(&mtx, physicsPhase);
	}
}

void Drawable::applyPhysicsXform(Matrix3D* mtx, Real t) const
{
	if (m_physicsXform != nullptr)
	{
		// TheSuperHackers @tweak bobtista 14/09/2026 Interpolate the rendered transform between
		// logic frames, so the motion stays smooth when the render rate is above the logic rate.
		// Upstream reference: bobtista, PR #2055 (open at 713b92525)
		// https://github.com/TheSuperHackers/GeneralsGameCode/pull/2055
		// GeneralsX: Adapted: the phase is the client-side Display::getLogicPhase (GXLogicPhase.h),
		// not FramePacer::getLogicFramePhase, which lives in digested code. At exactly 1 - every draw gets a tick, a
		// headless peer, GX_INTERP=0 - the totals are applied as they are: WWMath::Lerp is not bit-exact at t=1, and
		// those frames must stay exactly what they were.
		const PhysicsXformInfo& x = *m_physicsXform;
		t = gxRetailPhysicsPhase(t);
		const Bool lerp = (t != 1.0f);

		mtx->Translate(0.0f, 0.0f, lerp ? WWMath::Lerp(x.m_prevTotalZ, x.m_totalZ, t) : x.m_totalZ);
		mtx->Rotate_Y( lerp ? WWMath::Lerp(x.m_prevTotalPitch, x.m_totalPitch, t) : x.m_totalPitch );
		mtx->Rotate_X( -(lerp ? WWMath::Lerp(x.m_prevTotalRoll, x.m_totalRoll, t) : x.m_totalRoll) );
		mtx->Rotate_Z( lerp ? WWMath::Lerp(x.m_prevTotalYaw, x.m_totalYaw, t) : x.m_totalYaw );
	}
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
Bool Drawable::calcPhysicsXform(PhysicsXformInfo& info)
{
	Bool hasPhysicsXform = false;

	if (const Locomotor *locomotor = getLocomotor())
	{
		switch (locomotor->getAppearance())
		{
			case LOCO_WHEELS_FOUR:
				calcPhysicsXformWheels(locomotor, info);
				hasPhysicsXform = true;
				break;
			case LOCO_TREADS:
				calcPhysicsXformTreads(locomotor, info);
				hasPhysicsXform = true;
				break;
			case LOCO_HOVER:
			case LOCO_WINGS:
				calcPhysicsXformHoverOrWings(locomotor, info);
				hasPhysicsXform = true;
				break;
			case LOCO_THRUST:
				calcPhysicsXformThrust(locomotor, info);
				hasPhysicsXform = true;
				break;
		}
	}

	if (hasPhysicsXform)
	{
     // HOTFIX: Ensure that we are not passing denormalized values back to caller
     // @todo remove hotfix
     if (info.m_totalPitch>-1e-20f&&info.m_totalPitch<1e-20f)
       info.m_totalPitch=0.f;
     if (info.m_totalRoll>-1e-20f&&info.m_totalRoll<1e-20f)
       info.m_totalRoll=0.f;
     if (info.m_totalYaw>-1e-20f&&info.m_totalYaw<1e-20f)
       info.m_totalYaw=0.f;
     if (info.m_totalZ>-1e-20f&&info.m_totalZ<1e-20f)
       info.m_totalZ=0.f;
	}

	return hasPhysicsXform;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void Drawable::calcPhysicsXformThrust( const Locomotor *locomotor, PhysicsXformInfo& info )
{
	if (m_locoInfo == nullptr)
		m_locoInfo = newInstance(DrawableLocoInfo);

	Real THRUST_ROLL = locomotor->getThrustRoll();
	Real WOBBLE_RATE = locomotor->getWobbleRate();
	Real MAX_WOBBLE  = locomotor->getMaxWobble();
	Real MIN_WOBBLE  = locomotor->getMinWobble();

	//
	// this is a kind of quick thrust implementation cause we need scud missiles to wobble *now*,
	// we deal with just adjusting pitch, yaw, and roll just a little bit
	//

	if( WOBBLE_RATE )
	{

		if( m_locoInfo->m_wobble >= 1.0f )
		{

			if( m_locoInfo->m_pitch < MAX_WOBBLE - WOBBLE_RATE * 2 )
			{

				m_locoInfo->m_pitch += WOBBLE_RATE;
				m_locoInfo->m_yaw += WOBBLE_RATE;

			}
			else
			{

				m_locoInfo->m_pitch += (WOBBLE_RATE / 2.0f);
				m_locoInfo->m_yaw += (WOBBLE_RATE / 2.0f);

			}

			if( m_locoInfo->m_pitch >= MAX_WOBBLE )
				m_locoInfo->m_wobble = -1.0f;

		}
		else
		{

			if( m_locoInfo->m_pitch >= MIN_WOBBLE + WOBBLE_RATE * 2.0f )
			{

				m_locoInfo->m_pitch -= WOBBLE_RATE;
				m_locoInfo->m_yaw -= WOBBLE_RATE;

			}
			else
			{

				m_locoInfo->m_pitch -= (WOBBLE_RATE / 2.0f);
				m_locoInfo->m_yaw -= (WOBBLE_RATE / 2.0f);

			}
			if( m_locoInfo->m_pitch <= MIN_WOBBLE )
				m_locoInfo->m_wobble = 1.0f;

		}

		info.m_totalPitch = m_locoInfo->m_pitch;
		info.m_totalYaw = m_locoInfo->m_yaw;

	}

	if( THRUST_ROLL )
	{

		m_locoInfo->m_roll += THRUST_ROLL;
		info.m_totalRoll = m_locoInfo->m_roll;

	}

}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void Drawable::calcPhysicsXformHoverOrWings( const Locomotor *locomotor, PhysicsXformInfo& info )
{
	if (m_locoInfo == nullptr)
		m_locoInfo = newInstance(DrawableLocoInfo);

	const Real ACCEL_PITCH_LIMIT = locomotor->getAccelPitchLimit();
	const Real PITCH_STIFFNESS = locomotor->getPitchStiffness();
	const Real ROLL_STIFFNESS =  locomotor->getRollStiffness();
	const Real PITCH_DAMPING = locomotor->getPitchDamping();
	const Real ROLL_DAMPING = locomotor->getRollDamping();
	const Real Z_VEL_PITCH_COEFF = locomotor->getPitchByZVelCoef();
	const Real FORWARD_VEL_COEFF = locomotor->getForwardVelCoef();
	const Real LATERAL_VEL_COEFF = locomotor->getLateralVelCoef();
	const Real FORWARD_ACCEL_COEFF = locomotor->getForwardAccelCoef();
	const Real LATERAL_ACCEL_COEFF = locomotor->getLateralAccelCoef();
	const Real UNIFORM_AXIAL_DAMPING = locomotor->getUniformAxialDamping();

	// get object from logic
	Object *obj = getObject();
	if (obj == nullptr)
		return;

	AIUpdateInterface *ai = obj->getAIUpdateInterface();
	if (ai == nullptr)
		return;

	// get object physics state
	PhysicsBehavior *physics = obj->getPhysics();
	if (physics == nullptr)
		return;

	// get our position and direction vector
//const Coord3D *pos = getPosition();
	const Coord3D* dir = getUnitDirectionVector2D();
	const Coord3D accelPerRetailFrame = gxAccelerationPerRetailFrame(physics);
	const Coord3D velPerRetailFrame = gxVelocityPerRetailFrame(physics);
	const Coord3D* accel = &accelPerRetailFrame;
	const Coord3D* vel = &velPerRetailFrame;

	m_locoInfo->m_pitchRate += ((-PITCH_STIFFNESS * m_locoInfo->m_pitch) + (-PITCH_DAMPING * m_locoInfo->m_pitchRate));		// spring/damper
	m_locoInfo->m_rollRate += ((-ROLL_STIFFNESS * m_locoInfo->m_roll) + (-ROLL_DAMPING * m_locoInfo->m_rollRate));		// spring/damper

	m_locoInfo->m_pitch += m_locoInfo->m_pitchRate * UNIFORM_AXIAL_DAMPING;
	m_locoInfo->m_roll += m_locoInfo->m_rollRate   * UNIFORM_AXIAL_DAMPING;

	// process chassis acceleration dynamics - damp back towards zero

	m_locoInfo->m_accelerationPitchRate += ((-PITCH_STIFFNESS * (m_locoInfo->m_accelerationPitch)) + (-PITCH_DAMPING * m_locoInfo->m_accelerationPitchRate));		// spring/damper
	m_locoInfo->m_accelerationPitch += m_locoInfo->m_accelerationPitchRate;

	m_locoInfo->m_accelerationRollRate += ((-ROLL_STIFFNESS * m_locoInfo->m_accelerationRoll) + (-ROLL_DAMPING * m_locoInfo->m_accelerationRollRate));		// spring/damper
	m_locoInfo->m_accelerationRoll += m_locoInfo->m_accelerationRollRate;

	// compute total pitch and roll of tank
	info.m_totalPitch = m_locoInfo->m_pitch + m_locoInfo->m_accelerationPitch;
	info.m_totalRoll = m_locoInfo->m_roll + m_locoInfo->m_accelerationRoll;

	if (physics->isMotive())
  {
		if (Z_VEL_PITCH_COEFF != 0.0f)
		{
			const Real TINY_DZ = 0.001f;
			if (fabs(vel->z) > TINY_DZ)
			{
				Real pitch = atan2(vel->z, sqrt(sqr(vel->x)+sqr(vel->y)));
				m_locoInfo->m_pitch -= Z_VEL_PITCH_COEFF * pitch;
			}
		}

		// cause the chassis to pitch & roll in reaction to current speed
		Real forwardVel = dir->x * vel->x + dir->y * vel->y;
		m_locoInfo->m_pitch += -(FORWARD_VEL_COEFF * forwardVel);

		Real lateralVel = -dir->y * vel->x + dir->x * vel->y;
		m_locoInfo->m_roll += -(LATERAL_VEL_COEFF * lateralVel);

		// cause the chassis to pitch & roll in reaction to acceleration/deceleration
		Real forwardAccel = dir->x * accel->x + dir->y * accel->y;
		m_locoInfo->m_accelerationPitchRate += -(FORWARD_ACCEL_COEFF * forwardAccel);

		Real lateralAccel = -dir->y * accel->x + dir->x * accel->y;
		m_locoInfo->m_accelerationRollRate += -(LATERAL_ACCEL_COEFF * lateralAccel);
	}

	// limit acceleration pitch and roll

	if (m_locoInfo->m_accelerationPitch > ACCEL_PITCH_LIMIT)
		m_locoInfo->m_accelerationPitch = ACCEL_PITCH_LIMIT;
	else if (m_locoInfo->m_accelerationPitch < -ACCEL_PITCH_LIMIT)
		m_locoInfo->m_accelerationPitch = -ACCEL_PITCH_LIMIT;

	if (m_locoInfo->m_accelerationRoll > ACCEL_PITCH_LIMIT)
		m_locoInfo->m_accelerationRoll = ACCEL_PITCH_LIMIT;
	else if (m_locoInfo->m_accelerationRoll < -ACCEL_PITCH_LIMIT)
		m_locoInfo->m_accelerationRoll = -ACCEL_PITCH_LIMIT;

	info.m_totalZ = 0.0f;
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void Drawable::calcPhysicsXformTreads( const Locomotor *locomotor, PhysicsXformInfo& info )
{
	if (m_locoInfo == nullptr)
		m_locoInfo = newInstance(DrawableLocoInfo);

	const Real OVERLAP_SHRINK_FACTOR = 0.8f;
	const Real FLATTENED_OBJECT_HEIGHT = 0.5f;
	const Real LEAVE_OVERLAP_PITCH_KICK = PI/128;
	const Real OVERLAP_ROUGH_VIBRATION_FACTOR = 5.0f;
	const Real MAX_ROUGH_VIBRATION = 0.5f;
	const Real ACCEL_PITCH_LIMIT = locomotor->getAccelPitchLimit();
	const Real PITCH_STIFFNESS = locomotor->getPitchStiffness();
	const Real ROLL_STIFFNESS =  locomotor->getRollStiffness();
	const Real PITCH_DAMPING = locomotor->getPitchDamping();
	const Real ROLL_DAMPING = locomotor->getRollDamping();
	const Real FORWARD_ACCEL_COEFF = locomotor->getForwardAccelCoef();
	const Real LATERAL_ACCEL_COEFF = locomotor->getLateralAccelCoef();
	const Real UNIFORM_AXIAL_DAMPING = locomotor->getUniformAxialDamping();

	// get object from logic
	Object *obj = getObject();
	if (obj == nullptr)
		return;

	AIUpdateInterface *ai = obj->getAIUpdateInterface();
	if (ai == nullptr)
		return ;

	// get object physics state
	PhysicsBehavior *physics = obj->getPhysics();
	if (physics == nullptr)
		return;

	// get our position and direction vector
	const Coord3D *pos = getPosition();
	const Coord3D *dir = getUnitDirectionVector2D();
	const Coord3D accelPerRetailFrame = gxAccelerationPerRetailFrame(physics);
	const Coord3D velPerRetailFrame = gxVelocityPerRetailFrame(physics);
	const Coord3D *accel = &accelPerRetailFrame;
	const Coord3D *vel = &velPerRetailFrame;

	// compute perpendicular (2d)
	Coord3D perp;
	perp.x = -dir->y;
	perp.y = dir->x;
	perp.z = 0.0f;

	// find pitch and roll of terrain under chassis
	Coord3D normal;
/*	Real hheight = */ TheTerrainLogic->getLayerHeight( pos->x, pos->y, obj->getLayer(), &normal );

	// override surface normal if we are overlapping another object - crushing it
	Real overlapZ = 0.0f;

	// get object we are currently overlapping, if any
	Object* overlapped = TheGameLogic->findObjectByID(physics->getCurrentOverlap());
	if (overlapped && overlapped->isKindOf(KINDOF_SHRUBBERY)) {
		overlapped = nullptr; // We just smash through shrubbery.  jba.
	}

	if (overlapped)
	{
		const Coord3D *overPos = overlapped->getPosition();
		Real dx = overPos->x - pos->x;
		Real dy = overPos->y - pos->y;
		Real centerDistSqr = sqr(dx) + sqr(dy);

		// compute maximum distance between objects, if their edges just touched
		Real ourSize = getDrawableGeometryInfo().getBoundingCircleRadius();
		Real otherSize = overlapped->getGeometryInfo().getBoundingCircleRadius();
		Real maxCenterDist = otherSize + ourSize;

		// shrink the overlap distance a bit to avoid floating
		maxCenterDist *= OVERLAP_SHRINK_FACTOR;
		if (centerDistSqr < sqr(maxCenterDist))
		{
			Real centerDist = sqrtf(centerDistSqr);
			Real amount = 1.0f - centerDist/maxCenterDist;
			if (amount < 0.0f)
				amount = 0.0f;
			else if (amount > 1.0f)
				amount = 1.0f;

			// rough vibrations proportional to speed when we drive over something
			Real rough = (vel->x*vel->x + vel->y*vel->y) * OVERLAP_ROUGH_VIBRATION_FACTOR;
			if (rough > MAX_ROUGH_VIBRATION)
				rough = MAX_ROUGH_VIBRATION;

			Real height = overlapped->getGeometryInfo().getMaxHeightAbovePosition();

			// do not "go up" flattened crushed things
			Bool flat = false;
			if (overlapped->isKindOf(KINDOF_LOW_OVERLAPPABLE) ||
					overlapped->isKindOf(KINDOF_INFANTRY) ||
					(overlapped->getBodyModule()->getFrontCrushed() && overlapped->getBodyModule()->getBackCrushed()))
			{
				flat = true;
				height = FLATTENED_OBJECT_HEIGHT;
			}

			if (amount < FLATTENED_OBJECT_HEIGHT && flat == false)
			{
				overlapZ = height * 2.0f * amount;

				// compute vector along "surface"
				// not proportional to actual geometry to avoid overlay steep inclines, etc
				Coord3D v;
				v.x = dx/centerDist;
				v.y = dy/centerDist;
				v.z = 0.2f;		// 0.25

				Coord3D up;
				up.x = GameClientRandomValueReal( -rough, rough );
				up.y = GameClientRandomValueReal( -rough, rough );
				up.z = 1.0f;
				up.normalize();

				Coord3D prp;
				prp.crossProduct( v, up, prp );
				normal.crossProduct( prp, v, normal );

				// compute unit normal
				normal.normalize();
			}
			else
			{
				// sitting on top of object
				overlapZ = height;

				normal.x = GameClientRandomValueReal( -rough, rough );
				normal.y = GameClientRandomValueReal( -rough, rough );
				normal.z = 1.0f;
				normal.normalize();
			}
		}
	}
	else	// no overlap this frame
	{
		// if we had an overlap last frame, and we're now in the air, give a
		// kick to the pitch for effect
		if (physics->getPreviousOverlap() != INVALID_ID && m_locoInfo->m_overlapZ > 0.0f)
			m_locoInfo->m_pitchRate += LEAVE_OVERLAP_PITCH_KICK;
	}



	Real dot = normal.x * dir->x + normal.y * dir->y;
	Real groundPitch = dot * (PI/2.0f);

	dot = normal.x * perp.x + normal.y * perp.y;
	Real groundRoll = dot * (PI/2.0f);

	// process chassis suspension dynamics - damp back towards groundPitch

	// the ground can only push back if we're touching it
	if (overlapped || m_locoInfo->m_overlapZ <= 0.0f)
	{
		m_locoInfo->m_pitchRate += ((-PITCH_STIFFNESS * (m_locoInfo->m_pitch - groundPitch)) + (-PITCH_DAMPING * m_locoInfo->m_pitchRate));		// spring/damper
		if (m_locoInfo->m_pitchRate > 0.0f)
			m_locoInfo->m_pitchRate *= 0.5f;

		m_locoInfo->m_rollRate += ((-ROLL_STIFFNESS * (m_locoInfo->m_roll - groundRoll)) + (-ROLL_DAMPING * m_locoInfo->m_rollRate));		// spring/damper
	}

	m_locoInfo->m_pitch += m_locoInfo->m_pitchRate * UNIFORM_AXIAL_DAMPING;
	m_locoInfo->m_roll += m_locoInfo->m_rollRate   * UNIFORM_AXIAL_DAMPING;

	// process chassis recoil dynamics - damp back towards zero

	m_locoInfo->m_accelerationPitchRate += ((-PITCH_STIFFNESS * (m_locoInfo->m_accelerationPitch)) + (-PITCH_DAMPING * m_locoInfo->m_accelerationPitchRate));		// spring/damper
	m_locoInfo->m_accelerationPitch += m_locoInfo->m_accelerationPitchRate;

	m_locoInfo->m_accelerationRollRate += ((-ROLL_STIFFNESS * m_locoInfo->m_accelerationRoll) + (-ROLL_DAMPING * m_locoInfo->m_accelerationRollRate));		// spring/damper
	m_locoInfo->m_accelerationRoll += m_locoInfo->m_accelerationRollRate;

	// compute total pitch and roll of tank
	info.m_totalPitch = m_locoInfo->m_pitch + m_locoInfo->m_accelerationPitch;
	info.m_totalRoll = m_locoInfo->m_roll + m_locoInfo->m_accelerationRoll;

	if (physics->isMotive())
	{
		// cause the chassis to pitch & roll in reaction to acceleration/deceleration
		Real forwardAccel = dir->x * accel->x + dir->y * accel->y;
		m_locoInfo->m_accelerationPitchRate += -(FORWARD_ACCEL_COEFF * forwardAccel);

		Real lateralAccel = -dir->y * accel->x + dir->x * accel->y;
		m_locoInfo->m_accelerationRollRate += -(LATERAL_ACCEL_COEFF * lateralAccel);
	}

	// limit recoil pitch and roll

	if (m_locoInfo->m_accelerationPitch > ACCEL_PITCH_LIMIT)
		m_locoInfo->m_accelerationPitch = ACCEL_PITCH_LIMIT;
	else if (m_locoInfo->m_accelerationPitch < -ACCEL_PITCH_LIMIT)
		m_locoInfo->m_accelerationPitch = -ACCEL_PITCH_LIMIT;

	if (m_locoInfo->m_accelerationRoll > ACCEL_PITCH_LIMIT)
		m_locoInfo->m_accelerationRoll = ACCEL_PITCH_LIMIT;
	else if (m_locoInfo->m_accelerationRoll < -ACCEL_PITCH_LIMIT)
		m_locoInfo->m_accelerationRoll = -ACCEL_PITCH_LIMIT;

	// adjust z
	if (overlapZ > m_locoInfo->m_overlapZ)
	{
		m_locoInfo->m_overlapZ = overlapZ;
		/// @todo Z needs to accelerate/decelerate, not be directly set (MSB)
		// m_locoInfo->m_overlapZ += 0.4f;
		m_locoInfo->m_overlapZVel = 0.0f;
	}

	Real ztmp = m_locoInfo->m_overlapZ/2.0f;

	// do fake Z physics
	if (m_locoInfo->m_overlapZ > 0.0f)
	{
		m_locoInfo->m_overlapZVel -= 0.2f;
		m_locoInfo->m_overlapZ += m_locoInfo->m_overlapZVel;
	}

	if (m_locoInfo->m_overlapZ <= 0.0f)
	{
		m_locoInfo->m_overlapZ = 0.0f;
		m_locoInfo->m_overlapZVel = 0.0f;
	}
	info.m_totalZ = ztmp;
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void Drawable::calcPhysicsXformWheels( const Locomotor *locomotor, PhysicsXformInfo& info )
{
	if (m_locoInfo == nullptr)
		m_locoInfo = newInstance(DrawableLocoInfo);

	const Real ACCEL_PITCH_LIMIT = locomotor->getAccelPitchLimit();
	const Real BOUNCE_ANGLE_KICK = locomotor->getBounceKick();
	const Real PITCH_STIFFNESS = locomotor->getPitchStiffness();
	const Real ROLL_STIFFNESS =  locomotor->getRollStiffness();
	const Real PITCH_DAMPING = locomotor->getPitchDamping();
	const Real ROLL_DAMPING = locomotor->getRollDamping();
	const Real FORWARD_ACCEL_COEFF = locomotor->getForwardAccelCoef();
	const Real LATERAL_ACCEL_COEFF = locomotor->getLateralAccelCoef();
	const Real UNIFORM_AXIAL_DAMPING = locomotor->getUniformAxialDamping();

	const Real MAX_SUSPENSION_EXTENSION = locomotor->getMaxWheelExtension(); //-2.3f;
//	const Real MAX_SUSPENSION_COMPRESSION = locomotor->getMaxWheelCompression(); //1.4f;
	const Real WHEEL_ANGLE = locomotor->getWheelTurnAngle(); //PI/8;

	const Bool DO_WHEELS = locomotor->hasSuspension();

	// get object from logic
	Object *obj = getObject();
	if (obj == nullptr)
		return;

	AIUpdateInterface *ai = obj->getAIUpdateInterface();
	if (ai == nullptr)
		return ;

	// get object physics state
	PhysicsBehavior *physics = obj->getPhysics();
	if (physics == nullptr)
		return ;

	// get our position and direction vector
	const Coord3D *pos = getPosition();
	const Coord3D *dir = getUnitDirectionVector2D();
	const Coord3D accelPerRetailFrame = gxAccelerationPerRetailFrame(physics);
	const Coord3D *accel = &accelPerRetailFrame;

	// compute perpendicular (2d)
	Coord3D perp;
	perp.x = -dir->y;
	perp.y = dir->x;
	perp.z = 0.0f;

	// find pitch and roll of terrain under chassis
	Coord3D normal;
	Real hheight = TheTerrainLogic->getLayerHeight( pos->x, pos->y, obj->getLayer(), &normal );

	Real dot = normal.x * dir->x + normal.y * dir->y;
	Real groundPitch = dot * (PI/2.0f);

	dot = normal.x * perp.x + normal.y * perp.y;
	Real groundRoll = dot * (PI/2.0f);

	Bool airborne = obj->isSignificantlyAboveTerrain();

	if (airborne)
	{
		if (DO_WHEELS)
		{
			// Wheels extend when airborne.
			m_locoInfo->m_wheelInfo.m_framesAirborne = 0;
			m_locoInfo->m_wheelInfo.m_framesAirborneCounter++;
			if (pos->z - hheight > -MAX_SUSPENSION_EXTENSION)
			{
				m_locoInfo->m_wheelInfo.m_rearLeftHeightOffset += (MAX_SUSPENSION_EXTENSION - m_locoInfo->m_wheelInfo.m_rearLeftHeightOffset)/2.0f;
				m_locoInfo->m_wheelInfo.m_rearRightHeightOffset += (MAX_SUSPENSION_EXTENSION - m_locoInfo->m_wheelInfo.m_rearRightHeightOffset)/2.0f;
			}
			else
			{
				m_locoInfo->m_wheelInfo.m_rearLeftHeightOffset += (0 - m_locoInfo->m_wheelInfo.m_rearLeftHeightOffset)/2.0f;
				m_locoInfo->m_wheelInfo.m_rearRightHeightOffset += (0 - m_locoInfo->m_wheelInfo.m_rearRightHeightOffset)/2.0f;
			}
		}
		// Calculate suspension info.
		Real length = obj->getGeometryInfo().getMajorRadius();
		Real width = obj->getGeometryInfo().getMinorRadius();
		Real pitchHeight = length*Sin(m_locoInfo->m_pitch + m_locoInfo->m_accelerationPitch - groundPitch);
		Real rollHeight = width*Sin(m_locoInfo->m_roll + m_locoInfo->m_accelerationRoll - groundRoll);
		info.m_totalZ = fabs(pitchHeight)/4 + fabs(rollHeight)/4;
		return; // maintain the same orientation while we fly through the air.
	}

	// Bouncy.
	Real curSpeed = physics->getVelocityMagnitude();
#if 1
	Real maxSpeed = ai->getCurLocomotorSpeed();
	if (!airborne && curSpeed > maxSpeed/10)
	{
		Real factor = curSpeed/maxSpeed;
		if (fabs(m_locoInfo->m_pitchRate)<factor*BOUNCE_ANGLE_KICK/4 && fabs(m_locoInfo->m_rollRate)<factor*BOUNCE_ANGLE_KICK/8)
		{
			// do the bouncy.
			switch (GameClientRandomValue(0,3))
			{
			case 0:
				m_locoInfo->m_pitchRate -= BOUNCE_ANGLE_KICK*factor;
				m_locoInfo->m_rollRate -= BOUNCE_ANGLE_KICK*factor/2;
				break;
			case 1:
				m_locoInfo->m_pitchRate += BOUNCE_ANGLE_KICK*factor;
				m_locoInfo->m_rollRate -= BOUNCE_ANGLE_KICK*factor/2;
				break;
			case 2:
				m_locoInfo->m_pitchRate -= BOUNCE_ANGLE_KICK*factor;
				m_locoInfo->m_rollRate += BOUNCE_ANGLE_KICK*factor/2;
				break;
			case 3:
				m_locoInfo->m_pitchRate += BOUNCE_ANGLE_KICK*factor;
				m_locoInfo->m_rollRate += BOUNCE_ANGLE_KICK*factor/2;
				break;
			}
		}
	}

#endif

	// process chassis suspension dynamics - damp back towards groundPitch

	// the ground can only push back if we're touching it
	if (!airborne)
	{
		m_locoInfo->m_pitchRate += ((-PITCH_STIFFNESS * (m_locoInfo->m_pitch - groundPitch)) + (-PITCH_DAMPING * m_locoInfo->m_pitchRate));		// spring/damper
		if (m_locoInfo->m_pitchRate > 0.0f)
			m_locoInfo->m_pitchRate *= 0.5f;

		m_locoInfo->m_rollRate += ((-ROLL_STIFFNESS * (m_locoInfo->m_roll - groundRoll)) + (-ROLL_DAMPING * m_locoInfo->m_rollRate));		// spring/damper
	}

	m_locoInfo->m_pitch += m_locoInfo->m_pitchRate * UNIFORM_AXIAL_DAMPING;
	m_locoInfo->m_roll += m_locoInfo->m_rollRate   * UNIFORM_AXIAL_DAMPING;

	// process chassis acceleration dynamics - damp back towards zero

	m_locoInfo->m_accelerationPitchRate += ((-PITCH_STIFFNESS * (m_locoInfo->m_accelerationPitch)) + (-PITCH_DAMPING * m_locoInfo->m_accelerationPitchRate));		// spring/damper
	m_locoInfo->m_accelerationPitch += m_locoInfo->m_accelerationPitchRate;

	m_locoInfo->m_accelerationRollRate += ((-ROLL_STIFFNESS * m_locoInfo->m_accelerationRoll) + (-ROLL_DAMPING * m_locoInfo->m_accelerationRollRate));		// spring/damper
	m_locoInfo->m_accelerationRoll += m_locoInfo->m_accelerationRollRate;

	// compute total pitch and roll of tank
	info.m_totalPitch = m_locoInfo->m_pitch + m_locoInfo->m_accelerationPitch;
	info.m_totalRoll = m_locoInfo->m_roll + m_locoInfo->m_accelerationRoll;

	if (physics->isMotive())
	{
		// cause the chassis to pitch & roll in reaction to acceleration/deceleration
		Real forwardAccel = dir->x * accel->x + dir->y * accel->y;
		m_locoInfo->m_accelerationPitchRate += -(FORWARD_ACCEL_COEFF * forwardAccel);

		Real lateralAccel = -dir->y * accel->x + dir->x * accel->y;
		m_locoInfo->m_accelerationRollRate += -(LATERAL_ACCEL_COEFF * lateralAccel);
	}

	// limit acceleration pitch and roll

	if (m_locoInfo->m_accelerationPitch > ACCEL_PITCH_LIMIT)
		m_locoInfo->m_accelerationPitch = ACCEL_PITCH_LIMIT;
	else if (m_locoInfo->m_accelerationPitch < -ACCEL_PITCH_LIMIT)
		m_locoInfo->m_accelerationPitch = -ACCEL_PITCH_LIMIT;

	if (m_locoInfo->m_accelerationRoll > ACCEL_PITCH_LIMIT)
		m_locoInfo->m_accelerationRoll = ACCEL_PITCH_LIMIT;
	else if (m_locoInfo->m_accelerationRoll < -ACCEL_PITCH_LIMIT)
		m_locoInfo->m_accelerationRoll = -ACCEL_PITCH_LIMIT;

	info.m_totalZ = 0;

	// Calculate suspension info.
	Real length = obj->getGeometryInfo().getMajorRadius();
	Real width = obj->getGeometryInfo().getMinorRadius();
	Real pitchHeight = length*Sin(info.m_totalPitch-groundPitch);
	Real rollHeight = width*Sin(info.m_totalRoll-groundRoll);
	if (DO_WHEELS)
	{
		// calculate each wheel position
		m_locoInfo->m_wheelInfo.m_framesAirborne = m_locoInfo->m_wheelInfo.m_framesAirborneCounter;
		m_locoInfo->m_wheelInfo.m_framesAirborneCounter = 0;
		TWheelInfo newInfo = m_locoInfo->m_wheelInfo;
		PhysicsTurningType rotation = physics->getTurning();
		if (rotation == TURN_NEGATIVE) {
			newInfo.m_wheelAngle = -WHEEL_ANGLE;
		} else if (rotation == TURN_POSITIVE) {
			newInfo.m_wheelAngle = WHEEL_ANGLE;
		}	else {
			newInfo.m_wheelAngle = 0;
		}
		if (physics->getForwardSpeed2D() < 0.0f) {
			// if we're moving backwards, the wheels rotate in the opposite direction.
			newInfo.m_wheelAngle = -newInfo.m_wheelAngle;
		}

		//
		///@todo Steven/John ... please review this and make sure it makes sense (CBD)
		// we're going to add the angle to the current wheel rotation ... but we're going to
		// divide that number to add small angles.  This allows for "smoother" wheel turning
		// transitions ... and when the AI has things move in a straight line, since it's
		// constantly telling the object to go left, go straight, go right, go straight,
		// etc, this smaller angle we'll be adding covers the constant wheel shifting
		// left and right when moving in a relatively straight line
		//
		#define WHEEL_SMOOTHNESS 10.0f  // higher numbers add smaller angles, make it more "smooth"
		m_locoInfo->m_wheelInfo.m_wheelAngle += (newInfo.m_wheelAngle - m_locoInfo->m_wheelInfo.m_wheelAngle)/WHEEL_SMOOTHNESS;

		const Real SPRING_FACTOR = 0.9f;
		if (pitchHeight<0) {	// Front raising up
			newInfo.m_frontLeftHeightOffset = SPRING_FACTOR*(pitchHeight/3+pitchHeight/2);
			newInfo.m_frontRightHeightOffset = SPRING_FACTOR*(pitchHeight/3+pitchHeight/2);
			newInfo.m_rearLeftHeightOffset = -pitchHeight/2 + pitchHeight/4;
			newInfo.m_rearRightHeightOffset = -pitchHeight/2 + pitchHeight/4;
		}	else {	// Back rasing up.
			newInfo.m_frontLeftHeightOffset = (-pitchHeight/4+pitchHeight/2);
			newInfo.m_frontRightHeightOffset = (-pitchHeight/4+pitchHeight/2);
			newInfo.m_rearLeftHeightOffset = SPRING_FACTOR*(-pitchHeight/2 + -pitchHeight/3);
			newInfo.m_rearRightHeightOffset = SPRING_FACTOR*(-pitchHeight/2 + -pitchHeight/3);
		}
		if (rollHeight>0) {	// Right raising up
			newInfo.m_frontRightHeightOffset += -SPRING_FACTOR*(rollHeight/3+rollHeight/2);
			newInfo.m_rearRightHeightOffset += -SPRING_FACTOR*(rollHeight/3+rollHeight/2);
			newInfo.m_rearLeftHeightOffset += rollHeight/2 - rollHeight/4;
			newInfo.m_frontLeftHeightOffset += rollHeight/2 - rollHeight/4;
		}	else {	// Left rasing up.
			newInfo.m_frontRightHeightOffset += -rollHeight/2 + rollHeight/4;
			newInfo.m_rearRightHeightOffset += -rollHeight/2 + rollHeight/4;
			newInfo.m_rearLeftHeightOffset += SPRING_FACTOR*(rollHeight/3+rollHeight/2);
			newInfo.m_frontLeftHeightOffset += SPRING_FACTOR*(rollHeight/3+rollHeight/2);
		}
		if (newInfo.m_frontLeftHeightOffset < m_locoInfo->m_wheelInfo.m_frontLeftHeightOffset) {
			// If it's going down, dampen the movement a bit
			m_locoInfo->m_wheelInfo.m_frontLeftHeightOffset += (newInfo.m_frontLeftHeightOffset - m_locoInfo->m_wheelInfo.m_frontLeftHeightOffset)/2.0f;
		}	else {
			m_locoInfo->m_wheelInfo.m_frontLeftHeightOffset = newInfo.m_frontLeftHeightOffset;
		}
		if (newInfo.m_frontRightHeightOffset < m_locoInfo->m_wheelInfo.m_frontRightHeightOffset) {
			// If it's going down, dampen the movement a bit
			m_locoInfo->m_wheelInfo.m_frontRightHeightOffset += (newInfo.m_frontRightHeightOffset - m_locoInfo->m_wheelInfo.m_frontRightHeightOffset)/2.0f;
		}	else {
			m_locoInfo->m_wheelInfo.m_frontRightHeightOffset = newInfo.m_frontRightHeightOffset;
		}
		if (newInfo.m_rearLeftHeightOffset < m_locoInfo->m_wheelInfo.m_rearLeftHeightOffset) {
			// If it's going down, dampen the movement a bit
			m_locoInfo->m_wheelInfo.m_rearLeftHeightOffset += (newInfo.m_rearLeftHeightOffset - m_locoInfo->m_wheelInfo.m_rearLeftHeightOffset)/2.0f;
		}	else {
			m_locoInfo->m_wheelInfo.m_rearLeftHeightOffset = newInfo.m_rearLeftHeightOffset;
		}
		if (newInfo.m_rearRightHeightOffset < m_locoInfo->m_wheelInfo.m_rearRightHeightOffset) {
			// If it's going down, dampen the movement a bit
			m_locoInfo->m_wheelInfo.m_rearRightHeightOffset += (newInfo.m_rearRightHeightOffset - m_locoInfo->m_wheelInfo.m_rearRightHeightOffset)/2.0f;
		}	else {
			m_locoInfo->m_wheelInfo.m_rearRightHeightOffset = newInfo.m_rearRightHeightOffset;
		}
		//m_locoInfo->m_wheelInfo = newInfo;
		if (m_locoInfo->m_wheelInfo.m_frontLeftHeightOffset<MAX_SUSPENSION_EXTENSION) {
			m_locoInfo->m_wheelInfo.m_frontLeftHeightOffset=MAX_SUSPENSION_EXTENSION;
		}
		if (m_locoInfo->m_wheelInfo.m_frontRightHeightOffset<MAX_SUSPENSION_EXTENSION) {
			m_locoInfo->m_wheelInfo.m_frontRightHeightOffset=MAX_SUSPENSION_EXTENSION;
		}
		if (m_locoInfo->m_wheelInfo.m_rearLeftHeightOffset<MAX_SUSPENSION_EXTENSION) {
			m_locoInfo->m_wheelInfo.m_rearLeftHeightOffset=MAX_SUSPENSION_EXTENSION;
		}
		if (m_locoInfo->m_wheelInfo.m_rearRightHeightOffset<MAX_SUSPENSION_EXTENSION) {
			m_locoInfo->m_wheelInfo.m_rearRightHeightOffset=MAX_SUSPENSION_EXTENSION;
		}
/*
		if (m_locoInfo->m_wheelInfo.m_frontLeftHeightOffset>MAX_SUSPENSION_COMPRESSION) {
			m_locoInfo->m_wheelInfo.m_frontLeftHeightOffset=MAX_SUSPENSION_COMPRESSION;
		}
		if (m_locoInfo->m_wheelInfo.m_frontRightHeightOffset>MAX_SUSPENSION_COMPRESSION) {
			m_locoInfo->m_wheelInfo.m_frontRightHeightOffset=MAX_SUSPENSION_COMPRESSION;
		}
		if (m_locoInfo->m_wheelInfo.m_rearLeftHeightOffset>MAX_SUSPENSION_COMPRESSION) {
			m_locoInfo->m_wheelInfo.m_rearLeftHeightOffset=MAX_SUSPENSION_COMPRESSION;
		}
		if (m_locoInfo->m_wheelInfo.m_rearRightHeightOffset>MAX_SUSPENSION_COMPRESSION) {
			m_locoInfo->m_wheelInfo.m_rearRightHeightOffset=MAX_SUSPENSION_COMPRESSION;
		}
		*/
	}
	// If we are > 22 degrees, need to raise height;
	Real divisor = 4;
	Real pitch = fabs(info.m_totalPitch-groundPitch);

	if (pitch>PI/8) {
		divisor = ((4*PI/8) + (1*(pitch-PI/8)))/pitch;
	}
	info.m_totalZ += fabs(pitchHeight)/divisor;
	info.m_totalZ += fabs(rollHeight)/divisor;
}

//-------------------------------------------------------------------------------------------------
/** decodes the current previous damage type and sets the ambient sound set from that. */
//-------------------------------------------------------------------------------------------------
const AudioEventRTS& Drawable::getAmbientSoundByDamage(BodyDamageType dt)
{
	switch (dt)
	{
		case BODY_DAMAGED:
			return *getTemplate()->getSoundAmbientDamaged();
		case BODY_REALLYDAMAGED:
			return *getTemplate()->getSoundAmbientReallyDamaged();
		case BODY_RUBBLE:
			return *getTemplate()->getSoundAmbientRubble();
		case BODY_PRISTINE:
		default:
			return *getTemplate()->getSoundAmbient();
	}
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
#ifdef RTS_DEBUG
void Drawable::validatePos() const
{
	const Coord3D* ourPos = getPosition();
	if (_isnan(ourPos->x) || _isnan(ourPos->y) || _isnan(ourPos->z))
	{
		DEBUG_CRASH(("Drawable/Object position NAN! '%s'", getTemplate()->getName().str()));
	}
	if (getObject())
	{
		const Coord3D* objPos = getObject()->getPosition();
		if (ourPos->x != objPos->x || ourPos->y != objPos->y || ourPos->z != objPos->z)
		{
			DEBUG_CRASH(("Drawable/Object position mismatch! '%s'", getTemplate()->getName().str()));
		}
	}
}
#endif


//=============================================================================
void Drawable::setStealthLook(StealthLookType look)
{
	if (look != m_stealthLook)
	{

		m_stealthOpacity = 1.0f;	//assume not transparent
		switch (look)
		{
			case STEALTHLOOK_NONE:
				m_hiddenByStealth = false;
				m_heatVisionOpacity = 0.0f;
				break;

			case STEALTHLOOK_VISIBLE_FRIENDLY:
			case STEALTHLOOK_VISIBLE_FRIENDLY_DETECTED:
			{
				Real opacity = TheGlobalData->m_stealthFriendlyOpacity;

				Object *obj = getObject();
				if( obj )
				{
					//Try to get the stealthupdate module and see if the opacity value is overridden.
					StealthUpdate* stealth = obj->getStealth();
					if( stealth )
					{
						if( stealth->isDisguised() )
						{
							//Disguised objects drive the opacity level directly, hence the break.
							m_hiddenByStealth = false;
							break;
						}
						else
						{
							Real friendlyOpacity = stealth->getFriendlyOpacity();
							if( friendlyOpacity != INVALID_OPACITY )
							{
								opacity = friendlyOpacity;
							}
						}
					}
				}

				m_stealthOpacity = opacity;	// make as partially transparent as this while pulsing
				m_hiddenByStealth = false;

			/** @todo srj -- evil hack here... this whole heat-vision thing is broken.
				don't want it on mines but no good way to do that. hack for now. */
				if (look == STEALTHLOOK_VISIBLE_FRIENDLY_DETECTED && !isKindOf(KINDOF_MINE))
					m_heatVisionOpacity = 1.0f;
				else
					m_heatVisionOpacity = 0.0f;

				break;
			}

			case STEALTHLOOK_DISGUISED_ENEMY:
				m_hiddenByStealth = false;
				m_heatVisionOpacity = 0.0f;
				break;

			// this is for the non-controllingplayer that can see me anyway
			case STEALTHLOOK_VISIBLE_DETECTED:
				m_hiddenByStealth = false;// let the scene omit the first drawing pass
				/** @todo srj -- evil hack here... this whole heat-vision thing is broken.
					don't want it on mines but no good way to do that. hack for now. */
				if (isKindOf(KINDOF_MINE))
					m_heatVisionOpacity = 0.0f;
				else
					m_heatVisionOpacity = 1.0f;// Draw() will fade until it is set to 1 again
				break;

			case STEALTHLOOK_INVISIBLE:
				m_hiddenByStealth = true;
				m_heatVisionOpacity = 0.0f;
				break;
		}
		m_stealthLook = look;
		updateHiddenStatus();
	}
}


//-------------------------------------------------------------------------------------------------
/** default draw is to just call the database defined draw */
//-------------------------------------------------------------------------------------------------
void Drawable::draw()
{
	if ( getObject() && getObject()->isEffectivelyDead() )
	{
		//dead folks don't stealth anyway
		m_heatVisionOpacity = 0.0f;
	}
	else if ( m_heatVisionOpacity > VERY_TRANSPARENT_HEATVISION )
	{
		// keep fading any added material unless something has set it to zero
		// TheSuperHackers @tweak The stealth opacity fade time step is now decoupled from the render update.
		static_assert(HEATVISION_FADE_SCALAR > 0.0f && HEATVISION_FADE_SCALAR < 1.0f, "HEATVISION_FADE_SCALAR must be between 0 and 1");

		// GeneralsX: GX_SHOT capture clock (GameClient.cpp): the pacer's value unless capturing.
		const Real timeScale = gxRenderTimeScale();
		const Real fadeScalar = 1.0f - (1.0f - HEATVISION_FADE_SCALAR) * timeScale;
		m_heatVisionOpacity *= fadeScalar;
	}
	else
	{
		m_heatVisionOpacity = 0.0f;
	}

	if (m_hidden || m_hiddenByStealth || getFullyObscuredByShroud())
		return;	// my, that was easy

	if ( getObject() && !getObject()->isEffectivelyDead() )
		setShadowsEnabled( m_stealthLook != STEALTHLOOK_VISIBLE_DETECTED );



#ifdef RTS_DEBUG
	validatePos();
#endif

	// call the database defined draw action method
	Matrix3D transformMtx = *getTransformMatrix();
	interpolateTransform(transformMtx);
	if (showsClientPhysics())
		advancePhysicsXform(FALSE);
	composeDrawnPose(transformMtx, TheDisplay->getLogicPhase());

	for (DrawModule** dm = getDrawModules(); *dm; ++dm)
	{
		(*dm)->doDrawModule(&transformMtx);
	}
}

// ------------------------------------------------------------------------------------------------
/** Compute the health bar region based on the health of the object and the
	* zoom level of the camera */
// ------------------------------------------------------------------------------------------------
static Bool computeHealthRegion( const Drawable *draw, IRegion2D& region )
{

	// sanity
	if( draw == nullptr )
		return FALSE;

	const Object *obj = draw->getObject();
	if( obj == nullptr )
		return FALSE;

	Coord3D p;
	obj->getHealthBoxPosition(p);
	p.add(draw->getInterpolationOffset());	// GeneralsX: follow the drawn body, not the logic position
	ICoord2D screenCenter;
	if( !TheTacticalView->worldToScreen( &p, &screenCenter ) )
		return FALSE;

	Real healthBoxWidth, healthBoxHeight;
	if (!obj->getHealthBoxDimensions(healthBoxHeight, healthBoxWidth))
		return FALSE;

	// scale the health bars according to the zoom
	Real zoom = TheTacticalView->getRetailReferenceZoom();
	//Real widthScale = 1.3f / zoom;
	Real widthScale = 1.0f / zoom;
	//Real heightScale = 0.8f / zoom;
	Real heightScale = 1.0f;

	healthBoxWidth *= widthScale;
	healthBoxHeight *= heightScale;

	// do this so health bar doesn't get too skinny or fat after scaling
	//healthBoxHeight = max(3.0f, healthBoxHeight);
	// GeneralsX: 3.0f is a raw PIXEL count written for a 600-line target.
	// Kept byte-for-byte in step with the Zero Hour twin, whose copy of this function carries the
	// full rationale - see GeneralsMD/Code/GameEngine/Source/GameClient/Drawable.cpp,
	// computeHealthRegion. Shipping only one of the two would give Zero Hour a HUD that scales and
	// Generals one that does not.
	// GeneralsX: DO NOT SCALE THIS - see the matching note in the Zero Hour
	// twin. Tried twice, reverted twice: the WIDTH already scales through worldToScreen, so scaling
	// only the height makes the bar fatter rather than bigger, and it was reported from a live game
	// the day it shipped. The complaint that prompted it was macOS rendering at point size and being
	// upscaled 2x, which the Retina fix addressed instead.
	healthBoxHeight = 3.0f;

	// figure out the final region for the health box
	region.lo.x = screenCenter.x - healthBoxWidth * 0.45f;
	region.lo.y = screenCenter.y - healthBoxHeight * 0.5f;
	region.hi.x = region.lo.x + healthBoxWidth;
	region.hi.y = region.lo.y + healthBoxHeight;

	return TRUE;

}


// ------------------------------------------------------------------------------------------------

Bool Drawable::drawsAnyUIText()
{
	if (!isSelected())
		return FALSE;

	const Object *obj = getObject();
	if ( !obj || obj->getControllingPlayer() != rts::getObservedOrLocalPlayer())
		return FALSE;

	Player *owner = obj->getControllingPlayer();
	Int groupNum = owner->getSquadNumberForObject(obj);

	if (groupNum > NO_HOTKEY_SQUAD && groupNum < NUM_HOTKEY_SQUADS )
		return TRUE;
	else
		m_groupNumber = nullptr;

	if ( obj->getFormationID() != NO_FORMATION_ID )
		return TRUE;

	return FALSE;
}


// ------------------------------------------------------------------------------------------------
/** This is called as part of the "post draw" phase when drawable a drawable.  It is there
	* that we should overlay on the screen any 2D elements for purposes of user interface
	* information (such as a heatlh bar, veterency levels, etc.) */
// ------------------------------------------------------------------------------------------------
void Drawable::drawIconUI()
{
	if( TheGameLogic->getDrawIconUI() && (TheScriptEngine->getFade()==ScriptEngine::FADE_NONE) )
	{
		IRegion2D healthBarRegionStorage;
		const IRegion2D* healthBarRegion = nullptr;
		if (computeHealthRegion(this, healthBarRegionStorage))
			healthBarRegion = &healthBarRegionStorage; //both data and a PointerAsFlag for logic in the methods below

		Object *obj = getObject();

		// we only draw icons drawables with objects, so one bail here -------------------------
		if ( ! obj )
			return;

		//Icons that can be drawn on dead things
		drawHealthBar( healthBarRegion );
		drawEmoticon( healthBarRegion );

		drawCaption( healthBarRegion );
		drawConstructPercent( healthBarRegion );

		//All Icons Below only draw on ALIVE things, so  bail here -------------------------
		if( obj->isEffectivelyDead() || obj->isKindOf( KINDOF_IGNORED_IN_GUI )) // object explicitly wants nothing to do with these icons, so...
			return;
		drawHealing( healthBarRegion );//call so dead things can kill their healing icons
		drawBombed( healthBarRegion );


		//Disabled for multiplay!
		//drawBattlePlans( healthBarRegion );

		if ( drawsAnyUIText() )
			TheGameClient->addTextBearingDrawable( this );

		drawEnthusiastic( healthBarRegion );
#ifdef ALLOW_DEMORALIZE
		drawDemoralized( healthBarRegion );
#endif
		drawDisabled( healthBarRegion );

		drawAmmo( healthBarRegion );
		drawContained( healthBarRegion );

		//Moved this to last so that it shows up over contained and ammo icons.
		drawVeterancy( healthBarRegion );
	}
}

//------------------------------------------------------------------------------------------------
DrawableIconInfo* Drawable::getIconInfo()
{
	if (m_iconInfo == nullptr)
		m_iconInfo = newInstance(DrawableIconInfo);
	return m_iconInfo;
}

//------------------------------------------------------------------------------------------------
void Drawable::clearEmoticon()
{
	if (!hasIconInfo())
		return;

	killIcon(ICON_EMOTICON);
}

//------------------------------------------------------------------------------------------------
void Drawable::setEmoticon( const AsciiString &name, Int duration )
{
	//A duration of -1 means FOREVER
	clearEmoticon();
	Anim2DTemplate *animTemplate = TheAnim2DCollection->findTemplate( name );
	if( animTemplate )
	{
		DEBUG_ASSERTCRASH( getIconInfo()->m_icon[ ICON_EMOTICON ] == nullptr, ("Drawable::setEmoticon - Emoticon isn't empty, need to refuse to set or destroy the old one in favor of the new one") );
		if( getIconInfo()->m_icon[ ICON_EMOTICON ] == nullptr )
		{
			getIconInfo()->m_icon[ ICON_EMOTICON ] = newInstance(Anim2D)( animTemplate, TheAnim2DCollection );
			getIconInfo()->m_keepTillFrame[ ICON_EMOTICON ] = duration >= 0 ? TheGameLogic->getFrame() + duration : FOREVER;
		}
	}
}

//------------------------------------------------------------------------------------------------
void Drawable::drawEmoticon( const IRegion2D *healthBarRegion )
{
	if( hasIconInfo() && getIconInfo()->m_icon[ ICON_EMOTICON ] )
	{
		UnsignedInt now = TheGameLogic->getFrame();
		if( healthBarRegion && getIconInfo()->m_keepTillFrame[ ICON_EMOTICON ] >= now )
		{
			//Draw the emoticon.
			Int barWidth = healthBarRegion->hi.x - healthBarRegion->lo.x;
			//Int barHeight = healthBarRegion.hi.y - healthBarRegion.lo.y;
			Int frameWidth = getIconInfo()->m_icon[ ICON_EMOTICON ]->getCurrentFrameWidth();
			Int frameHeight = getIconInfo()->m_icon[ ICON_EMOTICON ]->getCurrentFrameHeight();

#ifdef SCALE_ICONS_WITH_ZOOM_ML
			// adjust the width to be a % of the health bar region size
			Int size = REAL_TO_INT( barWidth * 0.3f );
			frameHeight = REAL_TO_INT((INT_TO_REAL(size) / INT_TO_REAL(frameWidth)) * frameHeight);
			frameWidth = size;
#else
			frameWidth = REAL_TO_INT( frameWidth * gxOverlayIconShrink() );
			frameHeight = REAL_TO_INT( frameHeight * gxOverlayIconShrink() );
#endif
			// given our scaled width and height we need to find the top left point to draw the image at
			ICoord2D screen;
			screen.x = (Int)(healthBarRegion->lo.x + (barWidth * 0.5f) - (frameWidth * 0.5f));
			screen.y = healthBarRegion->hi.y - frameHeight;
			getIconInfo()->m_icon[ ICON_EMOTICON ]->draw( screen.x, screen.y, frameWidth, frameHeight );
		}
		else
		{
			//Get rid of the emoticon.
			clearEmoticon();
		}
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void Drawable::drawAmmo( const IRegion2D *healthBarRegion )
{
	const Object *obj = getObject();
	s_ammoRectOwner = INVALID_DRAWABLE_ID;
	if (healthBarRegion == nullptr)
		return;

	if (!(
				TheGlobalData->m_showObjectHealth &&
				(isSelected() || (TheInGameUI && (TheInGameUI->getMousedOverDrawableID() == getID()))) &&
				obj->getControllingPlayer() == rts::getObservedOrLocalPlayer()
			))
		return;

	Int numTotal;
	Int numFull;
	if (!obj->getAmmoPipShowingInfo(numTotal, numFull))
		return;

	if (!s_fullAmmo || !s_emptyAmmo)
		return;



#ifdef SCALE_ICONS_WITH_ZOOM_ML
	Real scale = TheGlobalData->m_ammoPipScaleFactor / CLAMP_ICON_ZOOM_FACTOR( TheTacticalView->getRetailReferenceZoom() );
#else
	Real scale = gxOverlayIconShrink();
#endif

	// GeneralsX: The rocket-pod / bomb pips follow the container pips' size step: above 1080p the
	// hand-drawn orange 7x14 / 10x20 pair (!GXUIHD.big), picked by the same render-height rule, drawn at its own size x
	// the zoom-out shrink. Absent art falls back to retail's 5x10, exactly as the container pips do.
	Bool ammoPresent[GX_PIP_VARIANTS];
	for (Int v = 0; v < GX_PIP_VARIANTS; ++v)
		ammoPresent[v] = s_fullAmmoHD[v] != nullptr && s_emptyAmmoHD[v] != nullptr;
	const Int ammoVariant = gxPickPipVariant( TheDisplay->getHeight(), ammoPresent );
	const Image *fullAmmoImage = ammoVariant < 0 ? s_fullAmmo : s_fullAmmoHD[ammoVariant];
	const Image *emptyAmmoImage = ammoVariant < 0 ? s_emptyAmmo : s_emptyAmmoHD[ammoVariant];
	// Size cap: the row never exceeds the unit's own bar width, but a pip stays at least 4 px wide so it remains readable zoomed out.
	Int fitBoxWidth = -1;	// the widest whole-pixel pip whose row, with its fixed 1 px gaps, fits the bar; -1 = no cap
	{
		const Int imgW = emptyAmmoImage->getImageWidth();
		const Int barW = healthBarRegion->hi.x - healthBarRegion->lo.x;
		// GeneralsX: The scale below shrinks the gaps with the pips, but the row is drawn with
		// whole-pixel pips and FIXED 1 px gaps, so it could still run past the bar: 6 pips of 10 px art on a 34 px bar
		// came out 6 x 5 + 5 = 35 px (p1-client-misc). The pixel cap is applied after the scale.
		if (numTotal > 0)
			fitBoxWidth = (barW - (numTotal - 1)) / numTotal;
		const Real rowAtOne = (Real)((imgW + 1) * numTotal - 1);
		const Real minScale = 4.0f / (Real)imgW;
		Real capScale = (Real)barW / rowAtOne;
		if (capScale < minScale) capScale = minScale;
		if (capScale < scale) scale = capScale;
	}
	Int boxWidth  = REAL_TO_INT(emptyAmmoImage->getImageWidth() * scale);
	Int boxHeight = REAL_TO_INT(emptyAmmoImage->getImageHeight() * scale);
	if (fitBoxWidth >= 0 && boxWidth > fitBoxWidth)
		boxWidth = fitBoxWidth;	// the 4 px floor below still wins on a bar too narrow for every pip
	if (boxWidth < 4)
		boxWidth = 4;	// REAL_TO_INT truncates: imgW * (4 / imgW) can land on 3.9999
	const Int SPACING = 1;
	//Int totalWidth = (boxWidth+SPACING)*numTotal;

	ICoord2D screenCenter;
	Coord3D pos = *obj->getPosition();
	pos.add(getInterpolationOffset());	// GeneralsX: follow the drawn body
	pos.x += TheGlobalData->m_ammoPipWorldOffset.x;
	pos.y += TheGlobalData->m_ammoPipWorldOffset.y;
	pos.z += TheGlobalData->m_ammoPipWorldOffset.z + obj->getGeometryInfo().getMaxHeightAbovePosition();
	if( !TheTacticalView->worldToScreen( &pos, &screenCenter ) )
		return;

	Real bounding = obj->getGeometryInfo().getBoundingSphereRadius() * scale;
	//Int posx = screenCenter.x + REAL_TO_INT(TheGlobalData->m_ammoPipScreenOffset.x*bounding) - totalWidth;
	//**CHANGING CODE: Left justify with health bar min
	Int posx = healthBarRegion->lo.x;
	Int posy = screenCenter.y + REAL_TO_INT(TheGlobalData->m_ammoPipScreenOffset.y*bounding);
	s_ammoRect.lo.x = posx;
	s_ammoRect.lo.y = posy + 1;
	s_ammoRect.hi.y = posy + 1 + boxHeight;
	for (Int i = 0; i < numTotal; ++i)
	{
		TheDisplay->drawImage(i < numFull ? fullAmmoImage : emptyAmmoImage, posx, posy + 1, posx + boxWidth, posy + 1 + boxHeight);
		posx += boxWidth + SPACING;
	}
	s_ammoRect.hi.x = posx;
	s_ammoRectOwner = getID();
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void Drawable::drawContained( const IRegion2D *healthBarRegion )
{
	// GeneralsX: drawIconUI passes a null region when computeHealthRegion fails (the health box
	// off-screen, or no health box) and the pips are placed from it below: a selected container whose box sat just past
	// the screen edge while its pip anchor did not would dereference null (read from the code, not seen). GX_PIPS_ALL
	// makes it routine.
	if (healthBarRegion == nullptr)
		return;

	const Object *obj = getObject();

	ContainModuleInterface* container = obj->getContain();
	if (!container)
		return;

	if (!(
				TheGlobalData->m_showObjectHealth &&
				(isSelected() || (TheInGameUI && (TheInGameUI->getMousedOverDrawableID() == getID()))) &&
				obj->getControllingPlayer() == rts::getObservedOrLocalPlayer()
			))
		return;

	Int numTotal;
	Int numFull;
	if (!container->getContainerPipsToShow(numTotal, numFull))
		return;

	// if empty, don't show nothin'
	if (numFull == 0)
		return;

	Int numInfantry = 0;
	const ContainedItemsList* contained = container->getContainedItemsList();
	if (contained)
	{
		for (ContainedItemsList::const_iterator it = contained->begin(); it != contained->end(); ++it)
		{
			if ((*it)->isKindOf(KINDOF_INFANTRY))
				++numInfantry;
		}
	}

#ifdef SCALE_ICONS_WITH_ZOOM_ML
	Real scale = TheGlobalData->m_ammoPipScaleFactor / CLAMP_ICON_ZOOM_FACTOR( TheTacticalView->getRetailReferenceZoom() );
#else
	Real scale = gxOverlayIconShrink();
#endif
	// GeneralsX: Above 1080p the hand-drawn 7x16 / 10x24 pair, chosen by the render height and
	// drawn at its OWN size (times the zoom-out shrink, as retail's), never retail's 5x12 stretched (GXImageVariant.h).
	Bool present[GX_PIP_VARIANTS];
	for (Int v = 0; v < GX_PIP_VARIANTS; ++v)
		present[v] = s_fullContainerHD[v] != nullptr && s_emptyContainerHD[v] != nullptr;
	const Int variant = gxPickPipVariant( TheDisplay->getHeight(), present );
	const Image *fullImage = variant < 0 ? s_fullContainer : s_fullContainerHD[variant];
	const Image *emptyImage = variant < 0 ? s_emptyContainer : s_emptyContainerHD[variant];
	const Int emptyYOff = gxPipEmptyYOffset( variant );
	Int boxWidth  = REAL_TO_INT(emptyImage->getImageWidth() * scale);	// both pips at the empty one's size, as retail
	Int boxHeight = REAL_TO_INT(emptyImage->getImageHeight() * scale);
	const Int SPACING = 1;
	//Int totalWidth = (boxWidth+SPACING)*numTotal;

	ICoord2D screenCenter;
	Coord3D pos = *obj->getPosition();
	pos.add(getInterpolationOffset());	// GeneralsX: follow the drawn body
	pos.x += TheGlobalData->m_containerPipWorldOffset.x;
	pos.y += TheGlobalData->m_containerPipWorldOffset.y;
	pos.z += TheGlobalData->m_containerPipWorldOffset.z + obj->getGeometryInfo().getMaxHeightAbovePosition();
	if( !TheTacticalView->worldToScreen( &pos, &screenCenter ) )
		return;

	Real bounding = obj->getGeometryInfo().getBoundingSphereRadius() * scale;

	//Int posx = screenCenter.x + REAL_TO_INT(TheGlobalData->m_containerPipScreenOffset.x*bounding) - totalWidth;
	//**CHANGING CODE: Left justify with health bar min
	Int posx = healthBarRegion->lo.x;
	Int posy = screenCenter.y + REAL_TO_INT(TheGlobalData->m_containerPipScreenOffset.y*bounding);

	// The occupancy row is the other thing the rank chevron must come AFTER: hand its rect to drawVeterancy (union with the ammo row).
	{
		const Int rowRight = posx + (boxWidth + SPACING) * numTotal;
		if (s_ammoRectOwner != getID())
		{
			s_ammoRect.lo.x = posx;
			s_ammoRect.lo.y = posy + (emptyYOff < 0 ? emptyYOff : 0);
			s_ammoRect.hi.y = posy + boxHeight + (emptyYOff > 0 ? emptyYOff : 0);
			s_ammoRect.hi.x = rowRight;
			s_ammoRectOwner = getID();
		}
		else
		{
			if (posx < s_ammoRect.lo.x) s_ammoRect.lo.x = posx;
			if (posy + (emptyYOff < 0 ? emptyYOff : 0) < s_ammoRect.lo.y) s_ammoRect.lo.y = posy + (emptyYOff < 0 ? emptyYOff : 0);
			if (posy + boxHeight + (emptyYOff > 0 ? emptyYOff : 0) > s_ammoRect.hi.y) s_ammoRect.hi.y = posy + boxHeight + (emptyYOff > 0 ? emptyYOff : 0);
			if (rowRight > s_ammoRect.hi.x) s_ammoRect.hi.x = rowRight;
		}
	}
	for (Int i = 0; i < numTotal; ++i)
	{
		const Color INFANTRY_COLOR = GameMakeColor(0, 255, 0, 255);
		const Color NON_INFANTRY_COLOR = GameMakeColor(0, 0, 255, 255);
		if (i < numFull)
			TheDisplay->drawImage(fullImage, posx, posy, posx + boxWidth, posy + boxHeight,
				(i < numInfantry) ? INFANTRY_COLOR : NON_INFANTRY_COLOR);
		else
			TheDisplay->drawImage(emptyImage, posx, posy + emptyYOff, posx + boxWidth, posy + emptyYOff + boxHeight);
		posx += boxWidth + SPACING;
	}
}

//-------------------------------------------------------------------------------------------------
void Drawable::drawBattlePlans( const IRegion2D *healthBarRegion )
{
	Object *obj = getObject();
	if( !obj || !healthBarRegion )
	{
		return;
	}


	Player *player = obj->getControllingPlayer();
	if( player && player->getNumBattlePlansActive() > 0 && player->doesObjectQualifyForBattlePlan( obj ) )
	{
		if( player->getBattlePlansActiveSpecific( PLANSTATUS_BOMBARDMENT ) )
		{
			if( !getIconInfo()->m_icon[ ICON_BATTLEPLAN_BOMBARD ] )
			{
				getIconInfo()->m_icon[ ICON_BATTLEPLAN_BOMBARD ] = newInstance(Anim2D)( s_animationTemplates[ ICON_BATTLEPLAN_BOMBARD ], TheAnim2DCollection );
			}
			//Int barHeight = healthBarRegion.hi.y - healthBarRegion.lo.y;
			Int frameWidth = getIconInfo()->m_icon[ ICON_BATTLEPLAN_BOMBARD ]->getCurrentFrameWidth();
			Int frameHeight = getIconInfo()->m_icon[ ICON_BATTLEPLAN_BOMBARD ]->getCurrentFrameHeight();

#ifdef SCALE_ICONS_WITH_ZOOM_ML
			// adjust the width to be a % of the health bar region size
			Int barWidth = healthBarRegion->hi.x - healthBarRegion->lo.x;
			Int size = REAL_TO_INT( barWidth * 0.3f );
			frameHeight = REAL_TO_INT((INT_TO_REAL(size) / INT_TO_REAL(frameWidth)) * frameHeight);
			frameWidth = size;
#else
			frameWidth = REAL_TO_INT( frameWidth * gxOverlayIconShrink() );
			frameHeight = REAL_TO_INT( frameHeight * gxOverlayIconShrink() );
#endif
			// given our scaled width and height we need to find the top left point to draw the image at
			ICoord2D screen;
			screen.x = healthBarRegion->lo.x;
			screen.y = healthBarRegion->lo.y + frameHeight;
			getIconInfo()->m_icon[ ICON_BATTLEPLAN_BOMBARD ]->draw( screen.x, screen.y, frameWidth, frameHeight );
		}
		else
		{
			killIcon(ICON_BATTLEPLAN_BOMBARD);
		}

		if( player->getBattlePlansActiveSpecific( PLANSTATUS_HOLDTHELINE ) )
		{
			if( !getIconInfo()->m_icon[ ICON_BATTLEPLAN_HOLDTHELINE ] )
			{
				getIconInfo()->m_icon[ ICON_BATTLEPLAN_HOLDTHELINE ] = newInstance(Anim2D)( s_animationTemplates[ ICON_BATTLEPLAN_HOLDTHELINE ], TheAnim2DCollection );
			}
			// draw the icon
			Int frameWidth = getIconInfo()->m_icon[ ICON_BATTLEPLAN_HOLDTHELINE ]->getCurrentFrameWidth();
			Int frameHeight = getIconInfo()->m_icon[ ICON_BATTLEPLAN_HOLDTHELINE ]->getCurrentFrameHeight();

#ifdef SCALE_ICONS_WITH_ZOOM_ML
			// adjust the width to be a % of the health bar region size
			Int barWidth = healthBarRegion->hi.x - healthBarRegion->lo.x;
			Int size = REAL_TO_INT( barWidth * 0.3f );
			frameHeight = REAL_TO_INT((INT_TO_REAL(size) / INT_TO_REAL(frameWidth)) * frameHeight);
			frameWidth = size;
#else
			frameWidth = REAL_TO_INT( frameWidth * gxOverlayIconShrink() );
			frameHeight = REAL_TO_INT( frameHeight * gxOverlayIconShrink() );
#endif
			// given our scaled width and height we need to find the top left point to draw the image at
			ICoord2D screen;
			screen.x = healthBarRegion->lo.x;
			screen.y = healthBarRegion->lo.y + frameHeight;
			getIconInfo()->m_icon[ ICON_BATTLEPLAN_HOLDTHELINE ]->draw( screen.x + frameWidth, screen.y, frameWidth, frameHeight );
		}
		else
		{
			killIcon(ICON_BATTLEPLAN_HOLDTHELINE);
		}

		if( player->getBattlePlansActiveSpecific( PLANSTATUS_SEARCHANDDESTROY ) )
		{
			if( !getIconInfo()->m_icon[ ICON_BATTLEPLAN_SEARCHANDDESTROY ] )
			{
				getIconInfo()->m_icon[ ICON_BATTLEPLAN_SEARCHANDDESTROY ] = newInstance(Anim2D)( s_animationTemplates[ ICON_BATTLEPLAN_SEARCHANDDESTROY ], TheAnim2DCollection );
			}
			// draw the icon
			Int frameWidth = getIconInfo()->m_icon[ ICON_BATTLEPLAN_SEARCHANDDESTROY ]->getCurrentFrameWidth();
			Int frameHeight = getIconInfo()->m_icon[ ICON_BATTLEPLAN_SEARCHANDDESTROY ]->getCurrentFrameHeight();

#ifdef SCALE_ICONS_WITH_ZOOM_ML
			// adjust the width to be a % of the health bar region size
			Int barWidth = healthBarRegion->hi.x - healthBarRegion->lo.x;
			Int size = REAL_TO_INT( barWidth * 0.3f );
			frameHeight = REAL_TO_INT((INT_TO_REAL(size) / INT_TO_REAL(frameWidth)) * frameHeight);
			frameWidth = size;
#else
			frameWidth = REAL_TO_INT( frameWidth * gxOverlayIconShrink() );
			frameHeight = REAL_TO_INT( frameHeight * gxOverlayIconShrink() );
#endif

			// given our scaled width and height we need to find the top left point to draw the image at
			ICoord2D screen;
			screen.x = healthBarRegion->lo.x;
			screen.y = healthBarRegion->lo.y + frameHeight;
			getIconInfo()->m_icon[ ICON_BATTLEPLAN_SEARCHANDDESTROY ]->draw( screen.x + (frameWidth * 2), screen.y, frameWidth, frameHeight );
		}
		else
		{
			killIcon(ICON_BATTLEPLAN_SEARCHANDDESTROY);
		}

	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void Drawable::drawUIText()
{

	// This gets called by GameClient now
	// GameClient caches a list of us drawables during Drawablepostdraw()
	// then our group numbers get spit out last, so they draw in front

	const IRegion2D* healthBarRegion = nullptr;
	IRegion2D healthBarRegionStorage;
	if (computeHealthRegion(this, healthBarRegionStorage))
		healthBarRegion = &healthBarRegionStorage; //both data and a PointerAsFlag for logic in the methods below

	if (!healthBarRegion)
		return;

	const Object *obj = getObject();

	Player *owner = obj->getControllingPlayer();
	Int groupNum = owner->getSquadNumberForObject(obj);

	Color color = TheDrawGroupInfo->m_usePlayerColor ? owner->getPlayerColor() : TheDrawGroupInfo->m_colorForText;

	// GeneralsX: Text and its fixed pixel offsets shrink with the bar and icons
	// (percent offsets already follow the bar's width). 1.0 at or below retail's camera ceiling: the retail layout.
	const Real shrink = gxOverlayIconShrink();

	if (groupNum > NO_HOTKEY_SQUAD && groupNum < NUM_HOTKEY_SQUADS )
	{
		Int xPos = healthBarRegion->lo.x;
		Int yPos = healthBarRegion->lo.y;

		if (TheDrawGroupInfo->m_usingPixelOffsetX) {
			xPos += REAL_TO_INT(TheDrawGroupInfo->m_pixelOffsetX * shrink);
		} else {
			xPos += (healthBarRegion->width() * TheDrawGroupInfo->m_percentOffsetX);
		}

		if (TheDrawGroupInfo->m_usingPixelOffsetY) {
			yPos += REAL_TO_INT(TheDrawGroupInfo->m_pixelOffsetY * shrink);
		} else {
			yPos += (healthBarRegion->width() * TheDrawGroupInfo->m_percentOffsetY);
		}

		m_groupNumber = TheDisplayStringManager->getGroupNumeralString(groupNum);
		gxShrinkOverlayText(m_groupNumber, shrink);


		m_groupNumber->draw(xPos, yPos, color,
												TheDrawGroupInfo->m_colorForTextDropShadow,
												TheDrawGroupInfo->m_dropShadowOffsetX,
												TheDrawGroupInfo->m_dropShadowOffsetY);
	}


	if ( obj->getFormationID() != NO_FORMATION_ID )
	{
		//draw an F, here
		Coord3D p;
		ICoord2D screenCenter;
		obj->getHealthBoxPosition(p);
		p.add(getInterpolationOffset());	// GeneralsX: follow the drawn body
		if( ! TheTacticalView->worldToScreen( &p, &screenCenter ) )
			return;

		Real healthBoxWidth, healthBoxHeight;
		if ( ! obj->getHealthBoxDimensions(healthBoxHeight, healthBoxWidth))
			return;

		Real scale = 1.3f/CLAMP_ICON_ZOOM_FACTOR( TheTacticalView->getRetailReferenceZoom() );
		screenCenter.x += (healthBoxWidth * scale * 0.5f) + 10 * shrink ;


		DisplayString *formationMarker = TheDisplayStringManager->getFormationLetterString();
		gxShrinkOverlayText(formationMarker, shrink);
		//static DisplayString *formationMarker = TheDisplayStringManager->getGroupNumeralString( 5 );
		if ( formationMarker )
			formationMarker->draw(screenCenter.x, screenCenter.y, color,
													TheDrawGroupInfo->m_colorForTextDropShadow,
													TheDrawGroupInfo->m_dropShadowOffsetX,
													TheDrawGroupInfo->m_dropShadowOffsetY);

	}


}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void Drawable::drawHealing(const IRegion2D* healthBarRegion)
{

	const Object *obj = getObject();

	// we do show show icons for things that explicitly forbid it
	if( obj->isKindOf( KINDOF_NO_HEAL_ICON ) || obj->getStatusBits().test( OBJECT_STATUS_SOLD ) )
		return;


	// see if healing has been done to us recently
	Bool showHealing = FALSE;
	BodyModuleInterface *body = obj->getBodyModule();
	if( body->getHealth() != body->getMaxHealth() )
	{
//		const DamageInfo* lastDamage = body->getLastDamageInfo();
//		if( lastDamage != nullptr && lastDamage->in.m_damageType == DAMAGE_HEALING
//			&&(TheGameLogic->getFrame() - body->getLastHealingTimestamp()) <= HEALING_ICON_DISPLAY_TIME
//			)
		const UnsignedInt *lastHealingTimestamp = body->getLastHealingTimestamp();
		if ( TheGameLogic->getFrame() > HEALING_ICON_DISPLAY_TIME && // because so many things init health early in game
			lastHealingTimestamp != nullptr &&
			(TheGameLogic->getFrame() - *lastHealingTimestamp <= HEALING_ICON_DISPLAY_TIME) )

			showHealing = TRUE;
	}

	// based on our own kind of we have certain icons to display at a size scale
	Real scale;
	DrawableIconType typeIndex;
	if( isKindOf( KINDOF_STRUCTURE ) )
	{
		typeIndex = ICON_STRUCTURE_HEAL;
		scale = 0.33f;
	}
	else if( isKindOf( KINDOF_VEHICLE ) )
	{
		typeIndex = ICON_VEHICLE_HEAL;
		scale = 0.7f;
	}
	else
	{
		typeIndex = ICON_DEFAULT_HEAL;
		scale = 0.7f;
	}

	//
	// if we are to show healing make sure we have the animation for it allocated, otherwise
	// free any animation we may have allocated back to the animation memory pool
	//
	if( showHealing ) /// @todo HERE, WE NEED TO LEAVE STUFF ALONE, IF WE ARE ALREADY SHOWING HEALING
	{
		if (healthBarRegion != nullptr)
		{

			if( getIconInfo()->m_icon[ typeIndex ] == nullptr )
				getIconInfo()->m_icon[ typeIndex ] = newInstance(Anim2D)( s_animationTemplates[ typeIndex ], TheAnim2DCollection );

			// draw the animation if present
			if( getIconInfo()->m_icon[ typeIndex ] != nullptr)
			{

				//
				// we are going to draw the healing icon relative to the size of the health bar region
				// since that region takes into account hit point size and zoom factor of the camera too
				//
				Int barWidth = healthBarRegion->hi.x - healthBarRegion->lo.x;

				Int frameWidth = getIconInfo()->m_icon[ typeIndex ]->getCurrentFrameWidth();
				Int frameHeight = getIconInfo()->m_icon[ typeIndex ]->getCurrentFrameHeight();

#ifdef SCALE_ICONS_WITH_ZOOM_ML
				// adjust the width to be a % of the health bar region size
				Int size = REAL_TO_INT( barWidth * scale );
				frameHeight = REAL_TO_INT((INT_TO_REAL(size) / INT_TO_REAL(frameWidth)) * frameHeight);
				frameWidth = size;
#else
				frameWidth = REAL_TO_INT( frameWidth * gxOverlayIconShrink() );
				frameHeight = REAL_TO_INT( frameHeight * gxOverlayIconShrink() );
#endif
				// given our scaled width and height we need to find the top left point to draw the image at
				ICoord2D screen;
				screen.x = REAL_TO_INT( healthBarRegion->lo.x + (barWidth * 0.75f) - (frameWidth * 0.5f) );
				screen.y = REAL_TO_INT( healthBarRegion->lo.y - frameHeight );
				getIconInfo()->m_icon[ typeIndex ]->draw( screen.x, screen.y, frameWidth, frameHeight );

			}
		}
	}
	else
	{
		killIcon(typeIndex);
	}

}

// ------------------------------------------------------------------------------------------------
/** This enthusiastic effect is TEMPORARY for the multiplayer test */
// ------------------------------------------------------------------------------------------------
void Drawable::drawEnthusiastic(const IRegion2D* healthBarRegion)
{

	const Object *obj = getObject();
	//
	// if we are to show effect make sure we have the animation for it allocated, otherwise
	// free any animation we may have allocated back to the animation memory pool
	//
	// only display if have enthusiasm

	if( obj->testWeaponBonusCondition( WEAPONBONUSCONDITION_ENTHUSIASTIC ) == TRUE &&
			healthBarRegion != nullptr )
	{

		DrawableIconType iconIndex = ICON_ENTHUSIASTIC;

		if (obj->testWeaponBonusCondition( WEAPONBONUSCONDITION_SUBLIMINAL ) == TRUE )// unless...
			iconIndex = ICON_ENTHUSIASTIC_SUBLIMINAL;




		if( getIconInfo()->m_icon[ iconIndex ] == nullptr )
			getIconInfo()->m_icon[ iconIndex ] = newInstance(Anim2D)( s_animationTemplates[ iconIndex ], TheAnim2DCollection );

		// draw the animation if present
		if( getIconInfo()->m_icon[ iconIndex ] != nullptr)
		{

			//
			// we are going to draw the healing icon relative to the size of the health bar region
			// since that region takes into account hit point size and zoom factor of the camera too
			//
			Int barWidth = healthBarRegion->hi.x - healthBarRegion->lo.x;// used for position

			// based on our own kind of we have certain icons to display at a size scale
			Real scale;
			if( isKindOf( KINDOF_STRUCTURE ) || isKindOf( KINDOF_HUGE_VEHICLE ) )
				scale = 1.00f;
			else if( isKindOf( KINDOF_VEHICLE ) )
				scale = 0.75f;
			else
				scale = 0.5f;

			Int frameWidth = getIconInfo()->m_icon[ iconIndex ]->getCurrentFrameWidth() * scale;
			Int frameHeight = getIconInfo()->m_icon[ iconIndex ]->getCurrentFrameHeight() * scale;

#ifdef SCALE_ICONS_WITH_ZOOM_ML
			// adjust the width to be a % of the health bar region size
			Int size = REAL_TO_INT( barWidth * scale );
			frameHeight = REAL_TO_INT((INT_TO_REAL(size) / INT_TO_REAL(frameWidth)) * frameHeight);
			frameWidth = size;
#else
			frameWidth = REAL_TO_INT( frameWidth * gxOverlayIconShrink() );
			frameHeight = REAL_TO_INT( frameHeight * gxOverlayIconShrink() );
#endif
			// given our scaled width and height we need to find the bottom left point to draw the image at
			ICoord2D screen;
			screen.x = REAL_TO_INT( healthBarRegion->lo.x + (barWidth * 0.25f) - (frameWidth * 0.5f) );
			screen.y = healthBarRegion->hi.y;
			getIconInfo()->m_icon[ iconIndex ]->draw( screen.x, screen.y, frameWidth, frameHeight );

		}
	}
	else
	{
		killIcon(ICON_ENTHUSIASTIC);
		killIcon(ICON_ENTHUSIASTIC_SUBLIMINAL);
	}

}

#ifdef ALLOW_DEMORALIZE
// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void Drawable::drawDemoralized(const IRegion2D* healthBarRegion)
{

	const Object *obj = getObject();


	//
	// Demoralized
	//
	const AIUpdateInterface *ai = obj->getAIUpdateInterface();
	if (!ai)
		return;

	if( ai->isDemoralized() )
	{
		// draw the icon
		if( healthBarRegion )
		{
			// create icon if necessary
			if( getIconInfo()->m_icon[ ICON_DEMORALIZED ] == nullptr )
				getIconInfo()->m_icon[ ICON_DEMORALIZED ] = newInstance(Anim2D)( s_animationTemplates[ ICON_DEMORALIZED ], TheAnim2DCollection );

			if (getIconInfo()->m_icon[ ICON_DEMORALIZED ])
			{

				Int frameWidth = getIconInfo()->m_icon[ ICON_DEMORALIZED ]->getCurrentFrameWidth();
				Int frameHeight = getIconInfo()->m_icon[ ICON_DEMORALIZED ]->getCurrentFrameHeight();

#ifdef SCALE_ICONS_WITH_ZOOM_ML
				// adjust the width to be a % of the health bar region size
				Int barWidth = healthBarRegion->hi.x - healthBarRegion->lo.x;
				Int size = REAL_TO_INT( barWidth * 0.3f );
				frameHeight = REAL_TO_INT((INT_TO_REAL(size) / INT_TO_REAL(frameWidth)) * frameHeight);
				frameWidth = size;
#else
				frameWidth = REAL_TO_INT( frameWidth * gxOverlayIconShrink() );
				frameHeight = REAL_TO_INT( frameHeight * gxOverlayIconShrink() );
#endif
				// given our scaled width and height we need to find the top left point to draw the image at
				ICoord2D screen;
				screen.x = healthBarRegion->lo.x;
				screen.y = healthBarRegion->hi.y;
				getIconInfo()->m_icon[ ICON_DEMORALIZED ]->draw( screen.x, screen.y, frameWidth, frameHeight );
			}
		}
	}
	else
	{
		killIcon(ICON_DEMORALIZED);
	}
}
#endif

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void Drawable::drawBombed(const IRegion2D* healthBarRegion)
{

	const Object *obj = getObject();


	UnsignedInt now = TheGameLogic->getFrame();

	if( obj->testWeaponSetFlag( WEAPONSET_CARBOMB ) &&
				obj->getControllingPlayer() == rts::getObservedOrLocalPlayer())
	{
		if( !getIconInfo()->m_icon[ ICON_CARBOMB ] )
			getIconInfo()->m_icon[ ICON_CARBOMB ] = newInstance(Anim2D)( s_animationTemplates[ ICON_CARBOMB ], TheAnim2DCollection );

		if( getIconInfo()->m_icon[ ICON_CARBOMB ] )
		{
			//
			// we are going to draw the healing icon relative to the size of the health bar region
			// since that region takes into account hit point size and zoom factor of the camera too
			//
			if( healthBarRegion )
			{
				Int barWidth = healthBarRegion->hi.x - healthBarRegion->lo.x;
				Int barHeight = healthBarRegion->hi.y - healthBarRegion->lo.y;

				Int frameWidth = getIconInfo()->m_icon[ ICON_CARBOMB ]->getCurrentFrameWidth();
				Int frameHeight = getIconInfo()->m_icon[ ICON_CARBOMB ]->getCurrentFrameHeight();

				// adjust the width to be a % of the health bar region size
				Int size = REAL_TO_INT( barWidth * 0.5f );
				frameHeight = REAL_TO_INT((INT_TO_REAL(size) / INT_TO_REAL(frameWidth)) * frameHeight);
				frameWidth = size;

				// given our scaled width and height we need to find the top left point to draw the image at
				ICoord2D screen;
				screen.x = REAL_TO_INT( healthBarRegion->lo.x + (barWidth * 0.5f) - (frameWidth * 0.5f) );
				screen.y = REAL_TO_INT( healthBarRegion->lo.y + barHeight * 0.5f );

				getIconInfo()->m_icon[ ICON_CARBOMB ]->draw( screen.x, screen.y, frameWidth, frameHeight );
				getIconInfo()->m_keepTillFrame[ ICON_CARBOMB ] = FOREVER;
			}
		}
}
	else
	{
		killIcon(ICON_CARBOMB);
	}

	//
	// Bombed?
	//
	static NameKeyType key_StickyBombUpdate = NAMEKEY( "StickyBombUpdate" );
	StickyBombUpdate *update = (StickyBombUpdate*)obj->findUpdateModule( key_StickyBombUpdate );
	if( update )
	{
		//This case is tricky. The object that is bombed doesn't know it... but the bomb itself does.
		//So what we do is get it's target, then determine if the target has the icon or not.
		Object *target = update->getTargetObject();
		if( target )
		{
			if( update->isTimedBomb() )
			{
				//Timed bomb
				if( !getIconInfo()->m_icon[ ICON_BOMB_TIMED ] )
				{
					getIconInfo()->m_icon[ ICON_BOMB_REMOTE ] = newInstance(Anim2D)( s_animationTemplates[ ICON_BOMB_REMOTE ], TheAnim2DCollection );
					getIconInfo()->m_icon[ ICON_BOMB_TIMED ] = newInstance(Anim2D)( s_animationTemplates[ ICON_BOMB_TIMED ], TheAnim2DCollection );

					//Because this is a counter icon that ranges from 0-60 seconds, we need to calculate which frame to
					//start the animation from. Because timers are second based -- 1000 ms equal 1 frame. So we simply
					//calculate the time via detonation frame.
					//
					// srj sez: this may sound familiar somehow, but let me reiterate, just in case you missed it:
					//
					// hardcoding is bad.
					//
					// the anim got changed and now is only 20 seconds max, so the previous code was wrong.
					//
					// hey, I've got an idea! why don't we ASK the anim how long it is?
					//
					UnsignedInt dieFrame = update->getDetonationFrame();
					UnsignedInt seconds = REAL_TO_INT_CEIL( (dieFrame - now) * SECONDS_PER_LOGICFRAME_REAL);

					UnsignedInt numFrames = getIconInfo()->m_icon[ ICON_BOMB_TIMED ]->getAnimTemplate()->getNumFrames();
					// this anim goes from "N" seconds down to zero, so the max seconds we can use is N-1.
					if (seconds > numFrames - 1)
						seconds = numFrames - 1;

					getIconInfo()->m_icon[ ICON_BOMB_TIMED ]->setMinFrame(numFrames - seconds - 1);
					getIconInfo()->m_icon[ ICON_BOMB_TIMED ]->reset();
				}
				if( getIconInfo()->m_icon[ ICON_BOMB_TIMED ] )
				{
					//
					// we are going to draw the healing icon relative to the size of the health bar region
					// since that region takes into account hit point size and zoom factor of the camera too
					//
					if( healthBarRegion )
					{
						Int barWidth = healthBarRegion->hi.x - healthBarRegion->lo.x;
						Int barHeight = healthBarRegion->hi.y - healthBarRegion->lo.y;

						Int frameWidth = getIconInfo()->m_icon[ ICON_BOMB_TIMED ]->getCurrentFrameWidth();
						Int frameHeight = getIconInfo()->m_icon[ ICON_BOMB_TIMED ]->getCurrentFrameHeight();

						// adjust the width to be a % of the health bar region size
						Int size = REAL_TO_INT( barWidth * 0.65f );
						frameHeight = REAL_TO_INT((INT_TO_REAL(size) / INT_TO_REAL(frameWidth)) * frameHeight);
						frameWidth = size;

						// given our scaled width and height we need to find the top left point to draw the image at
						ICoord2D screen;
						screen.x = REAL_TO_INT( healthBarRegion->lo.x + (barWidth * 0.5f) - (frameWidth * 0.5f) );
						screen.y = REAL_TO_INT( healthBarRegion->lo.y + barHeight * 0.5f );

						getIconInfo()->m_icon[ ICON_BOMB_REMOTE ]->draw( screen.x, screen.y, frameWidth, frameHeight );
						getIconInfo()->m_keepTillFrame[ ICON_BOMB_REMOTE ] = now + 1;
						getIconInfo()->m_icon[ ICON_BOMB_TIMED ]->draw( screen.x, screen.y, frameWidth, frameHeight );
						getIconInfo()->m_keepTillFrame[ ICON_BOMB_TIMED ] = now + 1;
					}
				}
			}
			else
			{
				//Remote charge
				//Timed bomb
				if( !getIconInfo()->m_icon[ ICON_BOMB_REMOTE ] )
				{
					getIconInfo()->m_icon[ ICON_BOMB_REMOTE ] = newInstance(Anim2D)( s_animationTemplates[ ICON_BOMB_REMOTE ], TheAnim2DCollection );
				}
				if( getIconInfo()->m_icon[ ICON_BOMB_REMOTE ] )
				{
					//
					// we are going to draw the healing icon relative to the size of the health bar region
					// since that region takes into account hit point size and zoom factor of the camera too
					//
					if( healthBarRegion )
					{
						Int barWidth = healthBarRegion->hi.x - healthBarRegion->lo.x;
						Int barHeight = healthBarRegion->hi.y - healthBarRegion->lo.y;

						Int frameWidth = getIconInfo()->m_icon[ ICON_BOMB_REMOTE ]->getCurrentFrameWidth();
						Int frameHeight = getIconInfo()->m_icon[ ICON_BOMB_REMOTE ]->getCurrentFrameHeight();


						// adjust the width to be a % of the health bar region size
						Int size = REAL_TO_INT( barWidth * 0.65f );
						frameHeight = REAL_TO_INT((INT_TO_REAL(size) / INT_TO_REAL(frameWidth)) * frameHeight);
						frameWidth = size;

						// given our scaled width and height we need to find the top left point to draw the image at
						ICoord2D screen;
						screen.x = REAL_TO_INT( healthBarRegion->lo.x + (barWidth * 0.5f) - (frameWidth * 0.5f) );
						screen.y = REAL_TO_INT( healthBarRegion->lo.y + barHeight * 0.5f );

						getIconInfo()->m_icon[ ICON_BOMB_REMOTE ]->draw( screen.x, screen.y, frameWidth, frameHeight );
						getIconInfo()->m_keepTillFrame[ ICON_BOMB_REMOTE ] = now + 1;
					}
				}
			}
		}
	}

	if (hasIconInfo())
	{
		if(getIconInfo()->m_keepTillFrame[ ICON_BOMB_TIMED ] <= now )
		{
			killIcon(ICON_BOMB_TIMED);
		}
		if(getIconInfo()->m_keepTillFrame[ ICON_BOMB_REMOTE ] <= now )
		{
			killIcon(ICON_BOMB_REMOTE);
		}
	}
}

// ------------------------------------------------------------------------------------------------
/** Draw any icon information that needs to be drawn */
// ------------------------------------------------------------------------------------------------
void Drawable::drawDisabled(const IRegion2D* healthBarRegion)
{

	const Object *obj = getObject();


	//
	// Disabled Emoticon /Lightning
	//                   7/
	if( obj->isDisabledByType( DISABLED_HACKED )
		|| obj->isDisabledByType( DISABLED_PARALYZED )
		|| obj->isDisabledByType( DISABLED_EMP )
		|| obj->isDisabledByType( DISABLED_UNDERPOWERED )
		)
	{
		// create icon if necessary
		if( getIconInfo()->m_icon[ ICON_DISABLED ] == nullptr )
		{
			getIconInfo()->m_icon[ ICON_DISABLED ] = newInstance(Anim2D)
			( s_animationTemplates[ ICON_DISABLED ], TheAnim2DCollection );
		}

		// draw the icon
		if( healthBarRegion )
		{
			Int barHeight = healthBarRegion->hi.y - healthBarRegion->lo.y;

			Int frameWidth = getIconInfo()->m_icon[ ICON_DISABLED ]->getCurrentFrameWidth();
			Int frameHeight = getIconInfo()->m_icon[ ICON_DISABLED ]->getCurrentFrameHeight();

#ifdef SCALE_ICONS_WITH_ZOOM_ML
			// adjust the width to be a % of the health bar region size
			Int barWidth = healthBarRegion->hi.x - healthBarRegion->lo.x;
			Int size = REAL_TO_INT( barWidth * 0.3f );
			frameHeight = REAL_TO_INT((INT_TO_REAL(size) / INT_TO_REAL(frameWidth)) * frameHeight);
			frameWidth = size;
#else
			frameWidth = REAL_TO_INT( frameWidth * gxOverlayIconShrink() );
			frameHeight = REAL_TO_INT( frameHeight * gxOverlayIconShrink() );
#endif
			// given our scaled width and height we need to find the top left point to draw the image at
			ICoord2D screen;
			screen.x = healthBarRegion->lo.x;
			screen.y = healthBarRegion->hi.y - (frameHeight + barHeight);
			getIconInfo()->m_icon[ ICON_DISABLED ]->draw( screen.x, screen.y, frameWidth, frameHeight );

		}
	}
	else
	{
		// delete icon if necessary
		killIcon(ICON_DISABLED);

	}

}

//-------------------------------------------------------------------------------------------------
/** Draw construction percent for drawables that have objects that are "under construction" */
//-------------------------------------------------------------------------------------------------
void Drawable::drawConstructPercent( const IRegion2D *healthBarRegion )
{

	// this data is in an attached object
	Object *obj = getObject();

	if( obj == nullptr || !obj->getStatusBits().test( OBJECT_STATUS_UNDER_CONSTRUCTION ) ||
			obj->getStatusBits().test( OBJECT_STATUS_SOLD ) )
	{
		// no object, or we are now complete get rid of the string if we have one
		if( m_constructDisplayString )
		{

			TheDisplayStringManager->freeDisplayString( m_constructDisplayString );
			m_constructDisplayString = nullptr;
		}
		return;
	}

	//if( obj->isEffectivelyDead() )
	//{
		//Don't render icons for dead things.
	//	return;
	//}

	// construction is partially complete, allocate a display string if we need one
	if( m_constructDisplayString == nullptr )
	{
		m_constructDisplayString = TheDisplayStringManager->newDisplayString();
		// GeneralsX: A freshly allocated string is empty, so reset the displayed-percent
		// key (issue #3095 fix below); otherwise a string re-created mid-build would stay blank until the percent moved.
		m_lastConstructDisplayed = -1;
	}

	// GeneralsX: The caption font moved here from the constructor. A string
	// re-created after being freed used to keep the manager's default font; it now gets the caption font too.
	// GeneralsX: At this zoom's size (gxConstructLabelFont); a no-op unless the size stepped.
	// Before the text, so a new string measures its text once, in the font it draws with.
	m_constructDisplayString->setFont(gxConstructLabelFont());

	// set the string if the value has changed
	// GeneralsX: Fork-side fix for upstream issue #3095 (no upstream PR). This
	// compared the raw Real, which moves every logic frame, so every building under construction in view
	// re-fetched the CSF string (AsciiString + bsearch) and re-formatted it every logic frame. Key on the value
	// as displayed instead.
	// Upstream reference: xezon, issue #3095 https://github.com/TheSuperHackers/GeneralsGameCode/issues/3095
	// GeneralsX: ...AS THE FETCHED FORMAT DISPLAYS IT. The key was
	// lrintf(percent), which assumed retail's "%.0f%%": a localization or mod showing "%.1f%%" kept stale text
	// between whole percents. It is now the number at the format's own precision (GXConstructPercentKey.h), which
	// also carries that precision, so the next frames need no fetch to compare; a format it cannot read gets -1,
	// rebuilt every frame as retail did.
	const Real percent = obj->getConstructionPercent();
	if( m_lastConstructDisplayed < 0
		|| gxConstructPercentKey( percent, m_lastConstructDisplayed % 8 ) != m_lastConstructDisplayed )
	{
		UnicodeString buffer;
		const UnicodeString format = TheGameText->fetchGameOnly("CONTROLBAR:UnderConstructionDesc");
		buffer.format( format, percent );
		m_constructDisplayString->setText( buffer );

		// record this percent as our last displayed so we don't un-necessarily rebuild the string
		m_lastConstructDisplayed = gxConstructPercentKey( percent, gxFixedDecimals( format.str() ) );
	}

	// get center position in drawable
	ICoord2D screen;
	Coord3D pos;
	getDrawableGeometryInfo().getCenterPosition(*getPosition(), pos);
	pos.add(getInterpolationOffset());	// GeneralsX: follow the drawn body

	// convert drawable center position to screen coords
	TheTacticalView->worldToScreen( &pos, &screen );

	// draw the text
	Color color = GameMakeColor( 255, 255, 255, 255 );
	Color dropColor = GameMakeColor( 0, 0, 0, 255 );
	screen.x -= (m_constructDisplayString->getWidth() / 2);
	m_constructDisplayString->draw( screen.x, screen.y, color, dropColor );

}

//-------------------------------------------------------------------------------------------------
/** Draw caption */
//-------------------------------------------------------------------------------------------------
void Drawable::drawCaption( const IRegion2D *healthBarRegion )
{
	if (!m_captionDisplayString)
		return;

	// get center position in drawable
	ICoord2D screen;
	Coord3D pos;
	getDrawableGeometryInfo().getCenterPosition(*getPosition(), pos);
	pos.add(getInterpolationOffset());	// GeneralsX: follow the drawn body

	// convert drawable center position to screen coords
	TheTacticalView->worldToScreen( &pos, &screen );
	screen.x -= (m_captionDisplayString->getWidth() / 2);

	// draw background
	{
		Int width, xPos;
		Int height, yPos;
		m_captionDisplayString->getSize(&width,&height);
		xPos = screen.x - 1;
		yPos = screen.y - 1;

		TheDisplay->drawFillRect(xPos, yPos, width + 2,height + 2, GameMakeColor(0,0,0,125));
		TheDisplay->drawOpenRect(xPos, yPos, width + 2,height + 2, 1.0, GameMakeColor(20,20,20,255));
	}

	// draw the text
	Color color = TheInGameUI->getDrawableCaptionColor();
	Color dropColor = GameMakeColor( 0, 0, 0, 255 );
	m_captionDisplayString->draw( screen.x, screen.y, color, dropColor );

}

// ------------------------------------------------------------------------------------------------
/** Draw any veterency markers that should be displayed */
// ------------------------------------------------------------------------------------------------
void Drawable::drawVeterancy( const IRegion2D *healthBarRegion )
{
	// get object from drawable
	Object* obj = getObject();

	if( obj->getExperienceTracker() == nullptr )
	{
		//Only objects with experience trackers can possibly have veterancy.
		return;
	}

	VeterancyLevel level = obj->getVeterancyLevel();
	const Image* image = s_veterancyImage[level];
	// GeneralsX: Above 1080p the hand-drawn chevrons (!GXUIHD.big), picked by the pips' render-height rule.
	{
		Bool vetPresent[GX_PIP_VARIANTS];
		for (Int v = 0; v < GX_PIP_VARIANTS; ++v)
			vetPresent[v] = s_veterancyHD[v][1] != nullptr && s_veterancyHD[v][2] != nullptr && s_veterancyHD[v][3] != nullptr;
		const Int vetVariant = gxPickPipVariant( TheDisplay->getHeight(), vetPresent );
		if (vetVariant >= 0 && level >= 1 && level <= 3)
			image = s_veterancyHD[vetVariant][level];
	}
	if (!image)
		return;

	Real scale = 1.3f/CLAMP_ICON_ZOOM_FACTOR( TheTacticalView->getRetailReferenceZoom() );
#ifdef SCALE_ICONS_WITH_ZOOM_ML
	Real objScale = scale * 1.55f;
#else
	Real objScale = gxOverlayIconShrink();
#endif


	Real vetBoxWidth  = image->getImageWidth()*objScale;
	Real vetBoxHeight = image->getImageHeight()*objScale;

	//
	// take the center position of the object, go down to it's bottom extent, and project
	// that point to the screen, that will be the "center" of our veterancy box
	//

	Coord3D p;
	ICoord2D screenCenter;
	obj->getHealthBoxPosition(p);
	p.add(getInterpolationOffset());	// GeneralsX: follow the drawn body
	if( !TheTacticalView->worldToScreen( &p, &screenCenter ) )
		return;

	Real healthBoxWidth, healthBoxHeight;
	if (!obj->getHealthBoxDimensions(healthBoxHeight, healthBoxWidth))
		return;

	screenCenter.x += healthBoxWidth * scale * 0.5f;

	// GeneralsX: Anchor the chevron to the bar's bottom EDGE. The +1 this
	// replaces is the bar's half-height at the retail pin of 3 and at no other height, so it is the
	// only icon in the cluster that assumed a fixed bar thickness. Identical to the Zero Hour twin,
	// which carries the full rationale.
	Int vetTop = screenCenter.y + 1;
	if( healthBarRegion )
		vetTop = healthBarRegion->hi.y;

	Int vetLeft = screenCenter.x + 1;
	if (s_ammoRectOwner == getID())
	{
		s_ammoRectOwner = INVALID_DRAWABLE_ID;
		const Bool xHit = vetLeft < s_ammoRect.hi.x && vetLeft + vetBoxWidth > s_ammoRect.lo.x;
		const Bool yHit = vetTop < s_ammoRect.hi.y && vetTop + vetBoxHeight > s_ammoRect.lo.y;
		if (xHit && yHit)
		{
			vetLeft = s_ammoRect.hi.x + 1;
			if (vetLeft + vetBoxWidth > TheDisplay->getWidth())	// unit at the right screen edge: stay visible
				vetLeft = TheDisplay->getWidth() - (Int)vetBoxWidth;
		}
	}

	// draw the image
	TheDisplay->drawImage(image, vetLeft, vetTop, vetLeft + vetBoxWidth, vetTop + vetBoxHeight);

}

// ------------------------------------------------------------------------------------------------
/** Draw health bar information for drawable */
// ------------------------------------------------------------------------------------------------
void Drawable::drawHealthBar(const IRegion2D* healthBarRegion)
{
	if (!healthBarRegion)
		return;

	//
	// only draw health for selected drawables and drawables that have been moused over
	// by the cursor
	//
	if( TheGlobalData->m_showObjectHealth &&
			(isSelected() || (TheInGameUI && (TheInGameUI->getMousedOverDrawableID() == getID()))) )
	{
		Object *obj = getObject();

		// if no object, nothing to do
		if( obj == nullptr )
			return;

		if( obj->isKindOf( KINDOF_FORCEATTACKABLE ) )
		{
			//Currently (Nov 2002), everything that is forceattackable are civ fences, and they all have a
			//single hit point and they aren't selectable. However, a bug is when you force attack it, it shows
			//the healthbar. Well, this stops it, however, should force attackable kindofs change, then this
			//will require reevaluation.
			return;
		}

		// get body module of object
		BodyModuleInterface *body = obj->getBodyModule();

		// get the health and max health
		Real health = body->getHealth();
		Real maxHealth = body->getMaxHealth();

		// if no max health or health at all we will draw nothing
		if( maxHealth == 0.0f || health == 0.0f )
			return;

		// what is our health ratio
		Real healthRatio = health / maxHealth;

		//
		// what color will we use for the health bar based on our ratio, this makes it
		// slowly go from green to red, (or from blue to cyan if under construction, or disabled)
		//

		Color color, outlineColor;
		DisabledMaskType mask = obj->getDisabledFlags();
		mask.clear(MAKE_DISABLED_MASK(DISABLED_HELD));

		if (obj->getStatusBits().test(OBJECT_STATUS_UNDER_CONSTRUCTION) || DISABLEDMASK_ANY_SET(mask))
		{
			color = GameMakeColor( 0, healthRatio * 255.0f, 255, 255 );//blue to cyan
			outlineColor = GameMakeColor( 0, healthRatio * 128.0f, 128, 255 );//dark blue to dark cyan

		}
		else //red to green
		{

			RGBColor inColor, outColor;
			inColor.blue = 0; // health bars do not display blue...
			outColor.blue = 0; // health bars do not display blue...

			if( healthRatio >= 0.5f )
			{
				inColor.red = 1.0f - ((healthRatio - 0.5f) / 0.5f);
				inColor.green = 1.0f;
//				color = GameMakeColor       (  255 - ((healthRatio - 0.5f) / 0.5f) * 255, 255, 0, 255 );
//				outlineColor = GameMakeColor( (255 - ((healthRatio - 0.5f) / 0.5f) * 255) * 0.5, 255 * 0.5, 0, 255 );
			}
			else
			{
				inColor.red = 1.0f;
				inColor.green = 1.0f - ((0.5f - healthRatio) / 0.5f);
//				color = GameMakeColor( 255, 255 - ((0.5f - healthRatio) / 0.5f) * 255, 0, 255 );
//				outlineColor = GameMakeColor( 255 * 0.5, (255 - ((0.5f - healthRatio) / 0.5f) * 255) * 0.5, 0, 255 );
			}

			outColor.red = inColor.red * 0.5f;
			outColor.green =inColor.green * 0.5f;

			if( m_conditionState.test( MODELCONDITION_REALLY_DAMAGED ) == TRUE )
			{//average the above color with red
				inColor.red = (1.0f + inColor.red) * 0.5f;
				inColor.green *= 0.5f;
			}
			else if ( m_conditionState.test( MODELCONDITION_DAMAGED ) == FALSE )
			{//average the above color with green
				inColor.green = (1.0f + inColor.green) * 0.5f;
				inColor.red *= 0.5f;
			}

			color =        GameMakeColor( 255.0 * inColor.red, 255.0 * inColor.green, 255.0 * inColor.blue, 255);
			outlineColor = GameMakeColor( 255.0 * outColor.red, 255.0 * outColor.green, 255.0 * outColor.blue, 255);


		}




///		Real scale = 1.3f / TheTacticalView->getZoom();
		Real healthBoxWidth = healthBarRegion->hi.x - healthBarRegion->lo.x;

		Real healthBoxHeight = max(3, healthBarRegion->hi.y - healthBarRegion->lo.y);
		Real healthBoxOutlineSize = 1.0f;

		// draw the health box outline
		TheDisplay->drawOpenRect( healthBarRegion->lo.x, healthBarRegion->lo.y, healthBoxWidth, healthBoxHeight,
															healthBoxOutlineSize, outlineColor );

		// draw a filled bar for the health
		TheDisplay->drawFillRect( healthBarRegion->lo.x + 1, healthBarRegion->lo.y + 1,
															(healthBoxWidth - 2) * healthRatio, healthBoxHeight - 2,
															color );
	}

}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
void Drawable::clearAndSetModelConditionState( ModelConditionFlagType clr, ModelConditionFlagType set )
{
	ModelConditionFlags c, s;
	if (clr != MODELCONDITION_INVALID)
		c.set(clr);
	if (set != MODELCONDITION_INVALID)
		s.set(set);
	clearAndSetModelConditionFlags(c, s);
}
// GX-SIMID-END

//-------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
DrawModule** Drawable::getDrawModules()
{
	DrawModule** dm = (DrawModule**)getModuleList(MODULETYPE_DRAW);

#ifdef DIRTY_CONDITION_FLAGS
	if (m_isModelDirty)
	{
		if (s_modelLockCount > 0)
		{
			DEBUG_CRASH(("Should not need to update dirty stuff while locked-for-iteration. Ignoring."));
			// this shouldn't happen, but if it does, just return the current (dirty) scenario.
			// we must NOT update the condition state, as someone is relying on the current
			// list of W3D render objects not being munged. (srj)
		}
		else
		{
			replaceModelConditionStateInDrawable();
		}
	}
#endif

	DEBUG_ASSERTCRASH(dm != nullptr, ("Draw Module List is not expected null"));
	return dm;
}
// GX-SIMID-END

//-------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
DrawModule const** Drawable::getDrawModules() const
{
	DrawModule const** dm = (DrawModule const**)getModuleList(MODULETYPE_DRAW);

#ifdef DIRTY_CONDITION_FLAGS
	if (m_isModelDirty)
	{
		if (s_modelLockCount > 0)
		{
			DEBUG_CRASH(("Should not need to update dirty stuff while locked-for-iteration. Ignoring."));
			// this shouldn't happen, but if it does, just return the current (dirty) scenario.
			// we must NOT update the condition state, as someone is relying on the current
			// list of W3D render objects not being munged. (srj)
		}
		else
		{
			const_cast<Drawable*>(this)->replaceModelConditionStateInDrawable();
		}
	}
#endif

	DEBUG_ASSERTCRASH(dm != nullptr, ("Draw Module List is not expected null"));
	return dm;
}
// GX-SIMID-END

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
void Drawable::clearAndSetModelConditionFlags(const ModelConditionFlags& clr, const ModelConditionFlags& setf)
{
	ModelConditionFlags oldFlags = m_conditionState;

	m_conditionState.clearAndSet(clr, setf);

	if (m_conditionState == oldFlags)
		return;

#ifdef DIRTY_CONDITION_FLAGS
	m_isModelDirty = true;
	s_anyModelDirty = TRUE;
#else
	replaceModelConditionStateInDrawable();
#endif
}
// GX-SIMID-END

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
void Drawable::replaceModelConditionFlags( const ModelConditionFlags &flags, Bool forceReplace )
{

	//
	// this is a no-op if the new flags are the same as our existing flags (unless we
	// have the forceReplace parameter set, in which case we will force the setting of the
	// new flags)
	//
	if( forceReplace == FALSE && m_conditionState == flags )
		return;

	m_conditionState = flags;
#ifdef DIRTY_CONDITION_FLAGS
	// when forcing a replace we won't use dirty flags, we will immediately do an update now
	if( forceReplace == TRUE )
	{
		replaceModelConditionStateInDrawable();
	}
	else
	{
		m_isModelDirty = true;
		s_anyModelDirty = TRUE;
	}
#else
	replaceModelConditionStateInDrawable();
#endif
}
// GX-SIMID-END

//-------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
void Drawable::replaceModelConditionStateInDrawable()
{
	// TheSuperHackers @info Set not dirty early to avoid recursive calls from within getDrawModules().
	m_isModelDirty = false;

	// TheSuperHackers @bugfix Remove and re-add the terrain decal before processing the individual draw
	// modules, because the terrain decal is applied to the first draw module only, and the new first
	// draw module may be different than before.
	const TerrainDecalType terrainDecalType = getTerrainDecalType();
	setTerrainDecal(TERRAIN_DECAL_NONE);

	for (DrawModule** dm = getDrawModules(); *dm; ++dm)
	{
		ObjectDrawInterface* di = (*dm)->getObjectDrawInterface();
		if (di)
			di->replaceModelConditionState( m_conditionState );
	}

	setTerrainDecal(terrainDecalType);
}
// GX-SIMID-END

//-------------------------------------------------------------------------------------------------
void Drawable::setIndicatorColor(Color color)
{
	for (DrawModule** dm = getDrawModules(); *dm; ++dm)
	{
		ObjectDrawInterface* di = (*dm)->getObjectDrawInterface();
		if (di)
			di->replaceIndicatorColor(color);
	}
}

// ------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
const GeometryInfo& Drawable::getDrawableGeometryInfo() const
{
	return getObject() ? getObject()->getGeometryInfo() : getTemplate()->getTemplateGeometryInfo();
}
// GX-SIMID-END

// ------------------------------------------------------------------------------------------------
/** Set the id for this drawable */
// ------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
void Drawable::setID( DrawableID id )
{

	// if id hasn't changed do nothing
	if( m_id == id )
		return;

	// remove this objects previous id from the lookup table
	if( m_id != INVALID_DRAWABLE_ID )
		TheGameClient->removeDrawableFromLookupTable( this );

	// assign new id
	m_id = id;

	// add new id to lookup table
	if( m_id != INVALID_DRAWABLE_ID )
	{
		TheGameClient->addDrawableToLookupTable( this );
		if (m_ambientSound)
			m_ambientSound->setDrawableID(m_id);
	}

}
// GX-SIMID-END

// ------------------------------------------------------------------------------------------------
/** Return drawable ID, this ID is only good on the client */
// ------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
DrawableID Drawable::getID() const
{

	// we should never be getting the ID of a drawable who doesn't yet have and ID assigned to it
	DEBUG_ASSERTCRASH( m_id != 0, ("Drawable::getID - Using ID before it was assigned!!!!") );

	return m_id;

}
// GX-SIMID-END

//-------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
void Drawable::friend_bindToObject( Object *obj ) ///< bind this drawable to an object ID
{
	m_object = obj;
	if (getObject())
	{
		if (TheGlobalData->m_timeOfDay == TIME_OF_DAY_NIGHT)
			setIndicatorColor(getObject()->getNightIndicatorColor());
		else
			setIndicatorColor(getObject()->getIndicatorColor());
	}

	for (DrawModule** dm = getDrawModules(); *dm; ++dm)
	{
		(*dm)->onDrawableBoundToObject();
	}

	PhysicsXformInfo physicsXform;
	if (calcPhysicsXform(physicsXform))
	{
		DEBUG_ASSERTCRASH(m_physicsXform == nullptr, ("m_physicsXform is not null"));
		m_physicsXform = new PhysicsXformInfo;
		*m_physicsXform = physicsXform;
	}
}
// GX-SIMID-END
//-------------------------------------------------------------------------------------------------
	// when our Object changes teams, it calls us to let us know, so
	// we can update our model, etc., if necessary. NOTE, we don't guarantee
	// that the new team is different from the old team, nor do we guarantee
	// that the team is nonnull.
void Drawable::changedTeam()
{
	Object *object = getObject();
	if( object )
	{
		if (TheGlobalData->m_timeOfDay == TIME_OF_DAY_NIGHT)
			setIndicatorColor( object->getNightIndicatorColor() );
		else
			setIndicatorColor( object->getIndicatorColor() );
	}
}

//-------------------------------------------------------------------------------------------------
void Drawable::setPosition(const Coord3D *pos)
{
	// extend
	Thing::setPosition(pos);

}

//-------------------------------------------------------------------------------------------------
void Drawable::reactToTransformChange(const Matrix3D* oldMtx, const Coord3D* oldPos, Real oldAngle)
{

	// GeneralsX: A moved drawable can enter or leave a view region.
	GameClient::noteDrawableChange();

	for (DrawModule** dm = getDrawModules(); *dm; ++dm)
	{
		(*dm)->reactToTransformChange(oldMtx, oldPos, oldAngle);
	}
}

//-------------------------------------------------------------------------------------------------
void Drawable::reactToGeometryChange()
{
	for (DrawModule** dm = getDrawModules(); *dm; ++dm)
	{
		(*dm)->reactToGeometryChange();
	}
}

//-------------------------------------------------------------------------------------------------
Bool Drawable::handleWeaponFireFX(WeaponSlotType wslot, Int specificBarrelToUse, const FXList* fxl, Real weaponSpeed, Real recoilAmount, Real recoilAngle, const Coord3D* victimPos, Real damageRadius)
{
	if (recoilAmount != 0.0f)
	{
		// adjust recoil from absolute to relative.
		if (getObject())
			recoilAngle -= getObject()->getOrientation();
		// flip direction 180 degrees.
		recoilAngle += PI;
		if (m_locoInfo)
		{
			m_locoInfo->m_accelerationPitchRate += recoilAmount * Cos(recoilAngle);
			m_locoInfo->m_accelerationRollRate += recoilAmount * Sin(recoilAngle);
		}
	}

	for (DrawModule** dm = getDrawModules(); *dm; ++dm)
	{
		ObjectDrawInterface* di = (*dm)->getObjectDrawInterface();
		if (di && di->handleWeaponFireFX(wslot, specificBarrelToUse, fxl, weaponSpeed, victimPos, damageRadius))
			return true;
	}
	return false;
}

//-------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
Int Drawable::getBarrelCount(WeaponSlotType wslot) const
{
	for (const DrawModule** dm = getDrawModules(); *dm; ++dm)
	{
		const ObjectDrawInterface* di = (*dm)->getObjectDrawInterface();
		Int count = di ? di->getBarrelCount(wslot) : 0;
		if (count != 0)
			return count;
	}
	return 0;
}
// GX-SIMID-END

//-------------------------------------------------------------------------------------------------
/** Set the Drawable's instance transform */
//-------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
void Drawable::setInstanceMatrix( const Matrix3D *instance )
{
	if (instance)
	{
		m_instance = *instance;
		m_instanceIsIdentity = false;
	}
	else
	{
		m_instance.Make_Identity();
		m_instanceIsIdentity = true;
	}
}
// GX-SIMID-END


//-------------------------------------------------------------------------------------------------
/**
 * Return the Drawable's world transform.
 * If this Drawable is attached to an Object, return the Object's transform instead.
 */
//-------------------------------------------------------------------------------------------------
const Matrix3D *Drawable::getTransformMatrix() const
{
	const Object *obj = getObject();

	if (obj)
		return obj->getTransformMatrix();
	else
		return Thing::getTransformMatrix();
}

//-------------------------------------------------------------------------------------------------
/**
 * Set and clear the drawable's caption text
 */
//-------------------------------------------------------------------------------------------------
void Drawable::setCaptionText( const UnicodeString& captionText )
{
	if (captionText.isEmpty())
	{
		clearCaptionText();
		return;
	}

	UnicodeString sanitizedString = captionText;
	TheLanguageFilter->filterLine(sanitizedString);

	if( m_captionDisplayString == nullptr )
	{
		m_captionDisplayString = TheDisplayStringManager->newDisplayString();
		GameFont *font = TheFontLibrary->getFont(
			TheInGameUI->getDrawableCaptionFontName(),
			TheGlobalLanguageData->adjustFontSize(TheInGameUI->getDrawableCaptionPointSize()),
			TheInGameUI->isDrawableCaptionBold() );
		m_captionDisplayString->setFont( font );
		m_captionDisplayString->setText( sanitizedString );
	}
	else
	{
		// set the string if the value has changed
		if( m_captionDisplayString->getText().compare(sanitizedString) != 0 )
		{
			m_captionDisplayString->setText( sanitizedString );
		}
	}
}

//-------------------------------------------------------------------------------------------------
void Drawable::clearCaptionText()
{
	if (m_captionDisplayString)
		TheDisplayStringManager->freeDisplayString(m_captionDisplayString);
	m_captionDisplayString = nullptr;
}

//-------------------------------------------------------------------------------------------------
UnicodeString Drawable::getCaptionText()
{
	if (m_captionDisplayString)
		return m_captionDisplayString->getText();

	return UnicodeString::TheEmptyString;
}

//-------------------------------------------------------------------------------------------------
/** Attach and start playing an ambient sound to this drawable */
//-------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
void	Drawable::setTimeOfDay(TimeOfDay tod)
{
	BodyDamageType dt = BODY_PRISTINE;
	if (getObject() && getObject()->getBodyModule())
		dt = getObject()->getBodyModule()->getDamageState();

	startAmbientSound(dt, tod);

	ModelConditionFlags c = m_conditionState;
	c.set(MODELCONDITION_NIGHT, (tod == TIME_OF_DAY_NIGHT) ? 1 : 0);
	replaceModelConditionFlags(c);
}
// GX-SIMID-END

//-------------------------------------------------------------------------------------------------
/** Attach and start playing an ambient sound to this drawable */
//-------------------------------------------------------------------------------------------------
void Drawable::startAmbientSound(BodyDamageType dt, TimeOfDay tod)
{
	stopAmbientSound();

	//Get the specific ambient sound for the damage type.
	const AudioEventRTS& audio = getAmbientSoundByDamage(dt);
	Bool trySound = FALSE;
	if( audio.getEventName().isNotEmpty() )
	{
		if (m_ambientSound == nullptr)
			m_ambientSound.Assign_No_Add_Ref(newInstance(DynamicAudioEventRTS));

		*m_ambientSound = audio;
		trySound = TRUE;
	}
	else if( dt != BODY_PRISTINE && dt != BODY_RUBBLE )
	{
		//If the ambient sound was absent in the case of non-pristine damage types,
		//try getting the pristine one. Most of our cases actually specify just the
		//pristine sound and want to use it for all states (except dead/rubble).
		const AudioEventRTS& pristineAudio = getAmbientSoundByDamage( BODY_PRISTINE );
		if( pristineAudio.getEventName().isNotEmpty() )
		{
			if (m_ambientSound == nullptr)
				m_ambientSound.Assign_No_Add_Ref(newInstance(DynamicAudioEventRTS));
			*m_ambientSound = pristineAudio;
			trySound = TRUE;
		}
	}

	if( trySound && m_ambientSound )
	{
		const AudioEventInfo *info = m_ambientSound->getAudioEventInfo();
		if( info )
		{
			if( BitIsSet( info->m_type, ST_GLOBAL) || info->m_priority == AP_CRITICAL )
			{
				//Play it anyways.
				m_ambientSound->setDrawableID(getID());
				m_ambientSound->setTimeOfDay(tod);
				m_ambientSound->setPlayingHandle(TheAudio->addAudioEvent( m_ambientSound.Peek() ));
			}
			else
			{
				//Check if it's close enough to try playing (optimization)
				Coord3D vector = *getPosition();
				vector.sub( *TheAudio->getListenerPosition() );
				Real distSqr = vector.lengthSqr();
				if( distSqr < sqr( info->m_maxDistance ) )
				{
					m_ambientSound->setDrawableID(getID());
					m_ambientSound->setTimeOfDay(tod);
					m_ambientSound->setPlayingHandle(TheAudio->addAudioEvent( m_ambientSound.Peek() ));
				}
			}
		}
		else
		{
			DEBUG_CRASH( ("Ambient sound %s missing! Skipping...", m_ambientSound->getEventName().str() ) );
			m_ambientSound.Clear();
		}
	}
}

//-------------------------------------------------------------------------------------------------
// Attach and start playing an ambient sound to this drawable. Calculates states automatically.
//-------------------------------------------------------------------------------------------------
void Drawable::startAmbientSound()
{
	stopAmbientSound();
	BodyDamageType bodyCondition = BODY_PRISTINE;
	Object *obj = getObject();
	if( obj )
	{
		bodyCondition = obj->getBodyModule()->getDamageState();
	}
	startAmbientSound( bodyCondition, TheGlobalData->m_timeOfDay );
}

//-------------------------------------------------------------------------------------------------
/** Stop playing the drawables ambient sound if it has one */
//-------------------------------------------------------------------------------------------------
void	Drawable::stopAmbientSound()
{
	if (m_ambientSound)
		TheAudio->removeAudioEvent(m_ambientSound->getPlayingHandle());
}

//-------------------------------------------------------------------------------------------------
void Drawable::enableAmbientSound( Bool enable )
{
	if( m_ambientSoundEnabled == enable )
	{
		return;
	}

	m_ambientSoundEnabled = enable;
	if( enable )
	{
		startAmbientSound();
	}
	else
	{
		stopAmbientSound();
	}
}

//-------------------------------------------------------------------------------------------------
/** add self to the linked list */
//-------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
void Drawable::prependToList(Drawable **pListHead)
{
	// add the object to the global list
	m_prevDrawable = nullptr;
	m_nextDrawable = *pListHead;
	if (*pListHead)
		(*pListHead)->m_prevDrawable = this;
	*pListHead = this;
}
// GX-SIMID-END

//-------------------------------------------------------------------------------------------------
/** remove self from the linked list */
//-------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
void Drawable::removeFromList(Drawable **pListHead)
{
	if (m_nextDrawable)
		m_nextDrawable->m_prevDrawable = m_prevDrawable;

	if (m_prevDrawable)
		m_prevDrawable->m_nextDrawable = m_nextDrawable;
	else
		*pListHead = m_nextDrawable;
}
// GX-SIMID-END

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void Drawable::updateHiddenStatus()
{
	Bool hidden = m_hidden || m_hiddenByStealth;
	if( hidden )
		TheInGameUI->deselectDrawable( this );

	for (DrawModule** dm = getDrawModules(); *dm; ++dm)
	{
		ObjectDrawInterface* di = (*dm)->getObjectDrawInterface();
		if (di)
			di->setHidden(hidden != 0);
	}

}

//-------------------------------------------------------------------------------------------------
/** Hide or un-hide drawable */
//-------------------------------------------------------------------------------------------------
void Drawable::setDrawableHidden( Bool hidden )
{
	if (hidden != m_hidden)
	{
		m_hidden = hidden;
		updateHiddenStatus();
	}
}

//-------------------------------------------------------------------------------------------------
void Drawable::updateDrawableClipStatus( UnsignedInt shotsRemaining, UnsignedInt maxShots, WeaponSlotType slot )
{
	for (DrawModule** dm = getDrawModules(); *dm; ++dm)
	{
		ObjectDrawInterface* di = (*dm)->getObjectDrawInterface();
		if (di)
			di->updateProjectileClipStatus(shotsRemaining, maxShots, slot);
	}
}

//-------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
void Drawable::updateDrawableSupplyStatus( Int maxSupply, Int currentSupply )
{
	for (DrawModule** dm = getDrawModules(); *dm; ++dm)
	{
		ObjectDrawInterface* di = (*dm)->getObjectDrawInterface();
		if (di)
			di->updateDrawModuleSupplyStatus( maxSupply, currentSupply );
	}
}
// GX-SIMID-END

//-------------------------------------------------------------------------------------------------
void Drawable::notifyDrawableDependencyCleared()
{
	for (DrawModule** dm = getDrawModules(); *dm; ++dm)
	{
		ObjectDrawInterface* di = (*dm)->getObjectDrawInterface();
		if (di)
			di->notifyDrawModuleDependencyCleared();
	}
}

//-------------------------------------------------------------------------------------------------
/** Set as selectable or not. */
//-------------------------------------------------------------------------------------------------
void Drawable::setSelectable( Bool selectable )
{
	// unselect drawable if it is no longer selectable.
	if( !selectable )
		TheInGameUI->deselectDrawable( this );

	for (DrawModule** dm = getDrawModules(); *dm; ++dm)
	{
		ObjectDrawInterface* di = (*dm)->getObjectDrawInterface();
		if (di)
			di->setSelectable(selectable);
	}
}

//-------------------------------------------------------------------------------------------------
/** Return whether or not this Drawable is selectable. */
//-------------------------------------------------------------------------------------------------
Bool Drawable::isSelectable() const
{
	return getObject() && getObject()->isSelectable();
}

//-------------------------------------------------------------------------------------------------
/** Return whether or not this Drawable is selectable as part of a group. */
//-------------------------------------------------------------------------------------------------
Bool Drawable::isMassSelectable() const
{
	return getObject() && getObject()->isMassSelectable();
}

//-------------------------------------------------------------------------------------------------
/** Preload all our assets that we can for all our possible states in this time of day */
//-------------------------------------------------------------------------------------------------
void Drawable::preloadAssets( TimeOfDay timeOfDay )
{

	/// walk all our modules and preload any assets we need to
	for( Int i = 0; i < NUM_DRAWABLE_MODULE_TYPES; ++i )
		for( Module** m = m_modules[i]; m && *m; ++m )
			(*m)->preloadAssets( timeOfDay );

}

//-------------------------------------------------------------------------------------------------
// Simply searches for the first occurrence of a specified client update module.
//-------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
ClientUpdateModule* Drawable::findClientUpdateModule( NameKeyType key )
{
	ClientUpdateModule **clientModules = getClientUpdateModules();
	if( clientModules )
	{
		while( *clientModules )
		{
			if( (*clientModules)->getModuleNameKey() == key )
			{
				return *clientModules;
			}
			// GeneralsX: Advance: the loop never moved past the first module, so a drawable whose first
			// client update module was not the one asked for hung the game here (sweep c085-02). Every match found today is
			// the first module, so no shipped lookup changes.
			++clientModules;
		}
	}
	return nullptr;
}
// GX-SIMID-END

// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void Drawable::crc( Xfer *xfer )
{

}

// ------------------------------------------------------------------------------------------------
/** Xfer the drawable modules
	* Version Info:
	* 1: Initial version */
// ------------------------------------------------------------------------------------------------
void Drawable::xferDrawableModules( Xfer *xfer )
{

	// version
	const XferVersion currentVersion = 1;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	//
	// when using dirty condition flags ... we want to make sure that the modules are updated to
	// the current state of the drawable
	//
#ifdef DIRTY_CONDITION_FLAGS
	if( xfer->getXferMode() == XFER_SAVE )
		getDrawModules();  // will re-evaluate modules that are dirty and update them
#endif

	// xfer number of module types
	UnsignedShort moduleTypes = NUM_DRAWABLE_MODULE_TYPES;
	xfer->xferUnsignedShort( &moduleTypes );

	// xfer each set of modules for each type
	AsciiString moduleIdentifier;
	for( UnsignedShort curModuleType = 0; curModuleType < moduleTypes; ++curModuleType )
	{

		// how many modules are here for this type
		Module **m;
		UnsignedShort moduleCount = 0;
		for( m = m_modules[ curModuleType ]; m && *m; ++m )
			moduleCount++;
		xfer->xferUnsignedShort( &moduleCount );

		// xfer each module data
		if( xfer->getXferMode() == XFER_SAVE )
		{

			// save each module
			for( m = m_modules[ curModuleType ]; m && *m; ++m )
			{

				// write module identifier
				moduleIdentifier = TheNameKeyGenerator->keyToName( (*m)->getModuleTagNameKey() );
				DEBUG_ASSERTCRASH( moduleIdentifier != AsciiString::TheEmptyString,
													 ("Drawable::xferDrawableModules - module name key does not translate to a string!") );
				xfer->xferAsciiString( &moduleIdentifier );

				// begin data block
				xfer->beginBlock();

				// xfer data
				xfer->xferSnapshot( *m );

				// end data block
				xfer->endBlock();

			}

		}
		else
		{
			// read each module
			for( UnsignedShort j = 0; j < moduleCount; ++j )
			{

				// read module identifier
				xfer->xferAsciiString( &moduleIdentifier );
				NameKeyType moduleIdentifierKey = TheNameKeyGenerator->nameToKey(moduleIdentifier);

				// find module in the drawable module list
				Module* module = nullptr;
				for( Module **m = m_modules[curModuleType]; m && *m; ++m )
				{
					if (moduleIdentifierKey == (*m)->getModuleTagNameKey())
					{

						module = *m;
						break;  // exit for m

					}

				}

				// new block of data
				Int dataSize = xfer->beginBlock();

				//
				// if we didn't find the module, it's quite possible that we have removed
				// it from the object definition in a future patch, if that is so, we need to
				// skip the module data in the file
				//
				if( module == nullptr )
				{

					// for testing purposes, this module better be found
					DEBUG_CRASH(( "Drawable::xferDrawableModules - Module '%s' was indicated in file, but not found on Drawable %s %d",
												moduleIdentifier.str(), getTemplate()->getName().str(),getID() ));

					// skip this data in the file
					xfer->skip( dataSize );

				}
				else
				{

					// xfer the data into this module
					xfer->xferSnapshot( module );

				}

				// end of data block
				xfer->endBlock();

			}

		}

	}

}

// ------------------------------------------------------------------------------------------------
/** Xfer method
	* Version Info;
	* 1: Initial version
	* 2: Moved condition state xfer before module xfer so we can restore anim frame
	*    during the module xfer (CBD)
	* 4: Added m_ambientSoundEnabled flag
	* 5: save full mtx, not pos+orient.
	* 6: TheSuperHackers @bugfix Removed m_prevTintStatus because loading its value is unnecessary and undesirable
	*/
// ------------------------------------------------------------------------------------------------
void Drawable::xfer( Xfer *xfer )
{

	// version
#if RETAIL_COMPATIBLE_XFER_SAVE
	const XferVersion currentVersion = 5;
#else
	const XferVersion currentVersion = 6;
#endif
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	//Wow, because the constructor creates the ambient sound, the xfer can
	//change the ID the sound points to, therefore, we must remove it now
	//and restore it in loadPostProcess().
	if( xfer->getXferMode() == XFER_LOAD && m_ambientSound )
	{
		TheAudio->killAudioEventImmediately( m_ambientSound->getPlayingHandle() );
		m_ambientSound.Clear();
	}

	// drawable id
	DrawableID id = getID();
	xfer->xferDrawableID( &id );
	setID( id );

	// condition state, note that when we're loading we need to force a replace of these flags
	if( version >= 2 )
	{

		m_conditionState.xfer( xfer );
		if( xfer->getXferMode() == XFER_LOAD	)
			replaceModelConditionFlags( m_conditionState, TRUE );

	}

	if( version >= 3 )
	{
		if (version >= 5)
		{
			Matrix3D mtx = *getTransformMatrix();
			xfer->xferMatrix3D(&mtx);
			setTransformMatrix(&mtx);
		}
		else
		{
			// position
			Coord3D pos = *getPosition();
			xfer->xferCoord3D( &pos );
			setPosition( &pos );

			// orientation
			Real orientation = getOrientation();
			xfer->xferReal( &orientation );
			setOrientation( orientation );
		}
	}

	// selection flash envelope
	Bool selFlash = (m_selectionFlashEnvelope != nullptr);
	xfer->xferBool( &selFlash );
	if( selFlash )
	{

		// allocate selection flash envelope if we need to
		if( m_selectionFlashEnvelope == nullptr )
			m_selectionFlashEnvelope = newInstance( TintEnvelope );

		// xfer
		xfer->xferSnapshot( m_selectionFlashEnvelope );

	}

	// color tint envelope
	Bool colFlash = (m_colorTintEnvelope != nullptr);
	xfer->xferBool( &colFlash );
	if( colFlash )
	{

		// allocate envelope if we need to
		if( m_colorTintEnvelope == nullptr )
			m_colorTintEnvelope = newInstance( TintEnvelope );

		// xfer
		xfer->xferSnapshot( m_colorTintEnvelope );

	}

	// terrain decal type
	TerrainDecalType decal = getTerrainDecalType();
	xfer->xferUser( &decal, sizeof( TerrainDecalType ) );
	if( xfer->getXferMode() == XFER_LOAD )
		setTerrainDecal( decal );

	// explicit opacity
	xfer->xferReal( &m_explicitOpacity );

	// stealth opacity
	xfer->xferReal( &m_stealthOpacity );

	// effective stealth opacity
	xfer->xferReal( &m_effectiveStealthOpacity );

	// decalOpacityFadeTarget
	xfer->xferReal( &m_decalOpacityFadeTarget );

	// decalOpacityFadeRate
	xfer->xferReal( &m_decalOpacityFadeRate );

	// decalOpacityFadeRate
	xfer->xferReal( &m_decalOpacity );

	// object (if present)
	ObjectID objectID = m_object ? m_object->getID() : INVALID_ID;
	xfer->xferObjectID( &objectID );
	// sanity
	if( xfer->getXferMode() == XFER_LOAD )
	{

		if( m_object )
		{

			if( objectID != m_object->getID() )
			{

				DEBUG_CRASH(( "Drawable::xfer - Drawable '%s' is attached to wrong object '%s'",
											getTemplate()->getName().str(), m_object->getTemplate()->getName().str() ));
				throw SC_INVALID_DATA;

			}


		}
		else
		{

			if( objectID != INVALID_ID )
			{
#ifdef DEBUG_CRASHING
				Object *obj = TheGameLogic->findObjectByID( objectID );

				DEBUG_CRASH(( "Drawable::xfer - Drawable '%s' is not attached to an object but should be attached to object '%s' with id '%d'",
											getTemplate()->getName().str(),
											obj ? obj->getTemplate()->getName().str() : "Unknown",
											objectID ));
#endif
				throw SC_INVALID_DATA;

			}

		}

	}


	// particle
	// we don't need to worry about this, the particle itself will set it upon loading

	// selected
	// we won't worry about selection, we'll let TheInGameUI take care of it all

	// status
	xfer->xferUnsignedInt( &m_status );

	// tint status
	xfer->xferUnsignedInt( &m_tintStatus );

	if (version <= 5)
	{
		// prev tint status
		xfer->xferUnsignedInt( &m_prevTintStatus );

		// TheSuperHackers @bugfix Caball009 21/12/2025 Trigger tinting after loading a save game.
		if (xfer->getXferMode() == XFER_LOAD)
			m_prevTintStatus = 0;
	}

	// fading mode
	xfer->xferUser( &m_fadeMode, sizeof( FadingMode ) );

	// time elapsed fade
	xfer->xferUnsignedInt( &m_timeElapsedFade );

	// time to fade
	xfer->xferUnsignedInt( &m_timeToFade );

	Bool hasLocoInfo = (m_locoInfo != nullptr);
	xfer->xferBool( &hasLocoInfo );
	if (hasLocoInfo)
	{
		if( xfer->getXferMode() == XFER_LOAD && m_locoInfo == nullptr	)
			m_locoInfo = newInstance(DrawableLocoInfo);

		// pitch
		xfer->xferReal( &m_locoInfo->m_pitch );

		// pitch rate
		xfer->xferReal( &m_locoInfo->m_pitchRate );

		// roll
		xfer->xferReal( &m_locoInfo->m_roll );

		// roll rate
		xfer->xferReal( &m_locoInfo->m_rollRate );

		// yaw
		xfer->xferReal( &m_locoInfo->m_yaw );

		// acceleration pitch
		xfer->xferReal( &m_locoInfo->m_accelerationPitch );

		// acceleration pitch rate
		xfer->xferReal( &m_locoInfo->m_accelerationPitchRate );

		// acceleration roll
		xfer->xferReal( &m_locoInfo->m_accelerationRoll );

		// acceleration roll rate
		xfer->xferReal( &m_locoInfo->m_accelerationRollRate );

		// overlap z vel
		xfer->xferReal( &m_locoInfo->m_overlapZVel );

		// overlap z
		xfer->xferReal( &m_locoInfo->m_overlapZ );

		// wobble
		xfer->xferReal( &m_locoInfo->m_wobble );

		// wheel info
		xfer->xferReal( &m_locoInfo->m_wheelInfo.m_frontLeftHeightOffset );
		xfer->xferReal( &m_locoInfo->m_wheelInfo.m_frontRightHeightOffset );
		xfer->xferReal( &m_locoInfo->m_wheelInfo.m_rearLeftHeightOffset );
		xfer->xferReal( &m_locoInfo->m_wheelInfo.m_rearRightHeightOffset );
		xfer->xferReal( &m_locoInfo->m_wheelInfo.m_wheelAngle );
		xfer->xferInt( &m_locoInfo->m_wheelInfo.m_framesAirborneCounter );
		xfer->xferInt( &m_locoInfo->m_wheelInfo.m_framesAirborne );
	}

	// modules
	xferDrawableModules( xfer );

	// stealth look
	xfer->xferUser( &m_stealthLook, sizeof( StealthLookType ) );


	// flash count
	xfer->xferInt( &m_flashCount );

	// flash color
	xfer->xferColor( &m_flashColor );

	// hidden
	xfer->xferBool( &m_hidden );

	// hidden by stealth
	xfer->xferBool( &m_hiddenByStealth );

	// heat vision opacity
	xfer->xferReal( &m_heatVisionOpacity );

	// instance is identity
	xfer->xferBool( &m_instanceIsIdentity );

	// instance matrix
	xfer->xferUser( &m_instance, sizeof( Matrix3D ) );

	// instance scale
	xfer->xferReal( &m_instanceScale );

	// drawable Info - mostly hold FOW related data.
	xfer->xferObjectID(&m_drawableInfo.m_shroudStatusObjectID);

	// we do not need to save m_drawableInfo
	// m_drawableInfo <--- do nothing with this

	// condition state used to be here so we must keep it here for compatibility
	if( version < 2 )
	{

		// sanity, we don't write old versions we can only read them
		DEBUG_ASSERTCRASH( xfer->getXferMode() == XFER_LOAD,
											 ("Drawable::xfer - Writing an old format!!!") );

		// condition state, note that when we're loading we need to force a replace of these flags
		m_conditionState.xfer( xfer );
		if( xfer->getXferMode() == XFER_LOAD	)
			replaceModelConditionFlags( m_conditionState, TRUE );

	}

	// expiration date
	xfer->xferUnsignedInt( &m_expirationDate );

	// icon count
	UnsignedByte iconCount = 0;
	if (hasIconInfo())
	{
		for( UnsignedByte i = 0; i < MAX_ICONS; ++i )
			if( getIconInfo()->m_icon[ i ] )
				iconCount++;
	}
	xfer->xferUnsignedByte( &iconCount );

	// icon data
	AsciiString iconIndexName;
	AsciiString iconTemplateName;
	UnsignedInt iconKeepFrame;
	if( xfer->getXferMode() == XFER_SAVE )
	{

		for( UnsignedByte i = 0; i < MAX_ICONS; ++i )
		{

			// skip empty icon slots
			if( !hasIconInfo() || getIconInfo()->m_icon[ i ] == nullptr )
				continue;

			// icon index name
			iconIndexName.set( drawableIconIndexToName( (DrawableIconType)i ) );
			xfer->xferAsciiString( &iconIndexName );

			// keep till frame
			iconKeepFrame = getIconInfo()->m_keepTillFrame[ i ];
			xfer->xferUnsignedInt( &iconKeepFrame );

			// icon template name
			iconTemplateName = getIconInfo()->m_icon[ i ]->getAnimTemplate()->getName();
			xfer->xferAsciiString( &iconTemplateName );

			// icon data
			xfer->xferSnapshot( getIconInfo()->m_icon[ i ] );

		}

	}
	else
	{
		Int i;

		// destroy any icons that might be present right now in favor of what we'll load from the file
		if (hasIconInfo())
			getIconInfo()->clear();

		// GeneralsX: One entry per icon slot at most; the save writes only the slots in use (sweep c085-01).
		if( iconCount > MAX_ICONS )
			throw SC_INVALID_DATA;

		// read each data segment from the file
		DrawableIconType iconIndex;
		Anim2DTemplate *animTemplate;
		for( i = 0; i < iconCount; ++i )
		{

			// icon index name
			xfer->xferAsciiString( &iconIndexName );
			iconIndex = drawableIconNameToIndex( iconIndexName.str() );
			// GeneralsX: An unknown name gives ICON_INVALID (-1), which indexed m_keepTillFrame[] and
			// m_icon[] one before the arrays. Refuse the save, as an unknown icon template below already does (sweep c085-01).
			if( iconIndex < 0 || iconIndex >= MAX_ICONS )
			{

				DEBUG_CRASH(( "Drawable::xfer - Unknown icon '%s'", iconIndexName.str() ));
				throw SC_INVALID_DATA;

			}

			// keep till frame
			xfer->xferUnsignedInt( &iconKeepFrame );
			getIconInfo()->m_keepTillFrame[ iconIndex ] = iconKeepFrame;

			// icon template name
			xfer->xferAsciiString( &iconTemplateName );
			animTemplate = TheAnim2DCollection->findTemplate( iconTemplateName );
			if( animTemplate == nullptr )
			{

				DEBUG_CRASH(( "Drawable::xfer - Unknown icon template '%s'", iconTemplateName.str() ));
				throw SC_INVALID_DATA;

			}

			// create icon
			getIconInfo()->m_icon[ iconIndex ] = newInstance(Anim2D)( animTemplate, TheAnim2DCollection );

			// icon data
			xfer->xferSnapshot( getIconInfo()->m_icon[ iconIndex ] );

		}

	}

	if( xfer->getXferMode() == XFER_LOAD )
	{
		// On load, we want to set it to none, because stealthlook updates
		// when it changes.  So in the next stealth update, it will be set to
		// it's correct value, and the drawable updated (hide shadows and such).  jba.
		m_stealthLook = STEALTHLOOK_NONE;
		// Also, need to update the hidden status for all sub-modules.
		if (m_hidden || m_hiddenByStealth) {
			updateHiddenStatus();
		}
	}


	//
	// when saving we should never have dirty modules, but when loading we will force the modules
	// to be dirty just to be sure that they get re-evaluated after the load
	//
#ifdef DIRTY_CONDITION_FLAGS
	if( xfer->getXferMode() == XFER_SAVE )
		DEBUG_ASSERTCRASH( m_isModelDirty == FALSE, ("Drawable::xfer - m_isModelDirty is not FALSE!") );
	else
	{
		m_isModelDirty = TRUE;
		s_anyModelDirty = TRUE;
	}
#endif

	if( version >= 4 )
	{
		xfer->xferBool( &m_ambientSoundEnabled );
	}

}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void Drawable::loadPostProcess()
{
		// if we have an object, we don't need to save/load the pos, just restore it.
		// if we don't, we'd better save it!
	if (m_object != nullptr)
	{
		setTransformMatrix(m_object->getTransformMatrix());
	}

	if( m_ambientSoundEnabled )
	{
		startAmbientSound();
	}
	else
	{
		stopAmbientSound();
	}

}

//-------------------------------------------------------------------------------------------------
const Locomotor* Drawable::getLocomotor() const
{
	if (const Object* obj = getObject())
	{
		if (const AIUpdateInterface *ai = obj->getAIUpdateInterface())
		{
			return ai->getCurLocomotor();
		}
	}
	return nullptr;
}

//=================================================================================================
//=================================================================================================
#ifdef DIRTY_CONDITION_FLAGS
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
/*static*/ void Drawable::friend_lockDirtyStuffForIteration()
{
	// GeneralsX: Skip the walk when no drawable can be dirty. The walk's only
	// effect is getDrawModules() running replaceModelConditionStateInDrawable() on a drawable whose
	// m_isModelDirty is set (getModuleList is a plain read), and s_anyModelDirty is FALSE only when none is:
	// every write of m_isModelDirty = true also sets it, and it is cleared here, before the walk, so a drawable
	// dirtied during the walk sets it again and the next lock walks, exactly as it would have.
	// RTS3DScene::Customized_Render takes this lock on every rendered frame, and walked every drawable.
	// Mirrors the Zero Hour change (review of cc426e50c); scripts/test/audit/bench/dirty_lock_walk_skip.cpp checks it.
	if (s_modelLockCount == 0 && s_anyModelDirty)
	{
		if (TheGameClient)	// WB has no GameClient!
		{
			s_anyModelDirty = FALSE;
			for (Drawable* d = TheGameClient->firstDrawable(); d != nullptr; d = d->getNextDrawable())
			{
				// this will force us to update stuff.
				d->getDrawModules();
			}
		}
	}
	++s_modelLockCount;
}
// GX-SIMID-END
#endif

//=================================================================================================
//=================================================================================================
#ifdef DIRTY_CONDITION_FLAGS
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
/*static*/ void Drawable::friend_unlockDirtyStuffForIteration()
{
	if (s_modelLockCount > 0)
		--s_modelLockCount;
}
// GX-SIMID-END
#endif

//=================================================================================================
//=================================================================================================
TintEnvelope::TintEnvelope()
{
	m_attackRate.Set(0,0,0);
	m_decayRate.Set(0,0,0);
	m_peakColor.Set(0,0,0);
	m_currentColor.Set(0,0,0);
	m_envState = ENVELOPE_STATE_REST;
	m_sustainCounter = 0;
	m_affect = FALSE;
}

//-------------------------------------------------------------------------------------------------
const Real FADE_RATE_EPSILON = (0.001f);

//-------------------------------------------------------------------------------------------------
void TintEnvelope::play(const RGBColor *peak, UnsignedInt attackFrames, UnsignedInt decayFrames, UnsignedInt sustainAtPeak )
{
	setPeakColor( peak );

	setAttackFrames( attackFrames );
	setDecayFrames( decayFrames );

	m_envState = ENVELOPE_STATE_ATTACK;
	m_sustainCounter = sustainAtPeak;
	m_affect = TRUE;

	Vector3 delta;
	Vector3::Subtract(m_currentColor, m_peakColor, &delta);

	if ( delta.Length() <= FADE_RATE_EPSILON ) // we are practically already at this color
		m_envState = ENVELOPE_STATE_SUSTAIN;

}

//-------------------------------------------------------------------------------------------------
void TintEnvelope::setAttackFrames(UnsignedInt frames)
{
	Real recipFrames = 1.0f / (Real)MAX(1,frames);
	m_attackRate.Set( m_currentColor );
	Vector3::Subtract( m_peakColor, m_attackRate, &m_attackRate);
	m_attackRate.Scale( Vector3(recipFrames, recipFrames, recipFrames) );
}

//-------------------------------------------------------------------------------------------------
void TintEnvelope::setDecayFrames( UnsignedInt frames )
{
	Real recipFrames = ( -1.0f ) / (Real)MAX(1,frames);
	m_decayRate.Set( m_peakColor );
	m_decayRate.Scale( Vector3(recipFrames, recipFrames, recipFrames) );
}

//-------------------------------------------------------------------------------------------------
void TintEnvelope::update()
{
	// GeneralsX: REST is the steady state of nearly every envelope and never
	// reads the time scale, so return before the out-of-line FramePacer query. Identical output.
	if ( m_envState == ENVELOPE_STATE_REST ) //most likely case
	{
		m_currentColor.Set(0,0,0);
		m_affect = FALSE;
		return;
	}

	// TheSuperHackers @tweak The tint time step is now decoupled from the render update.
	// GeneralsX: GX_SHOT capture clock (GameClient.cpp): the pacer's value unless capturing. Under
	// capture an update with no clock step leaves the envelope exactly as it is - its attack/sustain/decay transitions
	// test the current colour, so an extra draw between two logic frames would otherwise move it on.
	//
	const Real timeScale = gxRenderTimeScale();
	if (timeScale <= 0.0f && gxShotCaptureOn())
		return;

	switch ( m_envState )
	{
		case ( ENVELOPE_STATE_DECAY ) : // much more likely than attack
		{
			const Vector3 decayRate = m_decayRate * timeScale;

			if (decayRate.Length() > m_currentColor.Length() || m_currentColor.Length() <= FADE_RATE_EPSILON)
			{
				// We are at rest
				m_envState = ENVELOPE_STATE_REST;
				m_affect = FALSE;
			}
			else
			{
				// Add the decayRate to the current color
				Vector3::Add( decayRate, m_currentColor, &m_currentColor );
				m_affect = TRUE;
			}
			break;
		}
		case ( ENVELOPE_STATE_ATTACK ) :
		{
			const Vector3 attackRate = m_attackRate * timeScale;
			Vector3 delta;
			Vector3::Subtract(m_currentColor, m_peakColor, &delta);

			if (attackRate.Length() > delta.Length() || delta.Length() <= FADE_RATE_EPSILON)
			{
				// We are at the peak
				if ( m_sustainCounter )
				{
					m_envState = ENVELOPE_STATE_SUSTAIN;
				}
				else
				{
					m_envState = ENVELOPE_STATE_DECAY;
				}
			}
			else
			{
				// Add the attackRate to the current color
				Vector3::Add( attackRate, m_currentColor, &m_currentColor );
				m_affect = TRUE;
			}

			break;
		}
		case ( ENVELOPE_STATE_SUSTAIN ) :
		{
			if ( m_sustainCounter > 0.0f )
				m_sustainCounter -= timeScale;
			else
				release();

			break;
		}
		default:
		{
			//do nothing, we are sustaining until externally triggered to release (decay)
			break;
		}
	}
	// here we transition the color from current to peak to release, according to

}

// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void TintEnvelope::crc( Xfer *xfer )
{

}

// ------------------------------------------------------------------------------------------------
/** Xfer Method
	* Version Info;
	* 1: Initial version
	* 2: TheSuperHackers @tweak Serialize sustain counter as float instead of integer
	* 3: GeneralsX: Serialize sustain counter as double (upstream #3127 reused 2 for it)
	*/
// ------------------------------------------------------------------------------------------------
void TintEnvelope::xfer( Xfer *xfer )
{

	// version
#if RETAIL_COMPATIBLE_XFER_SAVE
	XferVersion currentVersion = 1;
#else
	XferVersion currentVersion = 3;
#endif
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	// attack rate
	xfer->xferUser( &m_attackRate, sizeof( Vector3 ) );

	// decay rate
	xfer->xferUser( &m_decayRate, sizeof( Vector3 ) );

	// peak color
	xfer->xferUser( &m_peakColor, sizeof( Vector3 ) );

	// current color
	xfer->xferUser( &m_currentColor, sizeof( Vector3 ) );

	// sustain counter
	if (version <= 1)
	{
		// TheSuperHackers @info bobtista 23/09/2026 The double counter can represent SUSTAIN_INDEFINITELY exactly.
		UnsignedInt sustainCounter = (UnsignedInt)m_sustainCounter;
		xfer->xferUnsignedInt( &sustainCounter );
		if( xfer->getXferMode() == XFER_LOAD )
		{
			m_sustainCounter = sustainCounter;
		}
	}
	else if (version == 2)
	{
		// GeneralsX: Version 2 is the 4-byte float that RETAIL_COMPATIBLE_XFER_SAVE=0
		// builds wrote before #3127. Upstream reads it as the new double, which consumes the next 4 bytes
		// (affect, state and beyond) of an older save; it keeps its own layout here and only loads.
		Real sustainCounter = (Real)m_sustainCounter;
		xfer->xferReal( &sustainCounter );
		if( xfer->getXferMode() == XFER_LOAD )
		{
			m_sustainCounter = sustainCounter;
		}
	}
	else
	{
		// GeneralsX: Ported by hand from upstream PR #3127 (b44fa780e). Upstream
		// adds Xfer::xferDouble for this; that is a new method in Core/GameEngine/.../Common/Xfer (a
		// digested path), so the same 8 bytes go through xferUser instead: xferReal, xferDouble and
		// xferUser all come down to xferImplementation( ptr, size ). Never written while
		// RETAIL_COMPATIBLE_XFER_SAVE is 1.
		xfer->xferUser( &m_sustainCounter, sizeof( m_sustainCounter ) );
	}

	// affect
	xfer->xferBool( &m_affect );

	// state
	xfer->xferByte( &m_envState );

}

// ------------------------------------------------------------------------------------------------
/** Load Post Process */
// ------------------------------------------------------------------------------------------------
void TintEnvelope::loadPostProcess()
{

}

