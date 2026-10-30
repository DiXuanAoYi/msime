# 打完整测试包（installer\test.ps1）前把仓外输入准备到位：
#   - neural-model\sentence-model.safetensors / sentence-model-desktop.safetensors
#   - language-model\sc.lm
#   - MetasequoiaImeDict\out\ 下的词库（按 product-lock.json 钉住的 dict-* release）
# 全部按各自的锁校验 SHA256，且是幂等的：已就绪就直接跳过。
[CmdletBinding()]
param(
    # 忽略已有文件，强制重新下载 / 重新转换。
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot

& (Join-Path $PSScriptRoot 'fetch-neural-model.ps1') -Force:$Force
& (Join-Path $PSScriptRoot 'build-language-model.ps1') -Force:$Force

# 词库。CI 走 product_lock.py fetch-dictionaries，它依赖已登录的 gh；release 是公开的，
# 这里直接按 URL 下载，再用同一个 verify-dictionaries 校验，免得本机还要 gh auth login。
$lockPath = Join-Path $repoRoot 'product-lock.json'
$productLock = Join-Path $PSScriptRoot 'product_lock.py'
$dictionary = (Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json).dictionary
$dictRoot = Join-Path $repoRoot 'MetasequoiaImeDict'
$dictOut = Join-Path $dictRoot 'out'
$notice = Join-Path $dictRoot 'source\mozc_dictionary_oss\README.txt'

$ready = $false
# out\ 若是 build-dictionary.py 从 msime-dictionary 文本源数据构建的（有 local-dictionary.json），
# 它本来就对不上产品锁的摘要；这里只校验或重新构建它，不拿产品锁的旧词库覆盖。
$localBuild = Test-Path -LiteralPath (Join-Path $dictOut 'local-dictionary.json')
if ($localBuild) {
    $buildDictionary = Join-Path $PSScriptRoot 'build-dictionary.py'
    if ($Force) { python $buildDictionary } else { python $buildDictionary --verify }
    if ($LASTEXITCODE -ne 0) { throw "本地构建的词库校验失败（$LASTEXITCODE），运行 python scripts\build-dictionary.py 重新构建" }
    $ready = $true
}
elseif (-not $Force -and (Test-Path -LiteralPath $dictOut)) {
    python $productLock verify-dictionaries $dictOut 2>$null | Out-Null
    $ready = ($LASTEXITCODE -eq 0) -and (Test-Path -LiteralPath $notice)
}

if ($localBuild) {
    # build-dictionary.py 已经输出了就绪信息。
}
elseif ($ready) {
    Write-Host "词库已就绪：$dictOut"
}
else {
    # 整套校验通过之前不动已有的 out\。
    $incoming = Join-Path $dictRoot 'incoming'
    if (Test-Path -LiteralPath $incoming) { Remove-Item -LiteralPath $incoming -Recurse -Force }
    New-Item -ItemType Directory -Path $incoming -Force | Out-Null
    try {
        foreach ($name in $dictionary.assets.PSObject.Properties.Name) {
            $url = "https://github.com/$($dictionary.repository)/releases/download/$($dictionary.tag)/$name"
            Write-Host "下载 $name ..."
            curl.exe -L --fail --retry 3 -o (Join-Path $incoming $name) $url
            if ($LASTEXITCODE -ne 0) { throw "下载词库失败（$LASTEXITCODE）：$url" }
        }
        python $productLock verify-dictionaries $incoming
        if ($LASTEXITCODE -ne 0) { throw "词库校验失败（$LASTEXITCODE）" }

        New-Item -ItemType Directory -Path $dictOut -Force | Out-Null
        Get-ChildItem -LiteralPath $incoming -File | Copy-Item -Destination $dictOut -Force
        New-Item -ItemType Directory -Path (Split-Path -Parent $notice) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $dictOut 'mozc_dictionary_oss_README.txt') -Destination $notice -Force
    }
    finally {
        Remove-Item -LiteralPath $incoming -Recurse -Force -ErrorAction SilentlyContinue
    }
    Write-Host "词库准备完毕（$($dictionary.tag)）：$dictOut"
}

Write-Host '全部就绪，可以运行 .\installer\test.ps1。'
