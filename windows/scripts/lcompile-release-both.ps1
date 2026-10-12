[CmdletBinding()]
param([string]$Target = 'MetasequoiaImeTsf', [switch]$Reconfigure, [switch]$UseCompilerCache)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'lcompile-release.ps1') 32 -Target $Target -Reconfigure:$Reconfigure -UseCompilerCache:$UseCompilerCache
& (Join-Path $PSScriptRoot 'lcompile-release.ps1') 64 -Target $Target -Reconfigure:$Reconfigure -UseCompilerCache:$UseCompilerCache
