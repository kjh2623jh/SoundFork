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
$windowsOutput = [string](Read-RegistryValue64 -Path 'SOFTWARE\SoundFork\Output' -Name 'WindowsEndpoint')
if ($windowsOutput) { $backendName = 'Windows playback device'; $backendClsid = $windowsOutput }

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
$relayMode = 'entire-screen'
$userSettings = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\SoundFork', $false)
if ($null -ne $userSettings) {
    try {
        $savedMode = [string]$userSettings.GetValue('RelayMode', 'entire-screen')
        if ($savedMode -eq 'application') { $relayMode = 'application' }
    } finally {
        $userSettings.Dispose()
    }
}
$relayMutex = $null
$relayActive = $false
try {
    $relayMutex = [Threading.Mutex]::OpenExisting('Local\SoundFork.DiscordRelay.Singleton.v1')
    $relayActive = $true
} catch [Threading.WaitHandleCannotBeOpenedException] {
    $relayActive = $false
} finally {
    if ($null -ne $relayMutex) { $relayMutex.Dispose() }
}

$state = if ($asioRegistered -and $comRegistered -and $driverExists -and $backendName) {
    'Ready'
} elseif (-not $asioRegistered -and -not $comRegistered) {
    'Not registered'
} else {
    'Incomplete'
}

$x86Base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
    [Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry32)
$x86Backend = ''; $x86Dll = ''
try {
    $key32 = $x86Base.OpenSubKey('SOFTWARE\ASIO\SoundFork')
    if ($null -ne $key32) { try { $x86Backend = [string]$key32.GetValue('BackendCLSID', '') } finally { $key32.Dispose() } }
    $com32 = $x86Base.OpenSubKey($comPath)
    if ($null -ne $com32) { try { $x86Dll = [string]$com32.GetValue('', '') } finally { $com32.Dispose() } }
} finally { $x86Base.Dispose() }

[PSCustomObject]@{
    State = $state
    PlaybackMode = if ($windowsOutput) { 'Windows shared-mode WASAPI' } else { 'Vendor ASIO' }
    InstallType = if ($isInstalledCopy) { 'Installed copy' } elseif ($driverExists) { 'Development/build path' } else { 'None' }
    DriverDll = if ($driverDll) { $driverDll } else { '(not registered)' }
    DriverDllExists = [bool]$driverExists
    X86DriverDll = if ($x86Dll) { $x86Dll } else { '(not registered)' }
    X86BackendCLSID = if ($windowsOutput) { '(Windows output; vendor backend not required)' } elseif ($x86Backend) { $x86Backend } else { '(not configured; install matching x86 vendor driver)' }
    Backend = if ($backendName) { $backendName } elseif ($backendClsid) { "Unknown ($backendClsid)" } else { '(not configured)' }
    ControlApp = if ($controlProcesses.Count) { "Running (PID $($controlProcesses.Id -join ', '))" } else { 'Stopped' }
    RelayMode = $relayMode
    DiscordRelay = if ($relayProcesses.Count) {
        "Running externally (PID $($relayProcesses.Id -join ', '))"
    } elseif ($relayActive) {
        'Running inside the ASIO host process'
    } else {
        'Stopped'
    }
    CaptureHub = if ($hubProcesses.Count) { "Running (PID $($hubProcesses.Id -join ', '))" } else { 'Stopped' }
    CaptureMonitor = if ($monitorProcesses.Count) { "Running (PID $($monitorProcesses.Id -join ', '))" } else { 'Stopped' }
}
