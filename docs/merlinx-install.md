# Installing HakiOS on Xiaomi Redmi Note 9 (`merlinx`)

Complete guide to building and installing HakiOS (XNU 8796.121.2 / iOS 16.5) on the **Xiaomi Redmi Note 9 (`merlinx`, MediaTek Helio G85 MT6768, 3 GB RAM)** with on-eMMC `xnusys` (4.0 GiB) and persistent `iosdata` (`ext2`, ~51.9 GiB).

> [!WARNING]
> **This replaces Android's `super`, `cache`, and `userdata` partitions** when setting up `iosdata` + `xnusys`. Back up your data and GPT partition table first. Keep **TWRP** in `recovery` so you always have root ADB access and `/sys/fs/pstore` logs.

---

## 1. Current Status (`merlinx`)

- **Works:**
  - 8-core SMP (`XNU_CORES=8`, A75 @ 2000 MHz, A55 @ 1800 MHz via `mtk_cpufreq=3`), Mali-G52 @ 1050 MHz (`GL=1` / Mesa Panfrost, `mali_mhz=1050`), tear-free double-buffered display (`fbscale=2 fbfit=1 fb_filter=0 fb_threads=4 fb_flip=1`).
  - USB (`redwood_musb.c`, `mtk_usb=1`, `05ac:12a8` HS 480 Mbps) + `usbmux_relay` + `lockdownd` pairing (`idevice_id`, `idevicepair`, `ideviceinfo`, `idevicedate`, `idevicediagnostics restart` via `TOPRGU_SWRST`).
  - Touchscreen (`redwood_nvt.c`), power & volume buttons (`buttons=2`), battery gauge (`mtkbatt=1`).
  - Bluetooth (`connsys_bt=2`, `bluetoothd`, `BlueTool` — Settings → Bluetooth works and stays on).
  - Wi-Fi & NTP time sync (`connsys_step=17`, `WIFID=1`, `com.apple.timed`): scans, joins, and authenticates on Open & WPA2 networks (2.4 GHz & 5 GHz), DHCP, DNS, TCP/UDP, NTP.
  - Rear camera (`s5kgm1sp`, `mtkcam_skel=1`, live preview in `Camera.app`, currently laggy/tearing).
  - Persistent `/private/var` on **`ext2`** (`/dev/emmc1` → `iosdata`, ~51.9 GiB; `ext2` is used instead of `ext4` via XNU's custom `ext2` VFS driver) with 32 MB write-back cache, 128 KB coalesced flushes, per-inode `fsync`, and 32 KB read-ahead (**~2.5–3 min boot** [`~2 min 40 s` measured]; un-chopped icons, widgets).
  - Optional RAM-only `/private/var` mode (`no_ext2=1` boot-arg, ~2 min boot, non-persistent).
- **Known Limitations:**
  - **Laggy UI / apps** and **no wallpaper** currently.
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
- `adb`, `fastboot`, `python3`, `git`, `clang`, `make`
- Build dependencies from `./xnu-deps/setup.sh`
- An iOS 16.5 IPSW (downloaded automatically via `./ios165/fetch.sh`) to generate `userland/root.apfs` and `userland/system.apfs` (`xnusys`) — no Apple binaries are included in this repo
- `prebuilt/loader.efi` (included in this repo) and a stock `merlinx` `boot.img` (used as the Android boot container for `tools/chainboot.py` — you can pull it from your phone in TWRP before flashing: `adb shell dd if=/dev/block/by-name/boot of=/tmp/stock_boot.img && adb pull /tmp/stock_boot.img`)

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

### B. Sync XNU files & build the kernel
> [!IMPORTANT]
> `xnu/files/...` are copied (not symlinked) into `xnu-src/xnu-8796.121.2/`. Whenever you edit files under `xnu/files/`, copy them into `xnu-src/xnu-8796.121.2/` (or run `./xnu/apply.sh`) before building.

```bash
cd hakios
./xnu/apply.sh   # or cp modified files from xnu/files/ into xnu-src/xnu-8796.121.2/
./xnu-build.sh -j8 -k
```

### C. Build userland (`root.apfs` + `system.apfs` for `xnusys`)
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

### D. Build `payload.bin`, `mtkloader.bin`, and `boot-step7-storage.img`
```bash
cd hakios
python3 xnuloader/mkpayload.py \
  -o payload-step7-storage.bin \
  kernel=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 \
  loader=prebuilt/loader.efi \
  ramdisk=userland/root.apfs

bash mtkloader/build.sh \
  -DXNU_CORES=8 -DXNU_VERBOSE=0 \
  -D'XNU_EXTRA_ARGS="cpus=8 small_cores_after=0 buttons=2 mtk_cpufreq=3 a75_mhz=2000 a55_mhz=1800 mali=1 mali_mhz=1050 fbscale=2 fbfit=1 fb_filter=0 fb_threads=4 fb_flip=1 fb_beat=0 vblur=0 aks_keys=1 mtkbatt=1 connsys_step=17 connsys_bt=2 touchscreen=3 touch_poll_us=2000 mtkcam_skel=1 mtk_usb=1"'

python3 tools/chainboot.py \
  stock_boot.img \
  mtkloader/mtkloader.bin \
  boot-step7-storage.img \
  payload-step7-storage.bin
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
  --boot boot-step7-storage.img \
  --reboot
```

### Updating only the kernel / `boot.img` (keeps existing `iosdata` & `xnusys`)
```bash
bash tools/install-merlinx.sh --boot boot-step7-storage.img --reboot
```

### Wiping & reformatting `iosdata` only
```bash
bash tools/install-merlinx.sh --format-iosdata --boot boot-step7-storage.img --reboot
```

---

## 6. Troubleshooting & Logs

- Boot into TWRP (`Vol+ + Power`) and pull kernel/bootloader logs from `pstore`:
  ```bash
  adb pull /sys/fs/pstore/console-ramoops-2
  adb pull /sys/fs/pstore/pmsg-ramoops-0
  ```
- Inspect `/private/var` (`iosdata`) offline in TWRP:
  ```bash
  adb shell "mkdir -p /tmp/ios && mount -t ext2 -o ro /dev/block/by-name/iosdata /tmp/ios && ls -la /tmp/ios; umount /tmp/ios"
  ```
- See [merlinx-bringup-notes.md](merlinx-bringup-notes.md) for full technical details of all kernel, eMMC, `ext2`, Mali, CONNSYS, and camera fixes.
