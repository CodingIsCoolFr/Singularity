; Installer for Singularity.
;
; Built from whatever is in dist\, so the thing that ships is the thing that
; was tested rather than a second list of files that has to be kept in step.
;
; Per user by default. Installing into Program Files needs an administrator
; prompt, and nothing here requires one: no service, no driver, no shared
; component. Asking for elevation you do not need is how a small program
; starts looking untrustworthy.
;
; Build it with:
;     iscc installer\Singularity.iss

#define AppName       "Singularity"
#define AppVersion    "0.1.1"
#define AppPublisher  "Singularity"
#define AppExe        "Singularity.exe"

[Setup]
AppId={{B7F1A6C2-4E3D-4A91-9C57-3E0F2A18D4B6}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
VersionInfoVersion={#AppVersion}

; Per user, so no administrator prompt.
PrivilegesRequired=lowest
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
DisableDirPage=no

OutputDir=..\dist-installer
OutputBaseFilename=Singularity-{#AppVersion}-setup
SetupIconFile=..\brand\singularity.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}

; LZMA2 at maximum. The bulk of this is one very large video decoder, which
; compresses well, and a download happens once where a slow decompress happens
; once as well.
Compression=lzma2/max
SolidCompression=yes

WizardStyle=modern
WizardSizePercent=110
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a shortcut on the desktop"; GroupDescription: "Shortcuts:"

[Files]
; Everything dist holds, recursively. The exe, the Qt runtime, the FFmpeg
; libraries and the one plugins folder.
Source: "..\dist\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "Start {#AppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Qt writes a cache beside the program on first run. Left behind it would
; keep the install folder alive after an uninstall, which looks like a
; failure even though nothing is wrong.
Type: filesandordirs; Name: "{app}\.qt"

[Code]
// The settings and the sealed token live in the user's roaming profile, not
// in the install folder, so an uninstall leaves them alone unless asked. That
// is the behaviour people expect: removing a program should not silently take
// the account with it.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    DataDir := ExpandConstant('{userappdata}\Singularity');
    if DirExists(DataDir) then
    begin
      if MsgBox('Remove your saved sign-in and settings as well?'#13#10#13#10
                + 'Choose No to keep them for next time.',
                mbConfirmation, MB_YESNO) = IDYES then
        DelTree(DataDir, True, True, True);
    end;
  end;
end;
