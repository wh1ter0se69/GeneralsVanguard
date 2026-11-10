/*
**	GeneralsX: IN-MATCH VOTE KICK, the client's half (GameClient/GXVoteKick.h says what each entry
**	point is for). Included ONCE, at the end of InGameUI.cpp, after GXSpectatorHud.inl (whose top inset it reads).
**
**	READS ONLY: the simulation's vote (TheGameLogic->getGxVoteKick(), GameLogic::gxVoteKickLiving) and the Players. SENDS only
**	the two orders. So nothing here can desync anything: whatever this client shows or misses, the vote is decided by the
**	orders' execution on every peer (Common/GXVoteKick.h).
**
**	THE BANNER: small, top-centre under the players bar, the GeneralsX navy/steel look (tokens from,
**	06/10/2026), in both UI modes - it is game function, not a skin. It is drawn ABOVE the hide-everything switch (F11) like
**	the fast-forward readout: a vote that waits for you must not be hideable by accident. It has its own HIDE, per vote; the
**	keys still answer while it is hidden.
**
**	NO KEYS: no F-key is free (F1-F8 camera
**	views, F9 control bar, F5 Friends, F10/F11 HUD toggles, F12 screenshot), so the ballot is two buttons, a green YES and a
**	red NO, each labelled in words (never colour alone). Every click or tap on the banner is the banner's: none passes through
**	as an order to the units under it. On the iPad a tap arrives as the same left down/up (SDL3GameEngine.cpp's touch state
**	machine) and a long press as a right click, which the banner also swallows; the hit areas reach past the drawn buttons.
*/

#include "GameClient/GXVoteKick.h"
#include "GameClient/Mouse.h"
#include "Common/GXVoteKick.h"
#include "Common/Recorder.h"
#include "GameNetwork/NetworkInterface.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace GXVoteHud
{

static Color navy( Int a )	{ return GameMakeColor( 10, 26, 54, a ); }
static Color steel()		{ return GameMakeColor( 118, 158, 214, 235 ); }
static Color steelDim()		{ return GameMakeColor( 64, 92, 134, 220 ); }
static Color edge()			{ return GameMakeColor( 0, 0, 0, 190 ); }
static Color white()		{ return GameMakeColor( 238, 243, 250, 255 ); }
static Color label()		{ return GameMakeColor( 186, 210, 244, 255 ); }
static Color dim()			{ return GameMakeColor( 140, 156, 180, 255 ); }
static Color red()			{ return GameMakeColor( 255, 112, 112, 255 ); }
static Color amber()		{ return GameMakeColor( 240, 192, 82, 255 ); }
static Color green()		{ return GameMakeColor( 124, 206, 132, 255 ); }

struct Rect { Int x, y, w, h; Bool in( Int px, Int py ) const { return w > 0 && px >= x && px < x + w && py >= y && py < y + h; } };

struct Text { DisplayString *ds; UnicodeString last; GameFont *font; };
enum { T_TITLE, T_TIMER, T_SUB, T_TALLY, T_YES, T_NO, T_HIDE, T_INFO, T_COUNT };

static Text s_text[T_COUNT];
static Real s_scale = 1.0f;
static Int s_screenH = 0;
static GameFont *s_fonts[64][2];

static UnsignedInt s_seenStartId = 0;	///< the vote whose "started" line this client printed
static UnsignedInt s_seenEndId = 0;		///< the vote whose outcome line this client printed
static UnsignedInt s_ballotId = 0;		///< the vote this client already sent its ballot for
static UnsignedInt s_hiddenId = 0;		///< the vote whose banner this player hid
static UnsignedInt s_startAskedId = 0;	///< the vote id current when this client sent a start...
static UnsignedInt s_startAskedFrame = 0;	///< ...and the frame it sent it on (a start in flight is not offered twice)
static Bool s_kicked = FALSE;			///< this client was kicked by a vote: leave
static UnsignedInt s_kickedMs = 0;
static Bool s_left = FALSE;
static Bool s_live = FALSE;				///< the banner was drawn this frame: its buttons may take clicks
static Bool s_drawn = FALSE;			///< the banner (or its test preview) is on screen this frame, at s_panel: the spectator toasts keep off it
static Rect s_yes = { 0, 0, 0, 0 }, s_no = { 0, 0, 0, 0 }, s_hide = { 0, 0, 0, 0 }, s_panel = { 0, 0, 0, 0 };
static Int s_pressed = -1;				///< 0 yes, 1 no, 2 hide, 3 the panel body - a press taken here, so its release is too
static UnsignedInt s_pressedId = 0;		///< the vote that press was made on: a release over a NEWER vote's banner does nothing
static Bool s_rightOwned = FALSE;		///< a right press the banner took, so it takes that press's release - and no other

static Int S( Real v ) { return (Int)( v * s_scale + 0.5f ); }

/// Line heights are 1080p pixels; the point size is solved from a size-20 reference font, as GXSpectatorHud.inl does.
static GameFont *font( Int px, Bool bold )
{
	const Int want = max( 7, min( 63, S( (Real)px ) ) );
	GameFont *&f = s_fonts[want][bold ? 1 : 0];
	if( f == nullptr && TheWindowManager != nullptr )
	{
		GameFont *ref = TheWindowManager->winFindFont( AsciiString( "Arial" ), 20, bold );
		if( ref != nullptr && ref->height > 0 )
			f = TheWindowManager->winFindFont( AsciiString( "Arial" ), max( 4, (Int)( want * 20.0f / ref->height + 0.5f ) ), bold );
	}
	return f;
}

/// One DisplayString per slot; its text is re-laid only when it changes.
static DisplayString *text( Int slot, Int px, Bool bold, const UnicodeString &u )
{
	Text &t = s_text[slot];
	GameFont *f = font( px, bold );
	if( f == nullptr || TheDisplayStringManager == nullptr )
		return nullptr;
	if( t.ds == nullptr )
	{
		t.ds = TheDisplayStringManager->newDisplayString();
		if( t.ds == nullptr )
			return nullptr;
		t.font = nullptr;
	}
	if( t.font != f )
	{
		t.ds->setFont( f );
		t.font = f;
		t.last.clear();
	}
	if( t.last.compare( u ) != 0 || t.last.isEmpty() )
	{
		t.ds->setText( u );
		t.last = u;
	}
	return t.ds;
}

static Int tw( DisplayString *ds ) { return ds ? ds->getWidth() : 0; }
static void put( DisplayString *ds, Int x, Int y, Color c ) { if( ds ) ds->draw( x, y, c, 0 ); }
static void fill( const Rect &r, Color c ) { if( r.w > 0 && r.h > 0 ) TheDisplay->drawFillRect( r.x, r.y, r.w, r.h, c ); }
static void box( const Rect &r, Color c ) { if( r.w > 1 && r.h > 1 ) TheDisplay->drawOpenRect( r.x, r.y, r.w, r.h, 1.0f, c ); }

static UnicodeString nameOf( Int playerIndex )
{
	Player *p = ( ThePlayerList != nullptr ) ? ThePlayerList->getNthPlayer( playerIndex ) : nullptr;
	return p != nullptr ? p->getPlayerDisplayName() : UnicodeString( L"?" );
}

static Int localIndex()
{
	const Player *me = ( ThePlayerList != nullptr ) ? ThePlayerList->getLocalPlayer() : nullptr;
	return me != nullptr ? me->getPlayerIndex() : -1;
}

/// A live network match this client takes part in - not a replay, not single player.
static Bool inLiveMatch()
{
	return TheGameLogic != nullptr && TheGameLogic->isInGame() && TheGameLogic->isInMultiplayerGame() && TheNetwork != nullptr
		&& ThePlayerList != nullptr && ( TheRecorder == nullptr || !TheRecorder->isPlaybackMode() );
}

static UnicodeString clock( UnsignedInt frames )
{
	const UnsignedInt s = ( frames + LOGICFRAMES_PER_SECOND - 1 ) / LOGICFRAMES_PER_SECOND;
	UnicodeString u;
	u.format( L"%u:%02u", s / 60, s % 60 );
	return u;
}

static void line( const RGBColor &c, const UnicodeString &u )
{
	if( TheInGameUI != nullptr )
		TheInGameUI->messageNoFormat( &c, u );
}

static RGBColor rgb( Real r, Real g, Real b ) { RGBColor c; c.red = r; c.green = g; c.blue = b; return c; }

static void cast( Bool yes, const char *how )
{
	const GXVoteKickState &v = TheGameLogic->getGxVoteKick();
	if( !inLiveMatch() || s_ballotId == v.id || !gxVoteKickAwaits( &v, localIndex(), GameLogic::gxVoteKickLiving() ) )
		return;
	GameMessage *msg = TheMessageStream->appendMessage( GameMessage::MSG_GX_VOTEKICK_CAST );
	msg->appendIntegerArgument( (Int)v.id );
	msg->appendBooleanArgument( yes );
	s_ballotId = v.id;
	fprintf( stderr, "[GXVOTE] client ballot id=%u %s by %s at frame %u\n", (unsigned)v.id, yes ? "yes" : "no", how,
		(unsigned)TheGameLogic->getFrame() );
	fflush( stderr );
}

static void startRaw( Int target )
{
	TheMessageStream->appendMessage( GameMessage::MSG_GX_VOTEKICK_START )->appendIntegerArgument( target );
	s_startAskedId = TheGameLogic->getGxVoteKick().id;
	s_startAskedFrame = TheGameLogic->getFrame();
}

static void reset()
{
	s_seenStartId = s_seenEndId = s_ballotId = s_hiddenId = s_startAskedId = s_startAskedFrame = 0;
	s_kicked = s_left = s_live = FALSE;
	s_kickedMs = 0;
	s_pressed = -1;
	s_rightOwned = FALSE;
}

// ---------------------------------------------------------------------------------------------------------------------
// The banner. `v` is the simulation's vote, or the GX_TEST_VOTEKICK_PREVIEW fake (instrumented builds, for pictures).
// ---------------------------------------------------------------------------------------------------------------------
static void drawBanner( const GXVoteKickState &v, UnsignedInt frame, Int me, GXVoteKickMask living, Bool preview )
{
	const Int H = (Int)TheDisplay->getHeight(), W = (Int)TheDisplay->getWidth();
	if( H != s_screenH )
	{
		s_screenH = H;
		s_scale = H / 1080.0f;
		for( Int i = 0; i < 64; ++i )
			s_fonts[i][0] = s_fonts[i][1] = nullptr;
	}

	const GXVoteKickMask electorate = gxVoteKickElectorate( &v, living );
	const Bool awaits = preview || ( gxVoteKickAwaits( &v, me, living ) && s_ballotId != v.id );
	const Bool isTarget = !preview && me == v.target;
	const UnsignedInt left = ( v.endFrame > frame ) ? ( v.endFrame - frame ) : 0;

	UnicodeString title( L"Kick " );
	title.concat( nameOf( v.target ) );
	title.concat( L"?" );
	UnicodeString sub( L"Vote started by " );
	sub.concat( nameOf( v.starter ) );
	UnicodeString tally;
	tally.format( L"%u / %u YES", gxVoteKickCount( v.yes & electorate ), gxVoteKickCount( electorate ) );

	DisplayString *tTitle = text( T_TITLE, 17, TRUE, title );
	DisplayString *tTimer = text( T_TIMER, 17, TRUE, clock( left ) );
	DisplayString *tSub = text( T_SUB, 13, FALSE, sub );
	DisplayString *tTally = text( T_TALLY, 13, TRUE, tally );
	DisplayString *tHide = text( T_HIDE, 12, FALSE, UnicodeString( L"HIDE" ) );
	DisplayString *tYes = text( T_YES, 15, TRUE, UnicodeString( L"YES" ) );
	DisplayString *tNo = text( T_NO, 15, TRUE, UnicodeString( L"NO" ) );
	UnicodeString info;
	if( isTarget )
		info = L"The other players are voting to kick you.";
	else if( s_ballotId == v.id && !preview )
		info = L"You voted. Waiting for the others.";
	else if( !awaits )
		info = L"Waiting for the players to vote.";
	DisplayString *tInfo = info.isEmpty() ? nullptr : text( T_INFO, 13, FALSE, info );
	if( tTitle == nullptr || tTimer == nullptr || tSub == nullptr || tTally == nullptr || tHide == nullptr )
		return;

	const Int padX = S( 12 ), padY = S( 7 ), gap = S( 16 ), rowGap = S( 5 );
	const Int lineH1 = tTitle->getFont() ? tTitle->getFont()->height : S( 17 );
	const Int lineH2 = tSub->getFont() ? tSub->getFont()->height : S( 13 );
	const Int btnW = max( max( tw( tYes ), tw( tNo ) ) + S( 36 ), S( 84 ) ), btnH = max( lineH2 + S( 12 ), S( 28 ) );
	Int w = max( tw( tTitle ) + gap + tw( tTimer ), tw( tSub ) + gap + tw( tTally ) );
	w = max( w, awaits ? ( 2 * btnW + S( 12 ) + gap + tw( tHide ) ) : ( tw( tInfo ) + gap + tw( tHide ) ) );
	w = max( w + 2 * padX, S( 300 ) );
	const Int h = padY + lineH1 + rowGap + lineH2 + rowGap + S( 2 ) + ( awaits ? btnH : lineH2 ) + padY;
	const Int top = max( gxSpectatorHudTopInset(), S( 40 ) ) + S( 8 );
	const Rect r = { ( W - w ) / 2, top, w, h };
	s_panel = r;
	s_drawn = TRUE;

	// The panel: navy glass (never under 182 alpha behind text), a dark outer line, the steel frame. Square corners.
	fill( r, navy( 200 ) );
	const Rect o = { r.x - 1, r.y - 1, r.w + 2, r.h + 2 };
	box( o, edge() );
	box( r, steel() );

	Int y = r.y + padY;
	put( tTitle, r.x + padX, y, isTarget ? red() : white() );
	put( tTimer, r.x + r.w - padX - tw( tTimer ), y, left <= 10u * LOGICFRAMES_PER_SECOND ? amber() : white() );
	y += lineH1 + rowGap;
	put( tSub, r.x + padX, y, label() );
	put( tTally, r.x + r.w - padX - tw( tTally ), y, label() );
	y += lineH2 + rowGap;
	const Rect divider = { r.x + padX, y, r.w - 2 * padX, 1 };
	fill( divider, steelDim() );
	y += S( 2 );

	s_hide.x = r.x + r.w - padX - tw( tHide );
	s_hide.y = y + ( ( awaits ? btnH : lineH2 ) - lineH2 ) / 2;
	s_hide.w = tw( tHide );
	s_hide.h = lineH2;
	put( tHide, s_hide.x, s_hide.y, dim() );
	s_hide.x -= S( 6 ); s_hide.y -= S( 4 ); s_hide.w += S( 12 ); s_hide.h += S( 8 );	// a finger-sized target

	if( awaits )
	{
		const Rect yes = { r.x + padX, y, btnW, btnH }, no = { r.x + padX + btnW + S( 12 ), y, btnW, btnH };
		const Rect *b[2] = { &yes, &no };
		DisplayString *t[2] = { tYes, tNo };
		const Color c[2] = { green(), red() };
		const Color tint[2] = { GameMakeColor( 124, 206, 132, 70 ), GameMakeColor( 255, 112, 112, 70 ) };
		for( Int i = 0; i < 2; ++i )
		{
			fill( *b[i], GameMakeColor( 4, 13, 30, 230 ) );
			fill( *b[i], tint[i] );
			box( *b[i], c[i] );
			const Int lh = t[i] && t[i]->getFont() ? t[i]->getFont()->height : lineH2;
			put( t[i], b[i]->x + ( b[i]->w - tw( t[i] ) ) / 2, b[i]->y + ( b[i]->h - lh ) / 2, white() );
		}
		// The hit areas reach S(8) past the drawn buttons up and down and half the gap sideways: a finger is not a cursor.
		s_yes.x = yes.x - S( 6 ); s_yes.y = yes.y - S( 8 ); s_yes.w = yes.w + S( 12 ); s_yes.h = yes.h + S( 16 );
		s_no.x = no.x - S( 6 ); s_no.y = no.y - S( 8 ); s_no.w = no.w + S( 12 ); s_no.h = no.h + S( 16 );
	}
	else
	{
		s_yes.w = s_no.w = 0;
		put( tInfo, r.x + padX, y, isTarget ? red() : dim() );
	}
	s_live = !preview;
}

#if defined(GX_TEST_INSTRUMENTS)
// ---------------------------------------------------------------------------------------------------------------------
// GeneralsX: GX_TEST_VOTEKICK - drive the vote from a headless peer through the same calls the board
// and the keys make. NEVER in a shipping build.
//   GX_TEST_VOTEKICK="<frame>:<action>[:<arg>...],..."   at the first client frame whose logic frame is >= <frame>
//   start:<slot>          gxVoteKickStart on the player in <slot> (the client's own checks apply, as for the button)
//   forcestart:<slot>     MSG_GX_VOTEKICK_START straight onto the stream - the SIMULATION's refusal is what gets tested
//   yes | no              the ballot the YES / NO button sends (only if a vote waits for this player)
//   forcecast:<id>:<0|1>  MSG_GX_VOTEKICK_CAST with that vote id, whatever is running
//   hide                  this vote's banner HIDE
//   GX_TEST_VOTEKICK_PREVIEW=1  draw a sample banner over any match (pictures; reads nothing from the simulation)
// ---------------------------------------------------------------------------------------------------------------------
struct TestAction { UnsignedInt frame; char what[16]; Int a, b; Bool done; };
static TestAction s_test[16];
static Int s_testCount = -1;

static Int playerOfSlot( Int slot )
{
	const Player *p = ThePlayerList->getPlayerFromSlotIndex( slot );
	return p != nullptr ? p->getPlayerIndex() : -1;
}

static void testPump()
{
	if( s_testCount < 0 )
	{
		s_testCount = 0;
		const char *env = getenv( "GX_TEST_VOTEKICK" );
		AsciiString list( env ? env : "" ), entry;
		while( s_testCount < 16 && list.nextToken( &entry, "," ) )
		{
			TestAction &t = s_test[s_testCount];
			t.a = t.b = -1;
			t.done = FALSE;
			t.what[0] = 0;
			unsigned f = 0;
			if( sscanf( entry.str(), "%u:%15[a-z]:%d:%d", &f, t.what, &t.a, &t.b ) >= 2 )
			{
				t.frame = f;
				++s_testCount;
			}
		}
	}
	const UnsignedInt frame = TheGameLogic->getFrame();
	for( Int i = 0; i < s_testCount; ++i )
	{
		TestAction &t = s_test[i];
		if( t.done || frame < t.frame )
			continue;
		t.done = TRUE;
		fprintf( stderr, "[GXVOTE] test action %s %d %d at frame %u\n", t.what, t.a, t.b, (unsigned)frame );
		fflush( stderr );
		if( strcmp( t.what, "start" ) == 0 )
			gxVoteKickStart( playerOfSlot( t.a ) );
		else if( strcmp( t.what, "forcestart" ) == 0 )
			startRaw( playerOfSlot( t.a ) );
		else if( strcmp( t.what, "yes" ) == 0 || strcmp( t.what, "no" ) == 0 )
			cast( strcmp( t.what, "yes" ) == 0, "test" );
		else if( strcmp( t.what, "forcecast" ) == 0 )
		{
			GameMessage *msg = TheMessageStream->appendMessage( GameMessage::MSG_GX_VOTEKICK_CAST );
			msg->appendIntegerArgument( t.a );
			msg->appendBooleanArgument( t.b != 0 );
		}
		else if( strcmp( t.what, "hide" ) == 0 )
			s_hiddenId = TheGameLogic->getGxVoteKick().id;
	}
}
#endif

static void draw()
{
	s_live = FALSE;
	s_drawn = FALSE;
#if defined(GX_TEST_INSTRUMENTS)
	static const Bool s_preview = getenv( "GX_TEST_VOTEKICK_PREVIEW" ) != nullptr;
	if( s_preview && TheGameLogic != nullptr && TheGameLogic->isInGame() )
	{
		GXVoteKickState fake;
		gxVoteKickReset( &fake );
		fake.id = 1;
		fake.target = ThePlayerList->getPlayerCount() > 2 ? 2 : 1;
		fake.starter = ThePlayerList->getPlayerCount() > 3 ? 3 : 1;
		fake.startFrame = TheGameLogic->getFrame();
		fake.endFrame = fake.startFrame + 27 * LOGICFRAMES_PER_SECOND;
		fake.yes = 0x3;
		drawBanner( fake, fake.startFrame, -1, 0x7, TRUE );
		return;
	}
#endif
	if( !inLiveMatch() )
		return;
	// Under the quit menu the banner steps aside: its translator runs before the window system's, so a banner drawn over a
	// menu would take that menu's clicks.
	if( TheInGameUI != nullptr && TheInGameUI->isQuitMenuVisible() )
		return;
	const GXVoteKickState &v = TheGameLogic->getGxVoteKick();
	if( v.target < 0 || s_hiddenId == v.id )
		return;
	drawBanner( v, TheGameLogic->getFrame(), localIndex(), GameLogic::gxVoteKickLiving(), FALSE );
}

class Translator : public GameMessageTranslator
{
public:
	virtual GameMessageDisposition translateGameMessage( const GameMessage *msg ) override;
	virtual ~Translator() override {}
};

GameMessageDisposition Translator::translateGameMessage( const GameMessage *msg )
{
	const GameMessage::Type t = msg->getType();
	if( msg->getArgumentCount() < 1 || msg->getArgumentDataType( 0 ) != ARGUMENTDATATYPE_PIXEL )
		return KEEP_MESSAGE;
	const ICoord2D at = msg->getArgument( 0 )->pixel;
	// A right click on the banner is no order to the units under it. Only the release of a press the banner took is taken: a
	// camera drag started on the terrain and released over the banner must get its release, or the view keeps scrolling
	// (LookAtTranslator stops SCROLL_RMB on it).
	if( t == GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_UP )
	{
		const Bool owned = s_rightOwned;
		s_rightOwned = FALSE;
		return owned ? DESTROY_MESSAGE : KEEP_MESSAGE;
	}
	if( t == GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_DOWN || t == GameMessage::MSG_RAW_MOUSE_RIGHT_DOUBLE_CLICK )
	{
		s_rightOwned = s_live && s_panel.in( at.x, at.y );
		return s_rightOwned ? DESTROY_MESSAGE : KEEP_MESSAGE;
	}
	if( t != GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_DOWN && t != GameMessage::MSG_RAW_MOUSE_LEFT_DOUBLE_CLICK
		&& t != GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_UP )
		return KEEP_MESSAGE;
	if( t == GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_UP )
	{
		const Int pressed = s_pressed;
		s_pressed = -1;
		if( pressed < 0 )
			return KEEP_MESSAGE;
		const Rect &r = ( pressed == 0 ) ? s_yes : ( pressed == 1 ) ? s_no : s_hide;
		// Bound to the vote the press was made on: one vote can end and another start while the
		// button is held, and the release must not answer the new one.
		if( pressed <= 2 && s_live && r.in( at.x, at.y ) && TheGameLogic->getGxVoteKick().id == s_pressedId
			&& TheGameLogic->getGxVoteKick().target >= 0 )
		{
			if( pressed == 2 )
				s_hiddenId = TheGameLogic->getGxVoteKick().id;
			else
				cast( pressed == 0, "click" );
		}
		return DESTROY_MESSAGE;
	}
	if( !s_live )
		return KEEP_MESSAGE;
	s_pressed = s_yes.in( at.x, at.y ) ? 0 : s_no.in( at.x, at.y ) ? 1 : s_hide.in( at.x, at.y ) ? 2 : s_panel.in( at.x, at.y ) ? 3 : -1;
	s_pressedId = TheGameLogic->getGxVoteKick().id;
	return ( s_pressed >= 0 ) ? DESTROY_MESSAGE : KEEP_MESSAGE;
}

/// TRUE once, when this client was kicked by a vote and has shown it for 5 s: GameClient.cpp then leaves the match through
/// gxKickedExit (GXKickPump.h) - the moderator kick's own exit, a synchronised PLAYERLEAVE.
static Bool pump()
{
	if( TheGameLogic == nullptr || !TheGameLogic->isInGame() || TheGameLogic->getFrame() == 0 )
	{
		reset();
		return FALSE;
	}
	if( !inLiveMatch() )
		return FALSE;
#if defined(GX_TEST_INSTRUMENTS)
	testPump();
#endif
	const GXVoteKickState &v = TheGameLogic->getGxVoteKick();
	const Int me = localIndex();
	if( v.target >= 0 && v.id != s_seenStartId )
	{
		s_seenStartId = v.id;
		UnicodeString u = nameOf( v.starter );
		u.concat( L" started a vote to kick " );
		u.concat( nameOf( v.target ) );
		u.concat( L"." );
		line( rgb( 0.94f, 0.75f, 0.32f ), u );
	}
	if( v.lastId != 0 && v.lastId != s_seenEndId )
	{
		s_seenEndId = v.lastId;
		const UnicodeString who = nameOf( v.lastTarget );
		UnicodeString u;
		switch( v.lastVerdict )
		{
			case GXVK_PASSED:
				if( v.lastTarget == me )
				{
					u = L"You were kicked from the match by a vote.";
					s_kicked = TRUE;
					s_kickedMs = timeGetTime();
				}
				else
				{
					u = who;
					u.concat( L" was kicked by a vote." );
				}
				line( rgb( 1.0f, 0.0f, 0.0f ), u );
				break;
			case GXVK_VETOED:
				u = L"The vote to kick ";
				u.concat( who );
				u.concat( L" was vetoed." );
				line( rgb( 0.86f, 0.9f, 0.96f ), u );
				break;
			case GXVK_TIMED_OUT:
				u = L"The vote to kick ";
				u.concat( who );
				u.concat( L" failed: not everyone voted YES in time." );
				line( rgb( 0.86f, 0.9f, 0.96f ), u );
				break;
			case GXVK_TARGET_GONE:
				u = L"The vote to kick ";
				u.concat( who );
				u.concat( L" ended: they are out of the match." );
				line( rgb( 0.86f, 0.9f, 0.96f ), u );
				break;
			default:
				u = L"The vote to kick ";
				u.concat( who );
				u.concat( L" ended: too few players left to decide." );
				line( rgb( 0.86f, 0.9f, 0.96f ), u );
				break;
		}
	}
	if( s_kicked && !s_left && timeGetTime() - s_kickedMs >= 5000 )
	{
		s_left = TRUE;
		fprintf( stderr, "[GXVOTE] kicked by vote, leaving the match at frame %u\n", (unsigned)TheGameLogic->getFrame() );
		fflush( stderr );
		return TRUE;
	}
	return FALSE;
}

} // namespace GXVoteHud

static void gxVoteKickDraw() { GXVoteHud::draw(); }

/// The banner's rect while it is on screen this frame (drawn before the spectator HUD, so its toasts keep off it); FALSE when not.
Bool gxVoteKickBannerRect( Int *x, Int *y, Int *w, Int *h )
{
	if (!GXVoteHud::s_drawn || GXVoteHud::s_panel.w <= 0) return FALSE;
	*x = GXVoteHud::s_panel.x; *y = GXVoteHud::s_panel.y; *w = GXVoteHud::s_panel.w; *h = GXVoteHud::s_panel.h;
	return TRUE;
}

/// The banner's cached DisplayStrings back to the manager: its destructor asserts that no string is
/// still allocated, so a cache that outlives a match would abort an assertions-enabled build at shutdown. Called from
/// InGameUI::reset (every match end) and ~InGameUI; the next draw makes them again.
static void gxVoteKickFreeStrings()
{
	for( Int i = 0; i < GXVoteHud::T_COUNT; ++i )
	{
		GXVoteHud::Text &t = GXVoteHud::s_text[i];
		if( t.ds != nullptr && TheDisplayStringManager != nullptr )
			TheDisplayStringManager->freeDisplayString( t.ds );
		t.ds = nullptr;
		t.font = nullptr;
		t.last.clear();
	}
}

Bool gxVoteKickPump() { return GXVoteHud::pump(); }

Bool gxVoteKickCanStart( Int playerIndex, UnicodeString *whyNot )
{
	UnicodeString why;
	Bool ok = FALSE;
	const Int me = GXVoteHud::localIndex();
	if( !GXVoteHud::inLiveMatch() )
		why = L"Only in a multiplayer match";
	else
	{
		const GXVoteKickState &v = TheGameLogic->getGxVoteKick();
		const GXVoteKickRules &r = GameLogic::gxVoteKickRules();
		const GXVoteKickMask living = GameLogic::gxVoteKickLiving();
		const UnsignedInt frame = TheGameLogic->getFrame();
		const UnsignedInt cool = gxVoteKickCooldownLeft( &v, me, frame );
		if( ( living & gxVoteKickBit( me ) ) == 0 )
			why = L"Only players still in the match can vote";
		else if( playerIndex == me || ( living & gxVoteKickBit( playerIndex ) ) == 0 )
			why = L"Not a player you can vote to kick";
		else if( v.target >= 0 )
			why = L"A vote is already running";
		else if( GXVoteHud::s_startAskedId == v.id && frame < GXVoteHud::s_startAskedFrame + 2u * LOGICFRAMES_PER_SECOND )
			why = L"Starting the vote...";
		else if( cool > 0 )
		{
			why = L"You can start another vote in ";
			why.concat( GXVoteHud::clock( cool ) );
		}
		else if( gxVoteKickCount( living & ~gxVoteKickBit( playerIndex ) ) < r.minElectors )
			why = L"Needs at least 3 players in the match";
		else
			ok = TRUE;
	}
	if( whyNot != nullptr )
		*whyNot = why;
	return ok;
}

void gxVoteKickStart( Int playerIndex )
{
	if( gxVoteKickCanStart( playerIndex, nullptr ) )
		GXVoteHud::startRaw( playerIndex );
}

GameMessageTranslator *gxNewVoteKickTranslator()
{
	return MSGNEW( "GameClientSubsystem" ) GXVoteHud::Translator;
}
