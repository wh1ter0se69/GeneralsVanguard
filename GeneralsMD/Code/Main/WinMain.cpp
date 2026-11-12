/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// FILE: WinMain.cpp //////////////////////////////////////////////////////////
//
// Entry point for game application
//
// Author: Colin Day, April 2001
//
///////////////////////////////////////////////////////////////////////////////

// SYSTEM INCLUDES ////////////////////////////////////////////////////////////
#define WIN32_LEAN_AND_MEAN  // only bare bones windows stuff wanted
#include <windows.h>
#include <stdlib.h>
#include <crtdbg.h>
#include <eh.h>
#include <ole2.h>
#include <dbt.h>
#include <process.h>

// USER INCLUDES //////////////////////////////////////////////////////////////
#include "WinMain.h"
#include "Lib/BaseType.h"
#include "Common/CommandLine.h"
#include "Common/CriticalSection.h"
#include "Common/GlobalData.h"
#include "Common/GameEngine.h"
#include "Common/GameSounds.h"
#include "Common/Debug.h"
#include "Common/GameMemory.h"
#include "Common/StackDump.h"
#include "Common/MessageStream.h"
#include "Common/PlayerList.h"
#include "Common/Registry.h"
#include "Common/Team.h"
#include "GameClient/ClientInstance.h"
#include "GameClient/InGameUI.h"
#include "GameClient/GameClient.h"
#include "GameLogic/GameLogic.h"  ///< @todo for demo, remove
#include "GameClient/Mouse.h"
#include "GameClient/ThumbTilt.h"
#include "GameClient/IMEManager.h"
#include "Win32Device/GameClient/Win32Mouse.h"
#include "Win32Device/Common/Win32GameEngine.h"
#include "Common/version.h"
#include "GXStderrLog.h"  // GeneralsX: desync evidence on disk
#include "GameLogic/GXHookTimer.h"  // GeneralsX: GX_HOOK_FLUSH_ALL before WinMain returns (test-instruments builds)
#include "GXCpuSets.h"  // GeneralsX: prefer the V-cache CCD on X3D CPUs
#include "GXCrashLog.h"   // GeneralsX: readable crash log beside the minidump
#include "GameClient/SaveLoadFeedback.h"  // TheGXCrashLogWriter
#include "gitinfo.h"
#include "GXUserDataMigrate.h"  // GeneralsX: own user-data folder
#include "GameClient/GXEnvAllowList.h"  // GeneralsX: shipped builds ignore every GX_* not on the list
#include "GameClient/GXPlayerIdentity.h"  // holdNewIdentity, when the migration could not bring it
#include "GameClient/GXUserDataDir.h"  // GeneralsX: GX_USERDATA_DIR skips the migration
#include "BuildVersion.h"
#include "GeneratedVersion.h"
#include "resource.h"

#ifdef RTS_ENABLE_CRASHDUMP
#include "Common/MiniDumper.h"
#endif
#include "W3DDevice/GameClient/GxNativeInstall.h"	// GeneralsX: FlushFinalStats (outside the crash-dump block)
#include "GameClient/GXReplayDiag.h"	// GeneralsX: gxReplayTelAway - latch the replay telemetry's away flag on deactivation


// GLOBALS ////////////////////////////////////////////////////////////////////
HINSTANCE ApplicationHInstance = nullptr;  ///< our application instance
HWND ApplicationHWnd = nullptr;  ///< our application window handle
Win32Mouse *TheWin32Mouse = nullptr;  ///< for the WndProc() only
DWORD TheMessageTime = 0;	///< For getting the time that a message was posted from Windows.

const Char *g_strFile = "data\\Generals.str";
const Char *g_csfFile = "data\\%s\\Generals.csf";
const char *gAppPrefix = ""; /// So WB can have a different debug log file name.

static Bool gInitializing = false;
static Bool gDoPaint = true;
static Bool isWinMainActive = false;
// GeneralsX: A close (X, Alt+F4), or a session that really ended (WM_ENDSESSION),
// before TheGameEngine existed - during the first-run user-data copy, the only time this window is pumped
// that early (gxMigrateUserDataPumping). The window stays up under the copy; WinMain then skips
// GameMain() instead of starting the game the player just closed.
static Bool gQuitBeforeEngine = false;

static HBITMAP gLoadScreenBitmap = nullptr;

//#define DEBUG_WINDOWS_MESSAGES

#ifdef DEBUG_WINDOWS_MESSAGES
static const char *messageToString(unsigned int message)
{
	static char name[32];

	switch (message)
	{
	case WM_NULL: return "WM_NULL";
	case WM_CREATE: return  "WM_CREATE";
	case WM_DESTROY: return  "WM_DESTROY";
	case WM_MOVE: return  "WM_MOVE";
	case WM_SIZE: return  "WM_SIZE";
	case WM_ACTIVATE: return  "WM_ACTIVATE";
	case WM_SETFOCUS: return  "WM_SETFOCUS";
	case WM_KILLFOCUS: return  "WM_KILLFOCUS";
	case WM_ENABLE: return  "WM_ENABLE";
	case WM_SETREDRAW: return  "WM_SETREDRAW";
	case WM_SETTEXT: return  "WM_SETTEXT";
	case WM_GETTEXT: return  "WM_GETTEXT";
	case WM_GETTEXTLENGTH: return  "WM_GETTEXTLENGTH";
	case WM_PAINT: return  "WM_PAINT";
	case WM_CLOSE: return  "WM_CLOSE";
	case WM_QUERYENDSESSION: return  "WM_QUERYENDSESSION";
	case WM_QUIT: return  "WM_QUIT";
	case WM_QUERYOPEN: return  "WM_QUERYOPEN";
	case WM_ERASEBKGND: return  "WM_ERASEBKGND";
	case WM_SYSCOLORCHANGE: return  "WM_SYSCOLORCHANGE";
	case WM_ENDSESSION: return  "WM_ENDSESSION";
	case WM_SHOWWINDOW: return  "WM_SHOWWINDOW";
	case WM_WININICHANGE: return "WM_WININICHANGE";
	case WM_DEVMODECHANGE: return  "WM_DEVMODECHANGE";
	case WM_ACTIVATEAPP: return  "WM_ACTIVATEAPP";
	case WM_FONTCHANGE: return  "WM_FONTCHANGE";
	case WM_TIMECHANGE: return  "WM_TIMECHANGE";
	case WM_CANCELMODE: return  "WM_CANCELMODE";
	case WM_SETCURSOR: return  "WM_SETCURSOR";
	case WM_MOUSEACTIVATE: return  "WM_MOUSEACTIVATE";
	case WM_CHILDACTIVATE: return  "WM_CHILDACTIVATE";
	case WM_QUEUESYNC: return  "WM_QUEUESYNC";
	case WM_GETMINMAXINFO: return  "WM_GETMINMAXINFO";
	case WM_PAINTICON: return  "WM_PAINTICON";
	case WM_ICONERASEBKGND: return  "WM_ICONERASEBKGND";
	case WM_NEXTDLGCTL: return  "WM_NEXTDLGCTL";
	case WM_SPOOLERSTATUS: return  "WM_SPOOLERSTATUS";
	case WM_DRAWITEM: return  "WM_DRAWITEM";
	case WM_MEASUREITEM: return  "WM_MEASUREITEM";
	case WM_DELETEITEM: return  "WM_DELETEITEM";
	case WM_VKEYTOITEM: return  "WM_VKEYTOITEM";
	case WM_CHARTOITEM: return  "WM_CHARTOITEM";
	case WM_SETFONT: return  "WM_SETFONT";
	case WM_GETFONT: return  "WM_GETFONT";
	case WM_SETHOTKEY: return  "WM_SETHOTKEY";
	case WM_GETHOTKEY: return  "WM_GETHOTKEY";
	case WM_QUERYDRAGICON: return  "WM_QUERYDRAGICON";
	case WM_COMPAREITEM: return  "WM_COMPAREITEM";
	case WM_COMPACTING: return  "WM_COMPACTING";
	case WM_COMMNOTIFY: return  "WM_COMMNOTIFY";
	case WM_WINDOWPOSCHANGING: return  "WM_WINDOWPOSCHANGING";
	case WM_WINDOWPOSCHANGED: return  "WM_WINDOWPOSCHANGED";
	case WM_POWER: return  "WM_POWER";
	case WM_COPYDATA: return  "WM_COPYDATA";
	case WM_CANCELJOURNAL: return  "WM_CANCELJOURNAL";
	case WM_NOTIFY: return  "WM_NOTIFY";
	case WM_INPUTLANGCHANGEREQUEST: return  "WM_INPUTLANGCHANGEREQUES";
	case WM_INPUTLANGCHANGE: return  "WM_INPUTLANGCHANGE";
	case WM_TCARD: return  "WM_TCARD";
	case WM_HELP: return  "WM_HELP";
	case WM_USERCHANGED: return  "WM_USERCHANGED";
	case WM_NOTIFYFORMAT: return  "WM_NOTIFYFORMAT";
	case WM_CONTEXTMENU: return  "WM_CONTEXTMENU";
	case WM_STYLECHANGING: return  "WM_STYLECHANGING";
	case WM_STYLECHANGED: return  "WM_STYLECHANGED";
	case WM_DISPLAYCHANGE: return  "WM_DISPLAYCHANGE";
	case WM_GETICON: return  "WM_GETICON";
	case WM_SETICON: return  "WM_SETICON";
	case WM_NCCREATE: return  "WM_NCCREATE";
	case WM_NCDESTROY: return  "WM_NCDESTROY";
	case WM_NCCALCSIZE: return  "WM_NCCALCSIZE";
	case WM_NCHITTEST: return  "WM_NCHITTEST";
	case WM_NCPAINT: return  "WM_NCPAINT";
	case WM_NCACTIVATE: return  "WM_NCACTIVATE";
	case WM_GETDLGCODE: return  "WM_GETDLGCODE";
	case WM_SYNCPAINT: return  "WM_SYNCPAINT";
	case WM_NCMOUSEMOVE: return  "WM_NCMOUSEMOVE";
	case WM_NCLBUTTONDOWN: return  "WM_NCLBUTTONDOWN";
	case WM_NCLBUTTONUP: return  "WM_NCLBUTTONUP";
	case WM_NCLBUTTONDBLCLK: return  "WM_NCLBUTTONDBLCLK";
	case WM_NCRBUTTONDOWN: return  "WM_NCRBUTTONDOWN";
	case WM_NCRBUTTONUP: return  "WM_NCRBUTTONUP";
	case WM_NCRBUTTONDBLCLK: return  "WM_NCRBUTTONDBLCLK";
	case WM_NCMBUTTONDOWN: return  "WM_NCMBUTTONDOWN";
	case WM_NCMBUTTONUP: return  "WM_NCMBUTTONUP";
	case WM_NCMBUTTONDBLCLK: return  "WM_NCMBUTTONDBLCLK";
	case WM_KEYDOWN: return  "WM_KEYDOWN";
	case WM_KEYUP: return  "WM_KEYUP";
	case WM_CHAR: return  "WM_CHAR";
	case WM_DEADCHAR: return  "WM_DEADCHAR";
	case WM_SYSKEYDOWN: return  "WM_SYSKEYDOWN";
	case WM_SYSKEYUP: return  "WM_SYSKEYUP";
	case WM_SYSCHAR: return  "WM_SYSCHAR";
	case WM_SYSDEADCHAR: return  "WM_SYSDEADCHAR";
	case WM_KEYLAST: return  "WM_KEYLAST";
	case WM_IME_STARTCOMPOSITION: return  "WM_IME_STARTCOMPOSITION";
	case WM_IME_ENDCOMPOSITION: return  "WM_IME_ENDCOMPOSITION";
	case WM_IME_COMPOSITION: return  "WM_IME_COMPOSITION";
	case WM_INITDIALOG: return  "WM_INITDIALOG";
	case WM_COMMAND: return  "WM_COMMAND";
	case WM_SYSCOMMAND: return  "WM_SYSCOMMAND";
	case WM_TIMER: return  "WM_TIMER";
	case WM_HSCROLL: return  "WM_HSCROLL";
	case WM_VSCROLL: return  "WM_VSCROLL";
	case WM_INITMENU: return  "WM_INITMENU";
	case WM_INITMENUPOPUP: return  "WM_INITMENUPOPUP";
	case WM_MENUSELECT: return  "WM_MENUSELECT";
	case WM_MENUCHAR: return  "WM_MENUCHAR";
	case WM_ENTERIDLE: return  "WM_ENTERIDLE";
	case WM_CTLCOLORMSGBOX: return  "WM_CTLCOLORMSGBOX";
	case WM_CTLCOLOREDIT: return  "WM_CTLCOLOREDIT";
	case WM_CTLCOLORLISTBOX: return  "WM_CTLCOLORLISTBOX";
	case WM_CTLCOLORBTN: return  "WM_CTLCOLORBTN";
	case WM_CTLCOLORDLG: return  "WM_CTLCOLORDLG";
	case WM_CTLCOLORSCROLLBAR: return  "WM_CTLCOLORSCROLLBAR";
	case WM_CTLCOLORSTATIC: return  "WM_CTLCOLORSTATIC";
	case WM_MOUSEMOVE: return  "WM_MOUSEMOVE";
	case WM_LBUTTONDOWN: return  "WM_LBUTTONDOWN";
	case WM_LBUTTONUP: return  "WM_LBUTTONUP";
	case WM_LBUTTONDBLCLK: return  "WM_LBUTTONDBLCLK";
	case WM_RBUTTONDOWN: return  "WM_RBUTTONDOWN";
	case WM_RBUTTONUP: return  "WM_RBUTTONUP";
	case WM_RBUTTONDBLCLK: return  "WM_RBUTTONDBLCLK";
	case WM_MBUTTONDOWN: return  "WM_MBUTTONDOWN";
	case WM_MBUTTONUP: return  "WM_MBUTTONUP";
	case WM_MBUTTONDBLCLK: return  "WM_MBUTTONDBLCLK";
//	case WM_MOUSEWHEEL: return  "WM_MOUSEWHEEL";
	case WM_PARENTNOTIFY: return  "WM_PARENTNOTIFY";
	case WM_ENTERMENULOOP: return  "WM_ENTERMENULOOP";
	case WM_EXITMENULOOP: return  "WM_EXITMENULOOP";
	case WM_NEXTMENU: return  "WM_NEXTMENU";
	case WM_SIZING: return  "WM_SIZING";
	case WM_CAPTURECHANGED: return  "WM_CAPTURECHANGED";
	case WM_MOVING: return  "WM_MOVING";
	case WM_POWERBROADCAST: return  "WM_POWERBROADCAST";
	case WM_DEVICECHANGE: return  "WM_DEVICECHANGE";
	case WM_MDICREATE: return  "WM_MDICREATE";
	case WM_MDIDESTROY: return  "WM_MDIDESTROY";
	case WM_MDIACTIVATE: return  "WM_MDIACTIVATE";
	case WM_MDIRESTORE: return  "WM_MDIRESTORE";
	case WM_MDINEXT: return  "WM_MDINEXT";
	case WM_MDIMAXIMIZE: return  "WM_MDIMAXIMIZE";
	case WM_MDITILE: return  "WM_MDITILE";
	case WM_MDICASCADE: return  "WM_MDICASCADE";
	case WM_MDIICONARRANGE: return  "WM_MDIICONARRANGE";
	case WM_MDIGETACTIVE: return  "WM_MDIGETACTIVE";
	case WM_MDISETMENU: return  "WM_MDISETMENU";
	case WM_ENTERSIZEMOVE: return  "WM_ENTERSIZEMOVE";
	case WM_EXITSIZEMOVE: return  "WM_EXITSIZEMOVE";
	case WM_DROPFILES: return  "WM_DROPFILES";
	case WM_MDIREFRESHMENU: return  "WM_MDIREFRESHMENU";
	case WM_IME_SETCONTEXT: return  "WM_IME_SETCONTEXT";
	case WM_IME_NOTIFY: return  "WM_IME_NOTIFY";
	case WM_IME_CONTROL: return  "WM_IME_CONTROL";
	case WM_IME_COMPOSITIONFULL: return  "WM_IME_COMPOSITIONFULL";
	case WM_IME_SELECT: return  "WM_IME_SELECT";
	case WM_IME_CHAR: return  "WM_IME_CHAR";
	case WM_IME_KEYDOWN: return  "WM_IME_KEYDOWN";
	case WM_IME_KEYUP: return  "WM_IME_KEYUP";
//	case WM_MOUSEHOVER: return  "WM_MOUSEHOVER";
//	case WM_MOUSELEAVE: return  "WM_MOUSELEAVE";
	case WM_CUT: return  "WM_CUT";
	case WM_COPY: return  "WM_COPY";
	case WM_PASTE: return  "WM_PASTE";
	case WM_CLEAR: return  "WM_CLEAR";
	case WM_UNDO: return  "WM_UNDO";
	case WM_RENDERFORMAT: return  "WM_RENDERFORMAT";
	case WM_RENDERALLFORMATS: return  "WM_RENDERALLFORMATS";
	case WM_DESTROYCLIPBOARD: return  "WM_DESTROYCLIPBOARD";
	case WM_DRAWCLIPBOARD: return  "WM_DRAWCLIPBOARD";
	case WM_PAINTCLIPBOARD: return  "WM_PAINTCLIPBOARD";
	case WM_VSCROLLCLIPBOARD: return  "WM_VSCROLLCLIPBOARD";
	case WM_SIZECLIPBOARD: return  "WM_SIZECLIPBOARD";
	case WM_ASKCBFORMATNAME: return  "WM_ASKCBFORMATNAME";
	case WM_CHANGECBCHAIN: return  "WM_CHANGECBCHAIN";
	case WM_HSCROLLCLIPBOARD: return  "WM_HSCROLLCLIPBOARD";
	case WM_QUERYNEWPALETTE: return  "WM_QUERYNEWPALETTE";
	case WM_PALETTEISCHANGING: return  "WM_PALETTEISCHANGING";
	case WM_PALETTECHANGED: return  "WM_PALETTECHANGED";
	case WM_HOTKEY: return  "WM_HOTKEY";
	case WM_PRINT: return  "WM_PRINT";
	case WM_PRINTCLIENT: return  "WM_PRINTCLIENT";
	case WM_HANDHELDFIRST: return  "WM_HANDHELDFIRST";
	case WM_HANDHELDLAST: return  "WM_HANDHELDLAST";
	case WM_AFXFIRST: return  "WM_AFXFIRST";
	case WM_AFXLAST: return  "WM_AFXLAST";
	case WM_PENWINFIRST: return  "WM_PENWINFIRST";
	case WM_PENWINLAST: return  "WM_PENWINLAST";
	default: return "WM_UNKNOWN";
	};
}
#endif

// WndProc ====================================================================
/** Window Procedure */
//=============================================================================
LRESULT CALLBACK WndProc( HWND hWnd, UINT message,
													WPARAM wParam, LPARAM lParam )
{

	try
	{
		// First let the IME manager do it's stuff.
		if ( TheIMEManager )
		{
			if ( TheIMEManager->serviceIMEMessage( hWnd, message, wParam, lParam ) )
			{
				// The manager intercepted an IME message so return the result
				return TheIMEManager->result();
			}
		}

#ifdef	DEBUG_WINDOWS_MESSAGES
		static msgCount=0;
		char testString[256];
		sprintf(testString,"\n%d: %s (%X,%X)", msgCount++,messageToString(message), wParam, lParam);
		OutputDebugString(testString);
#endif

		// handle all window messages
		switch( message )
		{
			//-------------------------------------------------------------------------
			case WM_NCHITTEST:
				// Prevent the user from selecting the menu in fullscreen mode
				if( TheGlobalData && !TheGlobalData->m_windowed )
					return HTCLIENT;
				break;

			//-------------------------------------------------------------------------
			case WM_POWERBROADCAST:
				switch( wParam )
				{
					#ifndef PBT_APMQUERYSUSPEND
						#define PBT_APMQUERYSUSPEND 0x0000
					#endif
					case PBT_APMQUERYSUSPEND:
						// At this point, the app should save any data for open
						// network connections, files, etc., and prepare to go into
						// a suspended mode.
						return TRUE;

					#ifndef PBT_APMRESUMESUSPEND
						#define PBT_APMRESUMESUSPEND 0x0007
					#endif
					case PBT_APMRESUMESUSPEND:
						// At this point, the app should recover any data, network
						// connections, files, etc., and resume running from when
						// the app was suspended.
						return TRUE;
				}
				break;
			//-------------------------------------------------------------------------
			case WM_SYSCOMMAND:
				// Prevent moving/sizing and power loss in fullscreen mode
				switch( wParam & 0xFFF0 )
				{
					case SC_KEYMENU:
						// TheSuperHackers @bugfix Mauller 10/05/2025 Always handle this command to prevent halting the game when left Alt is pressed.
						return 1;
					case SC_MOVE:
					case SC_SIZE:
					case SC_MAXIMIZE:
					case SC_MONITORPOWER:
						if( TheGlobalData && !TheGlobalData->m_windowed )
							return 1;
						break;
				}
				break;

			case WM_QUERYENDSESSION:
			{
				if (TheGameEngine && !TheGameEngine->getQuitting())
				{
					if (TheMessageStream && TheMessageStream->isReadyForMessages())
					{
						TheMessageStream->appendMessage(GameMessage::MSG_META_DEMO_INSTANT_QUIT);
					}
					else
					{
						TheGameEngine->setQuitting(TRUE);
					}
				}
				// GeneralsX: Before the engine exists - the user-data copy, which pumps this
				// window - a sign-out or shutdown is never vetoed: 0 here cancelled it. An
				// interrupted copy is safe and resumes next start. WM_ENDSESSION says whether it really ends.
				if (TheGameEngine == nullptr)
					return TRUE;
				return 0;	//don't allow Windows to shutdown while game is running.
			}

			case WM_ENDSESSION:
				// GeneralsX: The session IS ending during the pre-engine copy: do not start
				// the game after it. A cancelled end (wParam FALSE) changes nothing.
				if (wParam && TheGameEngine == nullptr)
					gQuitBeforeEngine = true;
				break;

			// ------------------------------------------------------------------------
			case WM_CLOSE:
				// TheSuperHackers @feature Intercept Alt+F4/Close to show the quit menu in-game. 
				// Repeating the command when the menu is visible triggers a Self-Destruct followed by a sequenced quit.
				// If not in a match (e.g. main menu), the command instantly closes the application.
				if (TheGameEngine && !TheGameEngine->getQuitting())
				{
					if (TheMessageStream && TheMessageStream->isReadyForMessages())
					{
						TheMessageStream->appendMessage(GameMessage::MSG_META_DEMO_INSTANT_QUIT);
					}
					else
					{
						TheGameEngine->setQuitting(TRUE);
					}
				}
				if (TheGameEngine == nullptr)
					gQuitBeforeEngine = true;
				return 0;

			//-------------------------------------------------------------------------
			case WM_MOVE:
			{
				if (TheMouse)
					TheMouse->refreshCursorCapture();

				break;
			}

			//-------------------------------------------------------------------------
			case WM_SIZE:
			{
				// When W3D initializes, it resizes the window.  So stop repainting.
				// GeneralsX: Only once the engine exists - W3D's resize happens inside
				// GameMain. A minimise/restore during the pre-engine user-data copy (gxMigrateUserDataPumping)
				// turned the loading image off for the rest of the copy.
				if (!gInitializing && TheGameEngine != nullptr)
					gDoPaint = false;

				if (TheMouse)
					TheMouse->refreshCursorCapture();

				break;
			}

			// ------------------------------------------------------------------------
			case WM_SETFOCUS:
			{
				//
				// reset the state of our keyboard cause we haven't been paying
				// attention to the keys while focus was away
				//
				if (TheKeyboard)
					TheKeyboard->resetKeys();

				if (TheMouse)
					TheMouse->regainFocus();

				break;
			}

			//-------------------------------------------------------------------------
			case WM_KILLFOCUS:
			{
				if (TheKeyboard)
					TheKeyboard->resetKeys();

				if (TheMouse)
				{
					TheMouse->loseFocus();

					if (TheMouse->isCursorInside())
					{
						TheMouse->onCursorMovedOutside();
					}
				}

				break;
			}

			//-------------------------------------------------------------------------
			case WM_ACTIVATEAPP:
			{
				if ((bool) wParam != isWinMainActive)
				{
					// TheSuperHackers @bugfix xezon 11/05/2025 This event originally called DX8Wrapper::Reset_Device,
					// intended to clear resources on a lost device in fullscreen, but effectively also in
					// windowed mode, if the DXMaximizedWindowedMode shim was applied in newer versions of Windows,
					// which lead to unfortunate application crashing. Resetting the device on WM_ACTIVATEAPP instead
					// of TestCooperativeLevel() == D3DERR_DEVICENOTRESET is not a requirement. There are other code
					// paths that take care of that.

					isWinMainActive = (BOOL) wParam;

					if (TheGameEngine)
						TheGameEngine->setIsActive(isWinMainActive);

					// GeneralsX: Latch the replay telemetry's away flag HERE, not only from the level
					// Win32GameEngine::update() reads: a deactivate and a reactivate pumped in one serviceWindowsOS() pass would leave the
					// level true and the open row unflagged. update() clears it once the window is back.
					if (!isWinMainActive)
						gxReplayTelAway(true);

					if (isWinMainActive)
					{
						//restore mouse cursor to our custom version.
						if (TheWin32Mouse)
							TheWin32Mouse->setCursor(TheWin32Mouse->getMouseCursor());
					}
				}
				return 0;
			}

			//-------------------------------------------------------------------------
			case WM_ACTIVATE:
			{
				Int active = LOWORD( wParam );

				if( active == WA_INACTIVE )
				{
					if (TheAudio)
						TheAudio->muteAudio(AudioManager::MuteAudioReason_WindowFocus);
				}
				else
				{
					if (TheAudio)
						TheAudio->unmuteAudio(AudioManager::MuteAudioReason_WindowFocus);

					// Cursor can only be captured after one of the activation events.
					if (TheMouse)
						TheMouse->refreshCursorCapture();
				}
				break;
			}

			//-------------------------------------------------------------------------
			case WM_KEYDOWN:
			{
				Int key = (Int)wParam;

				switch( key )
				{
					case VK_ESCAPE:
					{
						PostQuitMessage( 0 );
						break;
					}
				}
				return 0;
			}

			//-------------------------------------------------------------------------
			case WM_LBUTTONDOWN:
			case WM_LBUTTONUP:
			case WM_LBUTTONDBLCLK:

			case WM_MBUTTONDOWN:
			case WM_MBUTTONUP:
			case WM_MBUTTONDBLCLK:

			case WM_RBUTTONDOWN:
			case WM_RBUTTONUP:
			case WM_RBUTTONDBLCLK:
			{
				if( TheWin32Mouse )
					TheWin32Mouse->addWin32Event( message, wParam, lParam, TheMessageTime );

				return 0;
			}

			//-------------------------------------------------------------------------
			// Mouse buttons 4 (XBUTTON1) / 5 (XBUTTON2): held state only.
			case 0x020B: // WM_XBUTTONDOWN
			case 0x020C: // WM_XBUTTONUP
			{
				if( TheWin32Mouse )
					TheWin32Mouse->setThumbButton( (Int)HIWORD( wParam ), gxThumbPressHeld( message == 0x020B, TheWin32Mouse->isLostFocus() ) );	// a DOWN while unfocused would outlive a lost UP
				return TRUE;	// the docs ask for TRUE: handled, no DefWindowProc app-command (browser back)
			}

			//-------------------------------------------------------------------------
			case 0x020A: // WM_MOUSEWHEEL
			{
				if( TheWin32Mouse == nullptr )
					return 0;

				long x = (long) LOWORD(lParam);
				long y = (long) HIWORD(lParam);
				RECT rect;

				// ignore when outside of client area
				GetWindowRect( ApplicationHWnd, &rect );
				if( x < rect.left || x > rect.right || y < rect.top || y > rect.bottom )
					return 0;

				TheWin32Mouse->addWin32Event( message, wParam, lParam, TheMessageTime );
				return 0;
			}

			//-------------------------------------------------------------------------
			case WM_MOUSEMOVE:
			{
				if( TheWin32Mouse == nullptr )
					return 0;

				// ignore when window is not active
				if( !isWinMainActive )
					return 0;

				Int x = (Int)LOWORD( lParam );
				Int y = (Int)HIWORD( lParam );
				RECT rect;

				// ignore when outside of client area
				GetClientRect( ApplicationHWnd, &rect );
				if( x < rect.left || x > rect.right || y < rect.top || y > rect.bottom )
				{
					if ( TheMouse->isCursorInside() )
					{
						TheMouse->onCursorMovedOutside();
					}
					return 0;
				}

				if( !TheMouse->isCursorInside() )
				{
					TheMouse->onCursorMovedInside();
				}

				TheWin32Mouse->addWin32Event( message, wParam, lParam, TheMessageTime );
				return 0;
			}

			//-------------------------------------------------------------------------
			case WM_SETCURSOR:
			{
				// GeneralsX: No game cursor exists before the engine, and returning TRUE
				// below set none, so during the pre-engine copy the pointer kept whatever shape it came in with.
				// Windows' "working in the background" cursor says the window is alive and busy.
				if (TheGameEngine == nullptr && LOWORD(lParam) == HTCLIENT)
				{
					SetCursor(LoadCursor(nullptr, IDC_APPSTARTING));
					return TRUE;
				}
				if (TheWin32Mouse && (HWND)wParam == ApplicationHWnd)
					TheWin32Mouse->setCursor(TheWin32Mouse->getMouseCursor());
				return TRUE;	//tell Windows not to reset mouse cursor image to default.
			}

			case WM_PAINT:
			{
				// GeneralsX: Before the engine exists the loading image is repainted in
				// fullscreen too. The pre-engine copy pumps this window, and gDoPaint - off from creation in
				// fullscreen - left an exposed or restored window blank until GameMain.
				if (gDoPaint || TheGameEngine == nullptr) {
					PAINTSTRUCT paint;
					HDC dc = ::BeginPaint(hWnd, &paint);
#if 0
					::SetTextColor(dc, RGB(255,255,255));
					::SetBkColor(dc, RGB(0,0,0));
					::TextOut(dc, 30, 30, "Loading Command & Conquer Generals...", 37);
#endif
					if (gLoadScreenBitmap!=nullptr) {
						Int savContext = ::SaveDC(dc);
						HDC tmpDC = ::CreateCompatibleDC(dc);
						HBITMAP savBitmap = (HBITMAP)::SelectObject(tmpDC, gLoadScreenBitmap);
						::BitBlt(dc, 0, 0, DEFAULT_DISPLAY_WIDTH, DEFAULT_DISPLAY_HEIGHT, tmpDC, 0, 0, SRCCOPY);
						::SelectObject(tmpDC, savBitmap);
						::DeleteDC(tmpDC);
						::RestoreDC(dc, savContext);
					}
					::EndPaint(hWnd, &paint);
					return TRUE;
				}
				break;
			}

			case WM_ERASEBKGND:
			{
				if (!gDoPaint)
					return TRUE;	//we don't need to erase the background because we always draw entire window.
				break;
			}

// Well, it was a nice idea, but we don't get a message for an ejection.
// (Really unfortunate, actually.) I'm leaving this in in-case some one wants
// to trap a different device change (for instance, removal of a mouse) - jkmcd
#if 0
			case WM_DEVICECHANGE:
			{
				if (((UINT) wParam) == DBT_DEVICEREMOVEPENDING)
				{
					DEV_BROADCAST_HDR *hdr = (DEV_BROADCAST_HDR*) lParam;
					if (!hdr) {
						break;
					}

					if (hdr->dbch_devicetype != DBT_DEVTYP_VOLUME)  {
						break;
					}

					// Lets discuss how Windows is a flaming pile of poo. I'm now casting the header
					// directly into the structure, because its the one I want, and this is just how
					// its done. I hate Windows. - jkmcd
					DEV_BROADCAST_VOLUME *vol = (DEV_BROADCAST_VOLUME*) (hdr);

					return TRUE;
				}
				break;
			}
#endif
		}

	}
	catch (...)
	{
		RELEASE_CRASH(("Uncaught exception in Main::WndProc... probably should not happen"));
		// no rethrow
	}

//In full-screen mode, only pass these messages onto the default windows handler.
//Appears to fix issues with dual monitor systems but doesn't seem safe?
///@todo: Look into proper support for dual monitor systems.
/*	if (!TheGlobalData->m_windowed)
	switch (message)
	{
		case WM_PAINT:
		case WM_NCCREATE:
		case WM_NCDESTROY:
		case WM_NCCALCSIZE:
		case WM_NCPAINT:
				return DefWindowProc( hWnd, message, wParam, lParam );
	}
	return 0;*/

	return DefWindowProc( hWnd, message, wParam, lParam );

}

// GeneralsX: The main window's class, and what a second instance looks for.
//
// Retail registered "Game Window" but its already-running branch searched FindWindow() for the mutex
// GUID as the CLASS, which nothing registers, so the branch never fired and a second double-click
// exited silently. Searching "Game Window" instead would also match retail and Generals Online, which
// register that class too, so the class is GeneralsX's own. Both games use it: they share one mutex
// (Core ClientInstance.cpp), so either one bails when the other runs and must be able to raise it.
//
// Only the copy holding the PRIMARY mutex registers GX_GAME_WINDOW_CLASS. A -replay copy skips that
// mutex (CommandLine.cpp parseReplay), and a copy that loses it must raise the game, not the replay
// Which mutex this copy holds is known here: the mutex is taken before this window.
#define GX_GAME_WINDOW_CLASS TEXT("GeneralsX Game Window")
#define GX_EXTRA_GAME_WINDOW_CLASS TEXT("GeneralsX Extra Game Window")

// initializeAppWindows =======================================================
/** Register windows class and create application windows. */
//=============================================================================
static Bool initializeAppWindows( HINSTANCE hInstance, Int nCmdShow, Bool runWindowed )
{
	DWORD windowStyle;
	Int startWidth = DEFAULT_DISPLAY_WIDTH,
			startHeight = DEFAULT_DISPLAY_HEIGHT;

	// register the window class

	const LPCTSTR windowClass = !rts::ClientInstance::isMultiInstance() || rts::ClientInstance::getInstanceIndex() == 0
		? GX_GAME_WINDOW_CLASS : GX_EXTRA_GAME_WINDOW_CLASS;
  WNDCLASS wndClass = { CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS, WndProc, 0, 0, hInstance,
                       LoadIcon (hInstance, MAKEINTRESOURCE(IDI_ApplicationIcon)),
                       nullptr/*LoadCursor(nullptr, IDC_ARROW)*/,
                       (HBRUSH)GetStockObject(BLACK_BRUSH), nullptr,
	                     windowClass };
  RegisterClass( &wndClass );

   // Create our main window
	windowStyle =  WS_POPUP|WS_VISIBLE;
	if (runWindowed)
		windowStyle |= WS_MINIMIZEBOX | WS_SYSMENU | WS_DLGFRAME | WS_CAPTION;
	else
		windowStyle |= WS_EX_TOPMOST | WS_SYSMENU;

	RECT rect;
	rect.left = 0;
	rect.top = 0;
	rect.right = startWidth;
	rect.bottom = startHeight;
	AdjustWindowRect (&rect, windowStyle, FALSE);
	if (runWindowed) {
		// Makes the normal debug 800x600 window center in the screen.
		startWidth = DEFAULT_DISPLAY_WIDTH;
		startHeight= DEFAULT_DISPLAY_HEIGHT;
	}

	gInitializing = true;

  HWND hWnd = CreateWindow( windowClass,
                            TEXT("Command and Conquer Generals"),
                            windowStyle,
														(GetSystemMetrics( SM_CXSCREEN ) / 2) - (startWidth / 2), // original position X
														(GetSystemMetrics( SM_CYSCREEN ) / 2) - (startHeight / 2),// original position Y
														// Lorenzen nudged the window higher
														// so the constantdebug report would
														// not get obliterated by assert windows, thank you.
														//(GetSystemMetrics( SM_CXSCREEN ) / 2) - (startWidth / 2),   //this works with any screen res
														//(GetSystemMetrics( SM_CYSCREEN ) / 25) - (startHeight / 25),//this works with any screen res
														rect.right-rect.left,
														rect.bottom-rect.top,
														nullptr,
														nullptr,
														hInstance,
														nullptr );


	if (!runWindowed)
	{	SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0,SWP_NOSIZE |SWP_NOMOVE);
	}
	else
		SetWindowPos(hWnd, HWND_TOP, 0, 0, 0, 0,SWP_NOSIZE |SWP_NOMOVE);

	SetFocus(hWnd);

	SetForegroundWindow(hWnd);
	ShowWindow( hWnd, nCmdShow );
	UpdateWindow( hWnd );

	// save our application window handle for future use
	ApplicationHWnd = hWnd;
	gInitializing = false;
	if (!runWindowed) {
		gDoPaint = false;
	}

	return true;  // success

}

// The first start after the user-data move copies Maps, Replays and Save on a worker while this thread keeps
// pumping window messages, so Windows does not mark the window Not Responding. WndProc has no engine here.
// Close requests are swallowed but remembered (gQuitBeforeEngine) so the game does not start after the copy.
struct GXMigrateJob
{
	const char *oldDir;
	const char *newDir;
	GXMigrateResult result;
};

static unsigned __stdcall gxMigrateThread(void *arg)
{
	GXMigrateJob *job = static_cast<GXMigrateJob *>(arg);
	job->result = gxMigrateUserData(job->oldDir, job->newDir);
	return 0;
}

static void gxDispatchPending()
{
	MSG msg;
	while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
}

static GXMigrateResult gxMigrateUserDataPumping(const char *oldDir, const char *newDir)
{
	GXMigrateJob job = { oldDir, newDir, GX_MIGRATE_IDENTITY_PENDING };
	HANDLE th = (HANDLE)_beginthreadex(nullptr, 0, gxMigrateThread, &job, 0, nullptr);
	if (th == nullptr)
		gxMigrateThread(&job);	// no thread: the old synchronous copy, through the one call site
	else
	{
		while (MsgWaitForMultipleObjectsEx(1, &th, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_OBJECT_0 + 1)
			gxDispatchPending();
		// Returns at once when the thread ended the loop. If the wait itself failed, the copy is still
		// running and `job` is on this stack: wait it out unpumped rather than leave.
		WaitForSingleObject(th, INFINITE);
		CloseHandle(th);
	}
	// The wait reports the finished thread BEFORE queued input, so a close clicked as the copy ended can
	// still be waiting here. Dispatch it now, while gQuitBeforeEngine can still stop GameMain().
	gxDispatchPending();
	return job.result;
}

// Necessary to allow memory managers and such to have useful critical sections
static CriticalSection critSec1, critSec2, critSec3, critSec4, critSec5;

#if defined(_MSC_VER) && defined(_M_X64)
// GeneralsX: The crash log's frames come from the FAULTING context, not
// from here: CaptureStackBackTrace in the filter records the filter and the exception dispatcher
// before it reaches the fault. This unwinds e_info->ContextRecord with the x64 unwind tables
// (kernel32, so nothing is loaded into a dying process). Frame 0 is the faulting instruction, kept
// even when it is 0 (a call through a null pointer), whose caller is then read from [rsp]. A
// corrupt stack ends the walk early; it never faults the filter.
static int gxCrashFrames(const CONTEXT *fault, void **frames, int max)
{
	CONTEXT ctx = *fault;
	volatile int n = 0;
	__try {
		for (;;) {
			frames[n] = (void *)ctx.Rip;
			n = n + 1;
			if (n >= max) break;
			const DWORD64 sp = ctx.Rsp;
			DWORD64 imageBase = 0;
			PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
			if (fn != nullptr) {
				PVOID handlerData = nullptr;
				DWORD64 establisher = 0;
				RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
			} else {
				// No unwind data: a leaf, or a call through a bad pointer. The return address is at [rsp].
				ctx.Rip = *(const DWORD64 *)ctx.Rsp;
				ctx.Rsp += 8;
			}
			if (ctx.Rip == 0 || ctx.Rsp <= sp) break;  // the end of the stack, or no progress (corrupt)
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return n;
}
#endif

// UnHandledExceptionFilter ===================================================
/** Handler for unhandled win32 exceptions. */
//=============================================================================
static LONG WINAPI UnHandledExceptionFilter( struct _EXCEPTION_POINTERS* e_info )
{
	// GeneralsX: <userdata>\CrashLogs\crash-*.txt FIRST: the minidump
	// below can take seconds, and this is the file the launcher offers to send. The faulting
	// address is frame 0.
	{
		void *frames[64];
#if defined(_MSC_VER) && defined(_M_X64)
		const int n = gxCrashFrames(e_info->ContextRecord, frames, 64);
#else
		// Not x64 (the x86 presets): the filter's own stack, which continues through the
		// dispatcher into the fault.
		frames[0] = e_info->ExceptionRecord->ExceptionAddress;
		const int n = 1 + (int)CaptureStackBackTrace(0, 63, frames + 1, nullptr);
#endif
		char reason[48];
		snprintf(reason, sizeof(reason), "EXCEPTION 0x%08lX", (unsigned long)e_info->ExceptionRecord->ExceptionCode);
		GXCrashLog_Write(reason, frames, n);
	}
	DumpExceptionInfo( e_info->ExceptionRecord->ExceptionCode, e_info );
#ifdef RTS_ENABLE_CRASHDUMP
	if (TheMiniDumper && TheMiniDumper->IsInitialized())
	{
		// GeneralsX @bugfix wh1ter0se 12/08/2026 Write only the minimal dump by default: the full dump is ~1.4 GB and blocks the faulting thread for 8-11 s with
		// no message pump, which looks like a hang on exit. The minimal dump still has the exception, registers and backtrace.
		// GX_FULL_CRASHDUMP=1 opts in to the full dump, which matters for heap corruption.
		TheMiniDumper->TriggerMiniDumpForException(e_info, DumpType_Minimal);

		const char* wantFull = getenv("GX_FULL_CRASHDUMP");
		if (wantFull != nullptr && wantFull[0] != '\0' && wantFull[0] != '0')
		{
			TheMiniDumper->TriggerMiniDumpForException(e_info, DumpType_Full);
		}
	}

	MiniDumper::shutdownMiniDumper();
#endif

	// GeneralsX: The stderr mirror is fed through a pipe drained
	// by a helper thread, so the last few lines before a fault can still be in flight when
	// the process is torn down. Give the pump a moment. Costs 150 ms on a path that already
	// spends seconds writing a minidump, and it deliberately does not touch stderr's FILE
	// lock — the faulting thread may not be the one holding it.
	GXStderrLog_Settle();

	return EXCEPTION_EXECUTE_HANDLER;
}

// WinMain ====================================================================
/** Application entry point */
//=============================================================================
Int APIENTRY WinMain( HINSTANCE hInstance, HINSTANCE hPrevInstance,
                      LPSTR lpCmdLine, Int nCmdShow )
{
	Int exitcode = 1;
	// GeneralsX: a shipped build reads only the GX_* variables on its allow-list: first, before anything reads one
	gxScrubEnvironment();

#ifdef RTS_PROFILE_LEGACY
  Profile::StartRange("init");
#endif

	try {

		SetUnhandledExceptionFilter( UnHandledExceptionFilter );
		//
		// there is something about checkin in and out the .dsp and .dsw files
		// that blows the working directory information away on each of the
		// developers machines so we're going to hack it for a while and set our
		// working directory to the directory with the .exe since that's not the
		// default in a DevStudio project
		//

		TheAsciiStringCriticalSection = &critSec1;
		TheUnicodeStringCriticalSection = &critSec2;
		TheDmaCriticalSection = &critSec3;
		TheMemoryPoolCriticalSection = &critSec4;
		TheDebugLogCriticalSection = &critSec5;

		// initialize the memory manager early
		initMemoryManager();

		CommandLine::parseCommandLineForStartup();

		// Mirror stderr into the user-data folder: a WIN32-subsystem process has no console, so the "[CRC] MISMATCH" desync
		// report would be discarded. After parseCommandLineForStartup(), which constructs TheGlobalData, so getPath_UserData()
		// matches where ReleaseCrash writes. Before the minidump init, so a throw there is logged.
		if (TheGlobalData != nullptr && GXStderrLog_Install(TheGlobalData->getPath_UserData().str()))
		{
			fprintf(stderr, "[gx] stderr mirror installed: %sgenerals-stderr.log\n",
				TheGlobalData->getPath_UserData().str());
		}

		// GeneralsX: Readable crash logs in <userdata>\CrashLogs\ (GXCrashLog.h).
		if (TheGlobalData != nullptr)
		{
			static char crashBuild[96];
			snprintf(crashBuild, sizeof(crashBuild), "GeneralsXZH %s%s windows %s", GitShortSHA1,
				GitUncommittedChanges ? "~" : "", sizeof(void *) == 8 ? "x86_64" : "x86");
			GXCrashLog_Install(TheGlobalData->getPath_UserData().str(), crashBuild);
		}
		// GeneralsX: The save gate writes a crash log for a load it stopped
		// because it would have crashed (SaveLoadFeedback.cpp); Core code reaches the writer through this.
		TheGXCrashLogWriter = GXCrashLog_Write;

		// GeneralsX: On an X3D CPU, prefer the CCD with the big L3 (GXCpuSets.h). Here: after
		// the stderr mirror, so its [GXCPU] line lands in generals-stderr.log, and before the engine or DXVK start threads.
		gxPreferLargestL3();


		#ifdef RTS_DEBUG
			// Turn on Memory heap tracking
			int tmpFlag = _CrtSetDbgFlag( _CRTDBG_REPORT_FLAG );
			tmpFlag |= (_CRTDBG_LEAK_CHECK_DF|_CRTDBG_ALLOC_MEM_DF);
			tmpFlag &= ~_CRTDBG_CHECK_CRT_DF;
			_CrtSetDbgFlag( tmpFlag );
		#endif



		// install debug callbacks
	//	WWDebug_Install_Message_Handler(WWDebug_Message_Callback);
	//	WWDebug_Install_Assert_Handler(WWAssert_Callback);


// Force "splash image" to be loaded from a file, not a resource so same exe can be used in different localizations.
#if defined(RTS_DEBUG) || defined RTS_PROFILE_LEGACY

			// check both localized directory and root dir
		char filePath[_MAX_PATH];
		const char *fileName = "Install_Final.bmp";
		static const char *localizedPathFormat = "Data/%s/";
		sprintf(filePath,localizedPathFormat, GetRegistryLanguage().str());
		strlcat(filePath, fileName, ARRAY_SIZE(filePath));
		FILE *fileImage = fopen(filePath, "r");
		if (fileImage) {
			fclose(fileImage);
			gLoadScreenBitmap = (HBITMAP)LoadImage(hInstance, filePath, IMAGE_BITMAP, 0, 0, LR_SHARED|LR_LOADFROMFILE);
		}
		else {
			gLoadScreenBitmap = (HBITMAP)LoadImage(hInstance, fileName, IMAGE_BITMAP, 0, 0, LR_SHARED|LR_LOADFROMFILE);
		}
#else

		// in release, the file only ever lives in the root dir
		gLoadScreenBitmap = (HBITMAP)LoadImage(hInstance, "Install_Final.bmp", IMAGE_BITMAP, 0, 0, LR_SHARED|LR_LOADFROMFILE);
#endif

#ifdef RTS_ENABLE_CRASHDUMP
		// Initialize minidump facilities - requires TheGlobalData so performed after parseCommandLineForStartup
		MiniDumper::initMiniDumper(TheGlobalData->getPath_UserData());
#endif

		// TheSuperHackers @refactor The instance mutex now lives in its own class.
		// GeneralsX: Taken BEFORE the game window exists, not after it. A copy
		// that loses the mutex then never creates one, so the GX_GAME_WINDOW_CLASS window is the primary
		// holder's. Checked after the window, two copies double-clicked together each created one first
		// and could raise each other instead of the running game.
		if (!rts::ClientInstance::initialize())
		{
			// The winner takes the mutex just before it creates its window, so a copy launched in the
			// same instant can look too early. Poll briefly. Trade-off: fixed 2 s cap, because a headless
			// winner never gets a window; this copy has none either, so the wait shows nothing.
			HWND ccwindow = FindWindow(GX_GAME_WINDOW_CLASS, nullptr);
			for (int tries = 0; ccwindow == nullptr && tries < 20; ++tries)
			{
				Sleep(100);
				ccwindow = FindWindow(GX_GAME_WINDOW_CLASS, nullptr);
			}
			if (ccwindow)
			{
				SetForegroundWindow(ccwindow);
				ShowWindow(ccwindow, SW_RESTORE);
			}

			DEBUG_LOG(("Generals is already running...Bail!"));
			shutdownMemoryManager();
			return exitcode;
		}
		DEBUG_LOG(("Create Generals Mutex okay."));

		// register windows class and create application window
		if(!TheGlobalData->m_headless && initializeAppWindows(hInstance, nCmdShow, TheGlobalData->m_windowed) == false)
		{
			return exitcode;
		}

		// save our application instance for future use
		ApplicationHInstance = hInstance;


		// BGC - initialize COM
	//	OleInitialize(nullptr);



		// Set up version info
		TheVersion = NEW Version;
		TheVersion->setVersion(VERSION_MAJOR, VERSION_MINOR, VERSION_BUILDNUM, VERSION_LOCALBUILDNUM,
			AsciiString(VERSION_BUILDUSER), AsciiString(VERSION_BUILDLOC),
			AsciiString(__TIME__), AsciiString(__DATE__));

		// Copy the player's data into GeneralsX's own folder once (see GXUserDataMigrate.h), after the single-instance
		// mutex so a second launch cannot start a parallel migration. A worker copies while this thread pumps messages.
		// Before GameMain(), which waits for it: engine init reads Options.ini, SagePatch.ini and the map cache.
		// IDENTITY_PENDING: no new identity may be minted. Skipped when GX_USERDATA_DIR applies (a test profile).
		std::string testDir, verdict;
		const bool testProfile = gxUserDataDirFromEnv(testDir, verdict);
		if (!verdict.empty())
			fprintf(stderr, "%s%s\n", verdict.c_str(), testProfile ? "; the one-time user-data migration is skipped" : "");
		if (!testProfile && TheGlobalData != nullptr && !TheGlobalData->getPath_UserData().isEmpty())
		{
			AsciiString leaf;
			if (!GetStringFromRegistry("", "UserDataLeafName", leaf))
				leaf = GX_LEGACY_USER_DATA_LEAF_DEFAULT;
			const char *userData = TheGlobalData->getPath_UserData().str();
			const std::string legacy = gxLegacyUserDataDir(userData, leaf.str());
			if (!legacy.empty() && gxMigrateUserDataPumping(legacy.c_str(), userData) == GX_MIGRATE_IDENTITY_PENDING)
				GXPlayerIdentity::holdNewIdentity();
			// A relative -mod the new folder lacks is looked for here, read only (GXUserDataDir.h).
			gxLegacyUserDataDirForMods() = legacy;
		}

		DEBUG_LOG(("CRC message is %d", GameMessage::MSG_LOGIC_CRC));

		// GeneralsX: Freed HERE, after the user-data copy, not right after the
		// window was made: the copy pumps the window (gxMigrateUserDataPumping), and a WM_PAINT during it
		// repainted the loading window without its image, black once the background was erased.
		if (gLoadScreenBitmap!=nullptr) {
			::DeleteObject(gLoadScreenBitmap);
			gLoadScreenBitmap = nullptr;
		}

		// run the game main loop - unless the player closed the window during the copy above
		if (gQuitBeforeEngine)
		{
			fprintf(stderr, "[GXMIGRATE] closed during the user-data copy; not starting the game\n");
			exitcode = 0;
		}
		else
			exitcode = GameMain();

		delete TheVersion;
		TheVersion = nullptr;

	#ifdef MEMORYPOOL_DEBUG
		TheMemoryPoolFactory->debugMemoryReport(REPORT_POOLINFO | REPORT_POOL_OVERFLOW | REPORT_SIMPLE_LEAKS, 0, 0);
	#endif
	#if defined(RTS_DEBUG)
		TheMemoryPoolFactory->memoryPoolUsageReport("AAAMemStats");
	#endif

	#ifdef GX_RENDER_NATIVE
		// GeneralsX: the native session's final stats line if its device outlived the
		// engine's teardown (as SDL3Main.cpp does): the line allocates, so before the memory manager goes.
		GxNative::FlushFinalStats();
	#endif
		shutdownMemoryManager();

		// BGC - shut down COM
	//	OleUninitialize();
	}
	catch (...)
	{

	}

#ifdef RTS_ENABLE_CRASHDUMP
	MiniDumper::shutdownMiniDumper();
#endif
	TheUnicodeStringCriticalSection = nullptr;
	TheDmaCriticalSection = nullptr;
	TheMemoryPoolCriticalSection = nullptr;

#if defined(GX_TEST_INSTRUMENTS)
	// GeneralsX: the [GXHOOK] timers' open frames and the "[GXHOOK] exit"
	// line, then let the stderr pump drain them - WinMain returns into ExitProcess: no static destructors, and the pump threads die
	GX_HOOK_FLUSH_ALL();
	GXStderrLog_Settle();
#endif

	return exitcode;

}

// CreateGameEngine ===========================================================
/** Create the Win32 game engine we're going to use */
//=============================================================================
GameEngine *CreateGameEngine()
{
	Win32GameEngine *engine;

	engine = NEW Win32GameEngine;
	//game engine may not have existed when app got focus so make sure it
	//knows about current focus state.
	engine->setIsActive(isWinMainActive);

	return engine;

}
