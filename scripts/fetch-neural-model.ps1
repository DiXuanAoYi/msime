<#
.SYNOPSIS
    下载 chinese-ime-lm 的神经整句模型（safetensors）到 neural-model/，并按 lock.json 校验 sha256。

.DESCRIPTION
    与 build-language-model.ps1 对 sc.lm 的处理同理：模型二进制不进仓库（见 .gitignore），
    只有 lock.json 和 NOTICE.md 进仓库。打包（installer/Prepare-PackageFiles.ps1）前跑一次
    默认仅准备随包速度档；效果档须显式 -Profile desktop/all。
    已存在且校验通过的文件跳过重复下载（除非 -Force）。-Offline 禁止下载。

.PARAMETER Force
    忽略已存在文件，强制重新下载。
#>
[CmdletBinding()]
param(
    [switch]$Force,
    [ValidateSet('keyboard', 'desktop', 'all')][string]$Profile = 'keyboard',
    [switch]$Offline
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$here = Split-Path -Parent $PSScriptRoot
$modelDir = Join-Path $here 'neural-model'
$lockPath = Join-Path $modelDir 'lock.json'
if (-not (Test-Path -LiteralPath $lockPath)) {
    throw "找不到 lock.json：$lockPath"
}
$lock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json

foreach ($model in $lock.models) {
    if ($Profile -eq 'keyboard' -and $model.file -ne 'sentence-model.safetensors') { continue }
    if ($Profile -eq 'desktop' -and $model.file -ne 'sentence-model-desktop.safetensors') { continue }
    $dest = Join-Path $modelDir $model.file
    $needsDownload = $true
    if ((Test-Path -LiteralPath $dest) -and -not $Force) {
        $hash = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hash -eq $model.sha256.ToLowerInvariant()) {
            Write-Host "已存在且校验通过，跳过：$($model.file)"
            $needsDownload = $false
        }
        else {
            Write-Host "校验不符，重新下载：$($model.file)"
        }
    }
    if ($needsDownload) {
        if ($Offline) { throw "缺少经校验的 $Profile 模型缓存；不会自动下载：$($model.file)" }
        Write-Host "下载 $($model.file) ..."
        $incoming = "$dest.incoming"
        Invoke-WebRequest -Uri $model.url -OutFile $incoming
        $hash = (Get-FileHash -LiteralPath $incoming -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hash -ne $model.sha256.ToLowerInvariant() -or (Get-Item -LiteralPath $incoming).Length -ne $model.size) {
            throw "$($model.file) sha256 校验失败：期望 $($model.sha256)，实际 $hash"
        }
        Move-Item -LiteralPath $incoming -Destination $dest -Force
        Write-Host "  校验通过：$($model.file)"
    }
}

Write-Host '神经整句模型准备完毕。'
