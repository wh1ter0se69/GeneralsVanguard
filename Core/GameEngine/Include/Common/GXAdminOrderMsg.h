/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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

// GeneralsX @security Claude 25/09/2026 Common/GXAdminOrder.h over a GameMessage: the payload of an order's LOGICAL
// arguments, and its trailer. The signer (Network::GetCommandsFromCommandList) and the simulation's check
// (GameLogicDispatch.cpp gxCheatTarget) both build the payload HERE, so they cannot disagree about what was signed.

#pragma once

#include "Common/GXAdminOrder.h"
#include "Common/MessageStream.h"

/// The argument count without the trailer. An order without one is its whole count.
inline Int gxAdminLogicalArgCount( const GameMessage *msg )
{
	const Int n = (Int)msg->getArgumentCount();
	if( n >= GX_ADMIN_TRAILER_ARGS && msg->getArgumentDataType( n - 1 ) == ARGUMENTDATATYPE_INTEGER
			&& msg->getArgument( n - 1 )->integer == (Int)GX_ADMIN_ORDER_MAGIC )
		return n - GX_ADMIN_TRAILER_ARGS;
	return n;
}

/// The payload for `msg` as sent by `slot` with `seq` in the match seeded `seed`. out->ok is false when an argument
/// has a type the payload cannot carry.
inline void gxAdminPayloadOf( const GameMessage *msg, Int slot, UnsignedInt seq, UnsignedInt seed, GXAdminPayload *out )
{
	const Int n = gxAdminLogicalArgCount( msg );
	gxAdminPayloadBegin( out, slot, seq, seed, (unsigned int)msg->getType(), (unsigned int)n );
	for( Int i = 0; i < n && out->ok; ++i )
	{
		const GameMessageArgumentType *a = msg->getArgument( i );
		switch( msg->getArgumentDataType( i ) )
		{
			case ARGUMENTDATATYPE_INTEGER:	gxAdminPayloadInt( out, a->integer ); break;
			case ARGUMENTDATATYPE_BOOLEAN:	gxAdminPayloadBool( out, a->boolean != FALSE ); break;
			case ARGUMENTDATATYPE_REAL:		gxAdminPayloadReal( out, a->real ); break;
			case ARGUMENTDATATYPE_LOCATION:	gxAdminPayloadLoc( out, a->location.x, a->location.y, a->location.z ); break;
			default:						gxAdminPayloadUnsupported( out ); break;
		}
	}
}

/// The trailer's seq and signature. FALSE when the order carries none, or one that is not 18 INTEGER arguments.
inline Bool gxAdminTrailerOf( const GameMessage *msg, UnsignedInt *seq, unsigned char sig[64] )
{
	const Int n = (Int)msg->getArgumentCount();
	const Int logical = gxAdminLogicalArgCount( msg );
	if( logical == n )
		return FALSE;
	for( Int i = logical; i < n; ++i )
		if( msg->getArgumentDataType( i ) != ARGUMENTDATATYPE_INTEGER )
			return FALSE;
	*seq = (UnsignedInt)msg->getArgument( logical )->integer;
	int words[16];
	for( Int i = 0; i < 16; ++i )
		words[i] = msg->getArgument( logical + 1 + i )->integer;
	gxAdminWordsToSig( words, sig );
	return TRUE;
}

inline void gxAdminAppendTrailer( GameMessage *msg, UnsignedInt seq, const unsigned char sig[64] )
{
	int words[16];
	gxAdminSigToWords( sig, words );
	msg->appendIntegerArgument( (Int)seq );
	for( Int i = 0; i < 16; ++i )
		msg->appendIntegerArgument( words[i] );
	msg->appendIntegerArgument( (Int)GX_ADMIN_ORDER_MAGIC );
}
