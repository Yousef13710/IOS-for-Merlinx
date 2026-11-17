# iOS 16.5 for Xiaomi Redmi Note 9 (`merlinx`)

**Real Apple Darwin/XNU (`xnu-8796.121.2`) + unmodified iOS 16.5 userland running natively on the Xiaomi Redmi Note 9 (`merlinx`, MediaTek Helio G85 MT6768).**

Based on [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel), this port runs full iOS 16.5 (`launchd` → `backboardd` → `SpringBoard` → apps) with GPU acceleration, Wi-Fi, Bluetooth, persistent `ext2` storage, USB `usbmuxd`/`libimobiledevice` support, and automatic detection for both **3 GB / 4 GB RAM** and **64 GB / 128 GB eMMC** variants.

> [Install Guide](docs/merlinx-install.md) · [Bring-up Notes](docs/merlinx-bringup-notes.md) · [TWRP Installer](tools/install-merlinx.sh) · [Upstream Docs](docs/README.md)

---

## Screenshots

Captured directly from a Redmi Note 9 (`merlinx`) over USB using `idevicescreenshot`:

| Home Screen | App Library | Control Center | Settings |
|:---:|:---:|:---:|:---:|
| ![Home Screen](docs/screenshots/homescreen.png) | ![App Library](docs/screenshots/app-library.png) | ![Control Center](docs/screenshots/control-center.png) | ![Settings](docs/screenshots/settings.png) |

---

## Hardware & Feature Support

| Subsystem | Status | Details |
|---|---|---|
| **CPU & Boot** | Working | `mtkloader` on MT6768, all **8 cores** (`A75 @ 2000 MHz`, `A55 @ 1800 MHz`), auto-detected **3 GB / 4 GB RAM**, warm reboot via `TOPRGU_SWRST` |
| **Graphics & UI** | Working | In-kernel **Mali-G52** (`1050 MHz` OC, wired `JOB_IRQ`) + Mesa **Panfrost** (`-O3` + persistent shader cache), tear-free double-buffered page flipping |
| **Storage** | Working | Read-only `system.apfs` (`xnusys`, 4.0 GiB) + persistent read-write `/private/var` on **`ext2`** (`iosdata`, auto-sized ~51.9 GiB / ~110 GiB), **~2 min boot** (`no_ext2=1` for RAM-only) |
| **Touch & Buttons** | Working | Novatek touchscreen (`redwood_nvt.c`), power & volume keys (`redwood_mtkkeys.c`) |
| **Wi-Fi & Time** | Working | CONNSYS (`2.4 GHz` & `5 GHz`, Open & WPA2), DHCP on `en0`, DNS/TCP/UDP, NTP via `com.apple.timed` |
| **Bluetooth** | Working | `connsys_bt=2` + `bluetoothd` + `BlueTool` + `securityd`/`keybagd` |
| **USB (`libimobiledevice`)** | Working | MUSB HS `480 Mbps` (`05ac:12a8`), `usbmux_relay`, `lockdownd` pairing, `ideviceinfo`, `idevicescreenshot`, `idevicediagnostics restart` |
| **Wallpapers** | Working | 20 `750x1624` wallpapers (`WWDC 2022` + Apple-style gradients) with live **Settings → Wallpaper** picker |
| **Rear Camera & Battery** | Partial / Working | Rear `s5kgm1sp` live preview in `Camera.app` (currently laggy/tearing); real battery percentage in status bar |

### Known Limitations
- **Not yet working:** Safari/WebKit rendering, `Settings → General → About` (crashes `Preferences`), live `PosterBoard` wallpapers, and extra USB services (`afcd`, `os_trace_relay`, `mobile_installation_proxy`).
- **Unsupported / not ported:** Front camera, speaker/mic audio, cellular/baseband, `ext4` (`iosdata` uses `ext2`), and Apple online services (activation, iCloud, App Store — no Secure Enclave).

---

## Install

> [!WARNING]
> Installing HakiOS on `merlinx` **replaces Android's `super`, `cache`, and `userdata`** with `xnusys` (4.0 GiB) + `iosdata` (`ext2`). **All data on the phone will be erased.** TWRP in `recovery` is preserved.

### Requirements
- **Phone:** Xiaomi Redmi Note 9 (`merlinx`, 3 GB / 4 GB RAM, 64 GB / 128 GB eMMC) with an **unlocked bootloader** and **custom recovery with root ADB** (TWRP, OrangeFox, PitchBlack, SHRP, etc.).
- **PC:** Linux `x86_64` with **~18–20 GB free disk space** during build (~2 GB after optional post-install cleanup) and a USB cable.

### Step-by-Step
1. **Clone the repository:**
   ```bash
   git clone -b mtk https://github.com/Yousef13710/IOS-for-Merlinx.git
   cd IOS-for-Merlinx
   ```
2. **Boot your phone into Recovery** (hold **Vol+ + Power**), connect it to your PC via USB, and confirm ADB sees it:
   ```bash
   adb devices
   ```
   *(It should show `<serial> recovery`. In TWRP/OrangeFox, make sure ADB is enabled.)*
3. **Run the installer:**
   ```bash
   ./setup-merlinx.sh
   ```
   - Choose **`1) Full Install`** and type **`YES`** when prompted.
   - The script automatically checks/guides you on host tools (Theos + `iPhoneOS16.5.sdk`, `clang`, `python3`, etc.), downloads the official iOS 16.5 (20F66) IPSW, builds XNU + Mesa Panfrost + userland, packs `boot-ios.img`, auto-detects your 64 GB / 128 GB eMMC layout, formats `ext2`, and flashes everything.
   - At the end, it asks if you want to **clean up ~8 GB** of temporary iOS/build files (`y`) or keep them (`N`) for faster future rebuilds.
4. **Wait ~2 minutes on boot:** The phone reboots into `mtkloader` → XNU → iOS 16.5.

For manual build/flash commands and troubleshooting, see **[docs/merlinx-install.md](docs/merlinx-install.md)**.

> [!TIP]
> Always use `idevicediagnostics restart` over USB for clean reboots instead of holding Power/Volume keys so the persistent `ext2` (`iosdata`) filesystem stays clean.

---

## Repositories & Legal

- **[IOS-for-Merlinx](https://github.com/Yousef13710/IOS-for-Merlinx)** — Redmi Note 9 (`merlinx`) port (`mtk` branch)
- **[hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel)** — Upstream HakiOS repository (`X6826B`)

No Apple binaries or vendor firmware are hosted in this repository (`./ios165/fetch.sh` downloads the official IPSW during setup). XNU patches are under the **Apple Public Source License 2.0**; original loader, driver, and userland code is under **MIT**. Not affiliated with or endorsed by Apple.
