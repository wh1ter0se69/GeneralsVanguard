// GeneralsX: wipe a buffer that held the sign-in when the scope that
// declared it ends, on EVERY path out of it.
//
// WHY A GUARD AND NOT A CLEAR BEFORE EACH return. The buffers this is for sit in functions with many
// early returns - GXIdentityAssertion::verifyWithKey has more than a dozen after it decodes the
// signature - and a hand-written clear before each one is exactly the kind of thing the next edit
// forgets. A destructor runs on every one of them, including the ones added later.
//
// The wipe itself is gxSecureClear: stores through a pointer to volatile, which the optimiser may not
// remove even though nothing reads the buffer again (GameNetwork/GXSecureClear.h says why a memset
// is not enough, and scripts/test/secure-clear-elision.sh measures it).
//
// PURE and dependency-free apart from that header, so scripts/test/signin-copies-test.cpp can include
// it on its own, and the Windows launcher, which compiles GXIdentityAssertion.cpp at /W4 /WX, needs
// nothing new. Under GameClient, which the simulation digest does not watch.
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
