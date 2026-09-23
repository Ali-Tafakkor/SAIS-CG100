param(
    [Parameter(Mandatory=$true)][string]$PythonEmbedZip,
    [Parameter(Mandatory=$true)][string]$OpenOcdRoot,
    [string]$Tag = 'local'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$pythonZip = (Resolve-Path -LiteralPath $PythonEmbedZip).Path
$openOcd = (Resolve-Path -LiteralPath $OpenOcdRoot).Path
$expectedPythonHash = '90B4E5B9898B72D744650524BFF92377C367F44BD5FBD09E3148656C080AD907'
if ((Get-FileHash -LiteralPath $pythonZip -Algorithm SHA256).Hash -ne $expectedPythonHash) {
    throw 'Embedded Python archive does not match the pinned official SHA-256.'
}
foreach ($path in @('bin/openocd.exe','bin/libusb-1.0.dll','bin/libftdi1.dll','openocd/scripts/interface/stlink.cfg','openocd/scripts/target/stm32h7x.cfg')) {
    if (-not (Test-Path -LiteralPath (Join-Path $openOcd $path))) { throw "OpenOCD runtime is incomplete: $path" }
}
$stamp = (Get-Date).ToString('yyyyMMdd-HHmmss-fff')
$stageParent = Join-Path $root "dist/stage-$stamp"
$stage = Join-Path $stageParent 'SAIS-CG100-Programmer'
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'installer/hardware'),(Join-Path $stage 'installer/web'),(Join-Path $stage 'scripts'),(Join-Path $stage 'docs'),(Join-Path $stage 'runtime/python'),(Join-Path $stage 'runtime/openocd') | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'installer/Run-Programmer.cmd') -Destination (Join-Path $stage 'Run-Programmer.cmd')
Copy-Item -Path (Join-Path $root 'installer/*.py') -Destination (Join-Path $stage 'installer')
Copy-Item -LiteralPath (Join-Path $root 'installer/download.ps1') -Destination (Join-Path $stage 'installer')
Copy-Item -LiteralPath (Join-Path $root 'installer/hardware/board.tcl') -Destination (Join-Path $stage 'installer/hardware')
Copy-Item -Path (Join-Path $root 'installer/web/*') -Destination (Join-Path $stage 'installer/web')
Copy-Item -LiteralPath (Join-Path $root 'scripts/stlink-h750.cfg') -Destination (Join-Path $stage 'scripts')
Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination (Join-Path $stage 'README.md')
Copy-Item -Path (Join-Path $root 'docs/*.md') -Destination (Join-Path $stage 'docs')
Copy-Item -LiteralPath (Join-Path $root 'installer/Run-Fleet-Updater.cmd') -Destination (Join-Path $stage 'Run-Fleet-Updater.cmd')
$releaseFolder = Join-Path $root 'dist/release'
if (-not (Test-Path -LiteralPath (Join-Path $releaseFolder 'recovery.bin'))) { throw 'Build the complete firmware release before packaging.' }
$firmwareManifest = Get-Content -LiteralPath (Join-Path $releaseFolder 'firmware-manifest.json') -Raw | ConvertFrom-Json
if ($firmwareManifest.release_tag -ne $Tag) { throw 'Package tag must match the included firmware manifest.' }
Copy-Item -LiteralPath $releaseFolder -Destination (Join-Path $stage 'firmware-release') -Recurse
$Tag | Set-Content -LiteralPath (Join-Path $stage 'PACKAGE_VERSION.txt') -Encoding ascii
Expand-Archive -LiteralPath $pythonZip -DestinationPath (Join-Path $stage 'runtime/python')
@('python313.zip','.','../../installer') | Set-Content -LiteralPath (Join-Path $stage 'runtime/python/python313._pth') -Encoding ascii
foreach ($path in @('bin','openocd/scripts','distro-info/licenses')) {
    $source = Join-Path $openOcd $path
    $destination = Join-Path (Join-Path $stage 'runtime/openocd') (Split-Path $path -Parent)
    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Recurse
}
Copy-Item -LiteralPath (Join-Path $openOcd 'README.md') -Destination (Join-Path $stage 'runtime/openocd/README.md')
$python = Join-Path $stage 'runtime/python/python.exe'
& $python -c 'import ctypes, hashlib, hmac, http.server, json, socket, credentials, ethernet, fleet_agent, launcher'
if ($LASTEXITCODE -ne 0) { throw 'Packaged Python runtime validation failed' }
foreach ($cmdFile in Get-ChildItem -LiteralPath $stage -Filter '*.cmd') {
    $cmdText = [System.IO.File]::ReadAllText($cmdFile.FullName).Replace("`r`n", "`n").Replace("`n", "`r`n")
    [System.IO.File]::WriteAllText($cmdFile.FullName, $cmdText, [System.Text.Encoding]::ASCII)
}
$zip = Join-Path $root 'dist/SAIS-CG100-Programmer-win-x64.zip'
Compress-Archive -LiteralPath $stage -DestinationPath $zip -Force
[System.IO.File]::WriteAllText((Join-Path $root 'dist/latest-package.txt'), "stage-$stamp\SAIS-CG100-Programmer", [System.Text.Encoding]::ASCII)
Write-Output $zip
Get-FileHash -LiteralPath $zip -Algorithm SHA256 | Select-Object Hash,Path
