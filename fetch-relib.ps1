# relib -- the shared C++ library this core takes its win32 dark-mode, path and INI helpers from.
# Called by each plugin's fetch-deps.ps1, in the same way as fetch-wv2.ps1.
#
# Cloned into vendor\ rather than copied, so it can be updated rather than re-copied, and because
# vendor\ is already this family's fetched-dependency area (fetch-wv2.ps1, three.js, the test
# models). build.cmd defaults to vendor\relib, so a plugin that fetches this needs nothing else.
#
# NOTE: relib is a PRIVATE repository, so cloning it requires credentials. On a machine where `git`
# is already authenticated it just works; in CI it needs a token. Pass -Ref to pin a commit or tag
# instead of tracking main.
param(
  [Parameter(Mandatory)][string]$Root,
  [string]$Ref = 'main',
  [switch]$Update
)

$ErrorActionPreference = 'Stop'

$target = "$Root\vendor\relib"
$header = "$target\win32\dark_mode.h"

if (Test-Path $header) {
  if (-not $Update) { return }
  Write-Host 'updating relib'
  git -C $target fetch --depth 1 origin $Ref
  if ($LASTEXITCODE -ne 0) { throw "could not fetch relib ($Ref). Is git authenticated for the private repo?" }
  git -C $target checkout --detach FETCH_HEAD
  if ($LASTEXITCODE -ne 0) { throw 'could not check out the fetched relib revision' }
  return
}

New-Item -ItemType Directory -Force "$Root\vendor" | Out-Null

# --depth 1: the plugins need four headers, not relib's history.
git clone --depth 1 --branch $Ref https://github.com/blippyblip/relib.git $target
if ($LASTEXITCODE -ne 0) {
  throw @"
Could not clone relib.

relib is a private repository, so this needs git credentials (or a token in CI).
Alternatively build against a checkout you already have, by passing its path to build.cmd:

  call core\build.cmd "<plugin root>" <name> "C:\path\to\relib"
"@
}

if (-not (Test-Path $header)) { throw "the relib clone did not produce $header" }
