; GeneralsX-Setup.exe - a ~1 MB downloader, not a 68 MB offline installer.
;
; GeneralsX:
;
; WHY THIS REPLACED A SELF-CONTAINED INSTALLER. The previous shape embedded the launcher, which is a
; self-contained WinForms app and therefore 66 MB on its own - so the installer was 68 MB, and every
; release meant redistributing all of it. It is also the worst possible profile for antivirus
; heuristics: a large single-file binary that downloads and executes other binaries.
;
; This stub embeds nothing but itself. The engine, the data and the launcher all arrive in the
; payload the manifest already describes, which is exactly where they were anyway.
;
; NO UAC, EVER. PrivilegesRequired=lowest and an install under {localappdata} - the same folder the
; old PowerShell installer used, and for the same reasons written there: per-user needs no elevation,
; an unsigned installer asking for administrator is the shape people are right to refuse, and
; Program Files would require both. Nothing here writes outside the user's own directories.
;
; INTEGRITY. Inno's DownloadTemporaryFile takes a RequiredSHA256OfFile and refuses the file if it
; does not match, so the payload hash is pinned into this installer at BUILD time by CI. That is
; install-time integrity without a code-signing certificate. Update-time integrity is a different
; mechanism and stays where it is: GeneralsXUpdate.exe verifies an Ed25519-signed manifest against a
; compiled-in key on every launch. Pinned hash to get on the machine; signature chain to stay
; current.
;
; The payload URL and hash are passed in by the build, never edited by hand:
;   iscc /DPayloadUrl=https://... /DPayloadSha256=<64 hex> /DPayloadSize=<bytes> GeneralsX.iss

#ifndef PayloadUrl
  #define PayloadUrl "https://dl.wh1ter0se.dev/GeneralsX-win64.zip"
#endif
#ifndef PayloadSha256
  #define PayloadSha256 ""
#endif
#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif

[Setup]
AppId={{8F3C1D42-6A21-4B7E-9F0C-2E5A7D9B4C11}
AppName=Generals Vanguard
AppVersion={#AppVersion}
AppPublisher=Generals Vanguard
AppPublisherURL=https://wh1ter0se.dev
; A NEW install goes to Programs\GeneralsVanguard. An existing one stays where it is: UsePreviousAppDir
; is left at its default (yes), so running this over an install in Programs\GeneralsX keeps that folder.
; Moving it is a separate step of its own. The Start Menu folder is only a name, so it always takes
; the new one (UsePreviousGroup=no) and [InstallDelete] removes the old-named shortcuts.
DefaultDirName={localappdata}\Programs\GeneralsVanguard
DefaultGroupName=Generals Vanguard
UsePreviousGroup=no
DisableProgramGroupPage=yes
DisableDirPage=no
OutputBaseFilename=GeneralsX-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; The two lines that keep Windows from putting a shield on this and asking for administrator.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
UninstallDisplayName=Generals Vanguard
; OVERWRITE, NOT INNO'S DEFAULT append. With append, a Setup run over an
; install made by an older Setup ADDS to that install's unins000.dat, and the old log still carries the
; `filesandordirs {app}` entry that deleted a player-chosen folder whole - so upgrading would have kept the very
; bug this Setup fixes. This Setup's log names everything Inno itself must remove (its [Files], [Icons], its
; uninstall key); the payload goes by gxfiles.txt, so nothing an older log recorded is lost by starting afresh.
UninstallLogMode=overwrite
; The game's own icon, on the downloaded file itself. A blank generic exe in a downloads folder is
; the shape people are right to be suspicious of - and this installer is already unsigned, so it
; needs every honest signal it can carry. Same .ico the launcher embeds, so the file a person
; downloads, the program it installs and the Start Menu entry all show one recognisable icon.
; Relative to this .iss; the loader project keeps its own copy for exactly this reason.
SetupIconFile=..\loader\GeneralsX.ico
; The Add/Remove Programs entry, pointed at the launcher this installs rather than the setup exe,
; which is gone by then.
UninstallDisplayIcon={app}\GeneralsXLauncher.exe
; Never write into the user's Zero Hour installation. Said here as well as in the UI because it is
; the single promise this installer makes that matters most.
AppComments=Installs Generals Vanguard without modifying your Command & Conquer: Generals Zero Hour install.

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full"; Description: "Generals Vanguard"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Shortcuts:"
; THERE IS DELIBERATELY NO "windowed" TASK, AND PUTTING ONE BACK WOULD BE A REGRESSION. An installer
; task is answered exactly once and can then only be changed by reinstalling, which is the wrong
; shape for a display preference somebody flips between sessions. Windowed mode is the launcher's to
; own; all this installer owes it is a run script that does not override it, which is why the last
; line of GeneralsX.cmd forwards its arguments instead of hardcoding -win.

; Files the OLD flow left behind, removed when installing over it. All three are inert now, but
; relay-settings.ini is actively misleading: it looks like it configures which server you connect
; to, and nothing has read it since the launcher started fetching a signed relay list.
[InstallDelete]
Type: files; Name: "{app}\relay-settings.ini"
Type: files; Name: "{app}\install-windows-share.ps1"
Type: files; Name: "{app}\GeneralsX-Setup.cmd"
Type: files; Name: "{app}\upd.txt"
; The shortcuts every release before the rename made, so a reinstall over one leaves a single set.
; The desktop one goes only when the new one is made in its place.
Type: files; Name: "{autoprograms}\GeneralsX\GeneralsX.lnk"
Type: dirifempty; Name: "{autoprograms}\GeneralsX"
Type: files; Name: "{userdesktop}\GeneralsX.lnk"; Tasks: desktopicon

; GPL-3 + EA's additional terms (LICENSE.md: "Any propagation or conveyance of this program must include this
; copyright notice and these terms"): this setup exe carries the licence too, not only the payload it fetches.
; Relative to this .iss (the repo root). The payload unpacked below lays the same file over it.
[Files]
Source: "..\..\..\..\LICENSE.md"; DestDir: "{app}"
Source: "..\..\..\..\THIRD_PARTY_NOTICES.txt"; DestDir: "{app}"

; THERE IS DELIBERATELY NO [UninstallDelete] FOR {app}, AND PUTTING ONE BACK WOULD BE A REGRESSION.
;
; It used to be `Type: filesandordirs; Name: "{app}"`, on the
; reasoning that {app} is ours entirely. It is not: DisableDirPage=no lets a player install into a
; folder of their own, and an install into C:\Games made Add/Remove Programs delete everything in
; C:\Games. The payload is unpacked by code, so Inno's own log knows none of it; ownership is
; recorded instead, in {app}\gxfiles.txt (see GX_RECORD below), and the uninstaller deletes only
; what that record names. {app} itself goes only if it is then empty - Inno's own rule for a
; folder it created - so a player's file left in it keeps the folder.
;
; The game's user data is NOT touched. Documents\GeneralsX\GeneralsZH holds the player's maps,
; replays, settings and GeneralsX identity, and an older engine's Options.ini is shared with retail
; and with Generals Online. Deleting either would take things the player did not ask to lose.

[Icons]
Name: "{group}\Generals Vanguard"; Filename: "{app}\GeneralsXLauncher.exe"; WorkingDir: "{app}"
Name: "{userdesktop}\Generals Vanguard"; Filename: "{app}\GeneralsXLauncher.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\GeneralsXLauncher.exe"; Description: "Start Generals Vanguard"; Flags: nowait postinstall skipifsilent

[Code]
// Pascal Script exposes no clock of its own, so the one used for the transfer rate is
// imported from kernel32 rather than assumed to exist - 'Unknown identifier GetTickCount'
// otherwise, at compile time.
function GetTickCount(): DWORD;
  external 'GetTickCount@kernel32.dll stdcall';
// Re-read just before the uninstaller walks into a folder (SweepOwned), not trusted from the listing.
function GetFileAttributesW(lpFileName: String): DWORD;
  external 'GetFileAttributesW@kernel32.dll stdcall';

// ------------------------------------------------------------------------------------------------
// What GeneralsX owns in {app}
//
// THE UNINSTALLER DELETES ONLY WHAT THIS RECORD NAMES. {app} is
// whatever folder the player picked, so "everything in it" is not ours to delete (see the note where
// [UninstallDelete] used to be). gxfiles.txt lists, one '/'-separated path per line relative to
// {app}, every file the payload unpacked (tar -t of the same zip), every file this script writes or
// copies in, and - appended by GeneralsXUpdate.exe (gxlaunch.c record_stage) - every file an update
// places later. GX_RUNTIME_* below are the few names the launcher and engine create at run time.
//
// Lines that could leave {app} (a drive, a leading slash, a '..' component) are ignored, never
// obeyed: a damaged or planted record can only make the uninstaller delete less.
// ------------------------------------------------------------------------------------------------
const
  GX_RECORD = 'gxfiles.txt';
  GX_RECORD_HEADER = 'gxfiles 1';
  FILE_ATTRIBUTE_DIRECTORY_ = $10;
  FILE_ATTRIBUTE_REPARSE_POINT_ = $400;

var
  DownloadPage: TDownloadWizardPage;
  GameDirPage: TInputDirWizardPage;
  DetectedGameDir: String;
  LastTickCount: DWORD;
  LastBytes: Int64;
  SpeedText: String;
  // GeneralsX @bugfix wh1ter0se 12/08/2026 What we ACTUALLY downloaded, so ssPostInstall can record
  // it. Global rather than local to NextButtonClick because the record has to be written after the
  // payload is extracted, which happens in a different procedure entirely.
  AppliedPayloadName: String;
  AppliedPayloadSha: String;
  // The uninstaller's view of gxfiles.txt: lower-cased keys, sorted, so IndexOf is a binary search
  // over ~20,000 names rather than a scan per file.
  OwnedFiles: TStringList;
  OwnedDirs: TStringList;
  UninstallKept: Integer;

// ------------------------------------------------------------------------------------------------
// Speaking the launcher's dialect
//
// GeneralsX @bugfix wh1ter0se 12/08/2026 THE INSTALLER AND THE LAUNCHER WRITE THE SAME FACT IN TWO
// DIFFERENT ENCODINGS, so a record written here never matched and every fresh install downloaded
// the same 64 MB twice - once by setup, then again by the launcher seconds later. On the 0.3 MB/s
// connections this project actually ships to, that is eight wasted minutes on every install.
//
//   the launcher records : item win32 GeneralsX-win64-6b7415bab.zip asyajVNwdKoMebLZYFT-SQ
//   setup has            : 6acc9a8d537074aa0c79b2d96054fe49f8da72c7b01965bdaf735566827c3619
//
// They are the SAME BYTES. The manifest carries base64url of the FIRST 16 BYTES of the SHA-256
// (gxlaunch.c:595, and hash_matches at :972 decodes it); Inno only ever sees the full hex, because
// hex is what DownloadTemporaryFile pins. Measured against the live CDN this session: gxsetup.txt's
// hex, truncated to 16 bytes and base64url-encoded, is byte-identical to gxmanifest.txt's hash.
//
// So the conversion is pure string arithmetic and needs no manifest fetch, no Ed25519, and NO
// LAUNCHER CHANGE - which is why this is the fix rather than teaching gxlaunch.c to match on the
// artifact name. Matching on name would have to treat name equality as content equality, and it
// would need a new engine payload published before any fresh install could benefit; this needs a
// new installer and nothing else.
//
// div/mod rather than shr/and on purpose: Pascal binds `and` tighter than `+`, so the idiomatic
// `(V shr 18) and 63 + 1` is a precedence trap, and this file gets no unit tests. The arithmetic
// below was proved against 20,000 random vectors plus both live artifacts before being written.
// ------------------------------------------------------------------------------------------------
function HexNibble(const S: String; I: Integer): Integer;
begin
  Result := Pos(Copy(S, I, 1), '0123456789abcdef') - 1;
  if Result < 0 then
    Result := Pos(Copy(S, I, 1), '0123456789ABCDEF') - 1;
end;

function B64Char(V: Integer): String;
begin
  Result := Copy('ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_', V + 1, 1);
end;

// '' on anything that is not at least 32 hex digits. The caller MUST treat '' as "write no record",
// never as "write an empty hash" - a wrong hash costs one redundant download, but a record that
// merely looks well-formed is how an install gets stranded.
function Sha256HexToManifestHash(const Hex: String): String;
var
  B: array[0..15] of Integer;
  I, Hi, Lo, V: Integer;
begin
  Result := '';
  if Length(Hex) < 32 then
    Exit;
  for I := 0 to 15 do
  begin
    Hi := HexNibble(Hex, I * 2 + 1);
    Lo := HexNibble(Hex, I * 2 + 2);
    if (Hi < 0) or (Lo < 0) then
      Exit;
    B[I] := Hi * 16 + Lo;
  end;
  I := 0;
  while I <= 12 do            // groups at 0,3,6,9,12 - bytes 0..14
  begin
    V := (B[I] * 65536) + (B[I + 1] * 256) + B[I + 2];
    Result := Result + B64Char((V div 262144) mod 64) + B64Char((V div 4096) mod 64) +
                       B64Char((V div 64) mod 64) + B64Char(V mod 64);
    I := I + 3;
  end;
  // Byte 15 alone: two characters, and the '==' padding is dropped because the manifest is unpadded.
  Result := Result + B64Char(B[15] div 4) + B64Char((B[15] mod 4) * 16);
end;

// The artifact name the launcher matches on is the URL's last path segment. ExtractFileName is NOT
// used: it is documented for paths, its treatment of '/' is a Delphi RTL detail rather than a
// promise, and a wrong name here fails silently as a redundant download. Query and fragment are
// stripped so a future ?v= on the CDN cannot become part of the recorded name.
function UrlBaseName(const Url: String): String;
var
  I: Integer;
begin
  Result := Url;
  I := Pos('?', Result);
  if I > 0 then
    Result := Copy(Result, 1, I - 1);
  I := Pos('#', Result);
  if I > 0 then
    Result := Copy(Result, 1, I - 1);
  while Pos('/', Result) > 0 do
    Delete(Result, 1, Pos('/', Result));
  while Pos('\', Result) > 0 do
    Delete(Result, 1, Pos('\', Result));
end;

// ------------------------------------------------------------------------------------------------
// gxfiles.txt - reading, writing, and the one rule for what may go
// ------------------------------------------------------------------------------------------------

// A record line as a lookup key: '/' separators whatever wrote it (tar lists a zip made on Windows
// with either), no leading './', lower case because NTFS compares names that way. A trailing '/'
// (tar's mark for a directory) is kept here and handled by the caller.
function RecordKey(const Line: String): String;
begin
  Result := Trim(Line);
  StringChangeEx(Result, '\', '/', True);
  while Copy(Result, 1, 2) = './' do
    Delete(Result, 1, 2);
  Result := LowerCase(Result);
end;

// True only for a path that stays inside {app}. Everything else is ignored, never obeyed.
function RecordKeyOk(const K: String): Boolean;
begin
  Result := (K <> '') and (Pos(':', K) = 0) and (Copy(K, 1, 1) <> '/') and
            (Pos('//', K) = 0) and (Pos('/../', '/' + K + '/') = 0) and
            (Pos('/./', '/' + K + '/') = 0) and (K <> '..') and (K <> '.');
end;

// One record line into the sets. Every folder above a listed file is ours to remove WHEN EMPTY, and
// only then: RemoveDir refuses a folder with anything left in it.
procedure AddOwned(const Line: String);
var
  K: String;
  I: Integer;
  IsDir: Boolean;
begin
  // Copy rather than K[Length(K)]: no reliance on short-circuit evaluation when K is empty.
  K := RecordKey(Line);
  if Copy(K, 1, 1) = '#' then
    Exit;               // a comment row, e.g. the '# setup' mark below
  IsDir := Copy(K, Length(K), 1) = '/';
  while Copy(K, Length(K), 1) = '/' do
    K := Copy(K, 1, Length(K) - 1);
  if not RecordKeyOk(K) then
    Exit;
  if IsDir then
    OwnedDirs.Add(K)
  else
    OwnedFiles.Add(K);
  for I := Length(K) downto 1 do
    if K[I] = '/' then
      OwnedDirs.Add(Copy(K, 1, I - 1));
end;

// Created by the launcher, the updater or the engine while the game is used, so never in a payload.
// gxwork is the updater's own staging folder and is ours whole; *.gxold is a file an update renamed
// aside because it was in use (gxlaunch.c OLD_SUFFIX).
function IsRuntimeFile(const K: String): Boolean;
begin
  Result := (K = 'gxlaunch.log') or (K = 'gxlaunch.ini') or (K = 'gxwork.lock') or
            (K = 'gxmanifest.txt') or (K = 'gxmanifest.tmp') or
            (K = 'gxinstalled.txt') or (K = 'gxinstalled.tmp') or
            (K = 'generalsx.cmd') or (K = 'gxfiles.txt') or
            (K = 'generalszh.dxvk-cache') or (K = 'generalszh.dxvk-ffkeys') or
            (K = 'generalszh_d3d9.log') or (K = 'generalszh_dxgi.log') or
            (Copy(K, 1, 7) = 'gxwork/');
end;

// <name>.gxold is the updater's renamed copy of <name> (gxlaunch.c place_file), so it is ours only when
// <name> is (a player's own Mods\x.gxold is not). <pack>.big.off is a listed pack the launcher switched off
// (compatibility graphics on a 4 GB PC, the HD options), ours only when <pack>.big is listed.
function IsOwnedFile(const K: String): Boolean;
begin
  Result := (OwnedFiles.IndexOf(K) >= 0) or IsRuntimeFile(K);
  if (not Result) and (Length(K) > 6) and (Copy(K, Length(K) - 5, 6) = '.gxold') then
    Result := OwnedFiles.IndexOf(Copy(K, 1, Length(K) - 6)) >= 0;
  if (not Result) and (Length(K) > 8) and (Copy(K, Length(K) - 7, 8) = '.big.off') then
    Result := OwnedFiles.IndexOf(Copy(K, 1, Length(K) - 4)) >= 0;
end;

function IsOwnedDir(const K: String): Boolean;
begin
  Result := (OwnedDirs.IndexOf(K) >= 0) or (K = 'gxwork') or (Copy(K, 1, 7) = 'gxwork/');
end;

procedure NewOwnedSets();
begin
  OwnedFiles := TStringList.Create;
  OwnedFiles.Sorted := True;
  OwnedFiles.Duplicates := dupIgnore;
  OwnedDirs := TStringList.Create;
  OwnedDirs.Sorted := True;
  OwnedDirs.Duplicates := dupIgnore;
end;

// False when there is no record this installer or GeneralsXUpdate.exe wrote: the header must be the
// first line exactly, or the file is read as no record at all - never as a list to obey.
function LoadOwnedRecord(const App: String): Boolean;
var
  Lines: TArrayOfString;
  I: Integer;
begin
  Result := False;
  if not LoadStringsFromFile(AddBackslash(App) + GX_RECORD, Lines) then
    Exit;
  if (GetArrayLength(Lines) = 0) or (Trim(Lines[0]) <> GX_RECORD_HEADER) then
    Exit;
  for I := 1 to GetArrayLength(Lines) - 1 do
    AddOwned(Lines[I]);
  Result := True;
end;

// Every file and folder under Abs, as Rel/<name> lines (a folder as Rel/<name>/) - what an xcopy /E of Abs
// is about to put in {app}, empty folders included. xcopy without /b copies THROUGH a link,
// so the listing does too; it only reads, and stops at a depth no real Data folder has, so
// a junction loop in the player's folder cannot run away.
procedure ListTree(const Abs, Rel: String; Into: TStringList; Depth: Integer);
var
  F: TFindRec;
begin
  if Depth > 64 then
    Exit;
  Into.Add(Rel + '/');
  if FindFirst(AddBackslash(Abs) + '*', F) then
  try
    repeat
      if (F.Name <> '.') and (F.Name <> '..') then
      begin
        if (F.Attributes and FILE_ATTRIBUTE_DIRECTORY_) <> 0 then
          ListTree(AddBackslash(Abs) + F.Name, Rel + '/' + F.Name, Into, Depth + 1)
        else
          Into.Add(Rel + '/' + F.Name);
      end;
    until not FindNext(F);
  finally
    FindClose(F);
  end;
end;

// Appends to the record, writing its header first when there is none. An install over an existing
// one MERGES: the updater has been appending what it placed since, and all of that is still ours.
procedure AppendRecord(const App: String; Lines: TStringList);
var
  Path: String;
  Existing: TArrayOfString;
  Rows: TArrayOfString;
  I: Integer;
begin
  Path := AddBackslash(App) + GX_RECORD;
  if not (LoadStringsFromFile(Path, Existing) and (GetArrayLength(Existing) > 0) and
          (Trim(Existing[0]) = GX_RECORD_HEADER)) then
    if not SaveStringToFile(Path, GX_RECORD_HEADER + #13#10, False) then
      RaiseException('Generals Vanguard: could not write ' + Path);
  SetArrayLength(Rows, Lines.Count);
  for I := 0 to Lines.Count - 1 do
    Rows[I] := Lines[I];
  if not SaveStringsToFile(Path, Rows, True) then
    RaiseException('Generals Vanguard: could not write ' + Path);
end;

// True when Dir holds nothing at all.
function DirIsEmpty(const Dir: String): Boolean;
var
  F: TFindRec;
begin
  Result := True;
  if FindFirst(AddBackslash(Dir) + '*', F) then
  try
    repeat
      if (F.Name <> '.') and (F.Name <> '..') then
      begin
        Result := False;
        Exit;
      end;
    until not FindNext(F);
  finally
    FindClose(F);
  end;
end;

// An earlier GeneralsX install, which this one may update in place. Anything else that is not empty
// is somebody's folder, and unpacking ~20,000 files into it could overwrite theirs. One file name is
// not enough to say so: a record with its header, or the three files every install has.
function IsOurInstallFolder(const Dir: String): Boolean;
var
  Lines: TArrayOfString;
begin
  Result := LoadStringsFromFile(AddBackslash(Dir) + GX_RECORD, Lines);
  if Result then
    Result := (GetArrayLength(Lines) > 0) and (Trim(Lines[0]) = GX_RECORD_HEADER);
  if not Result then
    Result := FileExists(AddBackslash(Dir) + 'GeneralsXLauncher.exe') and
              FileExists(AddBackslash(Dir) + 'generalszh.exe') and
              FileExists(AddBackslash(Dir) + 'GeneralsX.cmd');
end;

function InstallDirRefusal(const Dir: String): String;
begin
  Result := '';
  if DirExists(Dir) and (not DirIsEmpty(Dir)) and (not IsOurInstallFolder(Dir)) then
    Result := 'That folder already holds other files:' + #13#10 + Dir + #13#10#13#10 +
              'Generals Vanguard needs a folder of its own, so that uninstalling it can never take ' +
              'anything else. Choose an empty folder, or a new one - for example ' +
              AddBackslash(Dir) + 'GeneralsVanguard.';
end;

// Removes from Abs (relative path Rel, '' for {app} itself) every file the record or the runtime
// list names, then every listed folder that is empty after that. Nothing is followed through a
// junction or symbolic link: a listed link is removed as a link, an unlisted one is not touched.
procedure SweepOwned(const Abs, Rel: String);
var
  F: TFindRec;
  Names: TArrayOfString;
  Attrs: array of Cardinal;
  N, I: Integer;
  Child, K: String;
begin
  N := 0;
  if FindFirst(AddBackslash(Abs) + '*', F) then
  try
    repeat
      if (F.Name <> '.') and (F.Name <> '..') then
      begin
        SetArrayLength(Names, N + 1);
        SetArrayLength(Attrs, N + 1);
        Names[N] := F.Name;
        Attrs[N] := F.Attributes;
        N := N + 1;
      end;
    until not FindNext(F);
  finally
    FindClose(F);
  end;

  // Listed first, deleted after: a directory is never changed while it is being read.
  for I := 0 to N - 1 do
  begin
    Child := AddBackslash(Abs) + Names[I];
    if Rel = '' then
      K := LowerCase(Names[I])
    else
      K := Rel + '/' + LowerCase(Names[I]);
    if (Attrs[I] and FILE_ATTRIBUTE_REPARSE_POINT_) <> 0 then
    begin
      // Never removed, never followed. Nothing Setup or the updater places is a link - tar
      // unpacks files, xcopy copies through links - so a junction or symlink here is the player's, even at
      // a recorded path; it stays, and so does the folder around it.
    end
    else if (Attrs[I] and FILE_ATTRIBUTE_DIRECTORY_) <> 0 then
    begin
      // A folder swapped for a junction since it was listed is not walked into.
      if (GetFileAttributesW(Child) and FILE_ATTRIBUTE_REPARSE_POINT_) = 0 then
        SweepOwned(Child, K);
      if IsOwnedDir(K) then
        RemoveDir(Child);         // only succeeds when empty
    end
    else if IsOwnedFile(K) then
      DeleteFile(Child);

    if FileExists(Child) or DirExists(Child) then
      if IsOwnedFile(K) then
      begin
        UninstallKept := UninstallKept + 1;
        Log('gxuninstall: could not remove ' + Child);
      end;
  end;
end;

// ------------------------------------------------------------------------------------------------
// Finding Zero Hour
//
// INIZH.big alone is decisive and is the ONLY hard requirement. A base Generals folder has INI.big
// and no INIZH.big; the expansion ships INIZH.big and no INI.big at all. Testing for both rejects a
// real Zero Hour install, which is a mistake this project has already made once.
// ------------------------------------------------------------------------------------------------
function LooksLikeZeroHour(Dir: String): Boolean;
begin
  Result := (Dir <> '') and FileExists(AddBackslash(Dir) + 'INIZH.big');
end;

// GeneralsX @bugfix wh1ter0se 12/08/2026 ZERO HOUR ALONE IS NOT ENOUGH, AND WE USED TO SAY IT WAS.
//
// Zero Hour draws most of its world art from the BASE Generals archives - Textures.big, W3D.big,
// Terrain.big. This page checked INIZH.big, declared the folder good, and installed happily onto
// setups where the base game was nowhere the engine looks. The result is not a clear failure: the
// menu UI renders perfectly from the ZH archives while the entire 3D scene comes out magenta (the
// engine's MissingTexture fallback), and tree models fail to load, which then crashes the game the
// first time a unit moves near a tree. Two users hit exactly that before anyone connected it to
// missing data - it reads as a broken graphics driver.
//
// Same marker as above, inverted: a BASE Generals folder has INI.big and no INIZH.big. The
// candidates are the places the engine itself searches, so this answer and the engine's agree by
// construction - if this finds nothing, the engine will find nothing.
function FindBaseGenerals(ZhDir: String): String;
var
  Parent: String;
  Names: array[0..3] of String;
  Cand: String;
  i: Integer;
begin
  Result := '';
  if ZhDir = '' then
    Exit;

  // Steam keeps the base game INSIDE the Zero Hour folder.
  Cand := AddBackslash(ZhDir) + 'ZH_Generals';
  if FileExists(AddBackslash(Cand) + 'INI.big') then
  begin
    Result := Cand;
    Exit;
  end;

  // Everything else keeps it as a SIBLING, under one of these names.
  Parent := ExtractFileDir(RemoveBackslashUnlessRoot(ZhDir));
  if Parent = '' then
    Exit;
  Names[0] := 'Command and Conquer Generals';
  Names[1] := 'Command & Conquer Generals';
  Names[2] := 'C&C Generals';
  Names[3] := 'Generals';
  for i := 0 to 3 do
  begin
    Cand := AddBackslash(Parent) + Names[i];
    if FileExists(AddBackslash(Cand) + 'INI.big') and
       (not FileExists(AddBackslash(Cand) + 'INIZH.big')) then
    begin
      Result := Cand;
      Exit;
    end;
  end;
end;

function ReadSteamRoot(): String;
var
  S: String;
begin
  Result := '';
  if RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', S) then
    Result := S
  else if RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\Valve\Steam', 'InstallPath', S) then
    Result := S
  else if RegQueryStringValue(HKLM, 'SOFTWARE\Valve\Steam', 'InstallPath', S) then
    Result := S;
end;

// Steam libraries live on any drive, so the library list is READ rather than guessed. Note the
// entries may name drives that are no longer attached - every probe below is individually guarded,
// because one stale entry must never abort the scan. That exact case killed a whole install once.
function FindInSteamLibraries(): String;
var
  Vdf, Line, Path: String;
  Lines: TArrayOfString;
  I, P, Q: Integer;
  Candidate: String;
begin
  Result := '';
  Path := ReadSteamRoot();
  if Path = '' then
    Exit;

  Vdf := AddBackslash(Path) + 'steamapps\libraryfolders.vdf';
  if not FileExists(Vdf) then
    Exit;
  if not LoadStringsFromFile(Vdf, Lines) then
    Exit;

  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    Line := Lines[I];
    P := Pos('"path"', Line);
    if P > 0 then
    begin
      // "path"  "D:\\SteamLibrary"  ->  take what is inside the LAST pair of quotes
      Q := Length(Line);
      while (Q > 0) and (Line[Q] <> '"') do Q := Q - 1;
      if Q > 1 then
      begin
        P := Q - 1;
        while (P > 0) and (Line[P] <> '"') do P := P - 1;
        if P > 0 then
        begin
          Candidate := Copy(Line, P + 1, Q - P - 1);
          StringChangeEx(Candidate, '\\', '\', True);
          Candidate := AddBackslash(Candidate) +
            'steamapps\common\Command & Conquer Generals - Zero Hour';
          if LooksLikeZeroHour(Candidate) then
          begin
            Result := Candidate;
            Exit;
          end;
        end;
      end;
    end;
  end;
end;

function DetectZeroHour(): String;
var
  Candidates: TArrayOfString;
  I: Integer;
  S: String;
begin
  Result := FindInSteamLibraries();
  if Result <> '' then
    Exit;

  SetArrayLength(Candidates, 5);
  Candidates[0] := ExpandConstant('{commonpf32}\Steam\steamapps\common\Command & Conquer Generals - Zero Hour');
  Candidates[1] := ExpandConstant('{commonpf}\Steam\steamapps\common\Command & Conquer Generals - Zero Hour');
  Candidates[2] := ExpandConstant('{commonpf32}\EA Games\Command & Conquer Generals Zero Hour');
  Candidates[3] := ExpandConstant('{commonpf32}\Origin Games\Command and Conquer Generals Zero Hour');
  Candidates[4] := ExpandConstant('{commonpf}\EA Games\Command & Conquer Generals Zero Hour');

  for I := 0 to GetArrayLength(Candidates) - 1 do
    if LooksLikeZeroHour(Candidates[I]) then
    begin
      Result := Candidates[I];
      Exit;
    end;

  // The EA App and older installers both leave a registry trail.
  if RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\Electronic Arts\EA Games\Generals Zero Hour',
      'InstallPath', S) then
    if LooksLikeZeroHour(S) then
      Result := S;
end;

// ------------------------------------------------------------------------------------------------
// Wizard
// ------------------------------------------------------------------------------------------------
// Progress, megabytes and SPEED. Inno's own page shows a bar and a byte count; the transfer rate is
// computed here, sampled over ~500 ms rather than averaged across the whole download - a
// whole-transfer average keeps reporting the early speed long after a link has degraded, which
// reads as a frozen window with a confident number under it.
function OnDownloadProgress(const Url, FileName: String; const Progress, ProgressMax: Int64): Boolean;
var
  Now: DWORD;
  Elapsed: DWORD;
  Rate: Double;
begin
  Now := GetTickCount();
  if (LastTickCount = 0) then
  begin
    LastTickCount := Now;
    LastBytes := Progress;
  end
  else if (Now - LastTickCount) >= 500 then
  begin
    Elapsed := Now - LastTickCount;
    Rate := (Progress - LastBytes) * 1000.0 / Elapsed;
    if Rate > 0 then
      SpeedText := Format('  -  %.1f MB/s', [Rate / 1048576.0])
    else
      SpeedText := '';
    LastTickCount := Now;
    LastBytes := Progress;
  end;

  if ProgressMax > 0 then
    DownloadPage.SetText(
      Format('%.1f of %.1f MB%s', [Progress / 1048576.0, ProgressMax / 1048576.0, SpeedText]), '')
  else
    DownloadPage.SetText(Format('%.1f MB received%s', [Progress / 1048576.0, SpeedText]), '');

  Result := True;
end;

procedure InitializeWizard();
begin
  DetectedGameDir := DetectZeroHour();

  GameDirPage := CreateInputDirPage(wpSelectDir,
    'Your Zero Hour installation',
    'Generals Vanguard is an engine. It needs your own copy of the game.',
    'Generals Vanguard only ever READS from this folder. Nothing is written to it, renamed in it or ' +
    'deleted from it, so Generals Online and your retail install keep working exactly as they do ' +
    'now.' + #13#10#13#10 +
    'This must be the folder containing INIZH.big.',
    False, '');
  GameDirPage.Add('');
  GameDirPage.Values[0] := DetectedGameDir;

  DownloadPage := CreateDownloadPage(
    'Downloading Generals Vanguard',
    'The engine and its data are being fetched from dl.wh1ter0se.dev.',
    @OnDownloadProgress);
end;

// ------------------------------------------------------------------------------------------------
// Evergreen payload resolution.
//
// GeneralsX: The compiled-in PayloadUrl/PayloadSha256 pin goes stale
// the moment a newer engine is published: an old installer still WORKS (it installs the older
// build, which self-updates on first launch), but it downloads 67 MB it immediately replaces and
// shows a version that is no longer true (owner report). So at install time the setup asks the
// CDN what the CURRENT build is - gxsetup.txt, one line, published by publish-bootstrap.sh in
// the same breath as the manifest - and falls back to the compiled pin if the fetch or the parse
// fails, which is exactly the old behaviour.
//
// Trust, stated honestly: the resolver rides the same TLS origin the person downloaded this
// installer from, so it adds no new authority - and the payload is still verified end to end,
// because the resolver carries the FULL sha256 and Inno checks it natively. The Ed25519-signed
// manifest remains the authority for every update after this one; this file only exists because
// teaching Inno's Pascal to verify Ed25519 is how installers grow bugs.
function ResolveCurrentPayload(var Url, Sha: String): Boolean;
var
  Raw: AnsiString;
  Line, P, U, H: String;
  i: Integer;
begin
  Result := False;
  try
    DownloadTemporaryFile('https://dl.wh1ter0se.dev/gxsetup.txt', 'gxsetup.txt', '', nil);
  except
    Exit; // offline, blocked, or not yet published - the pinned build installs as before
  end;
  if not LoadStringFromFile(ExpandConstant('{tmp}\gxsetup.txt'), Raw) then
    Exit;

  // First non-comment line: "win32 <url> <sha256hex> <size>"
  Line := String(Raw);
  while (Line <> '') do
  begin
    i := Pos(#10, Line);
    if i > 0 then
    begin
      P := Trim(Copy(Line, 1, i - 1));
      Delete(Line, 1, i);
    end
    else
    begin
      P := Trim(Line);
      Line := '';
    end;
    if (P <> '') and (P[1] <> '#') then
    begin
      // token 1: platform. The file carries one row per platform in publish order - keep
      // scanning until the win32 row rather than judging the first row we meet (cross-model
      // review catch: a macOS-first publication would have silently kept the stale pin).
      i := Pos(' ', P);
      if (i > 0) and (Copy(P, 1, i - 1) = 'win32') then
      begin
        Delete(P, 1, i);
        P := TrimLeft(P);
        // token 2: url
        i := Pos(' ', P);
        if i = 0 then Exit;
        U := Copy(P, 1, i - 1);
        Delete(P, 1, i);
        P := TrimLeft(P);
        // token 3: sha256 hex
        i := Pos(' ', P);
        if i = 0 then i := Length(P) + 1;
        H := Copy(P, 1, i - 1);

        // The url must live on the origin this installer came from, and the hash must be a
        // full sha256 - anything else keeps the compiled pin.
        if Pos('https://dl.wh1ter0se.dev/', U) <> 1 then Exit;
        if Length(H) <> 64 then Exit;
        for i := 1 to 64 do
          if Pos(LowerCase(H[i]), '0123456789abcdef') = 0 then Exit;

        Url := U;
        Sha := LowerCase(H);
        Result := True;
        Exit;
      end;
    end;
  end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  PayloadUrl, PayloadSha, Refusal: String;
begin
  Result := True;

  if CurPageID = wpSelectDir then
  begin
    Refusal := InstallDirRefusal(WizardDirValue);
    if Refusal <> '' then
    begin
      MsgBox(Refusal, mbError, MB_OK);
      Result := False;
    end;
    Exit;
  end;

  if CurPageID = GameDirPage.ID then
  begin
    if not LooksLikeZeroHour(GameDirPage.Values[0]) then
    begin
      MsgBox('That folder does not contain INIZH.big, so it is not Zero Hour.' + #13#10#13#10 +
             'Pick the folder the EXPANSION is installed in - not the base Generals folder.',
             mbError, MB_OK);
      Result := False;
      Exit;
    end;
    // Zero Hour is present but the BASE game supplies most of the world art. Warn rather than
    // refuse: the engine also accepts GENERALSX_GENERALS_ASSET_PATH and a registry install path,
    // so somebody with an unusual layout can still be correct. But say exactly what breaks,
    // because the symptom - a perfect menu over a magenta world - looks like a driver fault and
    // sent two people hunting their GPU instead of their install.
    if FindBaseGenerals(GameDirPage.Values[0]) = '' then
    begin
      Log('gxsetup: no base Generals assets found next to ' + GameDirPage.Values[0]);
      if MsgBox('Zero Hour is here, but the ORIGINAL Command & Conquer: Generals data is not.'
                + #13#10#13#10 +
                'Zero Hour loads most of its terrain, trees and vehicles from the base game. '
                + 'Without it the game will start, the menus will look correct, and everything '
                + 'else will be bright pink - and it will crash shortly after.' + #13#10#13#10 +
                'Install the base game next to Zero Hour, then run this again.' + #13#10#13#10 +
                'Continue anyway?', mbError, MB_YESNO) <> IDYES then
      begin
        Result := False;
        Exit;
      end;
    end;
    Exit;
  end;

  if CurPageID = wpReady then
  begin
    LastTickCount := 0;
    LastBytes := 0;
    SpeedText := '';
    DownloadPage.Clear;
    // The hash is pinned at build time. An empty one means somebody compiled this by hand without
    // /DPayloadSha256 - refuse rather than download something unverifiable.
    if '{#PayloadSha256}' = '' then
    begin
      // NOTE: #13#10 must never begin a line. Inno's preprocessor reads any line whose first
      // non-space character is '#' as a directive, so a wrapped Pascal string continuing with
      // #13#10 fails to compile with "Unknown preprocessor directive" - pointing at the line
      // rather than at the wrap that caused it.
      MsgBox('This installer was built without a payload checksum and cannot verify its ' +
             'download.' + #13#10 + 'Please report this - do not work around it.',
             mbCriticalError, MB_OK);
      Result := False;
      Exit;
    end;
    // Evergreen: prefer what the CDN says is current; the compiled pin is the fallback. See
    // ResolveCurrentPayload above for the trust argument.
    PayloadUrl := '{#PayloadUrl}';
    PayloadSha := '{#PayloadSha256}';
    if ResolveCurrentPayload(PayloadUrl, PayloadSha) then
      Log('gxsetup: resolved current payload ' + PayloadUrl)
    else
      Log('gxsetup: using the compiled-in payload pin');
    DownloadPage.Add(PayloadUrl, 'payload.zip', PayloadSha);
    DownloadPage.Show;
    try
      try
        DownloadPage.Download;
        Result := True;
        // Remember EXACTLY what landed, so ssPostInstall can record it truthfully. Captured on the
        // success path only - a failed download must not leave a record claiming otherwise.
        AppliedPayloadName := UrlBaseName(PayloadUrl);
        AppliedPayloadSha := PayloadSha;
      except
        // The RESOLVED build failing - missing object, stale edge, hash mismatch - must not
        // brick an installer that still carries a known-good pin. Retry the compiled build
        // once; it installs an older engine that self-updates on first launch (cross-model
        // review catch: without this, every downloaded installer dies with the incident).
        if PayloadUrl <> '{#PayloadUrl}' then
        begin
          Log('gxsetup: resolved payload failed (' + GetExceptionMessage + '); retrying the compiled-in pin');
          DownloadPage.Clear;
          DownloadPage.Add('{#PayloadUrl}', 'payload.zip', '{#PayloadSha256}');
          try
            DownloadPage.Download;
            Result := True;
            // The fallback installed the PINNED build, not the resolved one - record that, or the
            // launcher is told it has a payload it does not have and skips the update that fixes it.
            AppliedPayloadName := UrlBaseName('{#PayloadUrl}');
            AppliedPayloadSha := '{#PayloadSha256}';
          except
            SuppressibleMsgBox(AddPeriod(GetExceptionMessage), mbCriticalError, MB_OK, IDOK);
            Result := False;
          end;
        end
        else
        begin
          // A hash mismatch lands here too, and it is NOT a network error - say so, because the
          // two send somebody to completely different places.
          SuppressibleMsgBox(AddPeriod(GetExceptionMessage), mbCriticalError, MB_OK, IDOK);
          Result := False;
        end;
      end;
    finally
      DownloadPage.Hide;
    end;
  end;
end;

// The same refusal as the folder page, for an install that never showed it (/DIR= on a silent one).
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := InstallDirRefusal(RemoveBackslashUnlessRoot(ExpandConstant('{app}')));
end;

// ------------------------------------------------------------------------------------------------
// Applying
// ------------------------------------------------------------------------------------------------
procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
  Cmd: String;
  GameDir: String;
  Listed: TArrayOfString;
  Rec: TStringList;
  I: Integer;
begin
  if CurStep <> ssPostInstall then
    Exit;

  GameDir := RemoveBackslashUnlessRoot(GameDirPage.Values[0]);

  // GeneralsX: CLEAR THE RECORD BEFORE EXTRACTING, so that "the file is
  // here afterwards" means "this payload supplied it" and cannot mean "the previous install left
  // it". The two are indistinguishable after the fact and they need opposite treatment: a record
  // this bundle brought is authoritative, a survivor from an older install describes files that
  // were just overwritten and is the stranded-install bug at :664-669.
  //
  // Deleting unconditionally is safe because every branch below writes or deletes it anyway; this
  // only moves the moment. If the payload carries no record - any bundle built before 19/08/2026 -
  // the fallback further down still writes the single-row version, so an old bundle keeps exactly
  // the behaviour it has today.
  DeleteFile(ExpandConstant('{app}\gxinstalled.txt'));

  // Unpack with the OS. tar.exe has shipped in System32 since Windows 10 1803 and handles .zip, so
  // this is not a dependency anybody installs.
  Exec(ExpandConstant('{sys}\tar.exe'),
       ExpandConstant('-x -f "{tmp}\payload.zip" -C "{app}"'),
       '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

  // And what it placed, from the same zip, for gxfiles.txt. The
  // listing goes to {tmp}, whose path cmd.exe cannot misread: {app} is never on this command line.
  // No list means an uninstaller that cannot tell its own files from the player's, so the install
  // stops here rather than carrying on without one.
  DeleteFile(ExpandConstant('{tmp}\gxlist.txt'));
  if (not Exec(ExpandConstant('{cmd}'),
               '/c ""' + ExpandConstant('{sys}\tar.exe') + '" -t -f "' + ExpandConstant('{tmp}\payload.zip') +
                 '" > "' + ExpandConstant('{tmp}\gxlist.txt') + '""',
               '', SW_HIDE, ewWaitUntilTerminated, ResultCode)) or (ResultCode <> 0) or
     (not LoadStringsFromFile(ExpandConstant('{tmp}\gxlist.txt'), Listed)) or
     (GetArrayLength(Listed) = 0) then
    RaiseException('Generals Vanguard: could not list the files in the download, so uninstalling could not tell them from yours');

  // GeneralsX @bugfix wh1ter0se 12/08/2026 RECORD WHAT WE JUST PLACED.
  //
  // gxinstalled.txt belongs to the LAUNCHER and names the artifact it last applied. This procedure
  // has just written engine files over {app}, so whatever the file said before is now wrong, and
  // both obvious answers are wrong in opposite directions:
  //
  //   LEAVING IT stranded the install. Measured on the owner's machine: the launcher applied
  //   b80e6b0df at 23:47, setup laid an older payload over it at 00:45, and every launch afterwards
  //   compared a stale record to the manifest, said "already current", and never looked at the
  //   files. That install was stranded permanently and every future publish sailed past it.
  //
  //   DELETING IT was the first fix and it cost a download instead. With no record the launcher
  //   treats the install as ordinal 0, wants every artifact, and re-fetches the same 64 MB setup
  //   just placed. The owner's friend on a 0.3 MB/s line pays eight minutes for it, on every
  //   install. Confirmed in the wild: his gxlaunch.log opens with "state: no gxinstalled.txt" and
  //   an immediate 67,053,139-byte fetch of the payload he had already downloaded.
  //
  // So write the truth. Setup knows the artifact it downloaded by name and by hash, and that is the
  // entire record - state_hash_of() (gxlaunch.c:1701-1710) matches on PLATFORM and NAME and then
  // compares the hash, and the ordinal is not consulted at all. `ordinal 0` is therefore honest and
  // harmless: the only ordinal comparison in the launcher is manifest-vs-CACHED-MANIFEST at :1087,
  // the state ordinal is used solely to fill in the "GXUPD <n>" datagram, and `minimum` is parsed
  // and logged but never enforced. All four re-read this session rather than taken on trust.
  //
  // Written only when the download actually succeeded, and only when the hash converts - a record
  // with a hash we could not produce would be a lie in the other direction. Falling back to the
  // delete keeps the failure mode exactly as it ships today: one redundant download, never a
  // stranded install.
  // GeneralsX: A RECORD THE PAYLOAD BROUGHT WINS, AND IT IS THE ONLY ONE
  // THAT CAN BE RIGHT SINCE THE SPLIT.
  //
  // The row this procedure writes names the BUNDLE. Since the payload was split into four tiers the
  // bundle is a container, and a container's name is in no manifest - so state_hash_of, which
  // matches on platform AND name (gxlaunch.c:1701-1710), misses all four and the launcher refetches
  // 118 MB that setup had just downloaded. Measured on a clean install 19/08/2026:
  //   "state: installed ordinal 0, 1 known artifact(s)" then "update: 4 artifact(s) to fetch".
  //
  // Setup cannot fix this itself: it never sees the manifest, and fetching one would risk recording
  // artifacts it does not have if a publish landed in between - the stranded install at :664-669,
  // which is worse than the wasted download. So package-windows-share.ps1 now writes the real
  // four-tier record INTO the bundle, where it cannot disagree with the bytes shipped beside it,
  // and it lands at {app}\gxinstalled.txt on extraction. The delete before tar above is what makes
  // its presence here trustworthy.
  if FileExists(ExpandConstant('{app}\gxinstalled.txt')) then
  begin
    Log('gxsetup: the payload carried its own gxinstalled.txt - leaving it, it names the tiers');
  end
  else if (AppliedPayloadName <> '') and (Sha256HexToManifestHash(AppliedPayloadSha) <> '') then
  begin
    SaveStringToFile(ExpandConstant('{app}\gxinstalled.txt'),
      '# GeneralsX installed state. Written by the installer; the launcher maintains it after.'#13#10 +
      'ordinal 0'#13#10 +
      'item win32 ' + AppliedPayloadName + ' ' +
        Sha256HexToManifestHash(AppliedPayloadSha) + #13#10, False);
    Log('gxsetup: recorded ' + AppliedPayloadName + ' ' +
        Sha256HexToManifestHash(AppliedPayloadSha) + ' in gxinstalled.txt');
  end
  else
  begin
    // No payload applied, or a hash this code cannot express in the launcher's encoding. Either way
    // any surviving record cannot describe this install, so remove it and let the launcher re-check.
    DeleteFile(ExpandConstant('{app}\gxinstalled.txt'));
    Log('gxsetup: no recordable payload - removed any stale gxinstalled.txt');
  end;

  // Data\Scripts IS NOT OPTIONAL. SidesList::prepareForMP_or_Skirmish loads
  // Data\Scripts\SkirmishScripts.scb by a path relative to the WORKING DIRECTORY when a map brings
  // no skirmish scripts of its own. Without it every match containing an AI player desyncs at frame
  // zero against a non-Windows peer, silently - the diagnostics are compiled out in release.
  // Measured: 13,919 of 13,919 frames differed before staging, 0 after. Copied FROM the user's
  // install; nothing is ever written back to it.
  if DirExists(AddBackslash(GameDir) + 'Data\Scripts') then
  begin
    ForceDirectories(ExpandConstant('{app}\Data\Scripts'));
    // GeneralsX: the payload already carries GeneralsX's own
    // SkirmishScripts.scb (scripts/tooling/make-skirmish-scripts.py). xcopy /Y would overwrite it with the retail
    // copy and every Windows peer would then be refused by DATA_DIFFERS. Keep a copy in {tmp} (an API call, so no
    // command-line quoting), run the copy, put the kept file back, and FAIL the install if either step fails.
    if FileExists(ExpandConstant('{app}\Data\Scripts\SkirmishScripts.scb')) then
    begin
      DeleteFile(ExpandConstant('{tmp}\gx-skirmish-scb.keep'));   // a leftover is from an earlier, interrupted install
      if not FileCopy(ExpandConstant('{app}\Data\Scripts\SkirmishScripts.scb'), ExpandConstant('{tmp}\gx-skirmish-scb.keep'), False) then
        RaiseException('Generals Vanguard: could not keep SkirmishScripts.scb before copying the retail scripts');
      Exec(ExpandConstant('{cmd}'),
           '/c xcopy "' + AddBackslash(GameDir) + 'Data\Scripts" "' +
             ExpandConstant('{app}\Data\Scripts') + '" /E /I /Y /Q',
           '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
      if not FileCopy(ExpandConstant('{tmp}\gx-skirmish-scb.keep'), ExpandConstant('{app}\Data\Scripts\SkirmishScripts.scb'), False) then
        RaiseException('Generals Vanguard: could not restore the generated SkirmishScripts.scb after copying the retail scripts');
      DeleteFile(ExpandConstant('{tmp}\gx-skirmish-scb.keep'));
    end
    else
      Exec(ExpandConstant('{cmd}'),
           '/c xcopy "' + AddBackslash(GameDir) + 'Data\Scripts" "' +
             ExpandConstant('{app}\Data\Scripts') + '" /E /I /Y /Q',
           '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
  if DirExists(AddBackslash(GameDir) + 'Data\Cursors') then
  begin
    ForceDirectories(ExpandConstant('{app}\Data\Cursors'));
    Exec(ExpandConstant('{cmd}'),
         '/c xcopy "' + AddBackslash(GameDir) + 'Data\Cursors" "' +
           ExpandConstant('{app}\Data\Cursors') + '" /E /I /Y /Q',
         '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;

  // GeneralsX.cmd, and every line of it is load-bearing.
  //
  // The TRAILING BACKSLASH on CNC_GENERALS_ZH_PATH is not cosmetic:
  // Win32LocalFileSystem::getFileListInDirectory builds its search string by concatenation with NO
  // separator inserted, so without it the mask becomes "...Zero Hour*.big", FindFirstFile matches
  // nothing, and the engine reports "did not provide BIG files" and then dies loading INI.
  //
  // The `cd` matters for a different reason: the engine resolves loose data relative to the working
  // directory AND hashes that data into a dataID every peer must agree on. Launching from anywhere
  // else refuses every join with DATA_DIFFERS, which reads like a network fault and is not one.
  //
  // The %* on the last line is what keeps the display mode out of this file. It first ended in a
  // hardcoded -win, so the game opened in a window forever no matter what any setting said; moving
  // that to an install-time checkbox only made the wrong answer permanent instead, because an
  // installer question can be changed only by reinstalling. So this script decides nothing now.
  // launch_game() in gxlaunch.c already appends the caller's switches to the command line it runs
  // this script with, and %* is what carries them the last hop to the engine. Drop it and the
  // launcher's options page goes back to being decorative, silently.
  //
  // The empty "" after `start` is the window title, and it stays. Without it the first quoted token
  // on the line is eaten as a title and `start` opens an empty console instead of the game.
  Cmd := '@echo off' + #13#10 +
         'set "CNC_GENERALS_ZH_PATH=' + AddBackslash(GameDir) + '"' + #13#10 +
         'cd /d "%~dp0"' + #13#10 +
         'start "" generalszh.exe %*' + #13#10;
  SaveStringToFile(ExpandConstant('{app}\GeneralsX.cmd'), Cmd, False);

  // Everything above, as gxfiles.txt rows: the payload's list, this procedure's own files, and the
  // retail scripts and cursors exactly as xcopy just copied them.
  Rec := TStringList.Create;
  try
    // The mark the launcher reads (gxio uninstall layout, SETUP_MARK) to know that the uninstaller
    // in this folder is one that takes only recorded files. An install whose record has no such row
    // was made by an older Setup, whose uninstaller still deletes {app} whole.
    Rec.Add('# setup {#AppVersion}');
    for I := 0 to GetArrayLength(Listed) - 1 do
      if Trim(Listed[I]) <> '' then
        Rec.Add(Trim(Listed[I]));
    Rec.Add('GeneralsX.cmd');
    Rec.Add('gxinstalled.txt');
    Rec.Add(GX_RECORD);
    Rec.Add('LICENSE.md');
    Rec.Add('THIRD_PARTY_NOTICES.txt');
    if DirExists(AddBackslash(GameDir) + 'Data\Scripts') then
      ListTree(AddBackslash(GameDir) + 'Data\Scripts', 'Data/Scripts', Rec, 0);
    if DirExists(AddBackslash(GameDir) + 'Data\Cursors') then
      ListTree(AddBackslash(GameDir) + 'Data\Cursors', 'Data/Cursors', Rec, 0);
    AppendRecord(RemoveBackslashUnlessRoot(ExpandConstant('{app}')), Rec);
    Log('gxsetup: recorded ' + IntToStr(Rec.Count) + ' paths in ' + GX_RECORD);
  finally
    Rec.Free;
  end;
end;

// ------------------------------------------------------------------------------------------------
// Uninstalling: only what gxfiles.txt and the runtime list name, then only folders left empty.
// Inno itself then removes its own files, the shortcuts, the Add/Remove entry, and {app} if - and
// only if - nothing is left in it.
// ------------------------------------------------------------------------------------------------
function LeftInApp(const App: String): Integer;
var
  F: TFindRec;
begin
  Result := 0;
  if FindFirst(AddBackslash(App) + '*', F) then
  try
    repeat
      // unins000.exe/.dat are Inno's, and still on disk while it finishes.
      if (F.Name <> '.') and (F.Name <> '..') and (LowerCase(Copy(F.Name, 1, 5)) <> 'unins') then
        Result := Result + 1;
    until not FindNext(F);
  finally
    FindClose(F);
  end;
end;

// The shortcuts every release before the rename made. UninstallLogMode=overwrite drops the older log that named them,
// and [InstallDelete] keeps the old desktop one when the player unticks the new one, so this uninstaller removes them
// itself - each only while it still points at this install's launcher: a shortcut of that name aimed anywhere else stays.
procedure RemoveOldShortcut(const Lnk, App: String);
var
  Shell, Sc: Variant;
  Target: String;
begin
  if not FileExists(Lnk) then
    Exit;
  Target := '';
  try
    Shell := CreateOleObject('WScript.Shell');
    Sc := Shell.CreateShortcut(Lnk);
    Target := Sc.TargetPath;
  except
    Log('gxuninstall: could not read ' + Lnk);
  end;
  if CompareText(Target, AddBackslash(App) + 'GeneralsXLauncher.exe') = 0 then
  begin
    if DeleteFile(Lnk) then
      Log('gxuninstall: removed ' + Lnk);
  end
  else
    Log('gxuninstall: kept ' + Lnk + ' (it points at ' + Target + ')');
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  App: String;
begin
  App := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));
  if CurUninstallStep = usUninstall then
  begin
    NewOwnedSets();
    UninstallKept := 0;
    if LoadOwnedRecord(App) then
      Log('gxuninstall: ' + GX_RECORD + ' names ' + IntToStr(OwnedFiles.Count) + ' files')
    else
      Log('gxuninstall: no ' + GX_RECORD + ' in ' + App + ' - only the runtime names are removed');
    SweepOwned(App, '');
    Log('gxuninstall: ' + IntToStr(UninstallKept) + ' of our files could not be removed');
  end
  else if CurUninstallStep = usPostUninstall then
  begin
    RemoveOldShortcut(ExpandConstant('{autoprograms}\GeneralsX\GeneralsX.lnk'), App);
    RemoveDir(ExpandConstant('{autoprograms}\GeneralsX'));   // only succeeds when empty
    RemoveOldShortcut(ExpandConstant('{userdesktop}\GeneralsX.lnk'), App);
    if DirExists(App) and (LeftInApp(App) > 0) then
    begin
      Log('gxuninstall: kept ' + App + ' - ' + IntToStr(LeftInApp(App)) + ' entries are not the game''s or could not be removed');
      if not UninstallSilent then
        MsgBox('Generals Vanguard has been removed.' + #13#10#13#10 +
               'This folder was kept because it still holds files Generals Vanguard did not install ' +
               '(or could not remove while they were in use):' + #13#10 + App,
               mbInformation, MB_OK);
    end;
  end;
end;
