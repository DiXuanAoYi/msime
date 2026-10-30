# Standalone Engine build. All dependencies come from the explicitly selected vcpkg root.
[CmdletBinding()]
param(
    [string]$BuildDirectory = '',
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$Generator = 'Ninja Multi-Config',
    [switch]$UseCompilerCache,
    [switch]$SkipManifestInstall
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (!$BuildDirectory) { $BuildDirectory = Join-Path $repo 'engine/build' }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
if (!$VcpkgRoot) { $VcpkgRoot = $env:VCPKG_INSTALLATION_ROOT }
if (!$VcpkgRoot -or !(Test-Path -LiteralPath (Join-Path $VcpkgRoot 'scripts/buildsystems/vcpkg.cmake'))) {
    throw 'Select an installed vcpkg registry with -VcpkgRoot or VCPKG_ROOT.'
}
. (Join-Path $PSScriptRoot 'ci/compiler-cache.ps1')
$cache = @(Get-MsimeCompilerCacheArguments -BuildDirectory $BuildDirectory -Generator $Generator -Enable:$UseCompilerCache)
$arguments = @('-S', (Join-Path $repo 'engine'), '-B', $BuildDirectory, '-G', $Generator,
    "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake", '-DVCPKG_TARGET_TRIPLET=x64-windows', '-DBUILD_TESTING=ON') + $cache
if ($SkipManifestInstall) { $arguments += '-DVCPKG_MANIFEST_INSTALL=OFF' }
if ($Generator -match '^Visual Studio') { $arguments += @('-A', 'x64') }
& cmake @arguments
if ($LASTEXITCODE) { throw "Engine configure failed ($LASTEXITCODE)." }
& cmake --build $BuildDirectory --config Release --parallel
if ($LASTEXITCODE) { throw "Engine build failed ($LASTEXITCODE)." }
if ($UseCompilerCache) { Show-MsimeCompilerCacheStats }
