# Build GeneralsX-Setup.exe from GeneralsX.iss with the payload pinned. Mirrors .github/workflows/build-launcher.yml.
param(
  [string]$Iss = "C:\dev\GeneralsX.iss",
  [Parameter(Mandatory=$true)][string]$PayloadUrl,
  [Parameter(Mandatory=$true)][string]$PayloadSha256,
  [string]$AppVersion = "0.1.0",
  [string]$OutDir = "C:\dev\setup-out"
)
$ErrorActionPreference = "Stop"

# Locate ISCC without guessing a single path: try the common install dirs, then the uninstall registry.
$cands = @(
  "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
  "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
  "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
)
$iscc = $cands | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) {
  $keys = 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
          'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
          'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*'
  $loc = Get-ItemProperty $keys -ErrorAction SilentlyContinue |
         Where-Object { $_.DisplayName -like '*Inno Setup*' } |
         Select-Object -First 1 -ExpandProperty InstallLocation
  if ($loc) { $iscc = Join-Path $loc "ISCC.exe" }
}
if (-not $iscc -or -not (Test-Path $iscc)) { throw "ISCC.exe not found (Inno Setup not installed?)" }
Write-Host "ISCC: $iscc"

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Get-ChildItem $OutDir -Filter *.exe -ErrorAction SilentlyContinue | Remove-Item -Force

& $iscc "/DPayloadUrl=$PayloadUrl" "/DPayloadSha256=$PayloadSha256" "/DAppVersion=$AppVersion" "/O$OutDir" $Iss
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with $LASTEXITCODE" }

$exe = Join-Path $OutDir "GeneralsX-Setup.exe"
if (-not (Test-Path $exe)) { throw "no GeneralsX-Setup.exe produced" }

$len = (Get-Item $exe).Length
$sha = (Get-FileHash $exe -Algorithm SHA256).Hash.ToLower()

# Same three gates CI asserts: small (not embedding the payload), GUI subsystem, no elevation.
if ($len -gt 10485760) { throw "installer is $len bytes - it is embedding the payload" }
$b   = [IO.File]::ReadAllBytes($exe)
$pe  = [BitConverter]::ToInt32($b, 0x3C)
$sub = [BitConverter]::ToInt16($b, $pe + 0x5C)
if ($sub -ne 2) { throw "expected GUI subsystem (2), got $sub" }
$txt = [Text.Encoding]::ASCII.GetString($b)
$lv  = [regex]::Matches($txt, '<requestedExecutionLevel[^>]*level\s*=\s*"([^"]+)"')
if ($lv.Count -eq 0) { throw "no requestedExecutionLevel in the installer" }
foreach ($m in $lv) { if ($m.Groups[1].Value -ne 'asInvoker') { throw "installer asks for elevation ($($m.Groups[1].Value))" } }

Write-Host ("SETUP_OK path={0} size={1} sha256={2} subsystem={3} level=asInvoker" -f $exe, $len, $sha, $sub)
