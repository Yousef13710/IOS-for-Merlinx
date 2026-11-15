# Installing HakiOS on Xiaomi Redmi Note 9 (`merlinx`)

Complete guide to building and installing HakiOS (XNU 8796.121.2 / iOS 16.5) on the **Xiaomi Redmi Note 9 (`merlinx`, MediaTek Helio G85 MT6768, 3 GB RAM)** with on-eMMC `xnusys` (4.0 GiB) and persistent `iosdata` (`ext2`, ~51.9 GiB).

> [!WARNING]
> **This replaces Android's `super`, `cache`, and `userdata` partitions** when setting up `iosdata` + `xnusys`. Back up your data and GPT partition table first. Keep **TWRP** in `recovery` so you always have root ADB access and `/sys/fs/pstore` logs.

---

## 1. Current Status (`merlinx`)

- **Works:**
  - 8-core SMP (`XNU_CORES=8`, A75 @ 2000 MHz, A55 @ 1700 MHz), Mali-G52 @ 950 MHz (`GL=1` / Mesa Panfrost), tear-free double-buffered display (`fbscale=2 fbfit=1 fb_filter=0 fb_threads=4 fb_flip=1`).
  - Touchscreen (`redwood_nvt.c`), power & volume buttons (`buttons=2`), battery gauge (`mtkbatt=1`).
  - Wi-Fi & Bluetooth (`connsys_step=17 connsys_bt=2`, `bluetoothd`, `BlueTool`).
  - Rear camera (`s5kgm1sp`, `mtkcam_skel=1`).
  - Persistent `/private/var` on `ext2` (`/dev/emmc1` -> `iosdata`, ~51.9 GiB) with 32 MB write-back cache, 128 KB coalesced flushes, per-inode `fsync`, and 32 KB read-ahead (~5 min first boot; smooth UI, un-chopped icons, widgets).
  - Optional RAM-only `/private/var` mode (`no_ext2=1` boot-arg, ~2 min boot, non-persistent).
- **Known Limitations:**
  - `Settings → General → About` currently crashes `Preferences`.
  - Front camera, speakers/mic audio, cellular/baseband, and Apple online services (iCloud/App Store/activation) are not supported.

---

## 2. Requirements

### Device
- Xiaomi Redmi Note 9 (`merlinx`, MT6768, 64 GB eMMC, 3 GB RAM)
- Unlocked bootloader
- **TWRP recovery** flashed to `recovery` (`mmcblk0p1`)

### Host (Linux x86_64)
- `adb`, `fastboot`, `python3`, `git`, `clang`, `make`
- Build dependencies from `./xnu-deps/setup.sh`
- An iOS 16.5 IPSW (downloaded by yourself via `ios165/fetch.sh`) to generate the `xnusys` `packfs` system image (no Apple binaries are included in this repo)
- Stock `merlinx` `boot.img` (used as the Android boot container for `tools/chainboot.py`) and `loader.efi`

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

### A. Sync XNU files & build the kernel
> [!IMPORTANT]
> `xnu/files/...` are copied (not symlinked) into `xnu-src/xnu-8796.121.2/`. Whenever you edit files under `xnu/files/`, copy them into `xnu-src/xnu-8796.121.2/` (or run `./xnu/apply.sh`) before building.

```bash
cd hakios
./xnu/apply.sh   # or cp modified files from xnu/files/ into xnu-src/xnu-8796.121.2/
./xnu-build.sh -j8 -k
```

### B. Build userland (`root.apfs`)
```bash
cd hakios/userland
GL=1 WIFID=1 RC=rc.launchd.aks \
  MORE_JOBS="com.apple.bluetoothd com.apple.BlueTool com.apple.tccd com.apple.mobile.keybagd com.apple.nanoregistryd com.apple.nanoprefsyncd com.apple.mediaserverd" \
  OSLOG="com.apple.backboardd com.apple.bluetoothd com.apple.Preferences" \
  bash build.sh
```

### C. Build `payload.bin`, `mtkloader.bin`, and `boot-step7-storage.img`
```bash
cd hakios
python3 xnuloader/mkpayload.py \
  -o payload-step7-storage.bin \
  kernel=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 \
  loader=/path/to/loader.efi \
  ramdisk=userland/root.apfs

bash mtkloader/build.sh \
  -DXNU_CORES=8 -DXNU_VERBOSE=0 \
  -D'XNU_EXTRA_ARGS="cpus=8 small_cores_after=0 buttons=2 mtk_cpufreq=3 mali=1 mali_mhz=950 fbscale=2 fbfit=1 fb_filter=0 fb_threads=4 fb_flip=1 aks_keys=1 mtkbatt=1 connsys_step=17 connsys_bt=2 touchscreen=3 touch_poll_us=2000 mtkcam_skel=1"'

python3 tools/chainboot.py \
  /path/to/stock_boot.img \
  mtkloader/mtkloader.bin \
  boot-step7-storage.img \
  payload-step7-storage.bin
```
*(To build a RAM-only `/private/var` image for faster ~2 min non-persistent boots, add `no_ext2=1` to `XNU_EXTRA_ARGS`.)*

### D. Build `xnusys` (`packfs`) from your IPSW
```bash
cd hakios
./ios165/fetch.sh
python3 mkpackfs.py ... -o xnusys.img
```

---

## 5. Installing onto `merlinx` (via TWRP)

Boot the phone into **TWRP**, connect USB, and run `tools/install-merlinx.sh`:

### First-time install (repartition + format `iosdata` + flash `xnusys` + flash `boot`)
```bash
bash tools/install-merlinx.sh \
  --repartition \
  --format-iosdata \
  --xnusys /path/to/xnusys.img \
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
