[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SetupPath,

    [Parameter(Mandatory = $true)]
    [string]$TestDirectory,

    [string]$LogDirectory,

    [switch]$AllowRegistryChanges
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Invoke-ReleaseTestProcess.ps1')
if (-not $AllowRegistryChanges) {
    throw 'This test temporarily changes HKLM and HKCU startup registration. Pass -AllowRegistryChanges only on an isolated test machine.'
}
if (-not [Environment]::Is64BitProcess) {
    throw 'Run the installer smoke test from 64-bit PowerShell.'
}
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'The installer smoke test requires an elevated or CI administrator session.'
}

$resolvedSetup = (Resolve-Path -LiteralPath $SetupPath).Path
$fullTestDirectory = [IO.Path]::GetFullPath($TestDirectory)
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
if (-not $fullTestDirectory.StartsWith(
        $tempRoot,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "TestDirectory must be inside the system temporary directory '$tempRoot'."
}
if ([string]::IsNullOrWhiteSpace($LogDirectory)) {
    $LogDirectory = $fullTestDirectory + '-logs'
}
$LogDirectory = [IO.Path]::GetFullPath($LogDirectory)
if ($LogDirectory.Equals($fullTestDirectory, [StringComparison]::OrdinalIgnoreCase) -or
    $LogDirectory.StartsWith($fullTestDirectory + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'LogDirectory must be outside the installation directory so uninstall cannot delete the logs.'
}
New-Item -ItemType Directory -Path $LogDirectory -Force | Out-Null
Write-Host "Installer test logs: $LogDirectory"

$fakeName = 'SoundFork CI Fake ASIO'
$fakeClsid = '{7C8656E5-9B2D-44E9-A43A-D767CF8B92F2}'
$preferredName = 'FiiO ASIO Driver'
$preferredClsid = '{916B700B-936B-4736-A8F8-57479A4394F8}'
$soundForkClsid = '{A04A0128-E43C-4D08-B701-87EC1A847A89}'
$baseKey = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
    [Microsoft.Win32.RegistryHive]::LocalMachine,
    [Microsoft.Win32.RegistryView]::Registry64)
$baseKey32 = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
    [Microsoft.Win32.RegistryHive]::LocalMachine,
    [Microsoft.Win32.RegistryView]::Registry32)
$fake32Created = $false
$soundFork32MayHaveBeenCreated = $false
$fakeKeysCreated = $false
$soundForkMayHaveBeenCreated = $false
$userRun = $null
$startupCommand = $null
$startupSeeded = $false
$testDirectoryMayHaveBeenCreated = $false
$windowsOutputMayHaveBeenCreated = $false

try {
    $existingOutput = $baseKey.OpenSubKey('SOFTWARE\SoundFork\Output')
    if ($null -ne $existingOutput) {
        try { if ($null -ne $existingOutput.GetValue('WindowsEndpoint', $null)) { throw 'Refusing to overwrite an existing Windows playback selection.' } }
        finally { $existingOutput.Dispose() }
    }
    foreach ($path in @("SOFTWARE\ASIO\$fakeName", "SOFTWARE\Classes\CLSID\$fakeClsid",
            'SOFTWARE\ASIO\SoundFork', "SOFTWARE\Classes\CLSID\$soundForkClsid")) {
        $existing32 = $baseKey32.OpenSubKey($path)
        if ($null -ne $existing32) { $existing32.Dispose(); throw "Refusing to overwrite existing x86 key: $path" }
    }
    $userRun = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Software\Microsoft\Windows\CurrentVersion\Run', $true)
    if ($null -ne $userRun.GetValue('SoundFork', $null)) {
        throw 'The installer test refuses to overwrite an existing user SoundFork startup entry.'
    }
    foreach ($keyPath in @(
            "SOFTWARE\ASIO\$fakeName",
            "SOFTWARE\Classes\CLSID\$fakeClsid",
            "SOFTWARE\ASIO\$preferredName",
            "SOFTWARE\Classes\CLSID\$preferredClsid",
            'SOFTWARE\ASIO\SoundFork',
            "SOFTWARE\Classes\CLSID\$soundForkClsid")) {
        $existingKey = $baseKey.OpenSubKey($keyPath, $false)
        if ($null -ne $existingKey) {
            $existingKey.Dispose()
            throw "The installer test refuses to overwrite an existing registry key: HKLM:\$keyPath"
        }
    }
    if (Test-Path -LiteralPath $fullTestDirectory) {
        throw "Test directory already exists: $fullTestDirectory"
    }
    $testDirectoryMayHaveBeenCreated = $true

    $fakeKeysCreated = $true
    $fake32Created = $true
    $fake32 = $baseKey32.CreateSubKey("SOFTWARE\ASIO\$fakeName", $true)
    try { $fake32.SetValue('CLSID', $fakeClsid) } finally { $fake32.Dispose() }
    $com32 = $baseKey32.CreateSubKey("SOFTWARE\Classes\CLSID\$fakeClsid\InprocServer32", $true)
    try { $com32.SetValue('', "$env:SystemRoot\SysWOW64\kernel32.dll") } finally { $com32.Dispose() }
    $fakeAsio = $baseKey.CreateSubKey("SOFTWARE\ASIO\$fakeName", $true)
    try { $fakeAsio.SetValue('CLSID', $fakeClsid, [Microsoft.Win32.RegistryValueKind]::String) } finally { $fakeAsio.Dispose() }
    $fakeCom = $baseKey.CreateSubKey("SOFTWARE\Classes\CLSID\$fakeClsid\InprocServer32", $true)
    try { $fakeCom.SetValue('', "$env:SystemRoot\System32\kernel32.dll", [Microsoft.Win32.RegistryValueKind]::String) } finally { $fakeCom.Dispose() }
    $preferredAsio = $baseKey.CreateSubKey("SOFTWARE\ASIO\$preferredName", $true)
    try { $preferredAsio.SetValue('CLSID', $preferredClsid, [Microsoft.Win32.RegistryValueKind]::String) } finally { $preferredAsio.Dispose() }
    $preferredCom = $baseKey.CreateSubKey("SOFTWARE\Classes\CLSID\$preferredClsid\InprocServer32", $true)
    try { $preferredCom.SetValue('', "$env:SystemRoot\System32\kernel32.dll", [Microsoft.Win32.RegistryValueKind]::String) } finally { $preferredCom.Dispose() }
    # A fresh unattended install must never guess a driver, even with FiiO present.
    $soundForkMayHaveBeenCreated = $true
    $soundFork32MayHaveBeenCreated = $true
    $rejectedLog = Join-Path $LogDirectory '01-rejected-missing-backend.log'
    $unselectedExit = Invoke-ReleaseTestProcess -StepName 'Reject missing ASIO selection' `
        -FilePath $resolvedSetup -LogPath $rejectedLog -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-',
        "/LOG=`"$rejectedLog`"", "/DIR=`"$fullTestDirectory`""
    )
    if ($unselectedExit -ne 1) { throw "Expected setup initialization rejection (exit 1), received $unselectedExit." }
    $unexpectedAsio = $baseKey.OpenSubKey('SOFTWARE\ASIO\SoundFork', $false)
    if ($null -ne $unexpectedAsio) {
        $unexpectedAsio.Dispose()
        throw 'Setup registered an ASIO driver without an explicit selection.'
    }
    if (Test-Path -LiteralPath $fullTestDirectory) { throw 'Rejected setup created the installation directory.' }
    $installArguments = @(
        '/VERYSILENT',
        '/SUPPRESSMSGBOXES',
        '/NORESTART',
        '/SP-',
        "/LOG=`"$(Join-Path $LogDirectory '02-install.log')`"",
        "/DIR=`"$fullTestDirectory`"",
        "/BACKENDCLSID=`"$fakeClsid`""
    )
    $soundForkMayHaveBeenCreated = $true
    $installExit = Invoke-ReleaseTestProcess -StepName 'Install with explicit ASIO selection' `
        -FilePath $resolvedSetup -ArgumentList $installArguments `
        -LogPath (Join-Path $LogDirectory '02-install.log')
    if ($installExit -ne 0) {
        throw "Setup failed with exit code $installExit."
    }

    foreach ($file in @(
        'x86\soundfork_driver.dll', 'x86\soundfork_app_relay.dll',
        'x86\soundfork_register.exe', 'x86\soundfork_host.exe',
        'soundfork_driver.dll', 'soundfork_app_relay.dll', 'soundfork_control.exe',
        'soundfork_discord_relay.exe', 'soundfork_capture_hub.exe', 'unins000.exe',
        'README.md', 'README.en.md', 'LICENSE',
        'THIRD_PARTY_NOTICES.md', 'THIRD_PARTY_NOTICES.en.md',
        'docs\DETAILS.md', 'docs\DETAILS.en.md',
        'docs\DEVELOPMENT.md', 'docs\DEVELOPMENT.en.md'
    )) {
        if (-not (Test-Path -LiteralPath (Join-Path $fullTestDirectory $file) -PathType Leaf)) {
            throw "Installed file is missing: $file"
        }
    }

    $soundForkAsio = $baseKey.OpenSubKey('SOFTWARE\ASIO\SoundFork', $false)
    if ($null -eq $soundForkAsio) {
        throw 'SoundFork ASIO registry key was not created.'
    }
    try {
        if ([string]$soundForkAsio.GetValue('BackendCLSID') -ne $fakeClsid) {
            throw 'Installed BackendCLSID does not match the selected fake backend.'
        }
    } finally {
        $soundForkAsio.Dispose()
    }
    $registeredCom = $baseKey.OpenSubKey(
        "SOFTWARE\Classes\CLSID\$soundForkClsid\InprocServer32",
        $false)
    if ($null -eq $registeredCom) {
        throw 'SoundFork COM registration was not created.'
    }
    try {
        $expectedDriver = Join-Path $fullTestDirectory 'soundfork_driver.dll'
        $registeredDriver = [IO.Path]::GetFullPath([string]$registeredCom.GetValue(''))
        if (-not $registeredDriver.Equals(
                $expectedDriver,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "SoundFork COM registration points to an unexpected DLL: $registeredDriver"
        }
    } finally {
        $registeredCom.Dispose()
    }

    $control = Join-Path $fullTestDirectory 'soundfork_control.exe'
    foreach ($phase in @('install')) {
        $asio32 = $baseKey32.OpenSubKey('SOFTWARE\ASIO\SoundFork')
        if ($null -eq $asio32) { throw "x86 ASIO registration missing after $phase" }
        try { if ([string]$asio32.GetValue('BackendCLSID') -ne $fakeClsid) { throw 'x86 backend selection mismatch' } }
        finally { $asio32.Dispose() }
        $registered32 = $baseKey32.OpenSubKey("SOFTWARE\Classes\CLSID\$soundForkClsid\InprocServer32")
        if ($null -eq $registered32) { throw 'x86 COM registration missing' }
        try {
            if ([IO.Path]::GetFullPath([string]$registered32.GetValue('')) -ine
                (Join-Path $fullTestDirectory 'x86\soundfork_driver.dll')) { throw 'x86 COM DLL path mismatch' }
        } finally { $registered32.Dispose() }
    }
    # Payload must be available for later Control-panel installation even when
    # the optional OBS task was not selected. Never touch the runner's OBS path.
    $pluginPayload = Join-Path $fullTestDirectory 'obs-plugin\soundfork-obs\bin\64bit\soundfork-obs.dll'
    if (-not (Test-Path -LiteralPath $pluginPayload -PathType Leaf)) {
        throw 'The private OBS payload was not installed for Control-panel management.'
    }
    $selfTestExit = Invoke-ReleaseTestProcess -StepName 'Installed control self-test' `
        -FilePath $control -ArgumentList '--self-test' -TimeoutSeconds 30
    if ($selfTestExit -ne 0) {
        throw "Installed control app self-test failed with exit code $selfTestExit."
    }
    if ($null -ne $userRun.GetValue('SoundFork', $null)) {
        throw 'A fresh install enabled user startup without opt-in.'
    }
    # Simulate the user's opt-in without launching a GUI or audio process.
    $startupCommand = '"' + $control + '" --startup'
    $userRun.SetValue('SoundFork', $startupCommand, [Microsoft.Win32.RegistryValueKind]::String)
    $startupSeeded = $true

    # Reinstall without /BACKENDCLSID. This simulates an unattended in-place
    # upgrade and proves that the existing non-FiiO backend remains selected
    # even when a driver named "FiiO ASIO Driver" is also present.
    $upgradeArguments = @(
        '/VERYSILENT',
        '/SUPPRESSMSGBOXES',
        '/NORESTART',
        '/SP-',
        "/LOG=`"$(Join-Path $LogDirectory '03-upgrade.log')`"",
        "/DIR=`"$fullTestDirectory`""
    )
    $upgradeExit = Invoke-ReleaseTestProcess -StepName 'Upgrade preserving existing settings' `
        -FilePath $resolvedSetup -ArgumentList $upgradeArguments `
        -LogPath (Join-Path $LogDirectory '03-upgrade.log')
    if ($upgradeExit -ne 0) {
        throw "In-place upgrade failed with exit code $upgradeExit."
    }
    if ([string]$userRun.GetValue('SoundFork', '') -cne $startupCommand) {
        throw 'In-place upgrade did not preserve the user startup registration.'
    }
    $upgradedAsio = $baseKey.OpenSubKey('SOFTWARE\ASIO\SoundFork', $false)
    if ($null -eq $upgradedAsio) {
        throw 'SoundFork ASIO registry key disappeared during upgrade.'
    }
    try {
        if ([string]$upgradedAsio.GetValue('BackendCLSID') -ne $fakeClsid) {
            throw 'In-place upgrade did not preserve the existing ASIO backend.'
        }
    } finally {
        $upgradedAsio.Dispose()
    }
    $uninstallers = @(Get-ChildItem -LiteralPath $fullTestDirectory -Filter 'unins*.exe' -File)
    if ($uninstallers.Count -ne 1) {
        throw "In-place upgrade produced $($uninstallers.Count) uninstallers instead of one."
    }

    $uninstaller = Join-Path $fullTestDirectory 'unins000.exe'
    $uninstallLog = Join-Path $LogDirectory '04-uninstall.log'
    $uninstallExit = Invoke-ReleaseTestProcess -StepName 'Uninstall' `
        -FilePath $uninstaller -LogPath $uninstallLog -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=`"$uninstallLog`""
    )
    if ($uninstallExit -ne 0) {
        throw "Uninstaller failed with exit code $uninstallExit."
    }
    if ($null -ne $userRun.GetValue('SoundFork', $null)) {
        throw 'Uninstall left the removing account startup registration behind.'
    }
    $remainingSoundFork = $baseKey.OpenSubKey('SOFTWARE\ASIO\SoundFork', $false)
    foreach ($path in @('SOFTWARE\ASIO\SoundFork', "SOFTWARE\Classes\CLSID\$soundForkClsid")) {
        $remaining32 = $baseKey32.OpenSubKey($path)
        if ($null -ne $remaining32) { $remaining32.Dispose(); throw "x86 registration remains after uninstall: $path" }
    }
    $remainingBackend32 = $baseKey32.OpenSubKey("SOFTWARE\ASIO\$fakeName")
    if ($null -eq $remainingBackend32) { throw 'Uninstaller removed the x86 vendor driver' }
    $remainingBackend32.Dispose()
    if ($null -ne $remainingSoundFork) {
        $remainingSoundFork.Dispose()
        throw 'SoundFork ASIO registry key remains after uninstall.'
    }
    $remainingCom = $baseKey.OpenSubKey("SOFTWARE\Classes\CLSID\$soundForkClsid", $false)
    if ($null -ne $remainingCom) {
        $remainingCom.Dispose()
        throw 'SoundFork COM registry key remains after uninstall.'
    }
    $remainingFake = $baseKey.OpenSubKey("SOFTWARE\ASIO\$fakeName", $false)
    if ($null -eq $remainingFake) {
        throw 'Uninstall incorrectly removed the selected backend driver key.'
    }
    $remainingFake.Dispose()
    # Exercise explicit Windows playback with no dependency on a real ASIO DLL
    # or an active endpoint on this headless runner. Setup may leave device selection
    # pending; it must still register both proxy architectures.
    $windowsOutputMayHaveBeenCreated = $true
    $windowsExit = Invoke-ReleaseTestProcess -StepName 'Install Windows playback mode' `
        -FilePath $resolvedSetup -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-',
            "/DIR=`"$fullTestDirectory`"", '/BACKENDCLSID=windows')
    if ($windowsExit -ne 0) { throw "Windows playback setup failed: $windowsExit" }
    $windowsKey = $baseKey.OpenSubKey('SOFTWARE\SoundFork\Output')
    if ($null -eq $windowsKey) { throw 'Windows playback selection missing' }
    try { $savedWindowsEndpoint = [string]$windowsKey.GetValue('WindowsEndpoint', '') }
    finally { $windowsKey.Dispose() }
    if (-not $savedWindowsEndpoint) { throw 'Windows mode was not saved' }
    foreach ($view in @($baseKey, $baseKey32)) {
        $key = $view.OpenSubKey('SOFTWARE\ASIO\SoundFork')
        if ($null -eq $key) { throw 'Windows mode did not register both proxy architectures' }
        $key.Dispose()
    }
    $windowsUpgrade = Invoke-ReleaseTestProcess -StepName 'Upgrade preserving Windows playback mode' `
        -FilePath $resolvedSetup -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-',
            "/DIR=`"$fullTestDirectory`"")
    if ($windowsUpgrade -ne 0) { throw 'Windows playback upgrade failed' }
    $windowsKey = $baseKey.OpenSubKey('SOFTWARE\SoundFork\Output')
    try { if ([string]$windowsKey.GetValue('WindowsEndpoint', '') -cne $savedWindowsEndpoint) { throw 'Upgrade changed the selected Windows endpoint' } }
    finally { $windowsKey.Dispose() }
    $windowsRemove = Invoke-ReleaseTestProcess -StepName 'Uninstall Windows playback mode' `
        -FilePath $uninstaller -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')
    if ($windowsRemove -ne 0) { throw 'Windows playback removal failed' }
    foreach ($view in @($baseKey, $baseKey32)) {
        $key = $view.OpenSubKey('SOFTWARE\ASIO\SoundFork')
        if ($null -ne $key) { $key.Dispose(); throw 'Windows mode left ASIO registration behind' }
    }
    Write-Host 'SoundFork installer smoke test passed.'
} finally {
    if ($windowsOutputMayHaveBeenCreated) {
        $outputKey = $baseKey.OpenSubKey('SOFTWARE\SoundFork\Output', $true)
        if ($null -ne $outputKey) { try { $outputKey.DeleteValue('WindowsEndpoint', $false) } finally { $outputKey.Dispose() } }
    }
    if ($fake32Created) {
        $baseKey32.DeleteSubKeyTree("SOFTWARE\ASIO\$fakeName", $false)
        $baseKey32.DeleteSubKeyTree("SOFTWARE\Classes\CLSID\$fakeClsid", $false)
    }
    if ($soundFork32MayHaveBeenCreated) {
        $baseKey32.DeleteSubKeyTree('SOFTWARE\ASIO\SoundFork', $false)
        $baseKey32.DeleteSubKeyTree("SOFTWARE\Classes\CLSID\$soundForkClsid", $false)
    }
    $baseKey32.Dispose()
    if ($null -ne $userRun) {
        if ($startupSeeded -and [string]$userRun.GetValue('SoundFork', '') -ceq $startupCommand) {
            $userRun.DeleteValue('SoundFork', $false)
        }
        $userRun.Dispose()
    }
    if ($fakeKeysCreated) {
        $baseKey.DeleteSubKeyTree("SOFTWARE\ASIO\$fakeName", $false)
        $baseKey.DeleteSubKeyTree("SOFTWARE\Classes\CLSID\$fakeClsid", $false)
        $baseKey.DeleteSubKeyTree("SOFTWARE\ASIO\$preferredName", $false)
        $baseKey.DeleteSubKeyTree("SOFTWARE\Classes\CLSID\$preferredClsid", $false)
    }
    if ($soundForkMayHaveBeenCreated) {
        $baseKey.DeleteSubKeyTree("SOFTWARE\Classes\CLSID\$soundForkClsid", $false)
        $baseKey.DeleteSubKeyTree('SOFTWARE\ASIO\SoundFork', $false)
    }
    $baseKey.Dispose()
    if ($testDirectoryMayHaveBeenCreated -and (Test-Path -LiteralPath $fullTestDirectory)) {
        Remove-Item -LiteralPath $fullTestDirectory -Recurse -Force -ErrorAction SilentlyContinue
    }
}
