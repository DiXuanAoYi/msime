$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('msime-runner-registry-' + [guid]::NewGuid().ToString('N'))
$oldRoot = $env:VCPKG_ROOT
$oldInstalled = $env:VCPKG_INSTALLATION_ROOT
try {
    foreach ($name in @('hosted registry', 'self hosted')) {
        $directory = Join-Path $fixture $name
        New-Item -ItemType Directory -Path (Join-Path $directory 'scripts/buildsystems') -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $directory 'vcpkg.exe') -Value ''
        Set-Content -LiteralPath (Join-Path $directory 'scripts/buildsystems/vcpkg.cmake') -Value ''
    }
    $hosted = Join-Path $fixture 'hosted registry'
    $self = Join-Path $fixture 'self hosted'
    $environment = Join-Path $fixture 'github-env.txt'
    & "$repo/scripts/ci/select-vcpkg.ps1" -SelfHostedRoot $self -HostedRoot $hosted -RunnerEnvironment github-hosted -GithubEnvironmentFile $environment
    if ($env:VCPKG_ROOT -ne $hosted -or $env:VCPKG_INSTALLATION_ROOT -ne $hosted) { throw 'Hosted registry selection failed.' }
    if ((Get-Content $environment) -notcontains "VCPKG_ROOT=$hosted") { throw 'Hosted environment persistence failed.' }
    & "$repo/scripts/ci/select-vcpkg.ps1" -SelfHostedRoot $self -HostedRoot $hosted -RunnerEnvironment self-hosted -GithubEnvironmentFile ''
    if ($env:VCPKG_ROOT -ne $self) { throw 'Self-hosted registry selection failed.' }
    $rejected = $false
    try { & "$repo/scripts/ci/select-vcpkg.ps1" -SelfHostedRoot $self -HostedRoot '' -RunnerEnvironment github-hosted -GithubEnvironmentFile '' }
    catch { $rejected = $true }
    if (!$rejected) { throw 'Missing hosted registry silently selected a developer path.' }
    $rejected = $false
    try { & "$repo/scripts/ci/select-vcpkg.ps1" -SelfHostedRoot (Join-Path $fixture 'missing') -RunnerEnvironment self-hosted -GithubEnvironmentFile '' }
    catch { $rejected = $true }
    if (!$rejected) { throw 'Missing self-hosted registry was accepted.' }
    Write-Host 'PASS: 4 runner registry selection fixtures.'
} finally {
    $env:VCPKG_ROOT = $oldRoot
    $env:VCPKG_INSTALLATION_ROOT = $oldInstalled
    $resolved = [IO.Path]::GetFullPath($fixture)
    $temporary = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if ($resolved.StartsWith($temporary, [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $resolved)) {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
