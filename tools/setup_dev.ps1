# One-time setup of a Windows PC for developing the MFD (firmware, bridge, preview, releases).
# Run from PowerShell in the project folder:   powershell -ExecutionPolicy Bypass -File tools\setup_dev.ps1
# Installs (if missing): Git, Python 3.11, Inno Setup 6, then the Python packages.

$ErrorActionPreference = "Stop"

function Have($cmd) { return [bool](Get-Command $cmd -ErrorAction SilentlyContinue) }

function WingetInstall($id, $name) {
    Write-Host "Installing $name ..."
    winget install --id $id -e --silent --accept-package-agreements --accept-source-agreements
}

if (-not (Have winget)) { throw "winget is missing. Install 'App Installer' from the Microsoft Store, then run this again." }

if (-not (Have git)) { WingetInstall "Git.Git" "Git" }
if (-not (Have python)) {
    WingetInstall "Python.Python.3.11" "Python 3.11"
    $env:Path = [Environment]::GetEnvironmentVariable("Path", "User") + ";" + [Environment]::GetEnvironmentVariable("Path", "Machine")
}
$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ }
if (-not $iscc) { WingetInstall "JRSoftware.InnoSetup" "Inno Setup 6" }

Write-Host "Installing Python packages ..."
python -m pip install --upgrade pip
python -m pip install platformio -r bridge\requirements.txt pyinstaller esptool pillow ziglang pefile

# Airport database cache for the bridge (only if it wasn't copied along).
if (-not (Test-Path "bridge\data\navdb.pkl.gz")) {
    Write-Host "Building the airport database (downloads ~20 MB) ..."
    Push-Location bridge; python navdb.py; Pop-Location
}

Write-Host ""
Write-Host "Done. Next:"
Write-Host "  python -m platformio run          first firmware build: 20-40 min (downloads ESP-IDF, rebuilds the core)"
Write-Host "  python tools\ui_preview\preview.py   render all pages on the PC"
Write-Host "  python tools\make_release.py         build the installer"
Write-Host "Read CLAUDE.md before changing the display driver or the build settings."
