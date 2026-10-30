# Dot-source this helper. Optional cache failure never changes compiler or architecture selection.
function Get-MsimeCompilerCacheArguments {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$BuildDirectory,
        [string]$Generator = 'Ninja Multi-Config',
        [switch]$Enable,
        [string]$SccachePath = '',
        [string]$PythonPath = ''
    )
    $plain = @('-DCMAKE_C_COMPILER_LAUNCHER=', '-DCMAKE_CXX_COMPILER_LAUNCHER=')
    if (!$Enable -or $Generator -notmatch '^Ninja') { return $plain }
    if (!$SccachePath) {
        $binary = Get-Command sccache -ErrorAction SilentlyContinue
        if (!$binary) { Write-Host 'sccache not installed; ordinary MSVC compilation.'; return $plain }
        $SccachePath = $binary.Source
    }
    if (!$PythonPath) {
        $python = Get-Command python -ErrorAction SilentlyContinue
        if (!$python) { Write-Host 'Python launcher unavailable; ordinary MSVC compilation.'; return $plain }
        $PythonPath = $python.Source
    }
    $launcher = Join-Path $PSScriptRoot 'compile_cache_launcher.py'
    $flag = Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) '.msime-sccache-disabled'
    if (@($SccachePath, $PythonPath, $launcher, $flag) | Where-Object { $_.Contains(';') }) {
        Write-Host 'Cache path contains a CMake list separator; ordinary MSVC compilation.'
        return $plain
    }
    try {
        & $PythonPath --version | Out-Null
        if ($LASTEXITCODE -ne 0) { return $plain }
        & $SccachePath --start-server | Out-Null
        if ($LASTEXITCODE -ne 0) { Write-Host 'sccache service failed; ordinary MSVC compilation.'; return $plain }
        if (Test-Path -LiteralPath $flag) { Remove-Item -LiteralPath $flag -Force }
    } catch {
        Write-Host 'sccache setup failed; ordinary MSVC compilation.'
        return $plain
    }
    $value = "$PythonPath;$launcher;--cache;$SccachePath;--disable-flag;$flag;--"
    Write-Host 'Optional sccache enabled with an ordinary compiler fallback.'
    return @("-DCMAKE_C_COMPILER_LAUNCHER=$value", "-DCMAKE_CXX_COMPILER_LAUNCHER=$value")
}

function Show-MsimeCompilerCacheStats {
    [CmdletBinding()]
    param()
    $binary = Get-Command sccache -ErrorAction SilentlyContinue
    if ($binary) {
        try { & $binary.Source --show-stats; if ($LASTEXITCODE) { Write-Host 'sccache statistics unavailable.' } }
        catch { Write-Host 'sccache statistics unavailable.' }
    }
    $global:LASTEXITCODE = 0 # Reporting is optional; the build step separately owns compilation status.
}
