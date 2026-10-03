$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
$devkitProRoot = $env:DEVKITPRO
if ([string]::IsNullOrWhiteSpace($devkitProRoot) -or
    -not (Test-Path -LiteralPath (Join-Path $devkitProRoot 'msys2\usr\bin\bash.exe'))) {
    $devkitProRoot = Join-Path $env:SystemDrive 'devkitPro'
}
$bash = Join-Path $devkitProRoot 'msys2\usr\bin\bash.exe'
if (-not (Test-Path -LiteralPath $bash -PathType Leaf)) {
    throw 'devkitPro MSYS2 was not found. Set DEVKITPRO to the Windows devkitPro installation directory.'
}

# A fresh staging directory avoids spaces in make paths and stale build files.
$stagingParent = Join-Path $devkitProRoot 'msys2\tmp'
$buildRoot = Join-Path $stagingParent ('regionfix-build-' + [Guid]::NewGuid().ToString('N'))
if ($buildRoot -match '\s') {
    throw 'The devkitPro installation path must not contain spaces.'
}

function Get-BuildHash([string]$Path, [string]$Algorithm) {
    # Use .NET directly for compatibility with older Windows PowerShell versions.
    $hasher = [System.Security.Cryptography.HashAlgorithm]::Create($Algorithm)
    if ($null -eq $hasher) { throw "Hash algorithm unavailable: $Algorithm" }
    $stream = $null
    try {
        $stream = [System.IO.File]::OpenRead($Path)
        return [System.BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-', '')
    } finally {
        if ($null -ne $stream) { $stream.Dispose() }
        $hasher.Dispose()
    }
}

function Copy-SourceTree([string]$Source, [string]$Destination) {
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    Get-ChildItem -LiteralPath $Source -Recurse -Force -File |
        Where-Object { $_.FullName -notmatch '[\\/](\.git|build|build-diagnostic|release|lib)[\\/]' } |
        ForEach-Object {
            $relative = $_.FullName.Substring($Source.Length).TrimStart('\', '/')
            $target = Join-Path $Destination $relative
            New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
            Copy-Item -LiteralPath $_.FullName -Destination $target
        }
}

$payload = Join-Path $taskRoot 'upstream\PayloadLoaderInstaller\payload\root.rpx'
if ((Get-BuildHash $payload 'SHA1') -ne '1736574CF6C949557AED0C817EB1927E35A9B820') {
    throw 'The payload does not match the original installer hash.'
}
Copy-SourceTree (Join-Path $taskRoot 'source') (Join-Path $buildRoot 'source')
Copy-SourceTree (Join-Path $taskRoot 'upstream\PayloadLoaderInstaller') (Join-Path $buildRoot 'upstream\PayloadLoaderInstaller')
Copy-SourceTree (Join-Path $taskRoot 'upstream\libiosuhax') (Join-Path $buildRoot 'upstream\libiosuhax')
Copy-Item -LiteralPath (Join-Path $taskRoot 'Makefile') -Destination $buildRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'build.sh') -Destination $buildRoot

$previousBuildRoot = $env:REGIONFIX_BUILD_ROOT
try {
    $env:REGIONFIX_BUILD_ROOT = $buildRoot
    & $bash --login (Join-Path $buildRoot 'build.sh').Replace('\', '/')
    if ($LASTEXITCODE -ne 0) { throw "devkitPro make failed: $LASTEXITCODE" }
} finally {
    $env:REGIONFIX_BUILD_ROOT = $previousBuildRoot
}

$output = Join-Path $taskRoot 'artifacts'
New-Item -ItemType Directory -Force -Path $output | Out-Null
Copy-Item -LiteralPath (Join-Path $buildRoot 'regionfix.wuhb') -Destination $output
Copy-Item -LiteralPath (Join-Path $buildRoot 'regionfix.wuhb') -Destination $taskRoot
Copy-Item -LiteralPath (Join-Path $buildRoot 'upstream\PayloadLoaderInstaller\PayloadLoaderInstaller-RegionFix.wuhb') -Destination $output
Copy-Item -LiteralPath (Join-Path $buildRoot 'upstream\PayloadLoaderInstaller\PayloadLoaderInstaller-RegionFix-Diagnostic.wuhb') -Destination $output
Get-ChildItem -LiteralPath $output -Filter '*.wuhb' | ForEach-Object {
    [PSCustomObject]@{ Algorithm = 'SHA256'; Hash = (Get-BuildHash $_.FullName 'SHA256'); Path = $_.FullName }
} | Format-List
