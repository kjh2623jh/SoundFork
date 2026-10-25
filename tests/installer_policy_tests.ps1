$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
foreach ($name in @('Install-SoundFork.ps1', 'Register-SoundFork.ps1')) {
    $tokens = $null
    $parseErrors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $root "scripts\$name"), [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count) { throw "Cannot parse $name" }
    $parameter = @($ast.ParamBlock.Parameters | Where-Object { $_.Name.VariablePath.UserPath -eq 'BackendName' })
    if ($parameter.Count -ne 1 -or $null -ne $parameter[0].DefaultValue) {
        throw "$name must not default to a driver."
    }
    $mandatory = @($parameter[0].Attributes | Where-Object { $_.TypeName.Name -eq 'Parameter' } |
        ForEach-Object { $_.NamedArguments } | Where-Object {
            $_.ArgumentName -eq 'Mandatory' -and $_.Argument.SafeGetValue() -eq $true
        })
    if ($mandatory.Count -ne 1) { throw "$name must require explicit BackendName." }
}
$installer = Get-Content -LiteralPath (Join-Path $root 'installer\SoundFork.iss') -Raw -Encoding UTF8
if ($installer -match 'FiiO' -or $installer -match 'DefaultBackendIndex\s*:=\s*0\s*;') {
    throw 'Installer contains vendor preference or a first-driver fallback.'
}
if ($installer -notmatch 'DefaultBackendIndex := FindBackendByClsid\(ExistingBackendClsid\)' -or
    $installer -notmatch 'function NextButtonClick' -or
    $installer -notmatch 'BackendPage.SelectedValueIndex < 0') {
    throw 'Installer must preserve valid existing selection and guard missing selection.'
}
if ($installer -match '(?m)^\s*MsgBox\(') {
    throw 'Installer must not use unsuppressible MsgBox calls that can block unattended tests.'
}
foreach ($message in @('NoBackend', 'InvalidSilentBackend', 'CloseObsBeforeUninstall', 'SelectBackend')) {
    if (-not $installer.Contains("SuppressibleMsgBox(ExpandConstant('{cm:$message}'),")) {
        throw "Installer must use a suppressible message for $message."
    }
}
$smoke = Get-Content -LiteralPath (Join-Path $root 'scripts\Test-ReleaseInstaller.ps1') -Raw
if ($smoke -match 'Start-Process' -or $smoke -notmatch 'Invoke-ReleaseTestProcess' -or
    $smoke -notmatch 'rejectedLog' -or $smoke -notmatch 'uninstallLog') {
    throw 'Installer smoke tests must use the bounded process helper and retain setup/uninstall logs.'
}
Write-Output 'Read-only installer selection policy checks passed; no installer or registry operations executed.'
