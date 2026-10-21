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
    Get-Process -Name 'soundfork_discord_relay', 'soundfork_capture_hub', 'soundfork_capture_monitor', 'soundfork_control' -ErrorAction SilentlyContinue
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

function Assert-NoReparsePoints([string]$Path) {
    $candidate = [IO.Path]::GetFullPath($Path)
    while ($candidate) {
        if (Test-Path -LiteralPath $candidate) {
            if ((Get-Item -LiteralPath $candidate -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Refusing to remove files through a link: $candidate"
            }
        }
        $candidate = [IO.Path]::GetDirectoryName($candidate)
    }
}
Assert-NoReparsePoints $fullInstallDirectory
$payloadFiles = @('bin\64bit\soundfork-obs.dll', 'bin\64bit\soundfork_capture_hub.exe',
    'data\locale\en-US.ini', 'data\locale\ko-KR.ini', 'LICENSE',
    'THIRD_PARTY_NOTICES.md', 'THIRD_PARTY_NOTICES.en.md')
$payload = Join-Path $fullInstallDirectory 'obs-plugin\soundfork-obs'
foreach ($file in $payloadFiles) { Assert-NoReparsePoints (Join-Path $payload $file) }

if ($PSCmdlet.ShouldProcess($fullInstallDirectory, 'Unregister and remove SoundFork')) {
    $control = Join-Path $fullInstallDirectory 'soundfork_control.exe'
    # Older controls open a GUI for unknown commands. Check the explicit PE
    # capability marker rather than guessing from a version or payload files.
    if ((Test-Path -LiteralPath $control -PathType Leaf) -and
        (Get-Item -LiteralPath $control).VersionInfo.Comments -match '(^|;)SoundFork\.ObsPluginManagement/1(;|$)') {
        $pluginRemoval = Start-Process -FilePath $control -ArgumentList '--remove-obs-plugin' -Wait -PassThru -WindowStyle Hidden
        if ($pluginRemoval.ExitCode -ne 0) {
            throw "OBS plugin removal failed (code $($pluginRemoval.ExitCode)). Close OBS and retry before removing SoundFork."
        }
    }
    if ((Test-Path -LiteralPath $control -PathType Leaf) -and
        (Get-Item -LiteralPath $control).VersionInfo.Comments -match '(^|;)SoundFork\.StartupManagement/1(;|$)') {
        # Only the removing account's exact startup registration for this copy.
        $startupRemoval = Start-Process -FilePath $control -ArgumentList '--remove-user-startup' -Wait -PassThru -WindowStyle Hidden
        if ($startupRemoval.ExitCode -ne 0) {
            throw "SoundFork user startup cleanup failed (code $($startupRemoval.ExitCode))."
        }
    }
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
        'soundfork_capture_hub.exe',
        'soundfork_control.exe'
    )
    foreach ($file in $knownFiles) {
        Remove-Item -LiteralPath (Join-Path $fullInstallDirectory $file) -Force -ErrorAction SilentlyContinue
    }
    foreach ($file in $payloadFiles) {
        Remove-Item -LiteralPath (Join-Path $payload $file) -Force -ErrorAction SilentlyContinue
    }
    foreach ($directory in @('bin\64bit', 'bin', 'data\locale', 'data', '')) {
        $target = if ($directory) { Join-Path $payload $directory } else { $payload }
        if ((Test-Path -LiteralPath $target -PathType Container) -and
            -not @(Get-ChildItem -LiteralPath $target -Force).Count) {
            Remove-Item -LiteralPath $target -ErrorAction SilentlyContinue
        }
    }
    $payloadParent = Join-Path $fullInstallDirectory 'obs-plugin'
    if ((Test-Path -LiteralPath $payloadParent -PathType Container) -and
        -not @(Get-ChildItem -LiteralPath $payloadParent -Force).Count) {
        Remove-Item -LiteralPath $payloadParent -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $fullInstallDirectory) {
        if (-not @(Get-ChildItem -LiteralPath $fullInstallDirectory -Force).Count) {
            Remove-Item -LiteralPath $fullInstallDirectory -ErrorAction SilentlyContinue
        } else {
            Write-Warning "'$fullInstallDirectory' contains unknown files and was left in place."
        }
    }
    Write-Host 'SoundFork was removed. FiiO and other ASIO drivers were not modified.'
}
