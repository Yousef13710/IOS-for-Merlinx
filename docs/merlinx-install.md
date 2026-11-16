# Installing HakiOS on Xiaomi Redmi Note 9 (`merlinx`)

Complete guide to building and installing HakiOS (XNU 8796.121.2 / iOS 16.5) on the **Xiaomi Redmi Note 9 (`merlinx`, MediaTek Helio G85 MT6768, 3 GB RAM)** with on-eMMC `xnusys` (4.0 GiB) and persistent `iosdata` (`ext2`, ~51.9 GiB).

> [!WARNING]
> **ALL DATA ON YOUR PHONE WILL BE LOST.** Installing HakiOS completely deletes Android's `super`, `cache`, and `userdata` partitions and replaces them with `iosdata` (`ext2`) + `xnusys`. No backup is made.

## Automated Installer (Recommended)

Boot your `merlinx` into **any modern custom recovery with root ADB** (TWRP, OrangeFox, PitchBlack, SHRP, Lineage Recovery, etc.), connect USB, and run:
```bash
./setup-merlinx.sh
```
The ASCII TUI installer checks your host tools and recovery connection, guides you if anything is missing, fetches iOS 16.5, builds XNU/Mesa/userland/`boot-ios.img`, repartitions, formats `ext2`, and flashes the device.

---

## 1. Current Status (`merlinx`)

- **Works:**
  - 8-core SMP (`XNU_CORES=8`, A75 @ 2000 MHz, A55 @ 1800 MHz via `mtk_cpufreq=3`), Mali-G52 @ 1050 MHz (`GL=1` / Mesa Panfrost, `mali_mhz=1050`, wired `JOB_IRQ`), smooth UI (`cpu1` `backboardd` GL / `cpu2` `mali_worker` + app GL), tear-free double-buffered display (`fbscale=2 fbfit=1 fb_filter=0 fb_threads=4 fb_flip=1`).
  - Wallpapers: 20 crisp `750x1624` wallpapers (`WWDC 2022` + 19 Apple-style gradients/arcs) with live **Settings → Wallpaper** picker (`inject.c` + `inject_sb.c`, persisted to `/var/mobile/Library/Redwood/wallpaper.jpg`).
  - USB (`redwood_musb.c`, `mtk_usb=1`, `05ac:12a8` HS 480 Mbps) + `usbmux_relay` + `lockdownd` pairing (`idevice_id`, `idevicepair`, `ideviceinfo`, `idevicedate`, `idevicediagnostics restart` via `TOPRGU_SWRST`).
  - Touchscreen (`redwood_nvt.c`), power & volume buttons (`buttons=2`), battery gauge (`mtkbatt=1`).
  - Bluetooth (`connsys_bt=2`, `bluetoothd`, `BlueTool` — Settings → Bluetooth works and stays on).
  - Wi-Fi & NTP time sync (`connsys_step=17`, `WIFID=1`, `com.apple.timed`): scans, joins, and authenticates on Open & WPA2 networks (2.4 GHz & 5 GHz), DHCP, DNS, TCP/UDP, NTP.
  - Rear camera (`s5kgm1sp`, `mtkcam_skel=1`, live preview in `Camera.app`, currently laggy/tearing).
  - Persistent `/private/var` on **`ext2`** (`/dev/emmc1` → `iosdata`, ~51.9 GiB; `ext2` is used instead of `ext4` via XNU's custom `ext2` VFS driver) with 32 MB write-back cache, `CMD23`/`CMD25` multi-block eMMC writes, 2 s background dirty-block sync, per-inode `fsync`, and 32 KB read-ahead (**~60 s boot**; un-chopped icons, widgets).
  - Optional RAM-only `/private/var` mode (`no_ext2=1` boot-arg, non-persistent).
- **Known Limitations:**
  - **Live dynamic `PosterBoard` wallpapers** are not enabled (uses fast static `750x1624` wallpapers + **Settings → Wallpaper** picker).
  - **Safari / WebKit:** Stays blank/unresponsive despite working Wi-Fi/DNS/TCP.
  - **`Settings → General → About`:** Currently crashes `Preferences`.
  - **Not ported / unsupported:** Front camera, speakers/mic audio, cellular/baseband, `ext4` (use `ext2` for `iosdata`), and Apple online services (iCloud/App Store/activation).

---

## 2. Requirements

### Device
- Xiaomi Redmi Note 9 (`merlinx`, MT6768, 64 GB eMMC, 3 GB RAM)
- Unlocked bootloader
- **TWRP recovery** flashed to `recovery` (`mmcblk0p1`)

### Host (Linux x86_64)
- `adb`, `fastboot`, `python3` (with `venv`, `Pillow`, and `numpy`), `heif-convert` (`libheif-examples`, for rendering the `750x1624` `WWDC 2022` wallpaper), `git`, `curl`, `tar`, `xz`, `make`, `cc`, `clang`, `lld` (or run `./tools/get-zig.sh`), `flex`, `bison`, `m4`, `perl`, `autoconf`, `automake`, `meson`, `ninja`, `llvm-ar`, `7zz`/`7z`
- **Theos** (`$THEOS`, default `~/theos`) with its Linux toolchain (`toolchain/linux/iphone`) and `sdks/iPhoneOS16.5.sdk`
- Optional for USB tools on host: `usbmuxd` and `libimobiledevice` (`idevice_id`, `idevicepair`, `ideviceinfo`, `idevicediagnostics`)
- An iOS 16.5 IPSW (downloaded automatically via `./ios165/fetch.sh`) to generate `userland/root.apfs` and `userland/system.apfs` (`xnusys`) — no Apple binaries are included in this repo
- `prebuilt/loader.efi` (included in this repo) and a stock `merlinx` `boot.img` (used as the Android boot container for `tools/chainboot.py` — pull it from your phone in TWRP before flashing: `adb shell dd if=/dev/block/by-name/boot of=/tmp/stock_boot.img && adb pull /tmp/stock_boot.img`)

---

## 3. Partition Layout (`mmcblk0`)

On a 64 GB `merlinx`, `iosdata` + `xnusys` use the space previously occupied by Android `super`, `cache`, and `userdata`:

| Partition | Name | Sectors (512B) | Size | Purpose |
|---|---|---|---|---|
| `p1` | `recovery` | stock | stock | TWRP (keep!) |
| `p33` | `boot` | stock | 64 MiB | HakiOS `boot.img` (`mtkloader` + `payload.bin`) |
| `p43` | `iosdata` | `4767744 .. 113631231` | ~51.9 GiB | `ext2` (`-b 4096 -L iosdata`), mounted at `/private/var` |
| `p48` | `xnusys` | `113631232 .. 122021823` | 4.0 GiB | iOS 16.5 `packfs` rootfs (`/dev/emmc0`, read-only) |

All bootloader, TEE, modem/NV, and firmware partitions (`preloader`, `lk`, `tee`, `nvram`, `persist`, etc.) are untouched.

---

## 4. Building from Source

### A. Fetch iOS 16.5 (20F66) from Apple's IPSW
```bash
cd hakios
./ios165/fetch.sh
```
This downloads and extracts the iOS 16.5 rootfs, cryptexes, and `dyld_shared_cache` into `ios165/` (resumable if interrupted).

### B. Apply XNU patches, set up `xnu-deps`, & build the kernel
> [!IMPORTANT]
> `xnu/files/...` are copied (not symlinked) into `xnu-src/xnu-8796.121.2/`. Whenever you edit files under `xnu/files/`, copy them into `xnu-src/xnu-8796.121.2/` (or run `./xnu/apply.sh`) before building.

```bash
cd hakios
./xnu/apply.sh          # fetches Apple's xnu-8796.121.2 and applies redwood.patch + xnu/files/
./xnu-deps/setup.sh     # fetches/builds SDK, AvailabilityVersions, libdispatch, libpthread, host tools
./xnu-build.sh -j8 -k
```

### C. Build Mesa Panfrost (`libOSMesa.8.dylib` for `GL=1`)
```bash
cd hakios
DRIVERS="softpipe,panfrost" bash tools/mk-mesa.sh
```

### D. Build userland (`root.apfs` + `system.apfs` for `xnusys`)
```bash
cd hakios/userland
DRIVERS="softpipe,panfrost" GL=1 WIFID=1 RC=rc.launchd.aks \
  MORE_JOBS="com.apple.bluetoothd com.apple.BlueTool com.apple.tccd com.apple.mobile.keybagd com.apple.nanoregistryd com.apple.nanoprefsyncd com.apple.mediaserverd com.apple.mobile.lockdown com.apple.timed" \
  OSLOG="" \
  bash build.sh
```
This builds **both**:
- `userland/root.apfs` (init, launchd jobs, shims — embedded in `boot.img`)
- `userland/system.apfs` (the 4 GiB-compatible APFS iOS system image to flash to `xnusys`)

### E. Build `payload.bin`, `mtkloader.bin`, and `boot-ios.img`
```bash
cd hakios
python3 xnuloader/mkpayload.py \
  -o payload.bin \
  kernel=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 \
  loader=prebuilt/loader.efi \
  ramdisk=userland/root.apfs

bash mtkloader/build.sh \
  -DXNU_CORES=8 -DXNU_VERBOSE=0 \
  -D'XNU_EXTRA_ARGS="cpus=8 small_cores_after=0 buttons=2 mtk_cpufreq=3 a75_mhz=2000 a55_mhz=1800 mali=1 mali_mhz=1050 fbscale=2 fbfit=1 fb_filter=0 fb_threads=4 fb_flip=1 fb_beat=0 vblur=0 aks_keys=1 mtkbatt=1 connsys_step=17 connsys_bt=2 touchscreen=3 touch_poll_us=2000 mtkcam_skel=1 mtk_usb=1"'

python3 tools/chainboot.py \
  stock_boot.img \
  mtkloader/mtkloader.bin \
  boot-ios.img \
  payload.bin
```
*(To build a RAM-only `/private/var` image for faster ~2 min non-persistent boots, add `no_ext2=1` to `XNU_EXTRA_ARGS`.)*

---

## 5. Installing onto `merlinx` (via TWRP)

Boot the phone into **TWRP**, connect USB, and run `tools/install-merlinx.sh`:

### First-time install (repartition + format `iosdata` + flash `xnusys` + flash `boot`)
```bash
bash tools/install-merlinx.sh \
  --repartition \
  --format-iosdata \
  --xnusys userland/system.apfs \
  --boot boot-ios.img \
  --reboot
```

### Updating only the kernel / `boot.img` (keeps existing `iosdata` & `xnusys`)
```bash
bash tools/install-merlinx.sh --boot boot-ios.img --reboot
```

### Wiping & reformatting `iosdata` only
```bash
bash tools/install-merlinx.sh --format-iosdata --boot boot-ios.img --reboot
```

---

## 6. Troubleshooting & Logs

- Boot into TWRP (`Vol+ + Power`) and pull kernel/bootloader logs from `pstore`:
  ```bash
  adb pull /sys/fs/pstore/console-ramoops-2
  adb pull /sys/fs/pstore/pmsg-ramoops-0
  ```
- Inspect or repair `/private/var` (`iosdata`) offline in TWRP (for example after a forced `Vol+ + Power` reboot):
  ```bash
  adb shell "e2fsck -y /dev/block/by-name/iosdata"
  adb shell "mkdir -p /tmp/ios && mount -t ext2 -o ro /dev/block/by-name/iosdata /tmp/ios && ls -la /tmp/ios; umount /tmp/ios"
  ```
- Prefer `idevicediagnostics restart` over USB for clean warm reboots (`TOPRGU_SWRST`) so `ext2` stays clean.
- See [merlinx-bringup-notes.md](merlinx-bringup-notes.md) for full technical details of all kernel, eMMC, `ext2`, Mali, CONNSYS, and camera fixes.
