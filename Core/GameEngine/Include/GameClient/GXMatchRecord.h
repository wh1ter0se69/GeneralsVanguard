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

// The GXM3 in-match record envelope. Chat is audience-scoped, but a relay broadcasts plain GXCHT to the room, so
// this envelope carries the mask for the relay to RECORD the line and emit no GXSAY.
//   GXREC <room> GXM3 <mask:16 lowercase hex> <slot:2 hex> <chan:[eapd]> <flags:1 hex> <text>
// 29-byte header, 171 text; slot at most 0x21. GXREC, NOT GXCHT: an older relay fans GXCHT out as chat,
// an unknown GXREC is dropped.

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

/// Does this GX_MATCH_RECORD value turn recording ON? `value` is getenv's answer (NULL = unset).
/// DARK BY DEFAULT: against a relay without parseMatchRecord the envelope is plain GXCHT, so ally and private
/// chat would reach the room in plaintext. EXACTLY "1" enables: "true", "yes", "on", "01" and "" are OFF (a fuzzy
/// enable fires on a typo).
inline int gxMatchRecordOptInFromEnv( const char *value )
{
	if( value == 0 )
		return 0;						// unset is the default, and the default is dark
	return ( value[0] == '1' && value[1] == '\0' ) ? 1 : 0;
}

/// The relay's statement that it records the envelope: the word 'gxm1' on GXAUTH (relay.js GXM1_CAP). A HINT, not
/// the gate: a rollback cannot take it dark within one keepalive. The fail-closed property is the TAG: a relay
/// that does not record DROPS GXREC, so a stale word costs a missing transcript line, never a line in the room.
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

/// Encode a wide string as UTF-8 into `out`, stopping on a CODEPOINT BOUNDARY when the next character would not
/// fit in `outSize - 1` bytes. Always NUL terminates; returns the bytes written. `*outTruncated` is set on every
/// call. Control bytes are dropped (sendRelayMatchRecord strips < 0x20 and 0x7F), so the length reported matches
/// what is sent.
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

/// Read one code point out of a UTF-8 sequence, advancing *i past what it consumed; the inverse of
/// gxMatchUtf8Encode. The relay puts real UTF-8 BYTES on the wire, so widening byte by byte would show mojibake.
/// Returns 0 for anything malformed (stray continuation, TRUNCATED or OVERLONG, surrogate, above U+10FFFF,
/// U+0000) and advances *i by exactly one, so a looping caller cannot spin.
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

/// Build the body Transport::sendRelayMatchRecord puts after "GXREC <room> "; returns the length, or 0 if any
/// field is out of range. OUT OF RANGE IS A REFUSAL, NEVER A CLAMP: a player can type a well-formed envelope into
/// lobby chat, so strictness is the only mitigation. The relay takes mask, slot, chan, truncated and text from
/// the datagram: read them as the sender's CLAIMS. `truncated` is bit 0 of the flags nibble; bits 1-3 are written 0.
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
