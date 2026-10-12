[CmdletBinding()]
param(
    [string]$SourceDirectory = "$PSScriptRoot/../../work/audio-cpp",
    [string]$BuildDirectory = "$PSScriptRoot/../../work/audio-cpp-build",
    [string]$CMake = 'cmake',
    [ValidateSet('cpu','vulkan')][string]$Backend = 'vulkan',
    [string]$VulkanLoader = '',
    [string]$VulkanSdkRoot = ''
)
$ErrorActionPreference = 'Stop'
if ($Backend -eq 'vulkan') {
    if ($VulkanSdkRoot) { $env:VULKAN_SDK = [IO.Path]::GetFullPath($VulkanSdkRoot) }
    if (!$env:VULKAN_SDK -or !(Test-Path -LiteralPath "$env:VULKAN_SDK/Bin/glslc.exe")) {
        throw 'Vulkan SDK is missing. Explicitly run Ensure-VulkanDependencies.ps1 or pass -VulkanSdkRoot.'
    }
    if (!$VulkanLoader) { $VulkanLoader = $env:MSIME_VULKAN_LOADER }
}
$revision = '96269554d00621fb0fca6b52b58919749eba1c8d'
if (!(Test-Path -LiteralPath (Join-Path $SourceDirectory '.git'))) {
    $env:GIT_LFS_SKIP_SMUDGE = '1'
    & git clone --filter=blob:none https://github.com/0xShug0/audio.cpp.git $SourceDirectory
    if ($LASTEXITCODE) { throw 'audio.cpp source download failed' }
    & git -C $SourceDirectory checkout --detach $revision
    if ($LASTEXITCODE) { throw 'Could not select pinned audio.cpp revision' }
}
$actual = & git -C $SourceDirectory rev-parse HEAD
if ($actual.Trim() -ne $revision) { throw 'Voxtral runtime requires the pinned audio.cpp revision' }
$original = (& git -C $SourceDirectory show "${revision}:src/capi/audiocpp.cpp") -join "`n"
if ($LASTEXITCODE) { throw 'Could not read pinned C API source' }
$bridge = Get-Content -LiteralPath "$PSScriptRoot/native_voxtral_stream.inc" -Raw
[IO.File]::WriteAllText((Join-Path $SourceDirectory 'src/capi/audiocpp.cpp'), $original + "`n" + $bridge, [Text.UTF8Encoding]::new($false))
$vulkan = if ($Backend -eq 'vulkan') { 'ON' } else { 'OFF' }
& $CMake -S $SourceDirectory -B $BuildDirectory -A x64 -DAUDIOCPP_MODEL_SET=custom -DAUDIOCPP_MODELS=voxtral_tts -DAUDIOCPP_BUILD_C_API=ON -DAUDIOCPP_DEPLOYMENT_BUILD=ON -DENGINE_ENABLE_NATIVE_CPU=OFF -DENGINE_ENABLE_CUDA=OFF "-DENGINE_ENABLE_VULKAN=$vulkan" -DBUILD_SHARED_LIBS=OFF -DAUDIOCPP_VERSION=96269554
if ($LASTEXITCODE) { throw 'Voxtral native configure failed' }
& $CMake --build $BuildDirectory --config Release --target audiocpp audiocpp_gguf --parallel 2
if ($LASTEXITCODE) { throw 'Voxtral native build failed' }
$output = Join-Path $PSScriptRoot 'native-voxtral'
New-Item -ItemType Directory -Force $output | Out-Null
foreach ($name in @('audiocpp.dll','audiocpp_gguf.exe')) {
    Copy-Item -LiteralPath (Join-Path $BuildDirectory "bin/Release/$name") -Destination $output -Force
}
Copy-Item -LiteralPath "$env:WINDIR/System32/vcomp140.dll" -Destination $output -Force
if ($Backend -eq 'vulkan') {
    if (!$VulkanLoader) { $VulkanLoader = "$env:VULKAN_SDK/Bin/vulkan-1.dll" }
    if (!(Test-Path -LiteralPath $VulkanLoader)) { throw 'Pass -VulkanLoader with the x64 vulkan-1.dll from the official LunarG runtime components archive.' }
    Copy-Item -LiteralPath $VulkanLoader -Destination $output -Force
}
Write-Output "Voxtral $Backend runtime ready ($revision); no model weights downloaded."
