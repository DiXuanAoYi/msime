[CmdletBinding()]
param(
    [string]$Python = 'python',
    [string]$VenvDirectory = '',
    [string]$WorkDirectory = "$PSScriptRoot/../../work/local-asr-build",
    [string]$OutputDirectory = "$PSScriptRoot/../../server/build-release/bin/Release/local-asr",
    [string]$VulkanLoader = ''
)
$ErrorActionPreference = 'Stop'
$WorkDirectory = [IO.Path]::GetFullPath($WorkDirectory)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force $WorkDirectory | Out-Null
$venv = if ($VenvDirectory) { [IO.Path]::GetFullPath($VenvDirectory) } else { Join-Path $WorkDirectory 'venv' }
if (!(Test-Path "$venv/Scripts/python.exe")) {
    & $Python -m venv $venv
    if ($LASTEXITCODE) { throw 'Could not create local ASR build environment' }
}
$py = "$venv/Scripts/python.exe"
if (!$VulkanLoader) { $VulkanLoader = $env:MSIME_VULKAN_LOADER }
if ($VulkanLoader) {
    if (!(Test-Path -LiteralPath $VulkanLoader -PathType Leaf)) { throw 'Configured Vulkan loader is missing.' }
    Copy-Item -LiteralPath $VulkanLoader -Destination "$PSScriptRoot/native-voxtral/vulkan-1.dll" -Force
}
foreach ($native in @('audiocpp.dll','audiocpp_gguf.exe','vcomp140.dll','vulkan-1.dll')) {
    if (!(Test-Path -LiteralPath "$PSScriptRoot/native-voxtral/$native")) {
        throw 'Run Build-Voxtral.ps1 before packaging the local runtime.'
    }
}
& $py -m pip install -r "$PSScriptRoot/requirements-build.txt"
if ($LASTEXITCODE) { throw 'Local ASR dependency installation failed' }
& $py -m unittest discover -s $PSScriptRoot -v
if ($LASTEXITCODE) { throw 'Local ASR tests failed' }
& $py -m PyInstaller --noconfirm --onedir --name MetasequoiaLocalASR `
    --distpath "$WorkDirectory/dist" --workpath "$WorkDirectory/build" --specpath $WorkDirectory `
    --collect-all sherpa_onnx --copy-metadata sherpa-onnx --copy-metadata sherpa-onnx-core `
    --copy-metadata numpy --copy-metadata uv `
    --add-data "$PSScriptRoot/catalog.json;." --add-data "$PSScriptRoot/catalog.json;worker" `
    --add-data "$PSScriptRoot/manager.py;worker" --add-data "$PSScriptRoot/qwen_worker.py;worker" `
    --add-data "$PSScriptRoot/tts_catalog.json;." --add-data "$PSScriptRoot/tts_catalog.json;worker" `
    --add-data "$PSScriptRoot/tts_manager.py;worker" --add-data "$PSScriptRoot/tts_qwen_worker.py;worker" `
    --add-data "$PSScriptRoot/tts_qwen_cuda_worker.py;worker" --add-data "$PSScriptRoot/tts_moss_worker.py;worker" `
    --add-data "$PSScriptRoot/tts_edge_worker.py;worker" `
    --add-data "$PSScriptRoot/tts_moss_cuda_worker.py;worker" `
    --add-data "$PSScriptRoot/tts_mms_worker.py;worker" --add-data "$PSScriptRoot/tts_mms_cuda_worker.py;worker" `
    --add-data "$PSScriptRoot/tts_kokoro_cuda_worker.py;worker" `
    --add-data "$PSScriptRoot/tts_kokoro_worker.py;worker" --add-data "$PSScriptRoot/ocr_vlm_cpu_worker.py;worker" `
    --add-data "$PSScriptRoot/ocr_catalog.json;." --add-data "$PSScriptRoot/ocr_catalog.json;worker" `
    --add-data "$PSScriptRoot/ocr_runtime.py;worker" --add-data "$PSScriptRoot/ocr_vlm_worker.py;worker" `
    --add-data "$PSScriptRoot/ocr_layout.py;worker" `
    --add-data "$PSScriptRoot/ocr_paddle_worker.py;worker" --add-data "$PSScriptRoot/ocr_manager.py;worker" `
    --add-data "$PSScriptRoot/tts_options.py;worker" `
    --add-data "$PSScriptRoot/tts_voxtral_worker.py;worker" --add-data "$PSScriptRoot/tts_voxtral_native.py;worker" `
    --add-data "$PSScriptRoot/tts_voxtral_convert.py;worker" `
    --add-binary "$PSScriptRoot/native-voxtral/audiocpp.dll;worker/native-voxtral" `
    --add-binary "$PSScriptRoot/native-voxtral/audiocpp_gguf.exe;worker/native-voxtral" `
    --add-binary "$PSScriptRoot/native-voxtral/vcomp140.dll;worker/native-voxtral" `
    --add-binary "$PSScriptRoot/native-voxtral/vulkan-1.dll;worker/native-voxtral" `
    --add-data "$PSScriptRoot/tts_stream.py;worker" --add-data "$PSScriptRoot/vendor;worker/vendor" `
    --add-data "$PSScriptRoot/../../engine/contracts/tts_stream.json;." --add-data "$PSScriptRoot/../../engine/contracts/tts_stream.json;worker" `
    --add-data "$PSScriptRoot/worker_dependencies.py;worker" `
    --add-data "$PSScriptRoot/neural_qjm.py;worker" --add-data "$PSScriptRoot/neural_manager.py;worker" `
    --add-data "$PSScriptRoot/neural_worker.py;worker" `
    --add-data "$PSScriptRoot/THIRD-PARTY-NOTICES.txt;." `
    --add-data "$PSScriptRoot/licenses;licenses" `
    --add-binary "$venv/Scripts/uv.exe;." "$PSScriptRoot/manager.py"
if ($LASTEXITCODE) { throw 'Local ASR packaging failed' }
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
Copy-Item -Path "$WorkDirectory/dist/MetasequoiaLocalASR/*" -Destination $OutputDirectory -Recurse -Force
Write-Host "Local ASR runtime is ready: $OutputDirectory (models download separately)."
