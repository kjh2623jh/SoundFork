[CmdletBinding()]
param(
    [string]$Version,
    [string]$OutputDirectory,
    [switch]$SkipBuild,
    [switch]$SkipInstaller,
    [string]$CertificateThumbprint,
    [uri]$TimestampUrl
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$cmakeFile = Join-Path $projectRoot 'CMakeLists.txt'
$cmakeText = Get-Content -LiteralPath $cmakeFile -Raw
$versionMatch = [regex]::Match(
    $cmakeText,
    'project\s*\(\s*SoundFork\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)',
    [Text.RegularExpressions.RegexOptions]::IgnoreCase)
if (-not $versionMatch.Success) {
    throw 'Could not read the SoundFork version from CMakeLists.txt.'
}
$baseVersion = $versionMatch.Groups[1].Value
if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = $baseVersion
}
if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+(?:-(?:alpha|beta|rc)\.[0-9]+)?$') {
    throw "Version '$Version' is not a supported SoundFork semantic version."
}
$requestedBase = ($Version -split '-', 2)[0]
if ($requestedBase -ne $baseVersion) {
    throw "Package version '$Version' does not match CMake base version '$baseVersion'."
}
if ([string]::IsNullOrWhiteSpace($CertificateThumbprint) -ne ($null -eq $TimestampUrl)) {
    throw 'CertificateThumbprint and TimestampUrl must be supplied together.'
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $projectRoot 'artifacts'
}
$artifactRoot = [IO.Path]::GetFullPath($OutputDirectory)
if (-not $artifactRoot.StartsWith(
        $projectRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'OutputDirectory must be inside the SoundFork project directory.'
}

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'Build-SoundFork.ps1') `
        -Configuration Release -Version $Version -WithObsPlugin
    if ($LASTEXITCODE -ne 0) {
        throw 'Release build failed.'
    }
}

$cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
if ($null -eq $cmakeCommand) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ([string]::IsNullOrWhiteSpace($visualStudio)) {
        throw 'Visual Studio with the x64 C++ toolchain was not found.'
    }
    $cmake = Join-Path $visualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
} else {
    $cmake = $cmakeCommand.Source
}

$packageName = "SoundFork-v$Version-windows-x64"
$stagingRoot = Join-Path $artifactRoot 'staging'
$packageRoot = Join-Path $stagingRoot $packageName
foreach ($path in @($packageRoot)) {
    $fullPath = [IO.Path]::GetFullPath($path)
    if (-not $fullPath.StartsWith(
            $artifactRoot + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean unexpected package path '$fullPath'."
    }
    if (Test-Path -LiteralPath $fullPath) {
        Remove-Item -LiteralPath $fullPath -Recurse -Force
    }
}
New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null

& $cmake --install (Join-Path $projectRoot 'build\windows-msvc-x64') `
    --config Release --prefix $packageRoot
if ($LASTEXITCODE -ne 0) {
    throw 'CMake install staging failed.'
}

$signScript = Join-Path $PSScriptRoot 'Sign-SoundFork.ps1'
if (-not [string]::IsNullOrWhiteSpace($CertificateThumbprint)) {
    $binariesToSign = @(
        Get-ChildItem $packageRoot -File -Recurse |
            Where-Object { $_.Extension -in @('.exe', '.dll') } |
            Select-Object -ExpandProperty FullName
    )
    & $signScript -Path $binariesToSign `
        -CertificateThumbprint $CertificateThumbprint `
        -TimestampUrl $TimestampUrl
}

$zipPath = Join-Path $artifactRoot "$packageName.zip"
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
Compress-Archive -LiteralPath $packageRoot -DestinationPath $zipPath -CompressionLevel Optimal

$releaseFiles = [Collections.Generic.List[string]]::new()
$releaseFiles.Add($zipPath)
$setupPath = Join-Path $artifactRoot "SoundFork-Setup-v$Version.exe"
if (Test-Path -LiteralPath $setupPath) {
    Remove-Item -LiteralPath $setupPath -Force
}
if (-not $SkipInstaller) {
    $isccCandidates = @(
        (Get-Command ISCC.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1),
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
        (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe')
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) }
    $iscc = $isccCandidates | Select-Object -First 1
    if (-not $iscc) {
        throw 'Inno Setup 6 compiler (ISCC.exe) was not found. Use -SkipInstaller only for local ZIP testing.'
    }
    $installerSource = Join-Path $projectRoot 'installer\SoundFork.iss'
    & $iscc "/DMyAppVersion=$Version" "/DMySourceDir=$packageRoot" `
        "/DMyOutputDir=$artifactRoot" $installerSource
    if ($LASTEXITCODE -ne 0) {
        throw 'Inno Setup compilation failed.'
    }
    if (-not (Test-Path -LiteralPath $setupPath -PathType Leaf)) {
        throw "Expected installer was not produced: $setupPath"
    }
    if (-not [string]::IsNullOrWhiteSpace($CertificateThumbprint)) {
        & $signScript -Path $setupPath `
            -CertificateThumbprint $CertificateThumbprint `
            -TimestampUrl $TimestampUrl
    }
    $releaseFiles.Add($setupPath)
}

$checksumPath = Join-Path $artifactRoot 'SHA256SUMS.txt'
$checksumLines = foreach ($file in $releaseFiles) {
    $hash = Get-FileHash -LiteralPath $file -Algorithm SHA256
    "$($hash.Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($file))"
}
Set-Content -LiteralPath $checksumPath -Value $checksumLines -Encoding ascii

Write-Host "SoundFork release package created in '$artifactRoot'."
$releaseFiles | ForEach-Object { Write-Host "  $([IO.Path]::GetFileName($_))" }
Write-Host '  SHA256SUMS.txt'
