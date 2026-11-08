$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\scripts\Invoke-ReleaseTestProcess.ps1')
$shell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('SoundForkProcessTest-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$pidFile = Join-Path $testRoot 'owned-processes.txt'
try {
    # Inno may choose a nonzero suffix after an earlier uninstall. Resolve each
    # installation independently and reject missing or ambiguous payloads.
    $installFixture = Join-Path $testRoot 'install'
    New-Item -ItemType Directory -Path $installFixture | Out-Null
    foreach ($name in @('unins000.exe', 'unins001.exe')) {
        $fixture = Join-Path $installFixture $name
        New-Item -ItemType File -Path $fixture | Out-Null
        if ((Get-ReleaseTestUninstaller $installFixture) -cne $fixture) {
            throw "Failed to resolve $name"
        }
        Remove-Item -LiteralPath $fixture
    }
    foreach ($names in @(@(), @('unins000.exe', 'unins001.exe'))) {
        foreach ($name in $names) { New-Item -ItemType File -Path (Join-Path $installFixture $name) | Out-Null }
        $rejected = $false
        try { Get-ReleaseTestUninstaller $installFixture | Out-Null }
        catch { if ($_.Exception.Message -notlike 'Expected one uninstaller*') { throw }; $rejected = $true }
        if (-not $rejected) { throw 'Missing or ambiguous uninstaller was accepted.' }
    }
    foreach ($expected in @(0, 7)) {
        $actual = Invoke-ReleaseTestProcess -StepName "Fixture exit $expected" -FilePath $shell `
            -ArgumentList @('-NoLogo', '-NoProfile', '-NonInteractive', '-Command', "exit $expected") `
            -TimeoutSeconds 10
        if ($actual -ne $expected) { throw "Expected exit $expected, received '$actual'." }
    }
    $childCode = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes('Start-Sleep -Seconds 60'))
    $parentCode = @'
$child = Start-Process -FilePath '__SHELL__' -ArgumentList '-NoLogo -NoProfile -NonInteractive -EncodedCommand __CHILD__' -PassThru -WindowStyle Hidden
@($PID, $child.Id) | Set-Content -LiteralPath '__PIDFILE__'
Start-Sleep -Seconds 60
'@
    $parentCode = $parentCode.Replace('__SHELL__', $shell.Replace("'", "''")).Replace('__CHILD__', $childCode).Replace('__PIDFILE__', $pidFile.Replace("'", "''"))
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($parentCode))
    $timedOut = $false
    $timer = [Diagnostics.Stopwatch]::StartNew()
    try {
        Invoke-ReleaseTestProcess -StepName 'Fixture process-tree timeout' -FilePath $shell `
            -ArgumentList @('-NoLogo', '-NoProfile', '-NonInteractive', '-EncodedCommand', $encoded) `
            -TimeoutSeconds 3 | Out-Null
    } catch {
        if ($_.Exception.Message -notmatch 'timed out after 3s') { throw }
        $timedOut = $true
    }
    if (-not $timedOut -or $timer.Elapsed.TotalSeconds -gt 20) { throw 'Process timeout did not fail promptly.' }
    if (-not (Test-Path -LiteralPath $pidFile)) { throw 'Timeout fixture did not start its child.' }
    foreach ($ownedId in Get-Content -LiteralPath $pidFile) {
        $remaining = Get-Process -Id ([int]$ownedId) -ErrorAction SilentlyContinue
        if ($null -ne $remaining) { throw "Timed-out fixture process $ownedId is still running." }
    }
    $workflow = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\.github\workflows\release.yml') -Raw
    if ($workflow -notmatch [regex]::Escape('$releaseBranch = if ($tagSuffix) { "v$tagBase-dev" } else { ''main'' }') -or
        $workflow -notmatch 'refs/heads/\$releaseBranch' -or
        $workflow -notmatch 'origin/\$releaseBranch') {
        throw 'Release workflow must require stable tags on main and prerelease tags on the matching development branch.'
    }
    Write-Output 'Release process exit-code and parent/child timeout tests passed; no installer or registry changes.'
} finally {
    # Only fixture PIDs recorded in this newly created private directory.
    if (Test-Path -LiteralPath $pidFile) {
        foreach ($ownedId in Get-Content -LiteralPath $pidFile) {
            Stop-Process -Id ([int]$ownedId) -Force -ErrorAction SilentlyContinue
        }
    }
    Remove-Item -LiteralPath $testRoot -Recurse -Force
}
