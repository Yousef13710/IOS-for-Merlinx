# Windows Installer Launcher for HakiOS on Xiaomi Redmi Note 9 (merlinx)
# Usage (PowerShell):
#   powershell -ExecutionPolicy Bypass -File .\install-windows.ps1

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$WinTools = Join-Path $RepoRoot ".win-tools"
$PlatformTools = Join-Path $WinTools "platform-tools"
$LocalAdb = Join-Path $PlatformTools "adb.exe"

Write-Host ""
Write-Host "=== HakiOS for merlinx - Windows Setup ===" -ForegroundColor Cyan
Write-Host ""

# 1. Check WSL2 + Ubuntu
$wslCmd = Get-Command wsl.exe -ErrorAction SilentlyContinue
if (-not $wslCmd) {
    Write-Host "[!] WSL is not available. Please enable Windows Subsystem for Linux (Windows 10 2004+ / Windows 11)." -ForegroundColor Red
    exit 1
}

$distros = @(& wsl.exe -l -q 2>$null | ForEach-Object { ($_ -replace "`0", "").Trim() } | Where-Object { $_ -ne "" })
if ($distros.Count -eq 0 -or -not ($distros | Where-Object { $_ -match "^Ubuntu" })) {
    Write-Host "[*] Installing WSL2 Ubuntu (first-time setup)..." -ForegroundColor Yellow
    & wsl.exe --install -d Ubuntu
    Write-Host ""
    Write-Host "[!] Reboot Windows to finish WSL2 Ubuntu setup, open Ubuntu once to create your Linux user, then re-run .\install-windows.ps1." -ForegroundColor Yellow
    exit 0
}

$UbuntuDistro = ($distros | Where-Object { $_ -match "^Ubuntu" } | Select-Object -First 1)
Write-Host "[+] Using WSL distro: $UbuntuDistro" -ForegroundColor Green

# 2. Find or download Windows adb.exe (no usbipd needed!)
$AdbExe = $null
$adbInPath = Get-Command adb.exe -ErrorAction SilentlyContinue
if ($adbInPath) {
    $AdbExe = $adbInPath.Source
} elseif (Test-Path "C:\platform-tools\adb.exe") {
    $AdbExe = "C:\platform-tools\adb.exe"
} elseif (Test-Path $LocalAdb) {
    $AdbExe = $LocalAdb
} else {
    Write-Host "[*] Downloading Google Android Platform Tools (adb.exe)..." -ForegroundColor Yellow
    New-Item -ItemType Directory -Force -Path $WinTools | Out-Null
    $zipPath = Join-Path $WinTools "platform-tools.zip"
    Invoke-WebRequest -Uri "https://dl.google.com/android/repository/platform-tools-latest-windows.zip" -OutFile $zipPath
    Expand-Archive -Path $zipPath -DestinationPath $WinTools -Force
    Remove-Item -Force $zipPath
    $AdbExe = $LocalAdb
}
Write-Host "[+] Using Windows ADB: $AdbExe" -ForegroundColor Green

# 3. Check recovery connection on Windows USB
while ($true) {
    $state = (& $AdbExe get-state 2>$null | Out-String).Trim()
    if ($state -eq "recovery") {
        Write-Host "[+] Phone detected in recovery!" -ForegroundColor Green
        break
    }
    Write-Host "[!] No phone detected in recovery (state: '$state')." -ForegroundColor Yellow
    Write-Host "    1. Boot your merlinx into TWRP / custom recovery (Vol+ + Power) and plug in USB."
    Write-Host "    2. Make sure ADB is enabled in recovery (and Android/Google USB driver is installed in Device Manager)."
    $ans = Read-Host "Press [Enter] to retry, or type 'c' to continue anyway (e.g. build-only)"
    if ($ans -eq "c" -or $ans -eq "C") { break }
}

# 4. Sync repo into WSL native Linux filesystem (~/IOS-for-Merlinx) so hardlinks, permissions, and builds are fast & reliable
$RepoWinEscaped = $RepoRoot -replace '\\', '/'
$AdbWinEscaped = $AdbExe -replace '\\', '/'

Write-Host "[*] Preparing native WSL workspace (~/IOS-for-Merlinx)..." -ForegroundColor Cyan
$prepScript = @"
set -e
WIN_REPO=`$(wslpath -u '$RepoWinEscaped')
WIN_ADB=`$(wslpath -u '$AdbWinEscaped')
TARGET="`$HOME/IOS-for-Merlinx"
if ! command -v rsync >/dev/null 2>&1 || ! command -v clang >/dev/null 2>&1; then
    echo "[*] Installing required Ubuntu build packages (sudo password may be asked once)..."
    sudo apt-get update
    sudo apt-get install -y rsync python3 python3-venv python3-pil python3-numpy libheif-examples git curl tar xz-utils build-essential clang lld flex bison m4 perl autoconf automake meson ninja-build llvm 7zip
fi
mkdir -p "`$TARGET"
rsync -a --delete --exclude='.win-tools' --exclude='ios165/rootfs' --exclude='ios165/20F66__*' --exclude='ios165/cryptex*' --exclude='xnu-src' --exclude='xnu-deps' --exclude='userland/system.apfs' --exclude='userland/sysroot' "`$WIN_REPO/" "`$TARGET/"
cd "`$TARGET"
export ADB_BIN="`$WIN_ADB"
exec bash ./setup-merlinx.sh
"@

$prepBytes = [System.Text.Encoding]::UTF8.GetBytes(($prepScript -replace "`r`n", "`n"))
$prepB64 = [Convert]::ToBase64String($prepBytes)
& wsl.exe -d $UbuntuDistro -- bash -lc "echo '$prepB64' | base64 -d | bash"
