[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
param(
    [Parameter(Mandatory = $true)]
    [string]$DllPath,

    [string]$BackendName = 'FiiO ASIO Driver'
)

$ErrorActionPreference = 'Stop'
$soundForkClsid = '{A04A0128-E43C-4D08-B701-87EC1A847A89}'
$asioRoot = 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\ASIO'
$backendKey = Join-Path $asioRoot $BackendName
$soundForkKey = Join-Path $asioRoot 'SoundFork'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an elevated 64-bit PowerShell session. It will not self-elevate.'
}
if (-not [Environment]::Is64BitProcess) {
    throw 'Use 64-bit PowerShell to register the x64 SoundFork driver.'
}

$resolvedDll = (Resolve-Path -LiteralPath $DllPath).Path
if ([IO.Path]::GetExtension($resolvedDll) -ne '.dll') {
    throw 'DllPath must identify soundfork_driver.dll.'
}
if ([IO.Path]::GetFileName($resolvedDll) -ne 'soundfork_driver.dll') {
    throw 'DllPath must identify the built soundfork_driver.dll.'
}

$stream = [IO.File]::OpenRead($resolvedDll)
try {
    $reader = [IO.BinaryReader]::new($stream)
    if ($reader.ReadUInt16() -ne 0x5A4D) { throw 'The selected file is not a PE DLL.' }
    $stream.Position = 0x3C
    $peOffset = $reader.ReadInt32()
    $stream.Position = $peOffset
    if ($reader.ReadUInt32() -ne 0x00004550) { throw 'The selected file has an invalid PE signature.' }
    if ($reader.ReadUInt16() -ne 0x8664) { throw 'SoundFork registration requires an x64 DLL.' }
} finally {
    $stream.Dispose()
}
if (-not (Test-Path -LiteralPath $backendKey)) {
    throw "ASIO backend '$BackendName' was not found under HKLM\SOFTWARE\ASIO."
}
$backendClsid = (Get-ItemPropertyValue -LiteralPath $backendKey -Name 'CLSID').ToString()
$parsedBackend = [Guid]::Parse($backendClsid)
if ($parsedBackend.ToString('B').ToUpperInvariant() -eq $soundForkClsid) {
    throw 'SoundFork cannot use itself as its backend.'
}

$regsvr32 = Join-Path $env:SystemRoot 'System32\regsvr32.exe'
if ($PSCmdlet.ShouldProcess($resolvedDll, "Register SoundFork and route it to '$BackendName'")) {
    $registerArguments = "/s `"$resolvedDll`""
    $process = Start-Process -FilePath $regsvr32 -ArgumentList $registerArguments -Wait -PassThru -WindowStyle Hidden
    if ($process.ExitCode -ne 0) {
        throw "regsvr32 failed with exit code $($process.ExitCode)."
    }

    try {
        New-ItemProperty -LiteralPath $soundForkKey -Name 'BackendCLSID' -PropertyType String -Value $backendClsid -Force | Out-Null
    } catch {
        $rollbackArguments = "/u /s `"$resolvedDll`""
        Start-Process -FilePath $regsvr32 -ArgumentList $rollbackArguments -Wait -WindowStyle Hidden
        throw
    }
    Write-Host "SoundFork registered with backend '$BackendName' ($backendClsid)."
}
