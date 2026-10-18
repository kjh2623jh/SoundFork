[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SetupPath,

    [Parameter(Mandatory = $true)]
    [string]$TestDirectory,

    [switch]$AllowRegistryChanges
)

$ErrorActionPreference = 'Stop'
if (-not $AllowRegistryChanges) {
    throw 'This test temporarily changes HKLM. Pass -AllowRegistryChanges only on an isolated test machine.'
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

$fakeName = 'SoundFork CI Fake ASIO'
$fakeClsid = '{7C8656E5-9B2D-44E9-A43A-D767CF8B92F2}'
$soundForkClsid = '{A04A0128-E43C-4D08-B701-87EC1A847A89}'
$baseKey = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
    [Microsoft.Win32.RegistryHive]::LocalMachine,
    [Microsoft.Win32.RegistryView]::Registry64)
$fakeKeysCreated = $false
$soundForkMayHaveBeenCreated = $false

try {
    foreach ($keyPath in @(
            "SOFTWARE\ASIO\$fakeName",
            "SOFTWARE\Classes\CLSID\$fakeClsid",
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

    $fakeKeysCreated = $true
    $fakeAsio = $baseKey.CreateSubKey("SOFTWARE\ASIO\$fakeName", $true)
    try { $fakeAsio.SetValue('CLSID', $fakeClsid, [Microsoft.Win32.RegistryValueKind]::String) } finally { $fakeAsio.Dispose() }
    $fakeCom = $baseKey.CreateSubKey("SOFTWARE\Classes\CLSID\$fakeClsid\InprocServer32", $true)
    try { $fakeCom.SetValue('', "$env:SystemRoot\System32\kernel32.dll", [Microsoft.Win32.RegistryValueKind]::String) } finally { $fakeCom.Dispose() }
    $installArguments = @(
        '/VERYSILENT',
        '/SUPPRESSMSGBOXES',
        '/NORESTART',
        "/DIR=`"$fullTestDirectory`"",
        "/BACKENDCLSID=`"$fakeClsid`""
    )
    $soundForkMayHaveBeenCreated = $true
    $install = Start-Process -FilePath $resolvedSetup -ArgumentList $installArguments -Wait -PassThru -WindowStyle Hidden
    if ($install.ExitCode -ne 0) {
        throw "Setup failed with exit code $($install.ExitCode)."
    }

    foreach ($file in @('soundfork_driver.dll', 'soundfork_control.exe', 'soundfork_discord_relay.exe', 'unins000.exe')) {
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
    $selfTest = Start-Process -FilePath $control -ArgumentList '--self-test' -Wait -PassThru -WindowStyle Hidden
    if ($selfTest.ExitCode -ne 0) {
        throw "Installed control app self-test failed with exit code $($selfTest.ExitCode)."
    }

    $uninstaller = Join-Path $fullTestDirectory 'unins000.exe'
    $uninstall = Start-Process -FilePath $uninstaller -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART') -Wait -PassThru -WindowStyle Hidden
    if ($uninstall.ExitCode -ne 0) {
        throw "Uninstaller failed with exit code $($uninstall.ExitCode)."
    }
    $remainingSoundFork = $baseKey.OpenSubKey('SOFTWARE\ASIO\SoundFork', $false)
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
    Write-Host 'SoundFork installer smoke test passed.'
} finally {
    if ($fakeKeysCreated) {
        $baseKey.DeleteSubKeyTree("SOFTWARE\ASIO\$fakeName", $false)
        $baseKey.DeleteSubKeyTree("SOFTWARE\Classes\CLSID\$fakeClsid", $false)
    }
    if ($soundForkMayHaveBeenCreated) {
        $baseKey.DeleteSubKeyTree("SOFTWARE\Classes\CLSID\$soundForkClsid", $false)
        $baseKey.DeleteSubKeyTree('SOFTWARE\ASIO\SoundFork', $false)
    }
    $baseKey.Dispose()
    if (Test-Path -LiteralPath $fullTestDirectory) {
        Remove-Item -LiteralPath $fullTestDirectory -Recurse -Force -ErrorAction SilentlyContinue
    }
}
