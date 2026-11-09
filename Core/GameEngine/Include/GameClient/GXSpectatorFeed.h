/*
**	GeneralsX @feature Claude 02/10/2026 - WHO may see the opponent feed: the general's-skill and upgrade banners and
**	the observer table's intel columns (SP, K/D, Cash/m).
**
**	WHY A HEADER OF ITS OWN, AND PURE. Same reason as GXSkillAnnouncePolicy.h and GXUpgradeLedger.h next door: the
**	property it carries - a player who is still playing never sees what his opponents teched into - is invisible to a
**	compiler and to a four-platform verify, so it is only safe if it is testable without an engine.
**	scripts/test/spectator-feed-gate-test.cpp drives it with `c++ -std=c++17 -I Core/GameEngine/Include`.
**
**	WHAT CHANGED. The banners were REPLAY PLAYBACK ONLY (InGameUI::announceGeneralSkill / announceUpgrade each opened
**	with `if( !TheRecorder->isPlaybackMode() ) return;`), while the table's intel columns used a second convention,
**	"playback OR the observer control bar". The owner, spectating a live 6v6 (02/10/2026): "I would want to know as a
**	spectator from the beginning who's using what power ups just like in a replay". So every one of those surfaces now
**	asks this one question, and the two conventions cannot drift apart again.
**
**	THE PREDICATE: replay playback, OR the local player exists and is NOT ACTIVE.
**
**	  * Player::isPlayerActive() is `!m_observer && !m_isPlayerDead` (Player.cpp). It is the engine's own definition of
**	    "not playing", and the same test ControlBar::setControlBarSchemeByPlayer uses to hand a player the observer
**	    control bar - so this matches the bar that already shows the table, without reading a UI flag to decide it.
**	  * An OBSERVER SEAT (m_observer, fixed at match setup; every observer slot maps onto the ReplayObserver player) is
**	    included: it is the person the feature is for, and it already sees the whole map.
**	  * A DEFEATED PLAYER (m_isPlayerDead, set by Player::killPlayer) is included too, deliberately. EA already treats
**	    him as an observer: he gets the observer control bar and its table (ControlBar.cpp), his radar is forced on
**	    (VictoryConditions.cpp), and - the part that makes it fair - EA closed the channel he could relay intel through:
**	    a living receiver DROPS chat from any sender who is not active (ConnectionManager.cpp canSeeChat,
**	    unconditionally), on local defeat VictoryConditions switches his chat to EVERYONE ("can't chat to allies
**	    after death"), and CommandXlat's MSG_META_CHAT_ALLIES will not open ally chat for him (unless -netMinPlayers
**	    0; the default is 1). What stays open is OUT OF BAND - a defeated teammate on voice chat. That hole is not
**	    new: the observer bar he already holds lets him watch the match through any player's eyes
**	    (MSG_META_TOGGLE_PLAYER_OBSERVER), which says more than these banners do - whenever PlayerObserverEnabled is
**	    on, which is its default (OptionPreferences::getPlayerObserverEnabled: an absent key is TRUE) and the owner's
**	    setting. Observer seats only would be a one-word change in viewerMaySeeFeedFor below (isPlayerObserver()
**	    instead of !isPlayerActive()) if a tournament ever wants it.
**	  * Playback stays its own clause rather than being implied by "the ReplayObserver is inactive": a replay whose
**	    side table had no room for the ReplayObserver falls back to an ordinary - ACTIVE - player as the local player
**	    (GameLogic.cpp, the replayViewer fallback), and that viewer must keep his banners.
**	  * No local player outside playback answers NO. Fail closed.
**
**	DETERMINISM. Every input is either recorder mode (identical on every peer) or the local player's own sim state, and
**	the answer only decides what this client DRAWS. Nothing reads it back into the simulation.
**
**	It takes no engine types on purpose; InGameUI.cpp gathers the three inputs.
*/

#pragma once

#ifndef GX_SPECTATOR_FEED_H
#define GX_SPECTATOR_FEED_H

namespace GXSpectatorFeed
{

inline bool viewerMaySeeFeed( bool isPlayback, bool hasLocalPlayer, bool localPlayerIsActive )
{
	if (isPlayback)
		return true;
	return hasLocalPlayer && !localPlayerIsActive;
}

/*
 * The engine glue, here rather than in InGameUI.cpp so the test drives it too (CLAUDE-ONLY review P2, 02/10/2026): it
 * is the one place a dropped or doubled `!` would turn the gate inside out while the predicate above stayed green.
 * PlayerT is the engine's Player in InGameUI.cpp and a stand-in in the test; all it needs is isPlayerActive().
 */
template <class PlayerT>
inline bool viewerMaySeeFeedFor( bool isPlayback, const PlayerT *localPlayer )
{
	return viewerMaySeeFeed( isPlayback, localPlayer != nullptr, localPlayer != nullptr && localPlayer->isPlayerActive() );
}

} // namespace GXSpectatorFeed

#endif // GX_SPECTATOR_FEED_H
