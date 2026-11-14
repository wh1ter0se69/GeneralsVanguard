#ifndef __GXROSTERTONE_H_
#define __GXROSTERTONE_H_

// The name colour of a person in the lobby's online-player list, before the role (admin crimson, moderator purple)
// is applied over it: you, ignored, friend, everyone else. Plain 0x00RRGGBB so a test can include this alone.
//
// Ordinary players are white and friends blue. Both friend blues clear 4.5:1 against the list's dark background
// and sit well away from the moderator purple (hue 210 vs 266 degrees); the name-only friend is the dimmer one.

enum
{
	GX_ROSTER_TONE_PLAYER         = 0x00FFFFFFu,
	GX_ROSTER_TONE_ME             = 0x0078FF78u,
	GX_ROSTER_TONE_IGNORED        = 0x006E6E6Eu,
	GX_ROSTER_TONE_FRIEND         = 0x004696FFu,	///< a friend known by durable id
	GX_ROSTER_TONE_FRIEND_BY_NAME = 0x005F8CD7u		///< a name that only looks like a friend's
};

inline unsigned int gxRosterTone( bool me, bool ignored, bool isFriend, bool friendByName )
{
	if( me )
		return GX_ROSTER_TONE_ME;
	if( ignored )
		return GX_ROSTER_TONE_IGNORED;
	if( isFriend )
		return friendByName ? GX_ROSTER_TONE_FRIEND_BY_NAME : GX_ROSTER_TONE_FRIEND;
	return GX_ROSTER_TONE_PLAYER;
}

#endif // __GXROSTERTONE_H_
