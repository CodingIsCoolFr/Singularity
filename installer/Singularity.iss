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
; The wizard is gone. Every page it used to show asked a question that had
; already been answered - where to install (where it is already installed),
; whether to continue (you ran the installer), whether to start it afterwards
; (yes). What is left is one dark window that does the work and closes, which
; is what the program itself looks like.
;
; Build it with:
;     iscc installer\Singularity.iss

#define AppName       "Singularity"
#define AppVersion    "0.8.18"
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
; A self-update passes /DIR to a new folder beside the running copy. With
; this left on, Inno would ignore that and overwrite the copy that is open.
UsePreviousAppDir=no
DefaultGroupName={#AppName}

; Nothing to click through. Each of these pages asked something that was
; already decided by the act of running this.
DisableWelcomePage=yes
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
DisableFinishedPage=yes

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
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; Do not ask Windows to close programs.
;
; Restart Manager treats Explorer as a locker of this exe, because Explorer
; keeps the shortcut target open to draw its icon, and then shuts Explorer
; down. That is the taskbar ignoring clicks for the whole install.
;
; CloseApplications=no is not enough on its own. The copy that downloads this
; still passes /CLOSEAPPLICATIONS, and that switch turns Restart Manager back
; on. The filter is a name that is not in this install, so even then there is
; nothing for it to close. InitializeSetup waits until Singularity.exe itself
; has exited, which is the only process that needs to let go of the files.
CloseApplications=no
RestartApplications=no
CloseApplicationsFilter=do-not-close-explorer.none

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; Everything dist holds, recursively. The exe, the Qt runtime, the FFmpeg
; libraries and the one plugins folder.
Source: "..\dist\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

; The animation. Never installed - it is unpacked to a temporary folder, shown
; while the work happens, and goes away with the installer.
Source: "spinner\*.bmp"; Flags: dontcopy

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"

[Run]
; With no finished page there is no tick box to offer, so this simply runs.
; The silent path needs its own entry because skipifsilent - correct for
; somebody who ran this by hand - would otherwise leave a program that had
; just updated itself sitting closed, which is indistinguishable from a crash.
Filename: "{app}\{#AppExe}"; Flags: nowait postinstall skipifsilent
Filename: "{app}\{#AppExe}"; Flags: nowait; Check: InstalledSilently

[UninstallDelete]
; Qt writes a cache beside the program on first run. Left behind it would
; keep the install folder alive after an uninstall, which looks like a
; failure even though nothing is wrong.
Type: filesandordirs; Name: "{app}\.qt"

[Code]
// Posts a click at a button rather than calling its handler.
//
// Calling the handler from inside CurPageChanged does nothing: Setup is part
// way through changing page and will not change again until it has finished.
// A posted message waits in the queue until it has, and then lands.
function PostMessage(Wnd: HWND; Msg: Cardinal; W: Longint; L: Longint): Boolean;
  external 'PostMessageW@user32.dll stdcall';

// A real clock for the animation.
//
// The first version stepped the picture on every install-progress event, and
// those do not arrive evenly: a run of small files fires dozens in a moment
// and one large file fires none for a second. The result was not an animation
// but a flicker with pauses in it - unpleasant to look at, and for anyone
// sensitive to flashing, worse than unpleasant.
//
// Windows' own timer ticks at a steady rate whatever the install is doing.
function SetTimer(Wnd: HWND; Id: Longint; Interval: Cardinal; Proc: Longint): Longint;
  external 'SetTimer@user32.dll stdcall';
function KillTimer(Wnd: HWND; Id: Longint): Boolean;
  external 'KillTimer@user32.dll stdcall';

// The running program holds Local\SingularityRunning until it exits.
function OpenMutexW(dwDesiredAccess: Cardinal; bInheritHandle: Integer; lpName: String): THandle;
  external 'OpenMutexW@kernel32.dll stdcall';
function CloseHandle(hObject: THandle): Integer;
  external 'CloseHandle@kernel32.dll stdcall';
procedure WinSleep(dwMilliseconds: Cardinal);
  external 'Sleep@kernel32.dll stdcall';
function ShowWindow(hWnd: HWND; nCmdShow: Integer): Integer;
  external 'ShowWindow@user32.dll stdcall';
function GetWindowLongW(hWnd: HWND; nIndex: Integer): Longint;
  external 'GetWindowLongW@user32.dll stdcall';
function SetWindowLongW(hWnd: HWND; nIndex: Integer; dwNewLong: Longint): Longint;
  external 'SetWindowLongW@user32.dll stdcall';
function GetForegroundWindow: HWND;
  external 'GetForegroundWindow@user32.dll stdcall';
function SetForegroundWindow(hWnd: HWND): Boolean;
  external 'SetForegroundWindow@user32.dll stdcall';
function GetShellWindow: HWND;
  external 'GetShellWindow@user32.dll stdcall';

const
  FrameCount = 24;
  BM_CLICK = $00F5;

  // The program's own colours. Inno wants them the other way round from the
  // way they are written everywhere else - blue, green, red - so these do not
  // read like the hex in Theme.h even though they are the same colours.
  clGround   = $0E0907;   // #07090e
  clPanel    = $1F1510;   // #10151f
  clText     = $FBF4EE;   // #eef4fb
  clMuted    = $BCA69A;   // #9aa6bc
  clFaint    = $8C766B;   // #6b768c
  clAccent   = $E6D6CD;   // #cdd6e6
  clTrack    = $33241C;   // a dim version of the accent, for the empty bar

var
  Frames: array[0..FrameCount - 1] of TBitmap;
  Spinner: TBitmapImage;
  TitleLabel: TNewStaticText;
  StepLabel: TNewStaticText;
  Track: TPanel;
  Fill: TPanel;
  CancelText: TNewStaticText;
  FrameIndex: Integer;
  LastPercent: Integer;
  TimerId: Longint;

procedure CancelClicked(Sender: TObject);
begin
  // The same thing the real button does, including the "are you sure".
  WizardForm.Close;
end;

// True when nobody is driving this - which means the program updated itself
// and is waiting to be started again.
function InstalledSilently: Boolean;
begin
  Result := WizardSilent;
end;

procedure LoadFrames;
var
  I: Integer;
  Name: String;
begin
  for I := 0 to FrameCount - 1 do
  begin
    Name := Format('spin%.2d.bmp', [I]);
    ExtractTemporaryFile(Name);
    Frames[I] := TBitmap.Create;
    Frames[I].LoadFromFile(ExpandConstant('{tmp}\' + Name));
  end;
end;

// One step of the spin. Called by the timer, so the pace is even.
procedure Advance(Wnd: HWND; Msg: Cardinal; Id: Longint; Tick: Cardinal);
begin
  FrameIndex := (FrameIndex + 1) mod FrameCount;
  if Spinner <> nil then
    Spinner.Bitmap := Frames[FrameIndex];
  // The unpack runs on this same thread, so while a large file is extracted
  // the window stops answering. If it is the foreground window, Windows
  // saves every taskbar click until the unpack finishes. Never be that window.
  if WizardSilent and (WizardForm <> nil) and (GetForegroundWindow = WizardForm.Handle) then
    SetForegroundWindow(GetShellWindow);
end;

// Puts the wizard's own buttons back where they belong, every time.
//
// Doing this once at startup is not enough: Inno decides which of Back, Next
// and Cancel to show each time the page changes, and it undoes anything set
// beforehand. That is why the first version showed an Install button sitting
// on top of Cancel - it had been hidden, and then handed back.
procedure Relayout;
var
  W, H: Integer;
begin
  if WizardForm = nil then
    Exit;

  W := WizardForm.ClientWidth;
  H := WizardForm.ClientHeight;

  // Parked off the edge rather than hidden.
  //
  // Hiding them is what broke the first version. Setup still walks through
  // its pages even when every one of them is disabled, and it advances by
  // clicking its own Next button - so a Next button with Visible set to False
  // is a wizard with no way forward. It sat on "Getting ready" for ever, and
  // the log stopped after unpacking the animation with no complaint, because
  // nothing had gone wrong: it was waiting, correctly, for a press that could
  // never come.
  //
  // Moved out of sight, everything still works and none of it is on screen.
  WizardForm.NextButton.SetBounds(-2000, -2000, 10, 10);
  WizardForm.BackButton.SetBounds(-2000, -2000, 10, 10);
  WizardForm.CancelButton.SetBounds(-2000, -2000, 10, 10);
  WizardForm.OuterNotebook.SetBounds(-2000, -2000, 10, 10);
  WizardForm.MainPanel.SetBounds(-2000, -2000, 10, 10);

  WizardForm.Bevel.Visible := False;
  WizardForm.BeveledLabel.Visible := False;

  if CancelText <> nil then
  begin
    CancelText.Left := (W - CancelText.Width) div 2;
    CancelText.Top := ScaleY(264);
  end;
end;

// Sets the line under the title and keeps it centred.
//
// TNewStaticText has no alignment of its own, so centring means measuring the
// text and moving the control. The caption changes while this runs, so it has
// to happen every time rather than once at the start.
procedure SetStep(const Text: String);
begin
  if StepLabel = nil then
    Exit;

  StepLabel.Caption := Text;
  StepLabel.AutoSize := True;
  StepLabel.Left := (WizardForm.ClientWidth - StepLabel.Width) div 2;
end;

function SingularityStillRunning: Boolean;
var
  Held: THandle;
begin
  // SYNCHRONIZE. Opening it adds a handle, so it is closed again immediately
  // or this copy would keep the name alive and wait on itself.
  Held := OpenMutexW($00100000, 0, 'Local\SingularityRunning');
  Result := Held <> 0;
  if Result then
    CloseHandle(Held);
end;

// Copies from before the mutex existed have no name to wait on. The image
// name is enough, and it is not this installer's name.
function SingularityImageRunning: Boolean;
var
  TempFile, Command: String;
  Lines: TArrayOfString;
  I, ResultCode: Integer;
begin
  TempFile := ExpandConstant('{tmp}\singularity-running.txt');
  Command := '/C tasklist /FI "IMAGENAME eq Singularity.exe" /NH > "' + TempFile + '"';
  Exec(ExpandConstant('{cmd}'), Command, '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Result := False;
  if LoadStringsFromFile(TempFile, Lines) then
    for I := 0 to GetArrayLength(Lines) - 1 do
      if Pos('Singularity.exe', Lines[I]) > 0 then
        Result := True;
  DeleteFile(TempFile);
end;

function SkipRunningWait: Boolean;
begin
  // Set by the program when the new files are going into their own folder.
  // The open copy is not locking those files, and closing it is what used
  // to hand the foreground to this window.
  Result := CompareText(ExpandConstant('{param:SKIPWAIT|0}'), '1') = 0;
end;

function InitializeSetup: Boolean;
var
  Tries, ResultCode: Integer;
begin
  Result := True;
  if SkipRunningWait then
    Exit;
  // The program quits itself as it launches this. Give that time to finish
  // writing settings. Only Singularity.exe is asked to close, never Explorer.
  for Tries := 1 to 150 do
  begin
    if (not SingularityStillRunning) and (not SingularityImageRunning) then
      Exit;
    if Tries = 40 then
      Exec(ExpandConstant('{sys}\taskkill.exe'), '/IM Singularity.exe', '', SW_HIDE, ewNoWait, ResultCode);
    if Tries = 100 then
      Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM Singularity.exe', '', SW_HIDE, ewNoWait, ResultCode);
    WinSleep(200);
  end;
end;

// Shown without ever becoming the foreground window when the program
// started this itself.
//
// Discord does not put a second program in front while an update unpacks.
// The new files land beside the running app, and the window you are looking
// at keeps processing clicks. This window has to stay visible, and it has
// to refuse the foreground, because Singularity closing hands the foreground
// to whatever is on screen. A foreground window that then stops answering
// is the taskbar holding every click until the unpack ends.
procedure Reveal;
begin
  if WizardForm = nil then
    Exit;
  if WizardSilent then
  begin
    WizardForm.Visible := False;
    ShowWindow(WizardForm.Handle, 0);
    if GetForegroundWindow = WizardForm.Handle then
      SetForegroundWindow(GetShellWindow);
    Exit;
  end;

  WizardForm.Visible := True;
  ShowWindow(WizardForm.Handle, SW_SHOW);
end;

procedure InitializeWizard;
var
  W, H: Integer;
begin
  // A self-update must not put a window in front. The unpack runs on this
  // thread, and a front window that stops answering is the taskbar holding
  // every click. The program draws the hole itself while this runs hidden.
  if WizardSilent then
  begin
    WizardForm.Visible := False;
    ShowWindow(WizardForm.Handle, 0);
    if GetForegroundWindow = WizardForm.Handle then
      SetForegroundWindow(GetShellWindow);
    Exit;
  end;

  LoadFrames;
  FrameIndex := 0;
  LastPercent := -1;

  // Laid out from fixed positions rather than by stacking each control under
  // the last. Chaining offsets looked tidier and put the progress bar through
  // the middle of the Cancel button, because the title's height depends on
  // the font Windows actually had and every position after it inherited the
  // difference.
  W := ScaleX(420);
  H := ScaleY(310);

  // No title bar. There is nothing to put in one: no menu, nothing worth
  // minimising, and a close button would be a second way to cancel.
  //
  // This was blamed for the installer hanging and was innocent - the culprit
  // was the Ready page, which stayed in the way despite being disabled. Left
  // in because the difference is the whole look.
  WizardForm.BorderStyle := bsNone;
  // A hand-run install stays in front: the window has no title bar, so if
  // another program covers it there is no way to find it again. An update
  // the program started itself must not. A topmost window with no border
  // sits over the taskbar and clicks there do nothing until it finishes.
  if not WizardSilent then
    WizardForm.FormStyle := fsStayOnTop;
  WizardForm.ClientWidth := W;
  WizardForm.ClientHeight := H;
  WizardForm.Position := poScreenCenter;
  WizardForm.Color := clGround;

  // Everything the wizard would normally draw.
  WizardForm.MainPanel.Visible := False;
  WizardForm.Bevel.Visible := False;
  WizardForm.BeveledLabel.Visible := False;
  WizardForm.NextButton.Visible := False;
  WizardForm.BackButton.Visible := False;

  WizardForm.OuterNotebook.Visible := False;
  WizardForm.InnerNotebook.Visible := False;

  // The mark, centred and a little above the middle.
  Spinner := TBitmapImage.Create(WizardForm);
  Spinner.Parent := WizardForm;
  Spinner.Bitmap := Frames[0];
  Spinner.Width := ScaleX(96);
  Spinner.Height := ScaleY(96);
  Spinner.Left := (W - Spinner.Width) div 2;
  Spinner.Top := ScaleY(38);

  TitleLabel := TNewStaticText.Create(WizardForm);
  TitleLabel.Parent := WizardForm;
  TitleLabel.Caption := '{#AppName}';
  TitleLabel.Font.Name := 'Segoe UI Light';
  TitleLabel.Font.Size := 22;
  TitleLabel.Font.Color := clText;
  TitleLabel.AutoSize := True;
  TitleLabel.Top := ScaleY(150);
  TitleLabel.Left := (W - TitleLabel.Width) div 2;

  StepLabel := TNewStaticText.Create(WizardForm);
  StepLabel.Parent := WizardForm;
  StepLabel.Font.Name := 'Segoe UI';
  StepLabel.Font.Size := 9;
  StepLabel.Font.Color := clMuted;
  StepLabel.Top := ScaleY(200);
  SetStep('Getting ready...');

  // A progress bar of our own.
  //
  // Windows draws its own in the system accent colour and ignores anything
  // asked of it, which on a dark window is a bright band nothing else agrees
  // with. Two flat panels do what is wanted and always look the same.
  Track := TPanel.Create(WizardForm);
  Track.Parent := WizardForm;
  Track.BevelOuter := bvNone;
  Track.Color := clTrack;
  Track.Left := ScaleX(40);
  Track.Width := W - ScaleX(80);
  Track.Height := ScaleY(3);
  Track.Top := ScaleY(234);

  Fill := TPanel.Create(WizardForm);
  Fill.Parent := Track;
  Fill.BevelOuter := bvNone;
  Fill.Color := clAccent;
  Fill.Left := 0;
  Fill.Top := 0;
  Fill.Height := Track.Height;
  Fill.Width := 0;

  // Cancel stays, small and quiet at the bottom. Taking it away would leave
  // no way out of an install that has gone wrong.
  CancelText := TNewStaticText.Create(WizardForm);
  CancelText.Parent := WizardForm;
  CancelText.Caption := 'Cancel';
  CancelText.Font.Name := 'Segoe UI';
  CancelText.Font.Size := 9;
  CancelText.Font.Color := clFaint;
  CancelText.AutoSize := True;
  CancelText.Cursor := crHand;
  CancelText.OnClick := @CancelClicked;

  Relayout;

  // Forty milliseconds is twenty five frames a second, and twenty four frames
  // make one turn - so the hole goes round about once a second, which reads
  // as turning rather than as flashing.
  TimerId := SetTimer(0, 0, 40, CreateCallback(@Advance));
  Reveal;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  Reveal;
  Relayout;

  if CurPageID = wpInstalling then
  begin
    SetStep('Installing...');
    Exit;
  end;

  SetStep('Getting ready...');

  // Walk past anything that is not the install itself.
  //
  // DisableReadyPage did not remove the Ready page - Setup still stopped on
  // it, waiting for a press, which is what left this sitting on "Getting
  // ready" for ever once the button had been moved out of sight. Printing the
  // page number on screen is what found it: page 10, the one that had
  // supposedly been disabled.
  //
  // Every page before the install asks something already decided by the act
  // of running this, so the answer is posted rather than waited for.
  if (CurPageID <> wpFinished) and WizardForm.NextButton.Enabled then
    PostMessage(WizardForm.NextButton.Handle, BM_CLICK, 0, 0);
end;

// Inno has no timer available from here, and this is called often enough
// during the unpacking to animate on its own - which also means the spin is
// tied to real work rather than to a clock that keeps turning after a stall.
procedure CurInstallProgressChanged(CurProgress, MaxProgress: Integer);
var
  Percent: Integer;
begin
  if (Fill = nil) or (MaxProgress <= 0) then
    Exit;

  Fill.Width := (Track.Width * CurProgress) div MaxProgress;

  // Only when the number actually changes. Rewriting the caption re-measures
  // the text and moves the label to keep it centred, so setting it on every
  // progress event made the line twitch from side to side.
  Percent := (CurProgress * 100) div MaxProgress;
  if Percent = LastPercent then
    Exit;

  LastPercent := Percent;
  SetStep(Format('Installing... %d%%', [Percent]));
end;

procedure DeinitializeSetup;
var
  I: Integer;
begin
  // Stopped before the bitmaps go, or a tick that is already on its way would
  // reach for one that has been freed.
  if TimerId <> 0 then
    KillTimer(0, TimerId);

  for I := 0 to FrameCount - 1 do
    if Frames[I] <> nil then
      Frames[I].Free;
end;

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
