# SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
# SPDX-License-Identifier: GPL-3.0-or-later

param(
    [Parameter(Mandatory = $true)] [string] $Stage,
    [Parameter(Mandatory = $true)] [string] $Version,
    [Parameter(Mandatory = $true)] [string] $Output,
    [Parameter(Mandatory = $true)] [string] $WorkDirectory
)

$ErrorActionPreference = "Stop"
$Stage = (Resolve-Path $Stage).Path
$templateDirectory = Join-Path $PSScriptRoot "windows-installer"
$configDirectory = Join-Path $WorkDirectory "config"
$packagesDirectory = Join-Path $WorkDirectory "packages"
$componentId = "nl.ivofilot.p2000m.vid2vga.viewer"
if ($componentId -notmatch '^[A-Za-z0-9_]+(\.[A-Za-z0-9_]+)*$') {
    throw "Invalid Qt Installer Framework component ID: $componentId"
}
$packageDirectory = Join-Path $packagesDirectory $componentId
$metaDirectory = Join-Path $packageDirectory "meta"
$dataDirectory = Join-Path $packageDirectory "data"

# These directories contain generated input only. Recreate them so a renamed or
# removed component cannot remain in a reused work directory.
foreach ($generatedDirectory in @($configDirectory, $packagesDirectory)) {
    if (Test-Path $generatedDirectory) {
        Remove-Item $generatedDirectory -Recurse -Force
    }
}
New-Item -ItemType Directory -Force $configDirectory, $metaDirectory, $dataDirectory | Out-Null
Copy-Item (Join-Path $Stage "*") $dataDirectory -Recurse -Force
Copy-Item (Join-Path $PSScriptRoot "../assets/p2000m-vid2vga-viewer.ico") $configDirectory
Copy-Item (Join-Path $PSScriptRoot "../assets/p2000m-vid2vga-viewer-icon.png") $configDirectory
Copy-Item (Join-Path $templateDirectory "installscript.qs") $metaDirectory

$config = (Get-Content (Join-Path $templateDirectory "config.xml") -Raw -Encoding UTF8).
    Replace("@VERSION@", $Version)
Set-Content (Join-Path $configDirectory "config.xml") $config -Encoding UTF8

$package = (Get-Content (Join-Path $templateDirectory "package.xml") -Raw -Encoding UTF8).
    Replace("@COMPONENT_ID@", $componentId).
    Replace("@VERSION@", $Version).
    Replace("@RELEASE_DATE@", (Get-Date -Format "yyyy-MM-dd"))
Set-Content (Join-Path $metaDirectory "package.xml") $package -Encoding UTF8

Copy-Item (Join-Path $Stage "licenses/GPL-3.0-or-later.txt") `
    (Join-Path $metaDirectory "LICENSE-GPL-3.0-or-later.txt")
Copy-Item (Join-Path $Stage "licenses/ffmpeg/FFMPEG-COPYING.GPLv3") $metaDirectory
Copy-Item (Join-Path $Stage "licenses/ffmpeg/X264-COPYING") $metaDirectory

$binaryCreator = (Get-Command binarycreator -ErrorAction Stop).Source
New-Item -ItemType Directory -Force (Split-Path $Output -Parent) | Out-Null
& $binaryCreator --offline-only -c (Join-Path $configDirectory "config.xml") `
    -p $packagesDirectory $Output
if ($LASTEXITCODE -ne 0) {
    throw "binarycreator failed with exit code $LASTEXITCODE"
}
if (-not (Test-Path $Output -PathType Leaf) -or (Get-Item $Output).Length -eq 0) {
    throw "Windows installer was not created: $Output"
}
