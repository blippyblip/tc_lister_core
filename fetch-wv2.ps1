# WebView2 SDK (WebView2.h plus the static loader lib the plugins link against).
# Called by each plugin's fetch-deps.ps1, which then fetches its own libraries.
param([Parameter(Mandatory)][string]$Root)
$ErrorActionPreference = 'Stop'

if (Test-Path "$Root\vendor\wv2\build\native\include\WebView2.h") { return }
New-Item -ItemType Directory -Force "$Root\vendor" | Out-Null
Invoke-WebRequest 'https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2' -OutFile "$Root\vendor\wv2.zip"
Expand-Archive "$Root\vendor\wv2.zip" -DestinationPath "$Root\vendor\wv2" -Force
