# GeneralsX: The Windows launcher is the TAURI one (scripts/launcher-rs).
#
# Dot-sourced by setup-run-win64.ps1 and package-windows-share.ps1. Both stage a file called
# GeneralsXLauncher.exe, and two different programs have carried that name: the retired C# loader
# (scripts/build/windows/loader, ~60 MB .NET single-file, defaults WINDOWED) and the Tauri launcher
# (~12 MB, defaults FULLSCREEN). The published win32 tier carried Tauri while these scripts still
# built and staged the C# one, so a name and a
# size check proved nothing. This checks the bytes, with the same markers that probe used.
#
# Windows PowerShell 5.1 compatible: it runs under `powershell -File`.
function Assert-TauriLauncher([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "no launcher at $Path - build it with scripts/build/launcher/package-launcher-windows.sh (it lands in C:\dev\gx\out\GeneralsXLauncher.exe)"
    }
    $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($Path))
    # Ordinal, not culture: a 12-60 MB string and an ASCII marker.
    if ($text.IndexOf("System.Private.CoreLib", [StringComparison]::Ordinal) -ge 0) {
        throw "$Path is the retired C# loader (.NET marker found), not the Tauri launcher. Stage C:\dev\gx\out\GeneralsXLauncher.exe from package-launcher-windows.sh instead."
    }
    if ($text.IndexOf("gxlauncher", [StringComparison]::Ordinal) -lt 0 -or
        $text.IndexOf("tauri", [StringComparison]::Ordinal) -lt 0) {
        throw "$Path does not look like the Tauri launcher (no 'gxlauncher'/'tauri' marker)."
    }
}

# Presence is not identity, for GeneralsXUpdate.exe too. Three refusals, each a build that already shipped or nearly did:
#  (a) no embedded 40-hex "GX_UPDATER_COMMIT=" (built before the commit was embedded, or without the define);
#  (b) no "REFUSED - GX_USERDATA_DIR" string (aba1549a0 shipped without 54f9cf3b2's test-profile refusal);
#  (c) the embedded commit's updater inputs differ from the release sha's: a CI artifact is built at its own run's
#      sha, which need not be the release sha. $InputPaths is the workflow's `paths:` filter plus GXScopedWipe.h and its GXSecureClear.h. The workflow file is NOT a path
#      input: it also holds triggers, the VirusTotal/attest steps and the retired loader job, and 0.9.63's updater was
#      refused over an edit to those. Get-GxCiBuildStep compares only what can reach the bytes (see there).
# Needs git and the repo that contains the embedded commit; an unreachable commit is a refusal, never a pass.
$script:GxUpdaterInputPaths = @(
    "scripts/build/windows/launcher",
    "Core/GameEngine/Source/GameClient/GXEd25519.cpp", "Core/GameEngine/Source/GameClient/GXSha512.cpp",
    "Core/GameEngine/Source/GameClient/GXIdentityAssertion.cpp",
    "Core/GameEngine/Include/GameClient/GXEd25519.h", "Core/GameEngine/Include/GameClient/GXSha512.h",
    "Core/GameEngine/Include/GameClient/GXIdentityAssertion.h", "Core/GameEngine/Include/GameClient/GXScopedWipe.h", "Core/GameEngine/Include/GameNetwork/GXSecureClear.h",
    "scripts/launcher-rs/gxlauncher/icons/icon.ico")
# The workflow's build-relevant text at <rev>, as a COMPLEMENT: everything is in unless it is known not to reach the updater's bytes.
# Left out: comments and blank lines, the workflow `name:`, the whole `on:` block (triggers), every job but `launcher`, the
# launcher job's display `name:`, the VirusTotal secret reference, and the launcher job's VirusTotal, attestation and upload steps (the later proof / self-test
# steps ARE compared: a step that edited the exe after Build would change what ships). Everything else - workflow env, permissions, the job's runner / env / strategy wherever they sit,
# the toolchain step (`arch:`) and the Build step - is compared. An exclusion could wave a differently built updater through; an inclusion
# can only cause a false refusal (fixed by rebuilding the updater), so every doubt is an inclusion. The layout is this repo's (job key at 2 spaces,
# steps at 6): a re-indented or renamed workflow returns $null = refusal, loudly.
# $null when git cannot show the file or the Build step is missing - the caller treats that as a difference, never a pass.
function Get-GxCiBuildStep([string]$Repo, [string]$Rev) {
    $lines = & git -C $Repo show "${Rev}:.github/workflows/build-launcher.yml" 2>$null
    if ($LASTEXITCODE -ne 0) { return $null }
    $out = @(); $inOn = $false; $inJobs = $false; $inJob = $false; $seenBuild = $false; $dropSteps = $false
    $pendEnv = $false
    foreach ($l in $lines) {
        $t = $l.Trim()
        if ($t -eq "" -or $t.StartsWith("#") -or ($t -match '^rem(\s|$)' -and $t -notmatch '[&|<>]')) { continue }   # blank, YAML comment, cmd `rem` comment (a rem line with & | < > can still run a command)
        $indent = $l.Length - $l.TrimStart().Length
        if ($indent -eq 0) {
            $inOn = ($t -match '^on:'); $inJobs = ($t -match '^jobs:\s*$'); $pendEnv = $false
            if ($inOn -or $inJobs -or $t -match '^name:') { continue }
            if ($t -match '^env:\s*$') { $pendEnv = $true; continue }
            if ($t -match '^env:\s*\S') {   # a one-line map: the VirusTotal entry is dropped, any other entry stays
                $v = ($t -replace '(?<![A-Za-z0-9_])VT_API_KEY\s*:\s*("[^"]*"|\$\{\{.*?\}\}|[^,}]*)\s*,?\s*', '') -replace ',\s*\}', ' }'
                if ($v -notmatch '^env:\s*\{\s*\}\s*$') { $out += $v }
                continue
            }
            $out += $t; continue
        }
        if (-not $inJobs) {
            if ($inOn -or $t -match '^VT_API_KEY\s*:') { continue }
            if ($pendEnv) { $out += "env:"; $pendEnv = $false }   # the header only when a real key follows
            $out += $t; continue
        }
        if ($indent -eq 2) {
            $inJob = ($t -match '^launcher:\s*$'); $dropSteps = $false
            if ($inJob) { $out += "job: launcher" }
            continue
        }
        if (-not $inJob) { continue }
        if ($dropSteps) { if ($indent -ge 6 -and -not ($indent -eq 6 -and $t.StartsWith("- "))) { continue } else { $dropSteps = $false } }
        if ($indent -eq 4 -and $t -match '^name:') { continue }   # the job's display name
        if ($t -match '^- name: Build\s*(#.*)?$' -and $indent -eq 6) { $seenBuild = $true }
        elseif ($seenBuild -and $indent -eq 6 -and $t.StartsWith("- ") -and
                $t -match '^- (name: (Submit to VirusTotal|Attest build provenance)\s*(#.*)?$|uses: actions/upload-artifact@v\d+\s*(#.*)?$)') {
            $dropSteps = $true; continue   # the three steps that never touch the exe's bytes (scan report, attestation, upload)
        }
        $out += $t
    }
    if (-not $seenBuild) { return $null }
    return ($out -join "`n")
}
function Assert-GxUpdater([string]$Path, [string]$Repo, [string]$ReleaseSha) {
    if (-not (Test-Path -LiteralPath $Path)) { throw "no updater at $Path - build it with scripts/build/windows/launcher/build-launcher.cmd" }
    $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($Path))
    $m = [regex]::Match($text, "GX_UPDATER_COMMIT=([0-9a-f]{40})(?![0-9a-f])")
    if (-not $m.Success) { throw "$Path embeds no 40-hex GX_UPDATER_COMMIT - rebuild it with build-launcher.cmd (or take the build-launcher.yml artifact)." }
    if ($text.IndexOf("REFUSED - GX_USERDATA_DIR", [StringComparison]::Ordinal) -lt 0) {
        throw "$Path lacks the test-profile refusal (54f9cf3b2) - it is an older updater build."
    }
    $commit = $m.Groups[1].Value
    # Native stderr is an error record under -ErrorAction Stop in PowerShell 5.1; judge git by its exit code only.
    $ea = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    try {
        & git -C $Repo cat-file -e "$commit^{commit}" 2>&1 | Out-Null
        $haveCommit = ($LASTEXITCODE -eq 0)
        if ($haveCommit) {
            & git -C $Repo diff --quiet $commit $ReleaseSha -- @($script:GxUpdaterInputPaths) 2>&1 | Out-Null
            $same = ($LASTEXITCODE -eq 0)
            if ($same) {
                $a = Get-GxCiBuildStep $Repo $commit
                $b = Get-GxCiBuildStep $Repo $ReleaseSha
                $same = ($null -ne $a -and $a -ceq $b)
            }
        }
    } finally { $ErrorActionPreference = $ea }
    if (-not $haveCommit) { throw "$Path embeds commit $commit, which is not in $Repo - fetch it, or rebuild the updater at the release sha." }
    if (-not $same) { throw "$Path was built at $commit and the updater inputs (or the workflow Build step) differ at $ReleaseSha - rebuild the updater at the release sha." }
}
