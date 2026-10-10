[CmdletBinding()]
param(
    [string]$Python = 'python',
    [string]$WorkDirectory = "$PSScriptRoot/../../work/local-asr-build",
    [string]$OutputDirectory = "$PSScriptRoot/../../server/build-release/bin/Release/local-asr"
)
$ErrorActionPreference = 'Stop'
$WorkDirectory = [IO.Path]::GetFullPath($WorkDirectory)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force $WorkDirectory | Out-Null
$venv = Join-Path $WorkDirectory 'venv'
if (!(Test-Path "$venv/Scripts/python.exe")) {
    & $Python -m venv $venv
    if ($LASTEXITCODE) { throw 'Could not create local ASR build environment' }
}
$py = "$venv/Scripts/python.exe"
& $py -m pip install -r "$PSScriptRoot/requirements-build.txt"
if ($LASTEXITCODE) { throw 'Local ASR dependency installation failed' }
& $py -m unittest discover -s $PSScriptRoot -v
if ($LASTEXITCODE) { throw 'Local ASR tests failed' }
& $py -m PyInstaller --noconfirm --onedir --name MetasequoiaLocalASR `
    --distpath "$WorkDirectory/dist" --workpath "$WorkDirectory/build" --specpath $WorkDirectory `
    --collect-all sherpa_onnx --copy-metadata sherpa-onnx --copy-metadata sherpa-onnx-core `
    --copy-metadata numpy --copy-metadata uv `
    --add-data "$PSScriptRoot/catalog.json;." --add-data "$PSScriptRoot/manager.py;." `
    --add-data "$PSScriptRoot/qwen_worker.py;." --add-data "$PSScriptRoot/THIRD-PARTY-NOTICES.txt;." `
    --add-data "$PSScriptRoot/licenses;licenses" `
    --add-binary "$venv/Scripts/uv.exe;." "$PSScriptRoot/manager.py"
if ($LASTEXITCODE) { throw 'Local ASR packaging failed' }
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
Copy-Item -Path "$WorkDirectory/dist/MetasequoiaLocalASR/*" -Destination $OutputDirectory -Recurse -Force
Write-Host "Local ASR runtime is ready: $OutputDirectory (models download separately)."
