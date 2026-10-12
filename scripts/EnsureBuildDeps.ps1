[CmdletBinding()]
param(
    [string]$Python = 'python',
    [string]$CacheDirectory = "$PSScriptRoot/../work/developer-dependencies",
    [switch]$Dictionary,
    [switch]$Offline
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Called only by an explicit developer build. VS/Windows SDK/Boost and all
# inference weights remain outside this bootstrap; failure stops the build.
$CacheDirectory = [IO.Path]::GetFullPath($CacheDirectory)
$env:MSIME_PYTHON = $Python
New-Item -ItemType Directory -Force -Path $CacheDirectory | Out-Null
$settings = Join-Path $PSScriptRoot '../ui-html/webview2/settings/ime-settings'
$package = Get-Content -LiteralPath (Join-Path $settings 'package.json') -Raw | ConvertFrom-Json
$pnpmVersion = $package.packageManager -replace '^pnpm@', ''
if ($pnpmVersion -notmatch '^\d+\.\d+\.\d+$') { throw 'The pnpm version must be pinned in package.json.' }
$pnpm = Get-Command pnpm -ErrorAction SilentlyContinue
$validPnpm = $false
if ($pnpm) {
    $actualVersion = (& $pnpm --version | Select-Object -Last 1)
    $validPnpm = $LASTEXITCODE -eq 0 -and $actualVersion -eq $pnpmVersion
}
if (!$validPnpm) {
    $npmRoot = Join-Path $CacheDirectory 'node'
    $localPnpm = Join-Path $npmRoot 'node_modules/.bin/pnpm.cmd'
    if (!(Test-Path -LiteralPath $localPnpm)) {
        if ($Offline) { throw "Offline cache does not contain pnpm@$pnpmVersion." }
        if (!(Get-Command npm -ErrorAction SilentlyContinue)) { throw 'Node.js/npm is required; install it before building.' }
        & npm install --prefix $npmRoot --ignore-scripts --no-audit --no-fund "pnpm@$pnpmVersion"
        if ($LASTEXITCODE -ne 0) { throw "pnpm dependency installation failed ($LASTEXITCODE)." }
    }
    $actualVersion = (& $localPnpm --version | Select-Object -Last 1)
    if ($LASTEXITCODE -ne 0 -or $actualVersion -ne $pnpmVersion) { throw 'Cached pnpm has the wrong version.' }
    $env:PATH = (Split-Path -Parent $localPnpm) + [IO.Path]::PathSeparator + $env:PATH
}
Push-Location $settings
try {
    $arguments = @('install', '--frozen-lockfile')
    if ($Offline) { $arguments += '--offline' }
    & pnpm @arguments
    if ($LASTEXITCODE -ne 0) { throw "Settings dependencies could not be prepared ($LASTEXITCODE); no package was staged." }
} finally { Pop-Location }
if ($Dictionary) {
    $pythonPackages = Join-Path $CacheDirectory 'python'
    New-Item -ItemType Directory -Force -Path $pythonPackages | Out-Null
    $env:PYTHONPATH = $pythonPackages + [IO.Path]::PathSeparator + $env:PYTHONPATH
    & $Python -c "import pypinyin; assert pypinyin.__version__ == '0.55.0'" 2>$null
    if ($LASTEXITCODE -ne 0) {
        if ($Offline) { throw 'Offline Python cache does not contain pypinyin==0.55.0.' }
        & $Python -m pip install --target $pythonPackages --disable-pip-version-check 'pypinyin==0.55.0'
        if ($LASTEXITCODE -ne 0) { throw "Dictionary dependency installation failed ($LASTEXITCODE)." }
        & $Python -c "import pypinyin; assert pypinyin.__version__ == '0.55.0'"
        if ($LASTEXITCODE -ne 0) { throw 'Dictionary Python dependency could not be loaded by the selected interpreter.' }
    }
}
Write-Host 'Developer dependencies ready; inference models were not downloaded.'
