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

// Wipe a buffer that held the sign-in when the scope ends, on EVERY path out: a destructor covers all early returns
// (GXIdentityAssertion::verifyWithKey has over a dozen) that a hand-written clear would miss.
// The wipe is gxSecureClear: stores through a pointer to volatile, which the optimiser may not remove.
// Dependency-free apart from that header, so scripts/test/signin-copies-test.cpp includes it alone.
#pragma once

#include <stddef.h>

#include "GameNetwork/GXSecureClear.h"

class GXScopedWipe
{
public:
	/// Wipe `n` bytes at `p` when this object goes out of scope. Declare it DIRECTLY after the buffer,
	/// before anything can fill it, so there is no path on which the buffer holds a secret and the
	/// guard does not yet exist.
	GXScopedWipe( void *p, size_t n ) : m_p(p), m_n(n) {}
	~GXScopedWipe() { gxSecureClear(m_p, m_n); }

	GXScopedWipe( const GXScopedWipe & ) = delete;
	GXScopedWipe &operator=( const GXScopedWipe & ) = delete;

private:
	void *m_p;
	size_t m_n;
};
