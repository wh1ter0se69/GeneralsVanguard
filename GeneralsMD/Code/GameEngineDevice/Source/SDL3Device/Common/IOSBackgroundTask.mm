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
//                                                                            //
//  IOSBackgroundTask.mm                                                      //
//                                                                            //
//  GeneralsX: Keep the network alive for the ~30 s //
//  iOS grants a backgrounded app.                                            //
//                                                                            //
//  WHY THIS EXISTS, measured rather than assumed.                            //
//                                                                            //
//  In a live four-platform match on 2026-08-07 the Linux host recorded 35    //
//  [GXSTALL] events. FIVE of them were ~20 SECONDS each, every one blocked   //
//  on the iPad, at frames 15181/15281/15329/15530/15674 - about 100 seconds  //
//  of frozen wall clock inside a 240 second match, and roughly 95% of all    //
//  blocked time. All five reported retries=0: not packet loss. The peer had  //
//  stopped sending entirely because iOS had SUSPENDED the process.           //
//                                                                            //
//  Under lockstep a frame cannot execute anywhere until every peer's input   //
//  for it has arrived, so one suspended iPad freezes all four players. The   //
//  operator confirmed he had switched away from the game.                    //
//                                                                            //
//  The engine already does its part: SDL3GameEngine keeps GameEngine::update //
//  running while backgrounded in a network match and suspends only the GPU   //
//  (GX_SetRenderSuspended, honoured in W3DDisplay::draw). What was missing   //
//  is that iOS does not GIVE a backgrounded app execution time unless it is  //
//  asked. Nothing in this tree ever asked - an unbounded search for          //
//  beginBackgroundTask, UIBackgroundModes and backgroundTimeRemaining across //
//  the app, the Info.plist and the vendored SDL3 returned nothing, and none  //
//  of the five reference forks has it either (all are desktop-only).         //
//                                                                            //
//  THIS IS A BOUND, NOT A CURE. iOS grants roughly 30 seconds. Every episode //
//  observed was under 20.5 s and so would be fully absorbed, but a longer    //
//  absence still ends in expiration. That is why the expiration path sets a  //
//  flag for the engine to act on rather than pretending the problem is gone. //
//                                                                            //
//  Objective-C++ because UIApplication has no C API. Kept behind extern "C"  //
//  so exactly one translation unit in the build needs the Objective-C++      //
//  compiler and SDL3GameEngine.cpp stays plain C++.                          //
//                                                                            //
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
