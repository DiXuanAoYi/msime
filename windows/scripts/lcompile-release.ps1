# Build the TSF DLL without depending on the caller's working directory.
[CmdletBinding()]
param(
    [ValidateSet('32', '64')][string]$Architecture = '64',
    # ALL_BUILD also builds test_windows_ipc_contract and the tests/ subdirectory, neither of which
    # is packaged. Naming the DLL target keeps a local packaging run from paying for them; the test
    # jobs still build ALL_BUILD by passing -Target ALL_BUILD.
    [string]$Target = 'MetasequoiaImeTsf',
    # The Visual Studio generator wires ZERO_CHECK into every target, so a build regenerates itself
    # whenever CMakeLists.txt changes. Re-running configure up front only costs ~10s per build
    # directory without changing the result, so do it once and let ZERO_CHECK handle the rest.
    [switch]$Reconfigure,
    [switch]$UseCompilerCache
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDirectory = Join-Path $projectRoot "build$Architecture-release"
Push-Location $projectRoot
try {
    # A failed configure (e.g. vcpkg install error) still leaves CMakeCache.txt behind, so key off
    # generate.stamp, which is only written once project generation succeeds.
    $generated = (Test-Path -LiteralPath (Join-Path $buildDirectory 'CMakeFiles\generate.stamp')) -or
                 (Test-Path -LiteralPath (Join-Path $buildDirectory 'build.ninja'))
    if ($Reconfigure -or -not $generated) {
        cmake "--preset=for$Architecture-release"
        if ($LASTEXITCODE -ne 0) { throw "TSF $Architecture configure failed ($LASTEXITCODE)" }
    }
    $generator = 'Visual Studio 18 2026'
    $cacheFile = Join-Path $buildDirectory 'CMakeCache.txt'
    if (Test-Path -LiteralPath $cacheFile) {
        $entry = Get-Content -LiteralPath $cacheFile | Where-Object { $_ -like 'CMAKE_GENERATOR:INTERNAL=*' } | Select-Object -First 1
        if ($entry) { $generator = $entry.Substring($entry.IndexOf('=') + 1) }
    }
    if ($UseCompilerCache -or $generator -match '^Ninja') {
        . (Join-Path $projectRoot '../scripts/ci/compiler-cache.ps1')
        $cacheArguments = @(Get-MsimeCompilerCacheArguments -BuildDirectory $buildDirectory -Generator $generator -Enable:$UseCompilerCache)
        if ($UseCompilerCache -and $generator -notmatch '^Ninja') {
            Write-Host 'The Visual Studio generator uses ordinary MSVC compilation; optional cache requires a prepared Ninja build directory.'
        }
        cmake -S $projectRoot -B $buildDirectory @cacheArguments
        if ($LASTEXITCODE -ne 0) { throw "TSF $Architecture cache configuration failed ($LASTEXITCODE)" }
    }
    cmake --build $buildDirectory --config Release --target $Target --parallel
    if ($LASTEXITCODE -ne 0) { throw "TSF $Architecture build failed ($LASTEXITCODE)" }
} finally { Pop-Location }
