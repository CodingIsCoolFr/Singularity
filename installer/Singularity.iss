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
#define AppVersion    "0.5.7"
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

; Let Windows close the running copy rather than failing on a locked file.
;
; The program updates itself, so the usual case is that Singularity is running
; when this starts - it is the thing that downloaded and launched this. The
; Restart Manager asks it to close properly first, which matters: killing it
; outright would lose settings that Qt writes on the way out.
;
; "force" rather than "yes", and this is not a small distinction. With "yes"
; Inno shows a page listing what it needs to close - and the pages are hidden
; here, so that page appeared as nothing at all. The installer sat on "Getting
; ready" for ever, waiting for an answer to a question nobody could see. It
; looked exactly like a hang, and it only happens when the program is already
; running, which is every single self-update.
CloseApplications=yes
RestartApplications=no

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

procedure InitializeWizard;
var
  W, H: Integer;
begin
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
end;

procedure CurPageChanged(CurPageID: Integer);
begin
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
