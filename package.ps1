# Builds the driver and creates the release files in dist\:
#   G25FF-<version>.msi  installer (WiX 5, pinned in dotnet-tools.json)
#   G25FF-<version>.zip  Install.ps1, README.md, x64\G25FF.dll, x86\G25FF.dll
param(
    [Parameter(Mandatory = $true)][string]$Version
)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

# MSI versions are numeric: "2.0" or "v2.0.1" become 2.0 / 2.0.1; the DLLs get the same version
$msiVersion = $Version.TrimStart("v") -replace "[^0-9.].*$", ""
$env:G25FF_VERSION = $msiVersion
cmd /c "`"$PSScriptRoot\build.cmd`""
if ($LASTEXITCODE -ne 0) { throw "build failed" }
New-Item -ItemType Directory -Force dist | Out-Null

dotnet tool restore | Out-Null
dotnet tool run wix extension add WixToolset.UI.wixext/5.0.2 | Out-Null
$msi = Join-Path $PSScriptRoot "dist\G25FF-$Version.msi"
dotnet tool run wix build installer\G25FF.wxs -arch x64 -ext WixToolset.UI.wixext -d "Version=$msiVersion" -d "BinDir=bin" -pdbtype none -o $msi
if ($LASTEXITCODE -ne 0) { throw "MSI build failed" }
Write-Host "Created $msi"

$stage = Join-Path $PSScriptRoot "obj\package"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
foreach ($arch in "x64", "x86") {
    New-Item -ItemType Directory -Force (Join-Path $stage $arch) | Out-Null
    Copy-Item "bin\$arch\G25FF.dll" (Join-Path $stage $arch)
}
Copy-Item Install.ps1, README.md $stage
$zip = Join-Path $PSScriptRoot "dist\G25FF-$Version.zip"
Compress-Archive -Path "$stage\*" -DestinationPath $zip -Force
Write-Host "Created $zip"
