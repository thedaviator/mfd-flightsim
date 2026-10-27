#!/usr/bin/env python3
"""Build everything an end user needs, into release/:

    release/MFD-Bridge-Setup-<version>.exe   Windows installer (bridge app + firmware + guide)
    release/webflash/                        browser firmware installer (host on any https site)
    release/firmware/                        the four flash parts + parts.json (offsets)

Steps: firmware build (PlatformIO) -> flash parts -> airport database cache -> branding ->
PyInstaller (one folder, windowed) -> Inno Setup.

    pip install pyinstaller esptool pillow
    python tools/make_release.py              # everything
    python tools/make_release.py --no-installer
Needs Inno Setup 6 (winget install JRSoftware.InnoSetup) for the installer step.
"""
import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
REL = os.path.join(ROOT, "release")
BUILD = os.path.join(REL, "build")
ENV = "waveshare_7"


def run(cmd, **kw):
    print(">", " ".join(str(c) for c in cmd))
    subprocess.run(cmd, check=True, **kw)


def app_version():
    src = open(os.path.join(ROOT, "bridge", "mfd_bridge_app.py"), encoding="utf-8").read()
    return re.search(r'APP_VERSION = "([^"]+)"', src).group(1)


def build_firmware():
    # pioarduino's setup refuses to run under MSYS / Git Bash; hide those variables.
    env = {k: v for k, v in os.environ.items() if not k.startswith(("MSYSTEM", "MINGW"))}
    run([sys.executable, "-m", "platformio", "run", "-e", ENV], cwd=ROOT, env=env)
    out = os.path.join(ROOT, ".pio", "build", ENV)
    pio_home = os.environ.get("PLATFORMIO_CORE_DIR", os.path.join(os.path.expanduser("~"), ".platformio"))
    boot_app0 = os.path.join(pio_home, "packages", "framework-arduinoespressif32", "tools", "partitions", "boot_app0.bin")
    # Flashed as separate parts at their own offsets - never as one merged image: a merged
    # image fills the gaps with 0xFF, and the gap at 0x9000 is the NVS partition holding the
    # display's WiFi settings.
    fw_dir = os.path.join(BUILD, "firmware")
    shutil.rmtree(fw_dir, ignore_errors=True)
    os.makedirs(fw_dir)
    parts = [("0x0", os.path.join(out, "bootloader.bin")), ("0x8000", os.path.join(out, "partitions.bin")),
             ("0xe000", boot_app0), ("0x10000", os.path.join(out, "firmware.bin"))]
    listing = []
    for offset, src in parts:
        shutil.copy(src, fw_dir)
        listing.append({"offset": offset, "file": os.path.basename(src)})
    cfg = open(os.path.join(ROOT, "src", "config.h"), encoding="utf-8").read()
    version = re.search(r'#define FW_VERSION\s+"([^"]+)"', cfg).group(1)
    with open(os.path.join(fw_dir, "parts.json"), "w") as f:
        json.dump({"version": version, "parts": listing}, f, indent=2)
    # The ELF of every release, for decoding crash reports (addr2line).
    os.makedirs(os.path.join(REL, "elf"), exist_ok=True)
    shutil.copy(os.path.join(out, "firmware.elf"), os.path.join(REL, "elf", f"firmware-{version.split()[-1]}.elf"))
    shutil.rmtree(os.path.join(REL, "firmware"), ignore_errors=True)
    shutil.copytree(fw_dir, os.path.join(REL, "firmware"))
    return fw_dir


def build_navdb():
    cache = os.path.join(ROOT, "bridge", "data", "navdb.pkl.gz")
    if not os.path.exists(cache):
        run([sys.executable, "navdb.py"], cwd=os.path.join(ROOT, "bridge"))
    return cache


def build_branding():
    """AeroRelics icon, app banner and installer images (tools/make_branding.py)."""
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import make_branding
    return make_branding.build_app_assets(os.path.join(BUILD, "branding"))


def build_app(firmware, navdb_cache, brand):
    icon = brand["ico"]
    sep = os.pathsep
    guide = os.path.join(ROOT, "docs", "QUICKSTART.html")
    work = os.path.join(REL, "pyi-work")
    run([sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--windowed", "--onedir",
         "--name", "MFD Bridge", "--icon", icon,
         "--distpath", BUILD, "--workpath", work, "--specpath", work,
         "--paths", os.path.join(ROOT, "bridge"),
         "--add-data", f"{navdb_cache}{sep}data",
         "--add-data", f"{firmware}{sep}firmware",       # the parts folder
         "--add-data", f"{guide}{sep}.",
         "--add-data", f"{icon}{sep}.",
         "--add-data", f"{brand['banner']}{sep}.",
         "--collect-all", "SimConnect",
         "--collect-data", "esptool",
         "--hidden-import", "sim_msfs",
         "--hidden-import", "websockets.asyncio.server",
         "--hidden-import", "serial.tools.list_ports",
         os.path.join(ROOT, "bridge", "mfd_bridge_app.py")])
    return os.path.join(BUILD, "MFD Bridge")


VC_REDIST_URL = "https://aka.ms/vs/17/release/vc_redist.x64.exe"


def fetch_vc_redist():
    """Microsoft's VC++ 2015-2022 x64 runtime installer, bundled so setup can install it offline."""
    path = os.path.join(REL, "cache", "vc_redist.x64.exe")
    if not os.path.exists(path):
        import urllib.request
        os.makedirs(os.path.dirname(path), exist_ok=True)
        print(f"Downloading {VC_REDIST_URL}")
        urllib.request.urlretrieve(VC_REDIST_URL, path + ".part")
        os.replace(path + ".part", path)
    # Refuse anything not signed by Microsoft.
    ps = ("$s = Get-AuthenticodeSignature -LiteralPath '%s'; "
          "if ($s.Status -ne 'Valid' -or $s.SignerCertificate.Subject -notlike '*O=Microsoft Corporation*') { exit 1 }" % path)
    if subprocess.run(["powershell", "-NoProfile", "-Command", ps]).returncode != 0:
        os.remove(path)
        sys.exit("vc_redist.x64.exe is not a valid Microsoft-signed file; deleted it")
    return path


def build_installer(app_dir, version):
    iscc = shutil.which("iscc") or next(iter(glob.glob(os.path.join(os.environ.get("LOCALAPPDATA", ""), "Programs",
                                                                   "Inno Setup 6", "ISCC.exe")) +
                                             glob.glob(r"C:\Program Files (x86)\Inno Setup 6\ISCC.exe")), None)
    if not iscc:
        print("Inno Setup not found; skipping the installer (winget install JRSoftware.InnoSetup)")
        return
    vc = fetch_vc_redist()
    brand = os.path.join(BUILD, "branding")
    run([iscc, f"/DAppVersion={version}", f"/DSourceDir={app_dir}", f"/DVCRedist={vc}", f"/DBrandDir={brand}",
         os.path.join(ROOT, "installer", "mfd_bridge.iss")])


def build_webflash(firmware, version):
    out = os.path.join(REL, "webflash")
    os.makedirs(out, exist_ok=True)
    shutil.copy(os.path.join(ROOT, "webflash", "index.html"), out)
    with open(os.path.join(firmware, "parts.json")) as f:
        listing = json.load(f)["parts"]
    for p in listing:
        shutil.copy(os.path.join(firmware, p["file"]), out)
    for old in glob.glob(os.path.join(out, "mfd7-firmware.bin")):
        os.remove(old)
    manifest = {
        "name": "Flight-sim MFD",
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": "ESP32-S3",
                    "parts": [{"path": p["file"], "offset": int(p["offset"], 16)} for p in listing]}],
    }
    with open(os.path.join(out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-installer", action="store_true")
    args = ap.parse_args()
    version = app_version()
    os.makedirs(BUILD, exist_ok=True)
    firmware = build_firmware()
    cache = build_navdb()
    brand = build_branding()
    app_dir = build_app(firmware, cache, brand)
    build_webflash(firmware, version)
    if not args.no_installer:
        build_installer(app_dir, version)
    print(f"\nRelease {version} in {REL}")
    for p in sorted(glob.glob(os.path.join(REL, "*.exe"))) + [os.path.join(REL, "webflash"), os.path.join(REL, "firmware")]:
        print("  ", p)


if __name__ == "__main__":
    main()
