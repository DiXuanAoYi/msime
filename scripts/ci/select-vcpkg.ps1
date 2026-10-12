# Use the runner's installed registry; hosted builds must not inherit a developer's drive layout.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$SelfHostedRoot,
    [string]$RunnerEnvironment = $env:RUNNER_ENVIRONMENT,
    [string]$HostedRoot = $env:VCPKG_INSTALLATION_ROOT,
    [string]$GithubEnvironmentFile = $env:GITHUB_ENV
)
$ErrorActionPreference = 'Stop'
$selected = if ($RunnerEnvironment -eq 'github-hosted') { $HostedRoot } else { $SelfHostedRoot }
if (!$selected -or !(Test-Path -LiteralPath (Join-Path $selected 'vcpkg.exe')) -or
    !(Test-Path -LiteralPath (Join-Path $selected 'scripts/buildsystems/vcpkg.cmake'))) {
    throw "The $RunnerEnvironment runner has no installed vcpkg registry at the selected path. Configure VCPKG_INSTALLATION_ROOT for a hosted runner."
}
$selected = [IO.Path]::GetFullPath($selected)
$env:VCPKG_ROOT = $selected
$env:VCPKG_INSTALLATION_ROOT = $selected
if ($GithubEnvironmentFile) {
    @("VCPKG_ROOT=$selected", "VCPKG_INSTALLATION_ROOT=$selected") |
        Add-Content -LiteralPath $GithubEnvironmentFile -Encoding utf8
}
Write-Host "Selected installed vcpkg registry: $selected"
