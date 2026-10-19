[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$soundForkName = 'SoundFork'
$soundForkClsid = '{A04A0128-E43C-4D08-B701-87EC1A847A89}'
$installedDirectory = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'SoundFork'

function Open-Registry64ReadOnly {
    param([string]$Path)

    $baseKey = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
        [Microsoft.Win32.RegistryHive]::LocalMachine,
        [Microsoft.Win32.RegistryView]::Registry64)
    try {
        return $baseKey.OpenSubKey($Path, $false)
    } finally {
        $baseKey.Dispose()
    }
}

function Read-RegistryValue64 {
    param(
        [string]$Path,
        [AllowEmptyString()]
        [string]$Name
    )

    $key = Open-Registry64ReadOnly -Path $Path
    if ($null -eq $key) {
        return $null
    }
    try {
        return $key.GetValue($Name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    } finally {
        $key.Dispose()
    }
}

function Find-AsioNameByClsid {
    param([string]$Clsid)

    $baseKey = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
        [Microsoft.Win32.RegistryHive]::LocalMachine,
        [Microsoft.Win32.RegistryView]::Registry64)
    try {
        $asioRoot = $baseKey.OpenSubKey('SOFTWARE\ASIO', $false)
        if ($null -eq $asioRoot) {
            return $null
        }
        try {
            foreach ($name in $asioRoot.GetSubKeyNames()) {
                $driverKey = $asioRoot.OpenSubKey($name, $false)
                if ($null -ne $driverKey) {
                    try {
                        $candidate = [string]$driverKey.GetValue('CLSID', '')
                        if ($candidate -and $candidate.Equals($Clsid, [StringComparison]::OrdinalIgnoreCase)) {
                            return $name
                        }
                    } finally {
                        $driverKey.Dispose()
                    }
                }
            }
        } finally {
            $asioRoot.Dispose()
        }
    } finally {
        $baseKey.Dispose()
    }
    return $null
}

$asioPath = "SOFTWARE\ASIO\$soundForkName"
$registeredClsid = [string](Read-RegistryValue64 -Path $asioPath -Name 'CLSID')
$backendClsid = [string](Read-RegistryValue64 -Path $asioPath -Name 'BackendCLSID')
$comPath = "SOFTWARE\Classes\CLSID\$soundForkClsid\InprocServer32"
$driverDll = [string](Read-RegistryValue64 -Path $comPath -Name '')
$backendName = if ($backendClsid) { Find-AsioNameByClsid -Clsid $backendClsid } else { $null }

$asioRegistered = $registeredClsid -and $registeredClsid.Equals(
    $soundForkClsid,
    [StringComparison]::OrdinalIgnoreCase)
$comRegistered = -not [string]::IsNullOrWhiteSpace($driverDll)
$driverExists = $comRegistered -and (Test-Path -LiteralPath $driverDll -PathType Leaf)
$isInstalledCopy = $driverExists -and ([IO.Path]::GetFullPath($driverDll)).StartsWith(
    ([IO.Path]::GetFullPath($installedDirectory) + [IO.Path]::DirectorySeparatorChar),
    [StringComparison]::OrdinalIgnoreCase)

$relayProcesses = @(Get-Process -Name 'soundfork_discord_relay' -ErrorAction SilentlyContinue)
$monitorProcesses = @(Get-Process -Name 'soundfork_capture_monitor' -ErrorAction SilentlyContinue)
$controlProcesses = @(Get-Process -Name 'soundfork_control' -ErrorAction SilentlyContinue)
$hubProcesses = @(Get-Process -Name 'soundfork_capture_hub' -ErrorAction SilentlyContinue)

$state = if ($asioRegistered -and $comRegistered -and $driverExists -and $backendName) {
    'Ready'
} elseif (-not $asioRegistered -and -not $comRegistered) {
    'Not registered'
} else {
    'Incomplete'
}

[PSCustomObject]@{
    State = $state
    InstallType = if ($isInstalledCopy) { 'Installed copy' } elseif ($driverExists) { 'Development/build path' } else { 'None' }
    DriverDll = if ($driverDll) { $driverDll } else { '(not registered)' }
    DriverDllExists = [bool]$driverExists
    Backend = if ($backendName) { $backendName } elseif ($backendClsid) { "Unknown ($backendClsid)" } else { '(not configured)' }
    ControlApp = if ($controlProcesses.Count) { "Running (PID $($controlProcesses.Id -join ', '))" } else { 'Stopped' }
    DiscordRelay = if ($relayProcesses.Count) { "Running (PID $($relayProcesses.Id -join ', '))" } else { 'Stopped' }
    CaptureHub = if ($hubProcesses.Count) { "Running (PID $($hubProcesses.Id -join ', '))" } else { 'Stopped' }
    CaptureMonitor = if ($monitorProcesses.Count) { "Running (PID $($monitorProcesses.Id -join ', '))" } else { 'Stopped' }
}
