[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
param(
    [string]$InstallDirectory = (Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'SoundFork')
)

$ErrorActionPreference = 'Stop'
$soundForkClsid = '{A04A0128-E43C-4D08-B701-87EC1A847A89}'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an elevated 64-bit PowerShell session. It will not self-elevate.'
}
if (-not [Environment]::Is64BitProcess) {
    throw 'Use 64-bit PowerShell to remove the x64 SoundFork driver.'
}

$running = @(
    Get-Process -Name 'soundfork_discord_relay', 'soundfork_capture_monitor', 'soundfork_control' -ErrorAction SilentlyContinue
)
if ($running.Count) {
    $names = ($running | ForEach-Object { "$($_.ProcessName) (PID $($_.Id))" }) -join ', '
    throw "Close programs currently using SoundFork before removal: $names"
}

$baseKey = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
    [Microsoft.Win32.RegistryHive]::LocalMachine,
    [Microsoft.Win32.RegistryView]::Registry64)
try {
    $comKey = $baseKey.OpenSubKey("SOFTWARE\Classes\CLSID\$soundForkClsid\InprocServer32", $false)
    if ($null -ne $comKey) {
        try { $registeredDll = [string]$comKey.GetValue('') } finally { $comKey.Dispose() }
    }
} finally {
    $baseKey.Dispose()
}

$fullInstallDirectory = [IO.Path]::GetFullPath($InstallDirectory)
$programFiles = [IO.Path]::GetFullPath([Environment]::GetFolderPath('ProgramFiles'))
if (-not $fullInstallDirectory.StartsWith($programFiles + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw "InstallDirectory must be a child of '$programFiles'."
}

if ($PSCmdlet.ShouldProcess($fullInstallDirectory, 'Unregister and remove SoundFork')) {
    if ($registeredDll -and (Test-Path -LiteralPath $registeredDll -PathType Leaf)) {
        & (Join-Path $PSScriptRoot 'Unregister-SoundFork.ps1') -DllPath $registeredDll -Confirm:$false
    } else {
        $asioKey = 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\ASIO\SoundFork'
        $comKey = "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\CLSID\$soundForkClsid"
        Remove-Item -LiteralPath $asioKey -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $comKey -Recurse -Force -ErrorAction SilentlyContinue
        Write-Host 'Removed stale SoundFork registry entries.'
    }

    $knownFiles = @(
        'soundfork_driver.dll',
        'soundfork_host.exe',
        'soundfork_capture_monitor.exe',
        'soundfork_discord_relay.exe',
        'soundfork_control.exe'
    )
    foreach ($file in $knownFiles) {
        Remove-Item -LiteralPath (Join-Path $fullInstallDirectory $file) -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $fullInstallDirectory) {
        Remove-Item -LiteralPath $fullInstallDirectory -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $fullInstallDirectory) {
            Write-Warning "'$fullInstallDirectory' contains unknown files and was left in place."
        }
    }
    Write-Host 'SoundFork was removed. FiiO and other ASIO drivers were not modified.'
}
