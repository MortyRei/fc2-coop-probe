<#
Downloads steam_api64.dll for the relay's Steam transport, checked by SHA-256:
Steamworks.NET Standalone 2025.164.1 from GitHub (zip 9412348c...537d), file Windows-x64\steam_api64.dll (eb17909a...fb75).

  powershell -ExecutionPolicy Bypass -File get-steam-api.ps1 [-OutDir <folder>]
#>
param([string]$OutDir = (Join-Path $PSScriptRoot 'steam'))
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$url = 'https://github.com/rlabrecque/Steamworks.NET/releases/download/2025.164.1/Steamworks.NET-Standalone_2025.164.1.zip'
$zipHash = '9412348cc404563be5a43a28347cfeda3c679ee044a14d87a507ed2d796a537d'
$dllHash = 'eb17909a76668cf9ae0b92a618a34a50f6c73d3a6787cb4dd8ce36a8b10bfb75'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$zip = Join-Path $OutDir 'Steamworks.NET-Standalone_2025.164.1.zip'
Invoke-WebRequest $url -OutFile $zip -UseBasicParsing
if ((Get-FileHash $zip).Hash.ToLower() -ne $zipHash) { Remove-Item $zip; throw 'zip SHA-256 mismatch, deleted' }
$x = Join-Path $OutDir 'extracted'
Expand-Archive $zip -DestinationPath $x -Force
$dll = Join-Path $x 'Windows-x64\steam_api64.dll'
if ((Get-FileHash $dll).Hash.ToLower() -ne $dllHash) { throw 'steam_api64.dll SHA-256 mismatch' }
Copy-Item $dll (Join-Path $OutDir 'steam_api64.dll') -Force
Write-Output "[steam] steam_api64.dll ready: $(Join-Path $OutDir 'steam_api64.dll') (SHA-256 checked)"
