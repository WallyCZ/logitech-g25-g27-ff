param(
    [switch]$Uninstall
)

If ([IntPtr]::Size -ne 8) {
    Write-Host "Script is not running as 64bit!!"
    Exit
}

# Verify that user running script is an administrator, otherwise relaunch elevated
$IsAdmin = [Security.Principal.WindowsIdentity]::GetCurrent()
If ((New-Object Security.Principal.WindowsPrincipal $IsAdmin).IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator) -eq $FALSE) {
    $newProcess = New-Object System.Diagnostics.ProcessStartInfo "PowerShell"
    $newProcess.Arguments = "-ExecutionPolicy Bypass -NoExit -File `"$($myInvocation.MyCommand.Definition)`""
    if ($Uninstall) { $newProcess.Arguments += " -Uninstall" }
    $newProcess.Verb = "runas"
    [System.Diagnostics.Process]::Start($newProcess) | Out-Null
    exit
}

Set-Location $PSScriptRoot

Write-Host "Logitech G25/G27/DFGT force feedback driver for Windows 10/11 (https://github.com/WallyCZ/logitech-g25-g27-ff)"

$installDir = Join-Path $env:ProgramFiles "G25FF"
$regsvr64 = Join-Path $env:SystemRoot "System32\regsvr32.exe"
$regsvr32 = Join-Path $env:SystemRoot "SysWOW64\regsvr32.exe"
$rundll32 = Join-Path $env:SystemRoot "System32\rundll32.exe"

function Find-Dll($arch) {
    foreach ($path in "$PSScriptRoot\$arch\G25FF.dll", "$PSScriptRoot\bin\$arch\G25FF.dll") {
        if (Test-Path $path) { return $path }
    }
    return $null
}

function Register-Dll($regsvr, $dll, [switch]$Unregister) {
    $arguments = @("/s")
    if ($Unregister) { $arguments += "/u" }
    $process = Start-Process $regsvr -ArgumentList ($arguments + "`"$dll`"") -Wait -PassThru
    return $process.ExitCode -eq 0
}

# The agent (rundll32 G25FF.dll,Agent) keeps the installed DLL loaded
function Stop-Agent {
    Get-CimInstance Win32_Process -Filter "Name = 'rundll32.exe'" | Where-Object { $_.CommandLine -like "*G25FF.dll*Agent*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

# The original Logitech driver puts kernel filters on the wheel that Memory Integrity blocks, which leaves the
# wheel unusable. It has to go, so the wheel uses the Windows HID driver this driver is built for.
function Remove-LogitechDriver {
    $packages = @()
    $published = $null
    foreach ($line in (pnputil /enum-drivers)) {
        if ($line -match ':\s*(oem\d+\.inf)\s*$') { $published = $Matches[1] }
        elseif ($line -match ':\s*(wmjoyhid|wmvirhid|wmbenum)\.inf\s*$' -and $published) { $packages += @{ Published = $published; Original = $Matches[1] } }
    }
    if (-not $packages) { return $true }

    Write-Host ""
    Write-Host "The original Logitech wheel driver is installed:" -ForegroundColor Yellow
    $packages | ForEach-Object { Write-Host "  driver package $($_.Original).inf ($($_.Published))" }
    Write-Host "It must be removed, the wheel then uses the standard Windows HID driver."
    $confirmation = Read-Host "Remove the Logitech driver now? [Y/n]"
    if ($confirmation -ne "" -and $confirmation -ne "y" -and $confirmation -ne "Y") { return $false }
    foreach ($package in $packages) {
        pnputil /delete-driver $package.Published /uninstall /force | Out-Null
        Write-Host "Removed $($package.Original).inf"
    }
    pnputil /scan-devices | Out-Null
    return $true
}

if ($Uninstall) {
    Stop-Agent
    foreach ($arch in @(@{ Name = "x64"; Regsvr = $regsvr64 }, @{ Name = "x86"; Regsvr = $regsvr32 })) {
        $dll = Join-Path $installDir "$($arch.Name)\G25FF.dll"
        if (Test-Path $dll) { Register-Dll $arch.Regsvr $dll -Unregister | Out-Null }
    }
    Remove-Item $installDir -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "Driver removed."
    exit
}

$dll64 = Find-Dll "x64"
$dll32 = Find-Dll "x86"
if (-not $dll64 -or -not $dll32) {
    Write-Host "G25FF.dll not found next to this script (x64 and x86 folders). Download a release or run build.cmd first." -ForegroundColor Red
    exit
}

if (-not (Remove-LogitechDriver)) {
    Write-Host "Installation cancelled, the driver cannot work while the Logitech driver is installed." -ForegroundColor Red
    exit
}

# Copy both builds: 64-bit games load x64, 32-bit games load x86
Stop-Agent
try {
    foreach ($arch in "x64", "x86") {
        New-Item -ItemType Directory -Force (Join-Path $installDir $arch) | Out-Null
    }
    Copy-Item $dll64 (Join-Path $installDir "x64\G25FF.dll") -Force -ErrorAction Stop
    Copy-Item $dll32 (Join-Path $installDir "x86\G25FF.dll") -Force -ErrorAction Stop
}
catch {
    Write-Host "Cannot copy the driver to $installDir. Close all games and controller tools and run the script again." -ForegroundColor Red
    exit
}

if (-not (Register-Dll $regsvr64 (Join-Path $installDir "x64\G25FF.dll")) -or -not (Register-Dll $regsvr32 (Join-Path $installDir "x86\G25FF.dll"))) {
    Write-Host "Registration failed." -ForegroundColor Red
    exit
}
Write-Host "Driver installed to $installDir" -ForegroundColor Green

# The agent switches the wheel to native mode whenever it is plugged in; it starts at logon, start it now too
# (no -Wait: Start-Process would wait for the agent too, which keeps running)
Start-Process $rundll32 -ArgumentList "`"$(Join-Path $installDir 'x64\G25FF.dll')`",StartAgent"
Write-Host "Done. Test force feedback in the Game Controllers panel (joy.cpl), on the wheel's Force Feedback tab, or in a game."
