[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
param(
    [string]$BuildDirectory,
    [string]$InstallDirectory = (Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'SoundFork'),
    [string]$BackendName = 'FiiO ASIO Driver'
)

$ErrorActionPreference = 'Stop'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an elevated 64-bit PowerShell session. It will not self-elevate.'
}
if (-not [Environment]::Is64BitProcess) {
    throw 'Use 64-bit PowerShell to install the x64 SoundFork driver.'
}

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $packagedBin = Join-Path $PSScriptRoot '..\bin'
    $developmentBin = Join-Path $PSScriptRoot '..\build\windows-msvc-x64\Release'
    if (Test-Path -LiteralPath $packagedBin -PathType Container) {
        $BuildDirectory = $packagedBin
    } else {
        $BuildDirectory = $developmentBin
    }
}
$resolvedBuild = (Resolve-Path -LiteralPath $BuildDirectory).Path
$requiredFiles = @(
    'soundfork_driver.dll',
    'soundfork_host.exe',
    'soundfork_capture_monitor.exe',
    'soundfork_discord_relay.exe',
    'soundfork_capture_hub.exe',
    'soundfork_control.exe'
)
foreach ($file in $requiredFiles) {
    $source = Join-Path $resolvedBuild $file
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Required Release artifact was not found: $source"
    }
}

$blockingProcesses = @(
    Get-Process -Name 'soundfork_discord_relay', 'soundfork_capture_hub', 'soundfork_capture_monitor', 'soundfork_control' -ErrorAction SilentlyContinue
)
if ($blockingProcesses.Count) {
    $names = ($blockingProcesses | ForEach-Object { "$($_.ProcessName) (PID $($_.Id))" }) -join ', '
    throw "Close programs currently using SoundFork before installation: $names"
}

$fullInstallDirectory = [IO.Path]::GetFullPath($InstallDirectory)
$programFiles = [IO.Path]::GetFullPath([Environment]::GetFolderPath('ProgramFiles'))
if (-not $fullInstallDirectory.StartsWith($programFiles + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw "InstallDirectory must be a child of '$programFiles'."
}

if ($PSCmdlet.ShouldProcess($fullInstallDirectory, "Install SoundFork and register backend '$BackendName'")) {
    New-Item -ItemType Directory -Path $fullInstallDirectory -Force | Out-Null
    foreach ($file in $requiredFiles) {
        Copy-Item -LiteralPath (Join-Path $resolvedBuild $file) -Destination (Join-Path $fullInstallDirectory $file) -Force
    }

    $installedDll = Join-Path $fullInstallDirectory 'soundfork_driver.dll'
    & (Join-Path $PSScriptRoot 'Register-SoundFork.ps1') `
        -DllPath $installedDll -BackendName $BackendName -Confirm:$false

    Write-Host "SoundFork installed in '$fullInstallDirectory'."
    Write-Host "Check it with: & '$PSScriptRoot\Get-SoundForkStatus.ps1'"
}
