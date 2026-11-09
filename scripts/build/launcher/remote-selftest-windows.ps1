# GeneralsX launcher packaging - run --gx-selftest in SESSION 1 on the Windows box.
#
# An SSH session is not session 1. Measured on this box 2026-08-20: an ssh login reports
# SessionId 0 while `query session` shows the console user active in session 1. A WebView2 window
# created from a service session either fails outright or lands on a window station nobody can
# observe, and either way the result would not be evidence about what a player sees. So the
# selftest runs through an interactive scheduled task, the same mechanism this project already uses
# to drive the game's GUI on that box.
#
# Start-Process is deliberately absent HERE: the launcher is a GUI app and needs session 1, which
# is what the task provides. (remote-shipsmoke-windows.ps1 does use Start-Process -Wait, for a
# silent installer that renders nothing - a different problem with a different answer.)
#
# THE WAIT POLLS THE ARTIFACT, NOT THE TASK STATE. A task that has been created but has not yet
# transitioned to Running reports "Status: Ready", so a loop that breaks on Ready breaks on its
# first poll and deletes the task while the launcher is still starting - the deadline never
# engages. The evidence is the json file being non-empty, which is this plan's own rule.
param(
    [Parameter(Mandatory=$true)][string]$ExePath,
    # Fixed, space-free scratch directory. NOT derived from $ExePath: see the /TR note below, and
    # because writing the launcher's own selftest output into its install directory leaves residue
    # the uninstaller is not obliged to clear.
    [string]$WorkDir = "C:\dev\gx\out"
)
$ErrorActionPreference = "Stop"

if (-not (Test-Path $ExePath)) { throw "no $ExePath" }
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
$json = Join-Path $WorkDir "selftest.json"
$err  = Join-Path $WorkDir "selftest.err"
$runner = Join-Path $WorkDir "gxselftest.cmd"
Remove-Item -Force -ErrorAction SilentlyContinue $json, $err

# /TR TAKES A PATH TO A SCRIPT, NEVER A QUOTED COMMAND LINE, AND THE PLAN GOT THIS WRONG.
# The plan builds /TR as `cmd /c ""<exe>" --gx-selftest > "<json>" 2> "<err>""`. Measured
# 2026-08-20 that fails with  ERROR: Invalid argument/option - 'Launcher\GeneralsX'  because
# PowerShell re-quotes a native argument containing both spaces and embedded quotes, and schtasks
# then re-splits it on the first space inside the exe path. It only appeared to work against the
# loose build-tree exe at C:\dev\gx\out\GeneralsXLauncher.exe, whose path has no spaces; the
# INSTALLED path is "%LOCALAPPDATA%\GeneralsX Launcher\GeneralsX Launcher.exe" and always has two,
# so Task 14's Windows half could not have passed as written.
#
# Writing the command to a .cmd and pointing /TR at that path is this repository's own standing
# rule for exactly this class of failure, and it removes every layer of quoting at once: schtasks
# sees one space-free path.
# THE TASK RUNS IN SESSION 1, SO THIS SSH SESSION'S ENVIRONMENT NEVER REACHES IT; a scratch profile therefore
# has to be set INSIDE the runner. Without it the launcher's state dir (%APPDATA%\GeneralsX) and the webview's
# user data (%LOCALAPPDATA%\<identifier>\EBWebView) are the owner's real ones, and a smoke run rewrites them
# (the 0.9.50 staging smoke did). APPDATA alone is NOT enough, measured: the webview resolves its user-data folder
# through the shell's known-folder API and ignores %LOCALAPPDATA%, so EBWebView under the owner's profile kept
# changing. WEBVIEW2_USER_DATA_FOLDER moves it, and (unlike GX_USERDATA_DIR) leaves the launcher's state dir on its
# production %APPDATA% path, which smoke-launch.sh's --forbid-prefix check still has to see (Codex winsmoke r1).
# The Documents game profile (Options.ini, SagePatch.ini) is resolved through the shell and is NOT redirected here;
# measured 01/10/2026 with per-file sha256 of every Documents\*Generals* folder before and after: --gx-selftest leaves
# it byte-identical (9361 files, same digest), with this script and with the unmodified one.
# Beside $WorkDir, never inside it: package-launcher-windows.sh passes C:\dev\gx\out as smoke-launch.sh's forbidden
# prefix, and a state dir under it would fail every passing selftest (Codex winsmoke r2).
$scratch = Join-Path (Split-Path -Parent $WorkDir) "selftest-profile"
New-Item -ItemType Directory -Force -Path (Join-Path $scratch "AppData\Roaming"), (Join-Path $scratch "AppData\Local") | Out-Null
$body = @(
    "@echo off",
    ('set "APPDATA={0}"' -f (Join-Path $scratch "AppData\Roaming")),
    ('set "LOCALAPPDATA={0}"' -f (Join-Path $scratch "AppData\Local")),
    ('set "WEBVIEW2_USER_DATA_FOLDER={0}"' -f (Join-Path $scratch "EBWebView")),
    ('"{0}" --gx-selftest > "{1}" 2> "{2}"' -f $ExePath, $json, $err)
)
Set-Content -LiteralPath $runner -Value $body -Encoding ASCII

# /RU with the current user AND /IT. Without /RU, `schtasks /Create ... /IT` prompts for the
# current user's password, and over `ssh -o BatchMode=yes` that is a hang rather than an error.
#
# EVERY schtasks CALL IS CAPTURED, AND ErrorActionPreference IS LOWERED WHILE IT HAPPENS. Both
# halves of that are measured, and each failed on its own first:
#
#   `/SC ONCE /ST 00:00` makes schtasks print "WARNING: Task may not run because /ST is earlier
#   than current time." on STDERR while still exiting 0 and creating the task. `| Out-Null`
#   swallows only PowerShell's success stream, so that warning flows straight past it; the caller
#   merges the remote's stderr into the json file and gets two lines where the contract allows
#   exactly one. The run then reports "SMOKE FAIL: not exactly one line of output (2 lines)" about
#   a launcher that opened its window perfectly.
#
#   Capturing it with `2>&1` alone is worse, not better. Under $ErrorActionPreference = "Stop" a
#   native command's redirected stderr becomes a TERMINATING NativeCommandError, so the script
#   aborts at the /Create line and the selftest never runs at all - and the seven-line error block
#   lands in the json file, so the symptom still reads as "too many lines". The preference has to
#   come down for exactly these calls and go back up afterwards.
$prevEap = $ErrorActionPreference
$ErrorActionPreference = "Continue"
$createOut = (& schtasks /Create /TN GXLauncherSelfTest /TR $runner /SC ONCE /ST 00:00 /RU $env:USERNAME /IT /F 2>&1 | Out-String)
$createRc  = $LASTEXITCODE
$runOut = ""
$runRc  = 0
if ($createRc -eq 0) {
    $runOut = (& schtasks /Run /TN GXLauncherSelfTest 2>&1 | Out-String)
    $runRc  = $LASTEXITCODE
}
$ErrorActionPreference = $prevEap

# The parentheses matter. `"a" + "b" -f $x` binds -f tighter than +, so only the LAST fragment is
# formatted and every placeholder in the earlier ones prints literally as {0}. That is how the
# first run of this script reported `schtasks /Create exited {0} ({1})` and threw away the actual
# exit code and the actual schtasks message - the two facts needed to diagnose it.
if ($createRc -ne 0) {
    $msg = ("schtasks /Create exited {0} ({1}). If this box's policy refuses an unattended /RU, " +
            "register the task ONCE at the console with:  schtasks /Create /TN GXLauncherSelfTest " +
            "/TR {2} /SC ONCE /ST 00:00 /RU {3} /IT /F   and re-run.")
    throw ($msg -f $createRc, $createOut.Trim(), $runner, $env:USERNAME)
}
if ($runRc -ne 0) { throw (("schtasks /Run exited {0} ({1})") -f $runRc, $runOut.Trim()) }

$deadline = (Get-Date).AddSeconds(120)
while ((Get-Date) -lt $deadline) {
    if ((Test-Path $json) -and (Get-Item $json).Length -gt 0) { break }
    Start-Sleep -Seconds 2
}

# Last Result is read BEFORE the task is deleted: a task that never started and a task that ran and
# produced nothing are otherwise indistinguishable.
$prevEap = $ErrorActionPreference
$ErrorActionPreference = "Continue"
$query = (& schtasks /Query /TN GXLauncherSelfTest /FO LIST /V 2>&1 | Out-String)
$lastResult = ($query -split "`r?`n" | Where-Object { $_ -match "^Last Result:" }) -join " "
& schtasks /Delete /TN GXLauncherSelfTest /F 2>&1 | Out-Null
$ErrorActionPreference = $prevEap

if (-not (Test-Path $json) -or (Get-Item $json).Length -eq 0) {
    Write-Host "SELFTEST FAIL: no output at $json after 120s"
    Write-Host $lastResult
    if (Test-Path $err) { Get-Content $err | Select-Object -First 20 | ForEach-Object { Write-Host $_ } }
    exit 1
}
Get-Content -Raw $json
