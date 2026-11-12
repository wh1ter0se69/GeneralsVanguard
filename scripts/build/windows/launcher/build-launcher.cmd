@echo off
rem Build the GeneralsX UPDATER. Run ON the Windows box:
rem
rem NAMING: this used to produce GeneralsXLauncher.exe. That name now belongs to the program a
rem friend actually double-clicks - the Tauri launcher (scripts/launcher-rs) since 24/09/2026, the
rem C# loader (scripts/build/windows/loader, retired) before it. This is the engine underneath it: it fetches the signed manifest, verifies Ed25519,
rem downloads, hashes and stages. It is invoked by the launcher and is not meant to be run by hand.
rem
rem     ssh <WIN> "cmd /c C:\dev\gxlauncher\build-launcher.cmd"
rem
rem GeneralsX @build wh1ter0se 08/08/2026
rem
rem A .cmd, and driven over ssh as `cmd /c <file>`, because vcvarsall's quoting cannot survive
rem PowerShell-over-SSH - which is the default shell on that box. vcvarsall MUST be called first
rem or every translation unit fails with "Cannot open include file: 'stdlib.h'", which reads like a
rem broken toolchain rather than a missing environment.
rem
rem No CMake and no vcpkg on purpose. This program links nothing but OS import libraries, so a
rem build system would be pure ceremony - and keeping it out means the launcher cannot accidentally
rem acquire a dependency that a friend would have to install.
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" (
    echo BUILD_FAIL=no-vcvarsall
    exit /b 1
)
call "%VCVARS%" x64 >nul
if %ERRORLEVEL% NEQ 0 ( echo BUILD_FAIL=vcvarsall & exit /b 1 )

rem /MT, not /MD: a friend must not need the Visual C++ redistributable to launch a game.
rem
rem /SUBSYSTEM:WINDOWS with /ENTRY:wmainCRTStartup keeps a plain wmain(argc, argv) while producing
rem an executable that opens NO CONSOLE WINDOW when double-clicked. A console-subsystem launcher
rem would flash a black window in front of every launch. Redirected output still works - `cmd /c
rem gxlaunch.exe --selftest x > out.txt` both waits and captures - so --check and --selftest remain
rem usable from a shell, which is how this is verified.
rem
rem /MANIFESTUAC asInvoker is NOT decoration. Windows' installer-detection heuristic elevates a
rem legacy-looking executable, and an unsigned program asking for administrator is the exact shape
rem people are right to refuse. The install lives under %LOCALAPPDATA%, which the user owns; there
rem is nothing here to elevate for. The program's NAME also avoids "update"/"setup"/"install" for
rem the same reason, since that heuristic keys on the filename.
set "OUT=GeneralsXUpdate.exe"
rem Embed the commit (see g_updater_commit in gxlaunch.c) and refuse a tree whose updater inputs are dirty: a
rem commit that does not describe the bytes is worse than none. package-windows-share.ps1 checks it.
set "GXU_PATHS=scripts/build/windows/launcher Core/GameEngine/Source/GameClient/GXEd25519.cpp Core/GameEngine/Source/GameClient/GXSha512.cpp Core/GameEngine/Source/GameClient/GXIdentityAssertion.cpp Core/GameEngine/Include/GameClient/GXEd25519.h Core/GameEngine/Include/GameClient/GXSha512.h Core/GameEngine/Include/GameClient/GXIdentityAssertion.h Core/GameEngine/Include/GameClient/GXScopedWipe.h Core/GameEngine/Include/GameNetwork/GXSecureClear.h scripts/launcher-rs/gxlauncher/icons/icon.ico"
set "GXU_COMMIT="
for /f %%i in ('git -C "..\..\..\.." rev-parse HEAD') do set "GXU_COMMIT=%%i"
if not defined GXU_COMMIT ( echo BUILD_FAIL=no-git-commit & exit /b 1 )
set "GXU_ST=%TEMP%\gxu-status-%RANDOM%.txt"
git -C "..\..\..\.." status --porcelain --untracked-files=no -- %GXU_PATHS% > "%GXU_ST%"
if errorlevel 1 ( echo BUILD_FAIL=git-status-failed & del /q "%GXU_ST%" 2>nul & exit /b 1 )
set "GXU_DIRTY="
for /f "usebackq delims=" %%i in ("%GXU_ST%") do set "GXU_DIRTY=1"
del /q "%GXU_ST%" 2>nul
if defined GXU_DIRTY ( echo BUILD_FAIL=dirty-tree & exit /b 1 )
del /q "%OUT%" 2>nul

rem The icon and version resource - what Task Manager shows instead of a blank generic exe. See the
rem header of gxlaunch.rc. CI (.github/workflows/build-launcher.yml) runs the same two lines.
del /q gxlaunch.res 2>nul
rc /nologo /fo gxlaunch.res gxlaunch.rc
if %ERRORLEVEL% NEQ 0 ( echo BUILD_FAIL=rc & exit /b 1 )

rem THE VERIFIER NO LONGER LIVES IN THIS DIRECTORY. GXEd25519 moved into the engine on 11/08/2026
rem because the game needs it too - the identity assertion is Ed25519-signed and the Windows engine
rem has no HTTPS, so fetching stays here and verifying happens there. One implementation, not two:
rem a security anchor with a copy is a security anchor that drifts. GX_ED25519_WITH_CNG is what
rem keeps gx_sha256/gx_sha256_file compiled in for this program; the engine leaves it undefined and
rem therefore never links bcrypt.lib.
rem
rem The relative paths assume this file's own directory, which `cd /d "%~dp0"` above guarantees.
rem
rem GeneralsX @bugfix wh1ter0se 11/08/2026 /WX HERE TOO, and the reason is the whole point of this line.
rem This script used /W4 while .github/workflows/build-launcher.yml:93 used /W4 /WX. A warning was
rem therefore harmless locally and fatal in CI, so the launcher could be built, staged and believed
rem while the only job that compiles this code with MSVC was red. That is exactly what happened: CI
rem failed on GXIdentityAssertion.cpp's fopen from the login merge onwards and nobody saw it, because
rem the local build kept exiting 0. Parity is the fix - if it fails in CI it must fail here.
cl /nologo /W4 /WX /O2 /MT /GS /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /DGX_ED25519_WITH_CNG /DGX_UPDATER_COMMIT=\"%GXU_COMMIT%\" ^
   /I "..\..\..\..\Core\GameEngine\Include" ^
   /Fe:"%OUT%" gxlaunch.c ^
   "..\..\..\..\Core\GameEngine\Source\GameClient\GXEd25519.cpp" ^
   "..\..\..\..\Core\GameEngine\Source\GameClient\GXSha512.cpp" ^
   "..\..\..\..\Core\GameEngine\Source\GameClient\GXIdentityAssertion.cpp" ^
   gxlaunch.res ^
   /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup ^
   /MANIFESTUAC:"level='asInvoker' uiAccess='false'"
if %ERRORLEVEL% NEQ 0 ( echo BUILD_FAIL=cl & exit /b 1 )

rem Gate on the ARTIFACT, never on the exit code - "a build that does nothing exits 0" is the
rem failure the four-platform verifier exists to catch, and it applies just as much here.
if not exist "%OUT%" ( echo BUILD_FAIL=no-artifact & exit /b 1 )
for %%I in ("%OUT%") do echo BUILD_OK %%~zI bytes %%~tI
exit /b 0

rem ==============================================================================================
rem WIRING IT INTO THE SHARE BUILD - not done here, and deliberately left as three edits somebody
rem makes on purpose rather than something that arrives with a launcher.
rem
rem scripts/build/windows/package-windows-share.ps1
rem   1. ship GeneralsXLauncher.exe in the zip alongside generalszh.exe.
rem
rem scripts/build/windows/install-windows-share.ps1
rem   2. add "GeneralsXLauncher.exe" to the file list it copies into %LOCALAPPDATA%\Programs\GeneralsX.
rem   3. point both shortcuts at GeneralsXLauncher.exe instead of GeneralsX.cmd:
rem          $lnk.TargetPath = Join-Path $here "GeneralsXLauncher.exe"
rem      GeneralsX.cmd STAYS exactly as it is - the launcher runs it, because it is what knows
rem      CNC_GENERALS_ZH_PATH and that the working directory has to be the install folder. Nothing
rem      about the existing launch path changes; the launcher goes in front of it.
rem
rem Until then the launcher is inert: it is not shipped, and an install without it behaves exactly
rem as it does today. Nothing in this directory is reachable from the game's own build.
rem
rem THAT LAST SENTENCE STILL HOLDS, and it is worth saying why after 11/08/2026, because the build
rem line above now names files under Core/. The dependency goes ONE WAY: this program reads the
rem engine's verifier. The engine's build does not name a single file in this directory, so nothing
rem here can drag a dependency, a Windows header or a build step into the game. That direction is
rem the whole point - it is why GXEd25519 moved out rather than the engine reaching in.
rem
rem The RELAY it asks comes from RelayAddress in the game's Options.ini, which that installer
rem already writes - so there is no new setting for a friend to get wrong. gxlaunch.ini beside the
rem executable overrides it when a second relay or a different platform token is wanted.
