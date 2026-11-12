/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

////////////////////////////////////////////////////////////////////////////////
//
//  IOSBackgroundTask.mm: keep the network alive for the ~30 s iOS grants a backgrounded app.
//  Under lockstep one suspended iPad freezes every player; iOS grants execution time only when asked.
//  A bound, not a cure: a longer absence still expires, and the expiration path sets a flag for the engine.
//  Objective-C++ (UIApplication has no C API), behind extern "C" so only this unit needs that compiler.
//
////////////////////////////////////////////////////////////////////////////////

#include <atomic>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE

#import <UIKit/UIKit.h>

namespace
{
	// Touched from the UIKit expiration block and read from the engine thread.
	std::atomic<bool> s_expired{false};

	// UIKit types must only be touched on the main thread; every caller below is
	// already on it (the SDL lifecycle watcher runs inside UIKit's own stack).
	UIBackgroundTaskIdentifier s_task = UIBackgroundTaskInvalid;
}

extern "C" void GX_iOSBeginBackgroundTask(void)
{
	if (s_task != UIBackgroundTaskInvalid)
		return;			// already held; re-entering would leak the previous identifier

	s_expired.store(false);

	// The expiration handler MUST return promptly and must not touch engine state:
	// iOS kills the process if the task is not ended from within it. So it does the
	// two things it is allowed to do - record that time ran out, and end the task -
	// and leaves the consequences to GX_iOSBackgroundTaskExpired() on the engine
	// thread.
	s_task = [[UIApplication sharedApplication]
		beginBackgroundTaskWithName:@"GeneralsX network"
		          expirationHandler:^{
			s_expired.store(true);
			if (s_task != UIBackgroundTaskInvalid)
			{
				[[UIApplication sharedApplication] endBackgroundTask:s_task];
				s_task = UIBackgroundTaskInvalid;
			}
		}];
}

extern "C" void GX_iOSEndBackgroundTask(void)
{
	s_expired.store(false);

	if (s_task == UIBackgroundTaskInvalid)
		return;

	[[UIApplication sharedApplication] endBackgroundTask:s_task];
	s_task = UIBackgroundTaskInvalid;
}

extern "C" bool GX_iOSBackgroundTaskExpired(void)
{
	return s_expired.load();
}

extern "C" double GX_iOSBackgroundTimeRemaining(void)
{
	// UIApplication reports a very large sentinel when not backgrounded. Passed
	// through unaltered - the caller decides what to make of it - because clamping
	// it here would hide the difference between "plenty of time" and "not counting".
	return (double)[[UIApplication sharedApplication] backgroundTimeRemaining];
}

#else	// not iOS - keep the symbols so callers need no #if of their own

extern "C" void GX_iOSBeginBackgroundTask(void) {}
extern "C" void GX_iOSEndBackgroundTask(void) {}
extern "C" bool GX_iOSBackgroundTaskExpired(void) { return false; }
extern "C" double GX_iOSBackgroundTimeRemaining(void) { return 0.0; }

#endif
