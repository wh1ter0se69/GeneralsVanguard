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

// FILE: W3DLaserDraw.cpp /////////////////////////////////////////////////////////////////////////
// Author: Colin Day, May 2001
// Desc:   W3DLaserDraw
// Updated: Kris Morness July 2002 -- made it data driven and added new features to make it flexible.
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include <stdlib.h>

#include "Common/FramePacer.h"
#include "GameClient/Display.h"
#include "Common/Thing.h"
#include "Common/ThingTemplate.h"
#include "Common/Xfer.h"
#include "GameClient/Color.h"
#include "GameClient/Drawable.h"
#include "GameClient/GameClient.h"
#include "GameClient/ParticleSys.h"
#include "GameClient/RayEffect.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/Module/LaserUpdate.h"
#include "W3DDevice/GameClient/Module/W3DLaserDraw.h"
#include "W3DDevice/GameClient/W3DDisplay.h"
#include "W3DDevice/GameClient/W3DScene.h"
#include "WW3D2/rinfo.h"
#include "WW3D2/camera.h"
#include "WW3D2/segline.h"
#include "WWMath/vector3.h"
#include "WW3D2/assetmgr.h"



// PUBLIC FUNCTIONS ///////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
W3DLaserDrawModuleData::W3DLaserDrawModuleData()
{
	m_innerBeamWidth = 0.0f;         //The total width of beam
	m_outerBeamWidth = 1.0f;         //The total width of beam
  m_numBeams = 1;                 //Number of overlapping cylinders that make the beam. 1 beam will just use inner data.
  m_maxIntensityFrames = 0;				//Laser stays at max intensity for specified time in ms.
  m_fadeFrames = 0;               //Laser will fade and delete.
	m_scrollRate = 0.0f;
	m_tile = false;
	m_segments = 1;
	m_arcHeight = 0.0f;
	m_segmentOverlapRatio = 0.0f;
	m_tilingScalar = 1.0f;
}
// GX-SIMID-END

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
W3DLaserDrawModuleData::~W3DLaserDrawModuleData()
{
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
void W3DLaserDrawModuleData::buildFieldParse(MultiIniFieldParse& p)
{
  ModuleData::buildFieldParse(p);

	static const FieldParse dataFieldParse[] =
	{
		{ "NumBeams",							INI::parseUnsignedInt,					nullptr, offsetof( W3DLaserDrawModuleData, m_numBeams ) },
		{ "InnerBeamWidth",				INI::parseReal,									nullptr, offsetof( W3DLaserDrawModuleData, m_innerBeamWidth ) },
		{ "OuterBeamWidth",				INI::parseReal,									nullptr, offsetof( W3DLaserDrawModuleData, m_outerBeamWidth ) },
		{ "InnerColor",						INI::parseColorInt,							nullptr, offsetof( W3DLaserDrawModuleData, m_innerColor ) },
		{ "OuterColor",						INI::parseColorInt,							nullptr, offsetof( W3DLaserDrawModuleData, m_outerColor ) },
		{ "MaxIntensityLifetime",	INI::parseDurationUnsignedInt,	nullptr, offsetof( W3DLaserDrawModuleData, m_maxIntensityFrames ) },
		{ "FadeLifetime",					INI::parseDurationUnsignedInt,	nullptr, offsetof( W3DLaserDrawModuleData, m_fadeFrames ) },
		{ "Texture",							INI::parseAsciiString,					nullptr, offsetof( W3DLaserDrawModuleData, m_textureName ) },
		{ "ScrollRate",						INI::parseReal,									nullptr, offsetof( W3DLaserDrawModuleData, m_scrollRate ) },
		{ "Tile",									INI::parseBool,									nullptr, offsetof( W3DLaserDrawModuleData, m_tile ) },
		{ "Segments",							INI::parseUnsignedInt,					nullptr, offsetof( W3DLaserDrawModuleData, m_segments ) },
    { "ArcHeight",						INI::parseReal,									nullptr, offsetof( W3DLaserDrawModuleData, m_arcHeight ) },
		{ "SegmentOverlapRatio",	INI::parseReal,									nullptr, offsetof( W3DLaserDrawModuleData, m_segmentOverlapRatio ) },
		{ "TilingScalar",					INI::parseReal,									nullptr, offsetof( W3DLaserDrawModuleData, m_tilingScalar ) },
		{ nullptr, nullptr, nullptr, 0 }
	};
  p.add(dataFieldParse);
}
// GX-SIMID-END


//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
W3DLaserDraw::W3DLaserDraw( Thing *thing, const ModuleData* moduleData ) :
	DrawModule( thing, moduleData ),
	m_line3D(nullptr),
	m_texture(nullptr),
	m_textureAspectRatio(1.0f),
	m_selfDirty(TRUE),
	m_consolidated(FALSE),
	m_lineCount(0),
	m_points(nullptr),
	m_pointDist(nullptr),
	m_pointV(nullptr)
{
	m_drawnStart.zero();
	m_drawnEnd.zero();
	m_effectsPlaced = FALSE;
	Vector3 dummyPos1( 0.0f, 0.0f, 0.0f );
	Vector3 dummyPos2( 1.0f, 1.0f, 1.0f );
	Int i;

	const W3DLaserDrawModuleData *data = getW3DLaserDrawModuleData();

	m_texture = WW3DAssetManager::Get_Instance()->Get_Texture( data->m_textureName.str() );
	if (m_texture)
	{
		if (!m_texture->Is_Initialized())
			m_texture->Init();	//make sure texture is actually loaded before accessing surface.

		SurfaceClass::SurfaceDescription surfaceDesc;
		m_texture->Get_Level_Description(surfaceDesc);
		m_textureAspectRatio = (Real)surfaceDesc.Width/(Real)surfaceDesc.Height;
	}

	//Get the color components for calculation purposes.
	Real innerRed, innerGreen, innerBlue, innerAlpha, outerRed, outerGreen, outerBlue, outerAlpha;
	GameGetColorComponentsReal( data->m_innerColor, &innerRed, &innerGreen, &innerBlue, &innerAlpha );
	GameGetColorComponentsReal( data->m_outerColor, &outerRed, &outerGreen, &outerBlue, &outerAlpha );

	//Make sure our beams range between 1 and the maximum cap.
#ifdef I_WANT_TO_BE_FIRED
// srj sez: this data is const for a reason. casting away the constness because we don't like the values
// isn't an acceptable solution. if you need to constrain the values, do so at parsing time, when
// it's still legal to modify these values. (In point of fact, there's not even really any reason to limit
// the numBeams or segments anymore.)
	data->m_numBeams =		 __min( __max( 1, data->m_numBeams ), MAX_LASER_LINES );
	data->m_segments =		 __min( __max( 1, data->m_segments ), MAX_SEGMENTS );
	data->m_tilingScalar = __max( 0.01f, data->m_tilingScalar );
#endif

	// GeneralsX @perf wh1ter0se 11/08/2026 One polyline per beam instead of numBeams*segments
	// 2-point render objects. Gated on SegmentOverlapRatio == 0 (overlap needs separate quads);
	// arced multi-segment beams (the expensive case, e.g. the Patriot's Segments=20 targeting
	// stream) qualify with retail data. Legacy path is unchanged for everything else.
	m_consolidated = (data->m_arcHeight > 0.0f && data->m_segments > 1 &&
										data->m_segmentOverlapRatio == 0.0f);
	// Escape hatch and A/B lever: GX_LASER_LEGACY=1 forces the legacy per-segment path, so the
	// two implementations can be compared in one binary and the old path stays reachable.
	static Int s_forceLegacy = -1;
	if (s_forceLegacy < 0)
	{
		const char *legacyEnv = getenv("GX_LASER_LEGACY");
		s_forceLegacy = (legacyEnv != nullptr && *legacyEnv != '\0' && *legacyEnv != '0') ? 1 : 0;
	}
	if (s_forceLegacy)
		m_consolidated = FALSE;
	m_lineCount = m_consolidated ? data->m_numBeams : data->m_numBeams * data->m_segments;
	if (m_consolidated)
	{
		m_points = NEW Vector3[ data->m_segments + 1 ];
		m_pointDist = NEW Real[ data->m_segments + 1 ];
		m_pointV    = NEW Real[ data->m_segments + 1 ];
	}

	// GeneralsX experiment knob (default off): additive beams are order-independent against each
	// other; GX_LASER_NOSORT=1 skips the translucent z-sort path so its cost can be measured.
	static Int s_noSort = -1;
	if (s_noSort < 0)
	{
		const char *noSortEnv = getenv("GX_LASER_NOSORT");
		s_noSort = (noSortEnv != nullptr && *noSortEnv != '\0' && *noSortEnv != '0') ? 1 : 0;
	}

	//Allocate the line render objects (per beam when consolidated, per beam per segment otherwise)
	m_line3D = NEW SegmentedLineClass *[ m_lineCount ];

	for( UnsignedInt index = 0; index < m_lineCount; index++ )
	{
		//We don't care about positioning yet until we actually set the points

		// beam index: consolidated lines are one per beam; legacy interleaves beams per segment
		i = m_consolidated ? (Int)index : (Int)( index % data->m_numBeams );

		Real red, green, blue, alpha, width;

		if( data->m_numBeams == 1 )
		{
			width = data->m_innerBeamWidth;
			alpha = innerAlpha;
			red = innerRed * innerAlpha;
			green = innerGreen * innerAlpha;
			blue = innerBlue * innerAlpha;
		}
		else
		{
			//Calculate the scale between min and max values
			//0 means use min value, 1 means use max value
			//0.2 means min value + 20% of the diff between min and max
			Real scale = i / ( data->m_numBeams - 1.0f);

			width		= data->m_innerBeamWidth	+ scale * (data->m_outerBeamWidth - data->m_innerBeamWidth);
			alpha		= innerAlpha							+ scale * (outerAlpha - innerAlpha);
			red			= innerRed								+ scale * (outerRed - innerRed) * innerAlpha;
			green		= innerGreen							+ scale * (outerGreen - innerGreen) * innerAlpha;
			blue		= innerBlue								+ scale * (outerBlue - innerBlue) * innerAlpha;
		}

		m_line3D[ index ] = NEW SegmentedLineClass;

		SegmentedLineClass *line = m_line3D[ index ];
		if( line )
		{
			line->Set_Texture( m_texture );
			line->Set_Shader( ShaderClass::_PresetAdditiveShader );	//pick the alpha blending mode you want - see shader.h for others.
			line->Set_Width( width );
			line->Set_Color( Vector3( red, green, blue ) );
			line->Set_UV_Offset_Rate( Vector2(0.0f, data->m_scrollRate) );	//amount to scroll texture on each draw
			if( m_texture )
			{
				line->Set_Texture_Mapping_Mode(SegLineRendererClass::TILED_TEXTURE_MAP);	//this tiles the texture across the line
			}
			if( s_noSort )
				line->Set_Disable_Sorting( 1 );

			// add to scene
			if (W3DDisplay::m_3DScene != nullptr)
				W3DDisplay::m_3DScene->Add_Render_Object( line );	//add it to our scene so it gets rendered with other objects.

			// hide the render object until the first time we come to draw it and
			// set the correct position
			line->Set_Visible( 0 );
		}

	}

}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
W3DLaserDraw::~W3DLaserDraw()
{
	for( UnsignedInt i = 0; i < m_lineCount; i++ )
	{

		// remove line from scene
		if (W3DDisplay::m_3DScene != nullptr)
			W3DDisplay::m_3DScene->Remove_Render_Object( m_line3D[ i ] );

		// delete line
		REF_PTR_RELEASE( m_line3D[ i ] );

	}

	delete [] m_line3D;
	delete [] m_points;
	delete [] m_pointDist;
	delete [] m_pointV;
	// TheSuperHackers @fix Mauller 11/03/2025 Free reference counted material
	REF_PTR_RELEASE(m_texture);
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
Real W3DLaserDraw::getLaserTemplateWidth() const
{
	const W3DLaserDrawModuleData *data = getW3DLaserDrawModuleData();
	return data->m_outerBeamWidth * 0.5f;
}
// GX-SIMID-END

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Claude 28/09/2026 Render interpolation (Drawable::getInterpolationOffset). LaserUpdate anchors a beam to
// its parent (its position, or a bone) and to its target's position. The positions are LOGIC ones, while the bodies are
// drawn up to a tick behind them, so the beam would come loose from both ends of a moving pair (Codex interp-r1 P2). A bone
// start comes through Drawable::getCurrentWorldspaceClientBonePositions, which moves it to this frame's drawn pose. The rest
// is moved here. Zero Hour only: the base game's lasers have no parent or target.
// Not moved: the end of a weapon laser fired at an explicit impact point. Weapon::createLaser passes the victim AND the
// point, and LaserUpdate::initLaser's point branch keeps no target ID, so nothing here knows the victim. Retail leaves that
// end at the fire-time point while the victim moves on; interpolation adds at most one tick to that on the firing frame.
// Following the victim needs LaserUpdate to keep its ID, which moves the digest: a follow-up for a digest-moving release.
#if RTS_ZEROHOUR
// LaserUpdate keeps its anchors protected and its header is digested (GameLogic/Module), so they are read through pointers
// to member formed in a class derived from it, which the language allows without changing it. Never instantiated.
class GXLaserAnchors : public LaserUpdate
{
public:
	static DrawableID LaserUpdate::* parentID() { return &GXLaserAnchors::m_parentID; }
	static DrawableID LaserUpdate::* targetID() { return &GXLaserAnchors::m_targetID; }
	static AsciiString LaserUpdate::* parentBone() { return &GXLaserAnchors::m_parentBoneName; }
	static ParticleSystemID LaserUpdate::* muzzleFx() { return &GXLaserAnchors::m_particleSystemID; }
	static ParticleSystemID LaserUpdate::* targetFx() { return &GXLaserAnchors::m_targetParticleSystemID; }
};

// LaserUpdate::initLaser puts the muzzle and target effects at its (logic) ends once, where they stay. On the beam's
// first drawn frame under interpolation, put them at the DRAWN ends instead, so beam and effects start together (Codex
// interp-r4, r8: a bone start is already interpolated by then, so this cannot key on the ends having moved). Untouched at
// phase 1, so that picture is today's.
static void gxLaserPlaceEffects(const LaserUpdate *update, const Coord3D &start, const Coord3D &end)
{
	if (TheDisplay->getLogicPhase() == 1.0f)
		return;
	if (ParticleSystem *fx = TheParticleSystemManager->findParticleSystem(update->*GXLaserAnchors::muzzleFx()))
		fx->setPosition(&start);
	if (ParticleSystem *fx = TheParticleSystemManager->findParticleSystem(update->*GXLaserAnchors::targetFx()))
		fx->setPosition(&end);
}
#endif

static void gxLaserDrawnEnds(const LaserUpdate *update, Coord3D &start, Coord3D &end)
{
	start = *update->getStartPos();
	end = *update->getEndPos();
#if RTS_ZEROHOUR
	const DrawableID parentID = update->*GXLaserAnchors::parentID();
	const DrawableID targetID = update->*GXLaserAnchors::targetID();
	if (parentID != INVALID_DRAWABLE_ID)
	{
		if (const Drawable *parent = TheGameClient->findDrawableByID(parentID))
		{
			const AsciiString &bone = update->*GXLaserAnchors::parentBone();
			if (bone.isEmpty())
			{
				start.add(parent->getInterpolationOffset());
			}
			else if (TheDisplay->getLogicPhase() != 1.0f || TheFramePacer->isTimeFrozen() || TheFramePacer->isGameHalted())
			{
				// LaserUpdate caches the bone start in clientUpdate, which GameClient::update skips while time is frozen or
				// the network halts the game, and the body keeps moving along its interpolation meanwhile: read the bone as
				// drawn this frame instead (Codex interp-r7). At phase 1 in normal play, LaserUpdate's own value: today's.
				Matrix3D bonePose;
				if (parent->getCurrentWorldspaceClientBonePositions(bone.str(), bonePose))
					start.set(bonePose.Get_X_Translation(), bonePose.Get_Y_Translation(), bonePose.Get_Z_Translation());
			}
		}
	}
	if (targetID != INVALID_DRAWABLE_ID)
	{
		if (const Drawable *target = TheGameClient->findDrawableByID(targetID))
			end.add(target->getInterpolationOffset());
	}
#endif
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void W3DLaserDraw::doDrawModule(const Matrix3D* transformMtx)
{
	//UnsignedInt currentFrame = TheGameClient->getFrame();
	const W3DLaserDrawModuleData *data = getW3DLaserDrawModuleData();

	//Get the updatemodule that drives it...
	Drawable *draw = getDrawable();
	static NameKeyType key_LaserUpdate = NAMEKEY( "LaserUpdate" );
	LaserUpdate *update = (LaserUpdate*)draw->findClientUpdateModule( key_LaserUpdate );
	if( !update )
	{
		DEBUG_CRASH( ("W3DLaserDraw::doDrawModule() expects its owner drawable %s to have a ClientUpdate = LaserUpdate module.", draw->getTemplate()->getName().str() ));
		return;
	}

	// GeneralsX: the ends where the bodies are drawn this frame; they move between logic ticks, so a change rebuilds too.
	Coord3D drawnStart, drawnEnd;
	gxLaserDrawnEnds(update, drawnStart, drawnEnd);
	const Bool drawnMoved = memcmp(&drawnStart, &m_drawnStart, sizeof(Coord3D)) != 0 || memcmp(&drawnEnd, &m_drawnEnd, sizeof(Coord3D)) != 0;
#if RTS_ZEROHOUR
	// In time for the effects' first emission: draw modules run in W3DDisplay::draw's updateViews() (W3DView::update's
	// draw walk -> Drawable::draw), and the particle systems update after it, in the same frame (the LORENZEN call). The
	// systems were created by the logic update since the last frame, so none has emitted from the logic ends yet.
	if (!m_effectsPlaced)
	{
		m_effectsPlaced = TRUE;
		gxLaserPlaceEffects(update, drawnStart, drawnEnd);
	}
#endif

	//If the update has moved the laser, it requires a reset of the laser.
	if (update->isDirty() || m_selfDirty || drawnMoved)
	{
		update->setDirty(false);
		m_selfDirty = false;
		m_drawnStart = drawnStart;
		m_drawnEnd = drawnEnd;

		// GeneralsX @perf wh1ter0se 11/08/2026 Consolidated path: the whole arc is one polyline
		// of (segments+1) points per beam - one geometry rebuild, one VB/IB lock pair and one
		// sort insertion per beam per frame, instead of one of each per segment.
		if (m_consolidated)
		{
			Coord3D lineStart, lineEnd, lineVector;
			lineStart.set( drawnStart );
			lineEnd.set( drawnEnd );

			lineVector.set( lineEnd );
			lineVector.sub( lineStart );
			Real lineLength = lineVector.length();

			// A zero-length laser (start == end, e.g. before LaserUpdate ever positioned it)
			// degenerates every segment to a point: the arc math divides by halfLength == 0 and
			// the segment renderer normalizes a zero vector (upstream #257). An epsilon nudge on
			// the end point removes the degenerate geometry; the arc is suppressed with it, because
			// applying the full ArcHeight to a hairline chord would raise its midpoint by the whole
			// arc and draw a vertical spike (cross-model review caught that). The guarded legacy
			// path produces zero height in this case, and this now matches it.
			Bool degenerate = FALSE;
			if( lineLength < 0.01f )
			{
				degenerate = TRUE;
				lineEnd.x += 0.01f;
				lineVector.set( lineEnd );
				lineVector.sub( lineStart );
				lineLength = lineVector.length();
			}
			Real halfLength = lineLength * 0.5f;
			const Real arcHeight = degenerate ? 0.0f : data->m_arcHeight;

			Real polyLength = 0.0f;
			for( UnsignedInt pt = 0; pt <= data->m_segments; pt++ )
			{
				Real t = pt / ((Real)data->m_segments);
				Coord3D pos;
				pos.x = lineStart.x + lineVector.x * t;
				pos.y = lineStart.y + lineVector.y * t;
				pos.z = lineStart.z + lineVector.z * t;

				// same cos falloff as the legacy per-endpoint math: the distance from the line
				// middle along the chord is |halfLength - t * lineLength|
				Real dist = fabs( halfLength - t * lineLength );
				Real scaledRadians = dist / halfLength * PI * 0.5f;	// halfLength > 0 by the nudge above
				pos.z += cos( scaledRadians ) * arcHeight;

				//This makes the laser skim the ground rather than penetrate it!
				m_points[ pt ].Set( pos.x, pos.y,
					MAX( pos.z, 2.0f + TheTerrainLogic->getGroundHeight( pos.x, pos.y ) ) );

				if( pt > 0 )
				{
					Vector3 seg;
					Vector3::Subtract( m_points[ pt ], m_points[ pt - 1 ], &seg );
					polyLength += seg.Length();
				}
				// Arc length from the start of the beam to this point, AFTER the arc and the
				// terrain clamp have moved it - which is the whole reason the segments are not
				// equal in length and a single tile factor cannot describe them.
				m_pointDist[ pt ] = polyLength;
			}

			for( Int beam = data->m_numBeams - 1; beam >= 0; beam-- )
			{
				Real width;
				if( data->m_numBeams == 1 )
				{
					width = data->m_innerBeamWidth * update->getWidthScale();
				}
				else
				{
					Real scale = beam / ( data->m_numBeams - 1.0f );
					width = (data->m_innerBeamWidth + scale * (data->m_outerBeamWidth - data->m_innerBeamWidth)) * update->getWidthScale();
				}

				m_line3D[ beam ]->Set_Width( width );
				m_line3D[ beam ]->Set_Points( data->m_segments + 1, m_points );

				if( m_texture && data->m_tile && width > 0.0f )
				{
					// GeneralsX @bugfix wh1ter0se 11/08/2026 Map the texture by ARC LENGTH, not by
					// point index.
					//
					// The first consolidated version set one tile factor from the MEAN segment
					// length, because that is the only thing a single scalar can express. But the
					// renderer advances V once per point, so every segment then received the same
					// V span whatever its real length: on a beam that is arced or clamped to
					// terrain the texture stretched across the long segments and bunched up on the
					// short ones. Uniform beams were unaffected, which is exactly why the A/B
					// against the legacy path did not show it.
					//
					// Per-point V from the cumulative distance is the same quantity the legacy
					// per-segment path computed - length/width*aspect*tiling - just accumulated
					// instead of restarted. Accumulating also removes a seam the legacy path had
					// at every joint, where V snapped back to 0 mid-texture.
					// V is scaled into its own buffer rather than in place: each beam has its own
					// width and therefore its own scale, and scaling m_pointDist then dividing it
					// back would leave every later beam working from slightly rounded distances.
					const Real vScale = m_textureAspectRatio * data->m_tilingScalar / width;
					for( UnsignedInt pt = 0; pt <= data->m_segments; pt++ )
						m_pointV[ pt ] = m_pointDist[ pt ] * vScale;

					m_line3D[ beam ]->Set_Point_V_Coords( data->m_segments + 1, m_pointV );
				}
				else
				{
					m_line3D[ beam ]->Clear_Point_V_Coords();
				}
			}
			return;
		}

		Vector3 laserPoints[ 2 ];

		for( UnsignedInt segment = 0; segment < data->m_segments; segment++ )
		{
			if( data->m_arcHeight > 0.0f && data->m_segments > 1 )
			{
				//CALCULATE A CURVED LINE BASED ON TOTAL LENGTH AND DESIRED HEIGHT INCREASE
				//To do this we will use a portion of the cos wave ranging between -0.25PI
				//and +0.25PI. 0PI is 1.0 and 0.25PI is 0.70 -- resulting in a somewhat
				//gentle curve depending on the line height and length. We also have to make
				//the line *level* for this phase of the calculations.

				//Get the desired direct line
				Coord3D lineStart, lineEnd, lineVector;
				lineStart.set( drawnStart );
				lineEnd.set( drawnEnd );
				//This is critical -- in the case we have sloped lines (at the end, we'll fix it)
//				lineEnd.z = lineStart.z;

				//Get the length of the line
				lineVector.set( lineEnd );
				lineVector.sub( lineStart );
				Real lineLength = lineVector.length();

				//Get the middle point (we'll use this to determine how far we are from
				//that to calculate our height -- middle point is the highest).
				Coord3D lineMiddle;
				lineMiddle.set( lineStart );
				lineMiddle.add( lineEnd );
				lineMiddle.scale( 0.5 );

				//The half length is used to scale with the distance from middle to
				//get our cos( 0 to 0.25 PI) cos value
				Real halfLength = lineLength * 0.5f;

				//Now calculate which segment we will use.
				Real startSegmentRatio = segment / ((Real)data->m_segments);
				Real endSegmentRatio = (segment + 1.0f) / ((Real)data->m_segments);

				//Offset the segment ever-so-slightly to minimize overlap -- only apply
				//to segments that are not the start/end point
				if( segment > 0 )
				{
					startSegmentRatio -= data->m_segmentOverlapRatio;
				}
				if( segment < data->m_segments - 1 )
				{
					endSegmentRatio += data->m_segmentOverlapRatio;
				}

				//Calculate our start segment position on the *ground*.
				Coord3D segmentStart, segmentEnd, vector;
				vector.set( lineVector );
				vector.scale( startSegmentRatio );
				segmentStart.set( lineStart );
				segmentStart.add( vector );

				//Calculate our end segment position on the *ground*.
				vector.set( lineVector );
				vector.scale( endSegmentRatio );
				segmentEnd.set( lineStart );
				segmentEnd.add( vector );

				//--------------------------------------------------------------------------------
				//Now at this point, we have our segment line in the level positions that we want.
				//Calculate the raised height for the start/end segment positions using cosine.
				//--------------------------------------------------------------------------------

				//Calculate the distance from midpoint for the start positions.
				vector.set( lineMiddle );
				vector.sub( segmentStart );
				Real dist = vector.length();
				// GeneralsX @bugfix wh1ter0se 11/08/2026 Guard the zero-length laser (start == end,
				// e.g. before LaserUpdate ever positioned it): dist/halfLength is 0/0 == NaN, which
				// poisons the point data handed to SegmentedLineClass (upstream #257 / PR #2972).
				Real height = 0.0f;
				if( halfLength > 0.0f )
				{
					Real scaledRadians = dist / halfLength * PI * 0.5f;
					height = cos( scaledRadians );
				}
				height *= data->m_arcHeight;
				segmentStart.z += height;

				//Now do the same thing for the end position.
				vector.set( lineMiddle );
				vector.sub( segmentEnd );
				dist = vector.length();
				// GeneralsX @bugfix wh1ter0se 11/08/2026 Same zero-length guard as above.
				height = 0.0f;
				if( halfLength > 0.0f )
				{
					Real scaledRadians2 = dist / halfLength * PI * 0.5f;
					height = cos( scaledRadians2 );
				}
				height *= data->m_arcHeight;
				segmentEnd.z += height;

				//This makes the laser skim the ground rather than penetrate it!
				laserPoints[ 0 ].Set( segmentStart.x, segmentStart.y,
					MAX( segmentStart.z, 2.0f + TheTerrainLogic->getGroundHeight(segmentStart.x, segmentStart.y) ) );
				laserPoints[ 1 ].Set( segmentEnd.x, segmentEnd.y,
					MAX( segmentEnd.z, 2.0f + TheTerrainLogic->getGroundHeight(segmentEnd.x, segmentEnd.y) ) );

			}
			else
			{
				//No arc -- way simpler!
				laserPoints[ 0 ].Set( drawnStart.x, drawnStart.y, drawnStart.z );
				laserPoints[ 1 ].Set( drawnEnd.x, drawnEnd.y, drawnEnd.z );
			}

			//Get the color components for calculation purposes.
			Real innerRed, innerGreen, innerBlue, innerAlpha, outerRed, outerGreen, outerBlue, outerAlpha;
			GameGetColorComponentsReal( data->m_innerColor, &innerRed, &innerGreen, &innerBlue, &innerAlpha );
			GameGetColorComponentsReal( data->m_outerColor, &outerRed, &outerGreen, &outerBlue, &outerAlpha );

			for( Int i = data->m_numBeams - 1; i >= 0; i-- )
			{

				Real alpha, width;
				int index = segment * data->m_numBeams + i;

				if( data->m_numBeams == 1 )
				{
					width = data->m_innerBeamWidth * update->getWidthScale();
					alpha = innerAlpha;
				}
				else
				{
					//Calculate the scale between min and max values
					//0 means use min value, 1 means use max value
					//0.2 means min value + 20% of the diff between min and max
					Real scale = i / ( data->m_numBeams - 1.0f);
					Real ultimateScale = update->getWidthScale();
					width		= (data->m_innerBeamWidth	+ scale * (data->m_outerBeamWidth - data->m_innerBeamWidth));
					width *= ultimateScale;
					alpha		= innerAlpha							+ scale * (outerAlpha - innerAlpha);
				}


				//Calculate the number of times to tile the line based on the height of the texture used.
				if( m_texture && data->m_tile )
				{
					//Calculate the length of the line.
					Vector3 lineVector;
					Vector3::Subtract( laserPoints[1], laserPoints[0], &lineVector );
					Real length = lineVector.Length();

					//Adjust tile factor so texture is NOT stretched but tiled equally in both width and length.
					Real tileFactor = length/width*m_textureAspectRatio*data->m_tilingScalar;

					//Set the tile factor
					m_line3D[ index ]->Set_Texture_Tile_Factor( tileFactor );	//number of times to tile texture across each segment
				}

				m_line3D[ index ]->Set_Width( width );
				m_line3D[ index ]->Set_Points( 2, &laserPoints[0] );
			}
		}
	}
}

// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void W3DLaserDraw::crc( Xfer *xfer )
{

	// extend base class
	DrawModule::crc( xfer );

}

// ------------------------------------------------------------------------------------------------
/** Xfer method
	* Version Info:
	* 1: Initial version */
// ------------------------------------------------------------------------------------------------
void W3DLaserDraw::xfer( Xfer *xfer )
{

	// version
	const XferVersion currentVersion = 1;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	// extend base class
	DrawModule::xfer( xfer );

	// Kris says there is no data to save for these, go ask him.
	// m_selfDirty is not saved, is runtime only

}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void W3DLaserDraw::loadPostProcess()
{

	// extend base class
	DrawModule::loadPostProcess();

	m_selfDirty = true;	// so we update the first time after reload

}
