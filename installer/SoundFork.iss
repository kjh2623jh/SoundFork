#ifndef MyAppVersion
  #define MyAppVersion "0.1.0"
#endif
#ifndef MySourceDir
  #define MySourceDir "..\out"
#endif
#ifndef MyOutputDir
  #define MyOutputDir "..\artifacts"
#endif

[Setup]
AppId={{9A33D416-54A0-4A77-AF42-D487F4976558}
AppName=SoundFork
AppVersion={#MyAppVersion}
AppPublisher=SoundFork contributors
DefaultDirName={autopf}\SoundFork
DefaultGroupName=SoundFork
DisableProgramGroupPage=yes
OutputDir={#MyOutputDir}
OutputBaseFilename=SoundFork-Setup-v{#MyAppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
LicenseFile={#MySourceDir}\LICENSE
SetupIconFile={#MySourceDir}\assets\soundfork.ico
UninstallDisplayIcon={app}\soundfork_control.exe
CloseApplications=yes
RestartApplications=no
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"

[CustomMessages]
english.BackendTitle=Hardware ASIO backend
english.BackendDescription=Select the hardware ASIO driver SoundFork will wrap.
english.BackendSubCaption=SoundFork never modifies or removes this driver. FiiO ASIO Driver is preferred when available.
english.NoBackend=No compatible x64 ASIO driver is registered. Install your DAC or audio interface ASIO driver first.
english.InvalidSilentBackend=Silent setup requires /BACKENDCLSID={{GUID} identifying a registered x64 ASIO driver.
english.UnregisterFailed=The existing SoundFork driver could not be unregistered. Close every ASIO host and try again.
english.RegistrationFailed=SoundFork driver registration failed. Setup rolled back the partial registration.
english.GetVbCable=Open the official VB-CABLE download page (recommended for Discord)
english.LaunchControl=Launch SoundFork Control
korean.BackendTitle=하드웨어 ASIO 백엔드
korean.BackendDescription=SoundFork가 감쌀 하드웨어 ASIO 드라이버를 선택하세요.
korean.BackendSubCaption=SoundFork는 선택한 드라이버를 수정하거나 제거하지 않습니다. FiiO ASIO Driver가 있으면 우선 선택됩니다.
korean.NoBackend=호환되는 x64 ASIO 드라이버가 등록되어 있지 않습니다. DAC 또는 오디오 인터페이스의 ASIO 드라이버를 먼저 설치하세요.
korean.InvalidSilentBackend=자동 설치에는 등록된 x64 ASIO 드라이버의 /BACKENDCLSID={{GUID}가 필요합니다.
korean.UnregisterFailed=기존 SoundFork 드라이버의 등록을 해제하지 못했습니다. 모든 ASIO 호스트를 닫고 다시 시도하세요.
korean.RegistrationFailed=SoundFork 드라이버 등록에 실패했습니다. 부분 등록은 롤백했습니다.
korean.GetVbCable=VB-CABLE 공식 다운로드 페이지 열기 (Discord 권장)
korean.LaunchControl=SoundFork Control 실행

[Files]
Source: "{#MySourceDir}\bin\soundfork_driver.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MySourceDir}\bin\soundfork_control.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MySourceDir}\bin\soundfork_discord_relay.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MySourceDir}\bin\soundfork_host.exe"; DestDir: "{app}\tools"; Flags: ignoreversion
Source: "{#MySourceDir}\bin\soundfork_capture_monitor.exe"; DestDir: "{app}\tools"; Flags: ignoreversion
Source: "{#MySourceDir}\*.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "{#MySourceDir}\docs\*.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "{#MySourceDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\SoundFork Control"; Filename: "{app}\soundfork_control.exe"; WorkingDir: "{app}"
Name: "{group}\Uninstall SoundFork"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\soundfork_control.exe"; Parameters: "--initialize-language {language}"; Flags: runhidden waituntilterminated runasoriginaluser
Filename: "{app}\soundfork_control.exe"; Description: "{cm:LaunchControl}"; Flags: nowait postinstall skipifsilent
Filename: "https://vb-audio.com/Cable/"; Description: "{cm:GetVbCable}"; Flags: shellexec postinstall skipifsilent; Check: ShouldOfferVbCable

[UninstallRun]
Filename: "{app}\soundfork_control.exe"; Parameters: "--unregister-driver"; Flags: runhidden waituntilterminated; RunOnceId: "UnregisterSoundFork"

[Code]
const
  AsioRoot = 'SOFTWARE\ASIO';
  SoundForkClsid = '{A04A0128-E43C-4D08-B701-87EC1A847A89}';

var
  BackendPage: TInputOptionWizardPage;
  BackendNames: TArrayOfString;
  BackendClsids: TArrayOfString;
  RequestedBackendClsid: String;
  DefaultBackendIndex: Integer;

procedure SwapStrings(var Left: String; var Right: String);
var
  Temporary: String;
begin
  Temporary := Left;
  Left := Right;
  Right := Temporary;
end;

function FindBackendByClsid(const Clsid: String): Integer;
var
  Index: Integer;
begin
  Result := -1;
  for Index := 0 to GetArrayLength(BackendClsids) - 1 do
    if CompareText(BackendClsids[Index], Clsid) = 0 then
    begin
      Result := Index;
      Exit;
    end;
end;

procedure LoadBackends;
var
  Subkeys: TArrayOfString;
  Name: String;
  Clsid: String;
  Index: Integer;
  Inner: Integer;
  Count: Integer;
begin
  SetArrayLength(BackendNames, 0);
  SetArrayLength(BackendClsids, 0);
  DefaultBackendIndex := -1;
  if not RegGetSubkeyNames(HKLM64, AsioRoot, Subkeys) then
    Exit;

  Count := 0;
  for Index := 0 to GetArrayLength(Subkeys) - 1 do
  begin
    Name := Subkeys[Index];
    Clsid := '';
    if (CompareText(Name, 'SoundFork') <> 0) and
       RegQueryStringValue(HKLM64, AsioRoot + '\' + Name, 'CLSID', Clsid) and
       (Clsid <> '') and (CompareText(Clsid, SoundForkClsid) <> 0) and
       RegKeyExists(HKLM64, 'SOFTWARE\Classes\CLSID\' + Clsid + '\InprocServer32') then
    begin
      SetArrayLength(BackendNames, Count + 1);
      SetArrayLength(BackendClsids, Count + 1);
      BackendNames[Count] := Name;
      BackendClsids[Count] := Clsid;
      Count := Count + 1;
    end;
  end;

  for Index := 0 to Count - 2 do
    for Inner := Index + 1 to Count - 1 do
      if CompareText(BackendNames[Index], BackendNames[Inner]) > 0 then
      begin
        SwapStrings(BackendNames[Index], BackendNames[Inner]);
        SwapStrings(BackendClsids[Index], BackendClsids[Inner]);
      end;

  for Index := 0 to Count - 1 do
    if CompareText(BackendNames[Index], 'FiiO ASIO Driver') = 0 then
      DefaultBackendIndex := Index;
  if (DefaultBackendIndex < 0) and (Count > 0) then
    DefaultBackendIndex := 0;
end;

function InitializeSetup: Boolean;
begin
  LoadBackends;
  if GetArrayLength(BackendNames) = 0 then
  begin
    MsgBox(ExpandConstant('{cm:NoBackend}'), mbError, MB_OK);
    Result := False;
    Exit;
  end;

  RequestedBackendClsid := ExpandConstant('{param:BACKENDCLSID|}');
  if WizardSilent and
     ((RequestedBackendClsid = '') or
      (FindBackendByClsid(RequestedBackendClsid) < 0)) then
  begin
    MsgBox(ExpandConstant('{cm:InvalidSilentBackend}'), mbError, MB_OK);
    Result := False;
    Exit;
  end;
  Result := True;
end;

procedure InitializeWizard;
var
  Index: Integer;
  RequestedIndex: Integer;
begin
  BackendPage := CreateInputOptionPage(
    wpSelectDir,
    ExpandConstant('{cm:BackendTitle}'),
    ExpandConstant('{cm:BackendDescription}'),
    ExpandConstant('{cm:BackendSubCaption}'),
    True,
    False);
  for Index := 0 to GetArrayLength(BackendNames) - 1 do
    BackendPage.Add(BackendNames[Index]);

  RequestedIndex := FindBackendByClsid(RequestedBackendClsid);
  if RequestedIndex >= 0 then
    BackendPage.SelectedValueIndex := RequestedIndex
  else
    BackendPage.SelectedValueIndex := DefaultBackendIndex;
end;

function GetSelectedBackendClsid: String;
begin
  Result := BackendClsids[BackendPage.SelectedValueIndex];
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ExistingDriver: String;
  ResultCode: Integer;
begin
  Result := '';
  ExistingDriver := ExpandConstant('{app}\soundfork_driver.dll');
  if FileExists(ExistingDriver) then
  begin
    if (not Exec(
          ExpandConstant('{sys}\regsvr32.exe'),
          '/u /s "' + ExistingDriver + '"',
          '',
          SW_HIDE,
          ewWaitUntilTerminated,
          ResultCode)) or (ResultCode <> 0) then
      Result := ExpandConstant('{cm:UnregisterFailed}');
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
  ControlPath: String;
begin
  if CurStep = ssPostInstall then
  begin
    ControlPath := ExpandConstant('{app}\soundfork_control.exe');
    if (not Exec(
          ControlPath,
          '--register-driver --backend-clsid "' + GetSelectedBackendClsid + '"',
          ExpandConstant('{app}'),
          SW_HIDE,
          ewWaitUntilTerminated,
          ResultCode)) or (ResultCode <> 0) then
    begin
      Exec(
        ControlPath,
        '--unregister-driver',
        ExpandConstant('{app}'),
        SW_HIDE,
        ewWaitUntilTerminated,
        ResultCode);
      RaiseException(ExpandConstant('{cm:RegistrationFailed}'));
    end;
  end;
end;

function ShouldOfferVbCable: Boolean;
var
  ResultCode: Integer;
begin
  Result := True;
  if Exec(
       ExpandConstant('{app}\soundfork_control.exe'),
       '--has-broadcast-endpoint',
       ExpandConstant('{app}'),
       SW_HIDE,
       ewWaitUntilTerminated,
       ResultCode) then
    Result := ResultCode <> 0;
end;
