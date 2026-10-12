# Real MSVC/Ninja integration: a failed cache must still produce and run the correct binary.
[CmdletBinding()]
param([string]$Python = 'python', [string]$CMake = 'cmake', [string]$Ninja = 'ninja')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('msime-native-cache-' + [guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Force $fixture | Out-Null
    $source = Join-Path $fixture '中文 source.cpp'
    [IO.File]::WriteAllText($source, '#include <cstdio>' + "`n" + 'int main() { std::puts("cache-fallback-native-ok"); }' + "`n", [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $fixture 'CMakeLists.txt'), @'
cmake_minimum_required(VERSION 3.25)
project(CacheFallback LANGUAGES CXX)
add_executable(cache_fixture "中文 source.cpp")
'@, [Text.UTF8Encoding]::new($false))
    $cacheScript = Join-Path $fixture 'failed_cache.py'
    [IO.File]::WriteAllText($cacheScript, @'
import json,sys
from pathlib import Path
path=Path(__file__).with_suffix('.calls')
with path.open('a',encoding='utf-8') as output: output.write(json.dumps(sys.argv[1:],ensure_ascii=False)+'\n')
sys.exit(17)
'@, [Text.UTF8Encoding]::new($false))
    $pythonExe = (Get-Command $Python).Source
    $ninjaExe = (Get-Command $Ninja).Source
    $build = Join-Path $fixture 'build'
    $flag = Join-Path $build '.msime-sccache-disabled'
    $launcher = "$pythonExe;$repo/scripts/ci/compile_cache_launcher.py;--cache;$pythonExe;--cache-argument;$cacheScript;--disable-flag;$flag;--"
    & $CMake -S $fixture -B $build -G 'Ninja Multi-Config' "-DCMAKE_MAKE_PROGRAM=$ninjaExe" "-DCMAKE_CXX_COMPILER_LAUNCHER=$launcher"
    if ($LASTEXITCODE) { throw 'Native cache fixture configure failed.' }
    & $CMake --build $build --config Release --parallel 1
    if ($LASTEXITCODE) { throw 'A failing cache prevented MSVC compilation.' }
    $executable = Join-Path $build 'Release/cache_fixture.exe'
    $result = & $executable
    if ($LASTEXITCODE -or $result -ne 'cache-fallback-native-ok') { throw 'The fallback binary did not run correctly.' }
    if (!(Test-Path -LiteralPath $flag)) { throw 'Cache failure did not disable the cache.' }
    $calls = Get-Content -LiteralPath ([IO.Path]::ChangeExtension($cacheScript, '.calls'))
    if ($calls.Count -ne 1 -or $calls -notmatch '中文 source.cpp') { throw 'Cache/compiler argv was not preserved.' }
    Add-Content -LiteralPath $source -Value '// Force another compile while the cache is disabled.' -Encoding utf8
    & $CMake --build $build --config Release --parallel 1
    if ($LASTEXITCODE) { throw 'Disabled-cache incremental compilation failed.' }
    $calls = @(Get-Content -LiteralPath ([IO.Path]::ChangeExtension($cacheScript, '.calls')))
    if ($calls.Count -ne 1) { throw 'The disabled cache was called again.' }
    Write-Host 'PASS: real MSVC/Ninja fallback, Unicode/spaced argv, successful binary, incremental cache disable.'
} finally {
    $resolved = [IO.Path]::GetFullPath($fixture)
    $temporary = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if ($resolved.StartsWith($temporary, [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $resolved)) {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
