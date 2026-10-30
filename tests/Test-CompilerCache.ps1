$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../scripts/ci/compiler-cache.ps1')
$root = Join-Path ([IO.Path]::GetTempPath()) ('msime-cache-helper-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $root | Out-Null
try {
    $success = Join-Path $root 'success.cmd'
    $failure = Join-Path $root 'failure.cmd'
    [IO.File]::WriteAllText($success, "@echo off`r`nexit /b 0`r`n", [Text.Encoding]::ASCII)
    [IO.File]::WriteAllText($failure, "@echo off`r`nexit /b 7`r`n", [Text.Encoding]::ASCII)
    $build = Join-Path $root 'build'
    $python = (Get-Command python).Source
    $plain = @(Get-MsimeCompilerCacheArguments -BuildDirectory $build)
    if ($plain.Count -ne 2 -or $plain[0] -ne '-DCMAKE_C_COMPILER_LAUNCHER=') { throw 'Default must be ordinary compile' }
    $visualStudio = @(Get-MsimeCompilerCacheArguments -BuildDirectory $build -Generator 'Visual Studio 18 2026' -Enable -SccachePath $success -PythonPath $python)
    if ($visualStudio[1] -ne '-DCMAKE_CXX_COMPILER_LAUNCHER=') { throw 'VS generator must not pretend to use a launcher' }
    $missing = @(Get-MsimeCompilerCacheArguments -BuildDirectory $build -Enable -SccachePath (Join-Path $root 'missing.exe') -PythonPath $python)
    if ($missing[0] -ne $plain[0]) { throw 'Missing cache must recover' }
    $failed = @(Get-MsimeCompilerCacheArguments -BuildDirectory $build -Enable -SccachePath $failure -PythonPath $python)
    if ($failed[0] -ne $plain[0]) { throw 'Service failure must recover' }
    New-Item -ItemType Directory -Force $build | Out-Null
    $flag = Join-Path $build '.msime-sccache-disabled'
    New-Item -ItemType File -Force $flag | Out-Null
    $enabled = @(Get-MsimeCompilerCacheArguments -BuildDirectory $build -Enable -SccachePath $success -PythonPath $python)
    if ($enabled.Count -ne 2 -or !$enabled[0].Contains('compile_cache_launcher.py') -or (Test-Path -LiteralPath $flag)) {
        throw 'Successful service must prepare argv launcher and reset previous fallback'
    }
    $separated = @(Get-MsimeCompilerCacheArguments -BuildDirectory ($build + ';invalid') -Enable -SccachePath $success -PythonPath $python)
    if ($separated[0] -ne $plain[0]) { throw 'CMake list separator path must recover' }
    Write-Host 'Compiler-cache helper: 6 fallback/selection fixtures passed.'
} finally {
    $absolute = [IO.Path]::GetFullPath($root)
    $temporary = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (!$absolute.StartsWith($temporary, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe test cleanup path' }
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
