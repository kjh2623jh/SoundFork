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
Write-Output 'Read-only installer selection policy checks passed; no installer or registry operations executed.'
