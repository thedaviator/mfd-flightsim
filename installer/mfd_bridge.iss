; Inno Setup script for MFD Bridge. Built by tools/make_release.py, which passes
; /DAppVersion=..., /DSourceDir=<PyInstaller output folder>, /DVCRedist=<vc_redist.x64.exe> and
; /DBrandDir=<AeroRelics images from tools/make_branding.py>.
;
; Before installing, the setup checks the PC and shows the results on the "Ready" page:
;   - Windows 10 1809+ / 11, 64-bit (refuses otherwise)
;   - Microsoft Visual C++ 2015-2022 runtime (installs it silently when missing)
;   - Microsoft Flight Simulator 2020 / 2024 and X-Plane 12 (information only)
;   - an MFD Bridge that is already installed or running (upgraded in place, settings kept)

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\release\build\MFD Bridge"
#endif
#ifndef VCRedist
  #define VCRedist "..\release\cache\vc_redist.x64.exe"
#endif
#ifndef BrandDir
  #define BrandDir "..\release\build\branding"
#endif

[Setup]
AppId={{7C1E8A52-5B0D-4F7E-9C1A-3F4B2D6E8A10}
AppName=MFD Bridge
AppVersion={#AppVersion}
AppVerName=MFD Bridge {#AppVersion}
AppPublisher=AeroRelics
DefaultDirName={autopf}\MFD Bridge
DefaultGroupName=MFD Bridge
UninstallDisplayIcon={app}\MFD Bridge.exe
OutputDir=..\release
OutputBaseFilename=MFD-Bridge-Setup-{#AppVersion}
SetupIconFile={#BrandDir}\mfd.ico
; AeroRelics: logo on the inner pages and the last page; the welcome page is the full
; artwork (see InitializeWizard).
WizardImageFile={#BrandDir}\side_100.bmp,{#BrandDir}\side_125.bmp,{#BrandDir}\side_150.bmp,{#BrandDir}\side_200.bmp
WizardSmallImageFile={#BrandDir}\small_100.bmp,{#BrandDir}\small_125.bmp,{#BrandDir}\small_150.bmp,{#BrandDir}\small_200.bmp
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
UsedUserAreasWarning=no
; 64-bit Windows 10 version 1809 or newer
MinVersion=10.0.17763
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=force
RestartApplications=no
DisableWelcomePage=no
; "Use at your own risk" terms + MIT License; setup goes on only once the box is ticked
; (the agree tick box replaces the usual radio buttons, see InitializeWizard).
LicenseFile=LICENSE_AGREEMENT.txt
AppCopyright=Copyright (c) 2026 AeroRelics - David Eckert. MIT License.

[Messages]
WelcomeLabel2=This installs [name/ver] on your computer.%n%nMFD Bridge sends flight data from Microsoft Flight Simulator or X-Plane 12 to the MFD display over WiFi.%n%nSetup checks this PC for the components MFD Bridge needs and installs any that are missing.%n%nMFD Bridge is free, open-source software (MIT License), for flight simulation only.
LicenseLabel3=Please read these terms. MFD Bridge is for flight simulation only and is used at your own risk. Tick the box below to continue.

[Tasks]
Name: "autostart"; Description: "Start MFD Bridge with Windows (minimized)"; GroupDescription: "Options:"
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Options:"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#VCRedist}"; DestDir: "{tmp}"; DestName: "vc_redist.x64.exe"; Flags: deleteafterinstall; Check: VCRedistNeeded
Source: "{#BrandDir}\welcome_*.bmp"; Flags: dontcopy
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\THIRD_PARTY_NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "LICENSE_AGREEMENT.txt"; DestDir: "{app}"; DestName: "TERMS.txt"; Flags: ignoreversion

[Icons]
Name: "{group}\MFD Bridge"; Filename: "{app}\MFD Bridge.exe"
Name: "{group}\MFD Quick Start"; Filename: "{app}\_internal\QUICKSTART.html"
Name: "{group}\License and notices"; Filename: "{app}\THIRD_PARTY_NOTICES.txt"
Name: "{group}\Uninstall MFD Bridge"; Filename: "{uninstallexe}"
Name: "{autodesktop}\MFD Bridge"; Filename: "{app}\MFD Bridge.exe"; Tasks: desktopicon

[Registry]
; Per-user autostart for the user running setup.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "MFD Bridge"; \
  ValueData: """{app}\MFD Bridge.exe"" --minimized"; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; \
  StatusMsg: "Installing the Microsoft Visual C++ runtime..."; Flags: waituntilterminated; Check: VCRedistNeeded
; Let the display reach the bridge: WebSocket (TCP 8080). All network profiles, because
; Windows classes most new home networks as Public.
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""MFD Bridge"""; Flags: runhidden; \
  StatusMsg: "Allowing MFD Bridge through Windows Firewall..."
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""MFD Bridge"" dir=in action=allow program=""{app}\MFD Bridge.exe"" enable=yes profile=any"; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""MFD Bridge"" dir=in action=allow protocol=TCP localport=8080,8082 enable=yes profile=any"; Flags: runhidden
Filename: "{app}\_internal\QUICKSTART.html"; Description: "Open the Quick Start guide"; Flags: postinstall shellexec skipifsilent
Filename: "{app}\MFD Bridge.exe"; Description: "Start MFD Bridge now"; Flags: postinstall nowait skipifsilent

[UninstallRun]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""MFD Bridge"""; Flags: runhidden; RunOnceId: "fwdel"

[Code]
var
  VCNeeded: Boolean;
  VCChecked: Boolean;
  AgreeBox: TNewCheckBox;

// License page: one "I agree" tick box instead of the accept / do-not-accept radio buttons.
// The hidden radios still carry the answer, so Inno's own check (Next only when accepted)
// keeps working.
procedure AgreeBoxClick(Sender: TObject);
begin
  WizardForm.LicenseAcceptedRadio.Checked := AgreeBox.Checked;
  WizardForm.LicenseNotAcceptedRadio.Checked := not AgreeBox.Checked;
  WizardForm.NextButton.Enabled := AgreeBox.Checked;
end;

procedure BuildAgreeBox();
begin
  WizardForm.LicenseAcceptedRadio.Visible := False;
  WizardForm.LicenseNotAcceptedRadio.Visible := False;
  AgreeBox := TNewCheckBox.Create(WizardForm);
  AgreeBox.Parent := WizardForm.LicensePage;
  AgreeBox.Left := WizardForm.LicenseAcceptedRadio.Left;
  AgreeBox.Top := WizardForm.LicenseAcceptedRadio.Top;
  AgreeBox.Width := WizardForm.LicensePage.Width - AgreeBox.Left;
  AgreeBox.Height := WizardForm.LicenseAcceptedRadio.Height;
  AgreeBox.Caption := 'I have read and agree to these terms, and use MFD Bridge at my own risk';
  AgreeBox.Checked := False;
  AgreeBox.OnClick := @AgreeBoxClick;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpLicense then
    WizardForm.NextButton.Enabled := AgreeBox.Checked;
end;

// Welcome page: the AeroRelics artwork (it carries its own welcome text), centred on navy,
// in the largest of the pre-scaled sizes that fits (they match 100-200 % display scaling).
procedure InitializeWizard();
var
  Page: TNewNotebookPage;
  Img: TBitmapImage;
  Sizes: array[0..3] of Integer;
  I, Best: Integer;
  Name: String;
begin
  Page := WizardForm.WelcomePage;
  Sizes[0] := 314; Sizes[1] := 392; Sizes[2] := 471; Sizes[3] := 628;
  Best := Sizes[0];
  for I := 0 to 3 do
    if Sizes[I] <= Page.Height then Best := Sizes[I];
  Name := 'welcome_' + IntToStr(Best) + '.bmp';
  ExtractTemporaryFile(Name);
  WizardForm.WizardBitmapImage.Visible := False;
  WizardForm.WelcomeLabel1.Visible := False;
  WizardForm.WelcomeLabel2.Visible := False;
  Img := TBitmapImage.Create(WizardForm);
  Img.Parent := Page;
  Img.SetBounds(0, 0, Page.Width, Page.Height);
  Img.BackColor := $573B22;       // navy, BGR
  Img.Center := True;
  Img.Bitmap.LoadFromFile(ExpandConstant('{tmp}\' + Name));
  BuildAgreeBox();
end;

// Visual C++ 2015-2022 x64 runtime: present and at least 14.30.
function VCRedistNeeded(): Boolean;
var
  Installed, Major, Minor: Cardinal;
  Key: String;
begin
  if not VCChecked then begin
    Key := 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64';
    VCNeeded := True;
    if RegQueryDWordValue(HKLM64, Key, 'Installed', Installed) and (Installed = 1) and
       RegQueryDWordValue(HKLM64, Key, 'Major', Major) and RegQueryDWordValue(HKLM64, Key, 'Minor', Minor) then
      VCNeeded := (Major < 14) or ((Major = 14) and (Minor < 30));
    VCChecked := True;
  end;
  Result := VCNeeded;
end;

function SteamHas(AppId: String): Boolean;
var
  SteamPath: String;
begin
  Result := False;
  if RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', SteamPath) then begin
    StringChangeEx(SteamPath, '/', '\', True);
    Result := FileExists(SteamPath + '\steamapps\appmanifest_' + AppId + '.acf');
  end;
end;

function FoundMSFS2024(): Boolean;
begin
  Result := DirExists(ExpandConstant('{localappdata}\Packages\Microsoft.Limitless_8wekyb3d8bbwe')) or
            DirExists(ExpandConstant('{userappdata}\Microsoft Flight Simulator 2024')) or SteamHas('2537590');
end;

function FoundMSFS2020(): Boolean;
begin
  Result := DirExists(ExpandConstant('{localappdata}\Packages\Microsoft.FlightSimulator_8wekyb3d8bbwe')) or
            FileExists(ExpandConstant('{userappdata}\Microsoft Flight Simulator\UserCfg.opt')) or SteamHas('1250410');
end;

function FoundXPlane12(): Boolean;
begin
  Result := FileExists(ExpandConstant('{localappdata}\x-plane_install_12.txt'));
end;

function YesNo(Found: Boolean; Yes, No: String): String;
begin
  if Found then Result := Yes else Result := No;
end;

// The "Ready to install" page lists what setup found and what it will do.
function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
var
  S, Prev: String;
  Sims: Boolean;
begin
  S := 'System check:' + NewLine;
  S := S + Space + 'Windows 64-bit, version 10 or newer: OK' + NewLine;
  S := S + Space + 'Python and libraries: included in MFD Bridge (nothing to install)' + NewLine;
  S := S + Space + 'Microsoft Visual C++ runtime: ' +
       YesNo(VCRedistNeeded(), 'missing - will be installed', 'already installed') + NewLine;
  S := S + Space + 'Windows Firewall: will allow MFD Bridge (TCP 8080)' + NewLine;
  if RegQueryStringValue(HKLM64, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{7C1E8A52-5B0D-4F7E-9C1A-3F4B2D6E8A10}_is1',
                         'DisplayVersion', Prev) then
    S := S + Space + 'Existing MFD Bridge ' + Prev + ': will be updated, settings kept' + NewLine;
  S := S + NewLine + 'Simulators found:' + NewLine;
  Sims := False;
  if FoundMSFS2024() then begin S := S + Space + 'Microsoft Flight Simulator 2024' + NewLine; Sims := True; end;
  if FoundMSFS2020() then begin S := S + Space + 'Microsoft Flight Simulator 2020' + NewLine; Sims := True; end;
  if FoundXPlane12() then begin S := S + Space + 'X-Plane 12' + NewLine; Sims := True; end;
  if not Sims then
    S := S + Space + 'none found. MFD Bridge still installs; it connects once a simulator runs,' + NewLine +
             Space + 'and its Demo flight works without one.' + NewLine;
  S := S + NewLine + MemoDirInfo + NewLine;
  if MemoGroupInfo <> '' then S := S + NewLine + MemoGroupInfo + NewLine;
  if MemoTasksInfo <> '' then S := S + NewLine + MemoTasksInfo + NewLine;
  Result := S;
end;

// Verify the runtime really is there afterwards; warn instead of failing silently.
procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    VCChecked := False;
    if VCRedistNeeded() then
      MsgBox('The Microsoft Visual C++ runtime could not be installed automatically.' + #13#10 +
             'MFD Bridge includes its own copy and will normally still work. If it does not start, install' + #13#10 +
             '"Microsoft Visual C++ Redistributable 2015-2022 (x64)" from microsoft.com.', mbInformation, MB_OK);
  end;
end;
