[CmdletBinding()]
param(
    [string]$CacheDirectory = "$PSScriptRoot/../../work/vulkan-dependencies",
    [string]$GithubEnvironmentFile = '',
    [string]$SdkRoot = '',
    [string]$Loader = '',
    [switch]$Offline
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Explicit developer/CI bootstrap only. This script never downloads model weights,
# installs display drivers, or changes machine/user environment variables.
$version = '1.4.363.0'
$CacheDirectory = [IO.Path]::GetFullPath($CacheDirectory)
New-Item -ItemType Directory -Force -Path $CacheDirectory | Out-Null
function Get-PinnedArchive([string]$Name, [string]$Hash) {
    $path = Join-Path $CacheDirectory $Name
    if ((Test-Path -LiteralPath $path -PathType Leaf) -and
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -eq $Hash) { return $path }
    if ($Offline) { throw "Offline cache is missing or invalid: $Name" }
    $partial = "$path.incoming"
    & curl.exe --fail --location --retry 3 --user-agent 'Mozilla/5.0' --referer 'https://vulkan.lunarg.com/' `
        --output $partial "https://sdk.lunarg.com/sdk/download/$version/windows/$Name"
    if ($LASTEXITCODE -ne 0) { throw "Official Vulkan dependency download failed ($LASTEXITCODE): $Name" }
    if ((Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Hash) {
        throw "Official Vulkan dependency SHA256 mismatch: $Name"
    }
    Move-Item -LiteralPath $partial -Destination $path -Force
    return $path
}
if (!$SdkRoot) { $SdkRoot = $env:VULKAN_SDK }
if (!$SdkRoot) { $SdkRoot = Join-Path $CacheDirectory "sdk-$version" }
$SdkRoot = [IO.Path]::GetFullPath($SdkRoot)
$required = @('Bin/glslc.exe', 'Include/vulkan/vulkan.h', 'Lib/vulkan-1.lib')
if (@($required | Where-Object { !(Test-Path -LiteralPath (Join-Path $SdkRoot $_) -PathType Leaf) }).Count) {
    if ($Offline) { throw "Vulkan SDK is not ready: $SdkRoot" }
    $installer = Get-PinnedArchive "vulkansdk-windows-X64-$version.exe" '94a82d378f7a5e3e54c9db7d2fb7016af136e14ac0a18dbf0f2f67a36352d141'
    # Official Qt IFW copy-only mode; no system runtime or registry installation.
    & $installer --root $SdkRoot --accept-licenses --default-answer --confirm-command install com.lunarg.vulkan.core copy_only=1
    if ($LASTEXITCODE -ne 0) { throw "Vulkan SDK copy-only extraction failed ($LASTEXITCODE)" }
}
foreach ($relative in $required) {
    if (!(Test-Path -LiteralPath (Join-Path $SdkRoot $relative) -PathType Leaf)) { throw "Vulkan SDK component missing: $relative" }
}
if (!$Loader) { $Loader = $env:MSIME_VULKAN_LOADER }
if (!$Loader) {
    $runtime = Join-Path $CacheDirectory "runtime-$version"
    $Loader = Join-Path $runtime "VulkanRT-X64-$version-Components/x64/vulkan-1.dll"
    if (!(Test-Path -LiteralPath $Loader -PathType Leaf)) {
        $archive = Get-PinnedArchive 'vulkan-runtime-components.zip' 'a25a927aa8b9f0371048f1861cf88ac3b9bc9b1fb332c42d897c8ab32695769a'
        Expand-Archive -LiteralPath $archive -DestinationPath $runtime -Force
    }
}
if (!(Test-Path -LiteralPath $Loader -PathType Leaf)) { throw 'The official x64 Vulkan loader is missing.' }
$Loader = [IO.Path]::GetFullPath($Loader)
$env:VULKAN_SDK = $SdkRoot
$env:MSIME_VULKAN_LOADER = $Loader
if ($GithubEnvironmentFile) {
    [IO.File]::AppendAllText($GithubEnvironmentFile, "VULKAN_SDK=$SdkRoot`nMSIME_VULKAN_LOADER=$Loader`n", [Text.UTF8Encoding]::new($false))
}
Write-Host "Vulkan developer dependencies ready; model weights and display drivers are untouched."
