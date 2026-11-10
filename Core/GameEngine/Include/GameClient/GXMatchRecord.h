/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 GeneralsX contributors
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

// GeneralsX: The GXM1 in-match record envelope - the format half.
//
// WHAT THIS IS FOR. In-game chat is AUDIENCE-SCOPED: InGameChat.cpp builds a playerMask per chat
// type and ConnectionManager.cpp:1028 enforces it on receive. The relay's GXCHT carries no mask and
// relay.js broadcasts an accepted chat line to every member of the room, so mirroring in-match chat
// through the plain chat path would put ally and private conversation in plaintext on the wire to
// every player in the match. This envelope carries the mask so the relay can RECORD the line to its
// event sink and emit no GXSAY at all - nothing reaches a player who was not already going to
// receive it.
//
//   GXREC <room> GXM3 <mask:16 hex lowercase> <slot:2 hex> <chan:[eapd]> <flags:1 hex> <utf8 text>
//   (GeneralsX: scale32: THIS CLIENT NOW WRITES GXM3. 34 slots need a 34-bit mask; the contract with
//   the relay (D scale32-relay) is a fixed 16-digit field = a 64-bit RelayMaskType, slot at most 0x21, header 29 bytes, text
//   budget 171. GXM2 - "GXREC <room> GXM2 <mask:4 hex> <slot:2 hex> <chan> <flags> <text>", header 17, text 183, mask at most
//   0x3FFF, slot at most 0x0D - is what 14-slot clients wrote; this header no longer parses or writes it, the relay still reads it.)
//   (GXM1 was "GXCHT <room> GXM1 <mask:3 hex> ...", and briefly "GXREC <room> GXM1 ..." - see below)
//
// GeneralsX: GXREC, NOT GXCHT. The envelope used to ride GXCHT, which every relay
// older than parseMatchRecord fans out to the room as chat - so a client that wrongly believed its relay
// records (a relay rolled back mid-match) published the line it was protecting. An unknown GXREC is dropped
// by every relay since 07/08/2026 instead (relay.js forward()). The relay still reads GXCHT GXM1 from a
// client that predates this, and GXREC GXM1 from one that predates GXM2.
//
// GXM1 IS THE MAGIC AND THE VERSION AT ONCE. A version bump is the new literal GXM2, never a new
// field: an anchored, exact-width grammar is the only thing standing between a record and a chat
// line a player typed by hand, and a variable-width header cannot be anchored.
//
// GeneralsX: AND THIS IS THAT BUMP. GXM1 carried a three-digit mask and a slot
// of at most 0x0b, because MAX_SLOTS was 12. Fourteen slots (12 players + 2 observers) need a fourteen-bit
// mask - four digits - so this client writes GXM2, which differs from GXM1 in the mask width and the
// bounds alone - and in its CARRIER: GXM2 rides "GXREC", never "GXCHT", because a relay that knows only
// GXM1 would hand a GXM2 line under GXCHT to parseChat and deliver it to the room, where under GXREC it
// drops it (see GX_MATCH_RECORD_RELAY_GATE_PENDING). The relay parses GXM1 under GXCHT or GXREC and GXM2
// under GXREC only (relay.js RECORD_TAG), so a client that predates GXM2 keeps recording too.
//
// HEADER ONLY AND ENGINE-FREE, deliberately, following GameClient/ChatRateLimit.h next door. It
// includes nothing at all, so it is testable with no engine, no display and no network
// (scripts/test/gxm1-envelope-test.cpp), and adding it costs no CMake edit.
//
// THE BUDGET IS THE WHOLE REASON THIS FILE HAS ARITHMETIC IN IT. Transport::sendRelayMatchRecord (sendRelayChat's rules) caps the
// text it will send at GX_CHAT_TEXT_MAX = 200 BYTES (Transport.h:720, in-digest and immovable) and
// counts bytes, not code points, so a naive cap splits a UTF-8 sequence in half. The header costs
// exactly 17 of those bytes (GXM2), which leaves 183 - and the truncation below happens on a CODEPOINT
// boundary so a clipped line is still valid UTF-8 rather than a replacement character in evidence.

#pragma once

/// Bytes the envelope header costs, after "GXREC <room> ": GXM3(4) + 5 spaces + mask(16) + slot(2)
/// + chan(1) + flags(1) = 29 (GXM2's four-digit mask made it 17, GXM1's three-digit one 16).
/// Asserted, not assumed - see gxMatchBuildEnvelope's own check and the test that reads the produced
/// line back.
static const int GX_MATCH_HEADER_BYTES = 29;

/// UTF-8 bytes of chat text the client may put in one envelope. Transport.h:720's
/// GX_CHAT_TEXT_MAX (200) minus GX_MATCH_HEADER_BYTES. If that 200 ever moves, this must move with
/// it; scripts/test/gxm1-reach-test.sh greps Transport.h for it so the drift is loud.
static const int GX_MATCH_TEXT_BUDGET = 171;	// GXM3; 183 under GXM2

/// Header + budget + NUL. The size of the buffer gxMatchBuildEnvelope writes into.
static const int GX_MATCH_LINE_MAX = GX_MATCH_HEADER_BYTES + GX_MATCH_TEXT_BUDGET + 1;

/// 34 slots, one bit each - NetworkDefs.h ALL_SLOTS_MASK = (1 << MAX_SLOTS) - 1 (MAX_SLOTS = 34). This header includes
/// nothing, so the value is spelled out; a static_assert in the includer (GXMatchChat.h) ties it to ALL_SLOTS_MASK.
static const unsigned long long GX_MATCH_MASK_MAX = 0x3FFFFFFFFULL;
/// The highest slot index, 0x21 = 33 - the same MAX_PLAYER.
static const int GX_MATCH_SLOT_MAX = 0x21;

/// Does this GX_MATCH_RECORD value turn recording ON? `value` is getenv's answer, so a NULL means
/// the variable is not set at all.
///
/// GeneralsX: DARK BY DEFAULT, AND THE POLARITY IS THE WHOLE FIX.
///
/// This used to be the other way round: recording was on unless the variable said "0". That default
/// is the leak this envelope exists to prevent, wearing the envelope's own clothes. The envelope is
/// safe ONLY against a relay that implements parseMatchRecord; against one that does not - a
/// rollback, a relay named in somebody's Options.ini, an operator who has not upgraded - the
/// datagram is 28 bytes of well-formed GXCHT, under the relay's MAX_CHAT_TEXT (100) and MAX_CHAT_LEN
/// (320), so parseChat accepts it and the room is sent ally and private chat in plaintext. Shipping
/// that ON meant the failure mode was the default and the safe state needed an action.
///
/// SO THE POLARITY IS NOT A PREFERENCE, IT IS THE HOUSE PATTERN. Transport.h:709 states it for the
/// relay-auth word and Transport.h:393 states it for direct peer-to-peer: dark until the relay says
/// otherwise, because a client that cannot tell an old relay from a new one must assume old. The
/// difference here, and it is a real shortfall rather than a design choice, is WHO does the saying.
/// Since 25/09/2026 the relay says it, as a hint - see GX_MATCH_RECORD_RELAY_GATE_PENDING below.
///
/// EXACTLY "1", not "anything but 0". "true", "yes", "on", "01" and an empty string all read as OFF.
/// A gate whose enable is fuzzy is a gate that turns itself on for a typo, and this one publishes
/// chat when it is wrong.
inline int gxMatchRecordOptInFromEnv( const char *value )
{
	if( value == 0 )
		return 0;						// unset is the default, and the default is dark
	return ( value[0] == '1' && value[1] == '\0' ) ? 1 : 0;
}

/// WHAT WAS MISSING, written on the code rather than in a dropoff: a relay->client statement that the relay
/// records the envelope, so that a client need not rely on the GX_MATCH_RECORD environment variable nobody sets.
///
/// GeneralsX: LANDED, AND IT IS A HINT, NOT THE GATE. The word is 'gxm1' on GXAUTH
/// (relay.js GXM1_CAP) - "this relay records the in-match envelope", GXM1 or GXM2 - read by the lobby presence
/// socket and by the match transport (GXMatchChat.h gxMatchRecordEnabled). But no capability can take
/// recording dark "within one keepalive" of a rollback: the presence socket is suspended for the match and never
/// hears the old relay, and even a word on the match transport arrives one round trip after the old relay starts
/// accepting that transport's datagrams. So the fail-closed property is the TAG: the envelope goes out as GXREC,
/// which a relay that does not record - or cannot parse this version - DROPS (see the header note at the top of
/// this file), so a stale word costs a line missing from the transcript, never a line in the room.
static const int GX_MATCH_RECORD_RELAY_GATE_PENDING = 0;

/// Encode one Unicode code point as UTF-8. Returns the bytes written, or 0 when the code point is
/// not encodable or does not fit.
///
/// ZERO IS NOT AN ERROR CODE THE CALLER MAY IGNORE - it is the truncation signal. A caller that
/// treated a short buffer as "write what fits" would emit a partial sequence, which is precisely
/// the failure the codepoint-boundary rule exists to prevent.
inline int gxMatchUtf8Encode( unsigned int cp, char *out, int outSize )
{
	if( out == 0 || outSize <= 0 )
		return 0;
	if( cp >= 0xD800u && cp <= 0xDFFFu )
		return 0;						// a lone surrogate is not a character in any encoding
	if( cp > 0x10FFFFu )
		return 0;

	if( cp < 0x80u )
	{
		out[0] = (char)cp;
		return 1;
	}
	if( cp < 0x800u )
	{
		if( outSize < 2 ) return 0;
		out[0] = (char)( 0xC0u | ( cp >> 6 ) );
		out[1] = (char)( 0x80u | ( cp & 0x3Fu ) );
		return 2;
	}
	if( cp < 0x10000u )
	{
		if( outSize < 3 ) return 0;
		out[0] = (char)( 0xE0u | ( cp >> 12 ) );
		out[1] = (char)( 0x80u | ( ( cp >> 6 ) & 0x3Fu ) );
		out[2] = (char)( 0x80u | ( cp & 0x3Fu ) );
		return 3;
	}
	if( outSize < 4 ) return 0;
	out[0] = (char)( 0xF0u | ( cp >> 18 ) );
	out[1] = (char)( 0x80u | ( ( cp >> 12 ) & 0x3Fu ) );
	out[2] = (char)( 0x80u | ( ( cp >> 6 ) & 0x3Fu ) );
	out[3] = (char)( 0x80u | ( cp & 0x3Fu ) );
	return 4;
}

/// Read one code point out of a UTF-16 sequence, advancing *i past what it consumed.
///
/// Returns 0 for a lone surrogate - a high surrogate at the end of the buffer, a high surrogate not
/// followed by a low one, or a bare low surrogate. It ALWAYS advances *i by at least one, so a
/// caller looping on this cannot spin, and it does NOT swallow the character that follows a
/// malformed high surrogate.
inline unsigned int gxMatchDecodeUtf16( const unsigned short *in, int inCount, int *i )
{
	if( in == 0 || i == 0 || *i < 0 || *i >= inCount )
		return 0;

	const unsigned int hi = (unsigned int)in[*i];
	*i += 1;

	if( hi >= 0xD800u && hi <= 0xDBFFu )
	{
		if( *i < inCount )
		{
			const unsigned int lo = (unsigned int)in[*i];
			if( lo >= 0xDC00u && lo <= 0xDFFFu )
			{
				*i += 1;
				return 0x10000u + ( ( hi - 0xD800u ) << 10 ) + ( lo - 0xDC00u );
			}
		}
		return 0;						// lone high surrogate; the next unit is left for the caller
	}
	if( hi >= 0xDC00u && hi <= 0xDFFFu )
		return 0;						// lone low surrogate

	return hi;
}

/// Read one code point out of a wchar_t sequence, advancing *i.
///
/// wchar_t IS TWO BYTES ON WINDOWS AND FOUR EVERYWHERE ELSE, and the two are different encodings,
/// not different sizes of the same one: on Windows the units are UTF-16 and a surrogate pair is one
/// character; on macOS, Linux and iPadOS they are UTF-32 and the same two values would be two lone
/// surrogates. The 2-byte arm DELEGATES to gxMatchDecodeUtf16 rather than repeating its six lines,
/// so the function Windows actually runs is the one the test drives on a Mac.
///
/// wchar_t is signed on the POSIX platforms, so a value is widened through unsigned long before
/// comparing - a negative unit must land above every allowed range, not below it. That is the same
/// rule GXSanitizeChatText applies at NetworkUtil.cpp:258.
inline unsigned int gxMatchDecodeWide( const wchar_t *in, int inCount, int *i )
{
	if( in == 0 || i == 0 || *i < 0 || *i >= inCount )
		return 0;

	const bool kWide16 = ( sizeof( wchar_t ) == 2 );
	if( kWide16 )
	{
		unsigned short scratch[2];
		int have = 1;
		scratch[0] = (unsigned short)(unsigned long)in[*i];
		scratch[1] = 0;
		if( *i + 1 < inCount )
		{
			scratch[1] = (unsigned short)(unsigned long)in[*i + 1];
			have = 2;
		}
		int j = 0;
		const unsigned int cp = gxMatchDecodeUtf16( scratch, have, &j );
		*i += ( j > 0 ) ? j : 1;
		return cp;
	}

	const unsigned int u = (unsigned int)(unsigned long)in[*i];
	*i += 1;
	if( u >= 0xD800u && u <= 0xDFFFu )
		return 0;
	if( u > 0x10FFFFu )
		return 0;
	return u;
}

/// Encode a wide string as UTF-8 into `out`, stopping on a CODEPOINT BOUNDARY when the next
/// character would not fit in `outSize - 1` bytes. Always NUL terminates. Returns the bytes
/// written, not counting the NUL. `*outTruncated` is set to 1 when anything was dropped for space
/// and 0 otherwise - it is written on every call, including the early returns.
///
/// TRUNCATION IS ON A CODEPOINT BOUNDARY, NOT A BYTE ONE. Transport::sendRelayMatchRecord counts BYTES
/// (Transport.h:720), so a byte-boundary cut would hand the relay an invalid sequence and put a
/// replacement character in the operator's evidence. Because gxMatchUtf8Encode returns 0 rather
/// than a partial write when the remaining space is short, the boundary falls out of the encoder
/// rather than being computed here twice.
///
/// Control bytes are dropped rather than encoded. sendRelayMatchRecord strips every byte < 0x20 and 0x7F
/// on the way out (Transport.cpp:1855-1862), so a control byte left in here would shorten the line
/// the relay actually receives while this function reported the longer length.
inline int gxMatchUtf8FromWide( const wchar_t *in, int inCount, char *out, int outSize,
                                int *outTruncated )
{
	if( outTruncated != 0 )
		*outTruncated = 0;
	if( out == 0 || outSize <= 0 )
		return 0;
	out[0] = '\0';
	if( in == 0 || inCount <= 0 )
		return 0;

	int written = 0;
	int i = 0;
	while( i < inCount )
	{
		const int before = i;
		const unsigned int cp = gxMatchDecodeWide( in, inCount, &i );
		if( i == before )
			break;						// a decoder that did not advance would spin; it cannot,
										// but the loop must not depend on that being true

		if( cp == 0u )					continue;	// dropped: lone surrogate, or out of range
		if( cp < 0x20u || cp == 0x7Fu )	continue;	// control bytes never reach the envelope

		char enc[4];
		const int n = gxMatchUtf8Encode( cp, enc, (int)sizeof( enc ) );
		if( n == 0 )
			continue;					// not encodable at all - not a space problem

		if( written + n > outSize - 1 )
		{
			if( outTruncated != 0 )
				*outTruncated = 1;
			break;						// the codepoint boundary
		}
		for( int k = 0; k < n; ++k )
			out[written + k] = enc[k];
		written += n;
	}

	out[written] = '\0';
	return written;
}

/// Read one code point out of a UTF-8 sequence, advancing *i past what it consumed.
///
/// THIS IS THE INVERSE OF gxMatchUtf8Encode, AND THE OPERATOR DIRECTION NEEDS IT. The relay puts
/// real UTF-8 BYTES on the wire: plan E's gxAdminText converts the operator's JSON text to UTF-8,
/// cuts it on a codepoint boundary, and hands announceToRoom a latin1 VIEW of those bytes -
/// Buffer.from(view, 'latin1') then copies them out unchanged. Measured, node v24.20.0: the
/// operator typing "café €5" puts 63 61 66 c3 a9 20 e2 82 ac 35 on the wire. A drain
/// that widened byte -> code point would render those ten bytes as ten CODE POINTS - U+0063 U+0061
/// U+0066 U+00C3 U+00A9 U+0020 U+00E2 U+0082 U+00AC U+0035 - which is mojibake in front of every
/// player and, once recorded, permanently in the transcript.
///
/// Decoding here is the fix. Changing the relay's 'latin1' to 'utf8' is NOT: measured, that turns
/// those same bytes into 63 61 66 c3 83 c2 a9 20 c3 a2 c2 82 c2 ac 35 - double-encoded.
///
/// Returns 0 for anything malformed - a stray continuation byte, a TRUNCATED sequence, an OVERLONG
/// encoding, a surrogate, or a value above U+10FFFF - and in that case advances *i by exactly one,
/// so a caller looping on this cannot spin and resynchronises on the next byte rather than
/// swallowing it. U+0000 also reads back as 0: a NUL is not chat text.
inline unsigned int gxMatchUtf8Decode( const char *in, int inCount, int *i )
{
	if( in == 0 || i == 0 || *i < 0 || *i >= inCount )
		return 0;

	const unsigned int b0 = (unsigned int)(unsigned char)in[*i];
	*i += 1;

	if( b0 < 0x80u )
		return b0;						// ASCII; U+0000 correctly reads back as "nothing"
	if( b0 < 0xC2u )
		return 0;						// a stray continuation byte, or an overlong C0/C1 lead

	int need = 0;
	unsigned int cp = 0;
	unsigned int least = 0;				// the smallest value this length may legally encode
	if( b0 < 0xE0u )
	{
		need = 1; cp = b0 & 0x1Fu; least = 0x80u;
	}
	else if( b0 < 0xF0u )
	{
		need = 2; cp = b0 & 0x0Fu; least = 0x800u;
	}
	else if( b0 < 0xF5u )
	{
		need = 3; cp = b0 & 0x07u; least = 0x10000u;
	}
	else
	{
		return 0;						// F5..FF lead nothing at or below U+10FFFF
	}

	if( *i + need > inCount )
		return 0;						// TRUNCATED: the sequence runs off the end of the buffer

	for( int k = 0; k < need; ++k )
	{
		const unsigned int b = (unsigned int)(unsigned char)in[*i + k];
		if( ( b & 0xC0u ) != 0x80u )
			return 0;					// not a continuation byte - resync on it, do not eat it
		cp = ( cp << 6 ) | ( b & 0x3Fu );
	}

	if( cp < least )
		return 0;						// OVERLONG: a shorter encoding exists for this value
	if( cp >= 0xD800u && cp <= 0xDFFFu )
		return 0;						// a surrogate is not a character in any encoding
	if( cp > 0x10FFFFu )
		return 0;

	*i += need;
	return cp;
}

/// Write one code point as UTF-16 units. Returns the units written - 1, or 2 for a surrogate pair -
/// or 0 when the code point is not encodable or does not fit.
///
/// REACHABLE DIRECTLY FOR THE SAME REASON gxMatchDecodeUtf16 IS: this is the arm Windows runs, so
/// it is driven on a Mac rather than assumed.
inline int gxMatchUtf16FromCodepoint( unsigned int cp, unsigned short *out, int outCount )
{
	if( out == 0 || outCount <= 0 )
		return 0;
	if( cp == 0 || cp > 0x10FFFFu || ( cp >= 0xD800u && cp <= 0xDFFFu ) )
		return 0;
	if( cp < 0x10000u )
	{
		out[0] = (unsigned short)cp;
		return 1;
	}
	if( outCount < 2 )
		return 0;
	const unsigned int v = cp - 0x10000u;
	out[0] = (unsigned short)( 0xD800u + ( v >> 10 ) );
	out[1] = (unsigned short)( 0xDC00u + ( v & 0x3FFu ) );
	return 2;
}

/// Write one code point as wchar_t units - the inverse of gxMatchDecodeWide, and it has to exist
/// for the same reason that one does. On Windows a wchar_t is a UTF-16 unit, so `(wchar_t)cp` for
/// an astral code point would silently truncate U+1F600 to U+F600; there it must be a surrogate
/// pair. The 2-byte arm DELEGATES to gxMatchUtf16FromCodepoint rather than repeating it, so the
/// function Windows actually runs is the one the test drives on a Mac.
inline int gxMatchWideFromCodepoint( unsigned int cp, wchar_t *out, int outCount )
{
	if( out == 0 || outCount <= 0 )
		return 0;

	const bool kWide16 = ( sizeof( wchar_t ) == 2 );
	if( kWide16 )
	{
		unsigned short units[2];
		const int n = gxMatchUtf16FromCodepoint( cp, units, 2 );
		if( n <= 0 || n > outCount )
			return 0;
		for( int k = 0; k < n; ++k )
			out[k] = (wchar_t)units[k];
		return n;
	}

	if( cp == 0 || cp > 0x10FFFFu || ( cp >= 0xD800u && cp <= 0xDFFFu ) )
		return 0;
	out[0] = (wchar_t)cp;
	return 1;
}

/// Build the datagram body that Transport::sendRelayMatchRecord will put after "GXREC <room> ".
/// Returns the length written, or 0 when any field is out of range.
///
/// OUT OF RANGE IS A REFUSAL, NEVER A CLAMP, and that is the one rule a later editor must not
/// "improve". The relay's parser is anchored and exact-width precisely because sendRelayMatchRecord strips
/// control bytes and therefore no byte is impossible for a player to type: a player who types
/// "GXM3 0000000000003fff 00 e 0 hello" into the LOBBY chat box has that line parsed as a record. Structural
/// strictness is the only mitigation that exists inside the digest constraint - so a mask of 0x4000
/// must produce no datagram at all rather than a well-formed datagram claiming an audience the
/// sender never chose.
///
/// GeneralsX: WHAT A HAND-TYPED ENVELOPE ACTUALLY COSTS, corrected. This
/// note used to be filed under "self-inflicted, visible to nobody else, and costs one line", and
/// that is true of only the fields the relay overrides. relay.js takes nick, sub, member_ip, room
/// and session_id from the MEMBER RECORD (relay.js matchRecord, the `sinkEmit` call) and cannot be
/// lied to about any of them. It takes mask, slot, chan, truncated and text FROM THE DATAGRAM.
///
/// So the forger cannot become another player, but they can write a line into the transcript
/// carrying a SLOT THEY DO NOT OCCUPY and a CHANNEL THEY WERE NEVER ON - including 'd', which
/// renders in evidence as disconnect-screen chat from a match that may not have had a disconnect.
/// The line is still stamped with the forger's own identity, so the mitigation holds and nobody is
/// impersonated; what is lost is that slot, channel and audience are CLAIMS BY THE SENDER and must
/// be read as such. Anyone reading a transcript should treat the relay-stamped fields as fact and
/// these four as testimony. This is a relay-contract limit, not a defect in this parser, and it is
/// closed only by the relay deriving slot from membership - which it cannot do today, because the
/// relay has no view of the match's slot assignment.
///
/// `truncated` is bit 0 of the flags nibble. Bits 1-3 are reserved, are written 0, and must be
/// ignored on read.
inline int gxMatchBuildEnvelope( unsigned long long mask, int slot, char chan, int truncated,
                                 const char *utf8Text, char *out, int outSize )
{
	static const char HEX[] = "0123456789abcdef";

	if( out == 0 || outSize < GX_MATCH_HEADER_BYTES + 2 )
		return 0;
	if( mask > GX_MATCH_MASK_MAX )
		return 0;
	if( slot < 0 || slot > GX_MATCH_SLOT_MAX )
		return 0;
	if( chan != 'e' && chan != 'a' && chan != 'p' && chan != 'd' )
		return 0;
	if( utf8Text == 0 || utf8Text[0] == '\0' )
		return 0;

	int n = 0;
	out[n++] = 'G'; out[n++] = 'X'; out[n++] = 'M'; out[n++] = '3'; out[n++] = ' ';
	for( int shift = 60; shift >= 0; shift -= 4 )	// sixteen lowercase hex digits, most significant first
		out[n++] = HEX[( mask >> shift ) & 0xF];
	out[n++] = ' ';
	out[n++] = HEX[( slot >> 4 ) & 0xF];
	out[n++] = HEX[slot & 0xF];
	out[n++] = ' ';
	out[n++] = chan;
	out[n++] = ' ';
	out[n++] = HEX[( truncated != 0 ) ? 1 : 0];
	out[n++] = ' ';
	if( n != GX_MATCH_HEADER_BYTES )
		return 0;						// the arithmetic every budget here rests on, checked once

	for( int t = 0; utf8Text[t] != '\0'; ++t )
	{
		if( n >= outSize - 1 )
			return 0;					// the caller sized `out` wrong; refuse rather than clip -
										// a clipped envelope can end mid-UTF-8 sequence
		out[n++] = utf8Text[t];
	}
	out[n] = '\0';
	return n;
}
