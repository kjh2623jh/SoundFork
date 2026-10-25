[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CompilerPath
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\scripts\Invoke-ReleaseTestProcess.ps1')
$installer = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\installer\SoundFork.iss') -Raw -Encoding UTF8
$initializer = [regex]::Match($installer, '(?s)function InitializeSetup: Boolean;.*?(?=procedure InitializeWizard;)').Value
if (-not $initializer) { throw 'Could not extract the production InitializeSetup function.' }
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('SoundForkSetupRejection-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
try {
    # Compile the exact production initialization function with synthetic backend
    # discovery. Lowest privileges, no files, registry entries or audio processes.
    # If a rejection is broken, Abort stops the probe before installation.
    $probe = @'
[Setup]
AppName=SoundFork rejection probe
AppVersion=1
DefaultDirName={tmp}\SoundForkNeverInstall
PrivilegesRequired=lowest
Uninstallable=no
CreateAppDir=no
OutputBaseFilename=rejection-probe
SetupLogging=yes
[CustomMessages]
NoBackend=No backend fixture.
InvalidSilentBackend=Invalid backend fixture.
[Code]
var
  BackendNames: TArrayOfString;
  RequestedBackendClsid: String;
  ExistingBackendClsid: String;
function FindBackendByClsid(Value: String): Integer;
begin
  Result := -1;
  if Value = '{11111111-1111-1111-1111-111111111111}' then Result := 0;
end;
procedure LoadBackends;
begin
  ExistingBackendClsid := '';
  if ExpandConstant('{param:NOBACKEND|}') <> '1' then
  begin
    SetArrayLength(BackendNames, 1);
    BackendNames[0] := 'Synthetic backend';
  end;
end;
__INITIALIZER__
procedure InitializeWizard;
begin
  Abort;
end;
'@
    $probePath = Join-Path $testRoot 'rejection-probe.iss'
    $probe.Replace('__INITIALIZER__', $initializer) | Set-Content -LiteralPath $probePath -Encoding UTF8
    $compileExit = Invoke-ReleaseTestProcess -StepName 'Compile production setup initialization probe' `
        -FilePath $CompilerPath -ArgumentList @('/Qp', "/O`"$testRoot`"", "`"$probePath`"")
    if ($compileExit -ne 0) { throw "Rejection probe compilation failed ($compileExit)." }
    $cases = @(
        @{ Name = 'missing'; Arguments = @(); Reason = 'requires a valid ASIO backend selection' },
        @{ Name = 'invalid'; Arguments = @('/BACKENDCLSID=invalid'); Reason = 'requires a valid ASIO backend selection' },
        @{ Name = 'no-driver'; Arguments = @('/NOBACKEND=1'); Reason = 'no compatible x64 ASIO driver' }
    )
    foreach ($case in $cases) {
        $log = Join-Path $testRoot ($case.Name + '.log')
        $arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', "/LOG=`"$log`"") + $case.Arguments
        $result = Invoke-ReleaseTestProcess -StepName "Production setup rejects $($case.Name)" `
            -FilePath (Join-Path $testRoot 'rejection-probe.exe') -ArgumentList $arguments `
            -LogPath $log -TimeoutSeconds 10
        if ($result -ne 1 -or (Get-Content -LiteralPath $log -Raw) -notmatch $case.Reason) {
            throw "Probe did not reject $($case.Name) in production initialization (exit $result)."
        }
    }
    Write-Output 'All three compiled production rejection paths exited without dialogs; no installation or registry changes.'
} finally {
    Remove-Item -LiteralPath $testRoot -Recurse -Force
}
