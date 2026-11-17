# iOS for `merlinx` (Xiaomi Redmi Note 9 · HakiOS / XNU)

**Apple's open-source Darwin kernel (XNU, `xnu-8796.121.2` — the iOS 16.5 kernel) ported to the Xiaomi Redmi Note 9 (`merlinx`, MediaTek Helio G85 MT6768, 3 GB / 4 GB RAM, 64 GB / 128 GB eMMC)**, based on [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel). On top of XNU runs an unmodified iOS 16.5 user space: `launchd` → `backboardd` → `SpringBoard` → apps.

Unlike the temporary dual-boot `Infinix Hot 20` setup, this `merlinx` port replaces Android's `super`/`cache`/`userdata` with a 4.0 GiB `xnusys` system partition and a persistent **`ext2`** `iosdata` partition (~51.9 GiB on 64 GB, ~110 GiB on 128 GB, auto-sized) mounted read-write at `/private/var`, while keeping **TWRP** in `recovery` for root ADB access and `pstore` logs.

**Quick Links:** [Install Guide](docs/merlinx-install.md) | [Bring-up & Technical Notes](docs/merlinx-bringup-notes.md) | [TWRP Installer (`tools/install-merlinx.sh`)](tools/install-merlinx.sh) | [Upstream Docs](docs/README.md)

## What works on `merlinx`

- **Boot & SMP** — `mtkloader` on MT6768 (MMU off, eMMC over MSDC0, XNU DT with auto-detected 3 GB / 4 GB RAM sizing); all **8 cores** (`XNU_CORES=8`, A75 @ 2000 MHz, A55 @ 1800 MHz via `mtk_cpufreq=3`) + clean warm reboot via `TOPRGU_SWRST` (`PE_halt_restart`).
- **USB & Apple Tools (`usbmuxd` / `libimobiledevice`)** — MT6768 MUSB peripheral driver (`bsd/dev/redwood_musb.c`, `mtk_usb=1`, `05ac:12a8` High Speed 480 Mbps), userland USBMux v2 relay (`userland/src/usbmux_relay.c`, `com.redwood.usbmux.plist`), and `lockdownd` interposer (`userland/src/inject_lockdown.c`). Works with `usbmuxd`, `idevice_id`, `idevicepair`, `ideviceinfo`, `idevicename`, `idevicedate`, and `idevicediagnostics` (including `restart`).
- **Persistent Storage (`ext2`)** — Read-only iOS 16.5 `system.apfs` on `xnusys` (`/dev/emmc0`, 4.0 GiB) + persistent read-write `/private/var` on **`ext2`** (`iosdata`, `/dev/emmc1`, auto-sized to ~51.9 GiB on 64 GB or ~110 GiB on 128 GB) via XNU's custom `ext2` VFS driver (`bsd/miscfs/ext2/ext2.c`) with 32 MB write-back cache, `CMD23`/`CMD25` multi-block eMMC writes, 2 s background dirty-block sync, per-inode `fsync`, and 32 KB read-ahead (**~60 s boot**; `no_ext2=1` boot-arg enables RAM-only `/private/var`). Note: `ext4` is not supported — `iosdata` uses `ext2`.
- **Graphics & Display** — In-kernel Mali-G52 driver (`bsd/dev/redwood_mali.c`, **1050 MHz** OC via `mali_mhz=1050`, wired `JOB_IRQ` SPI 276), Mesa **panfrost** (`-O3` + persistent on-disk shader cache in `/private/var/tmp/mesa_cache`), `cpu1` dedicated to `backboardd` GL and `cpu2` for `mali_worker` + app GL threads, and tear-free double-buffered page flipping (`fb_flip=1`, `fbscale=2 fbfit=1 fb_threads=4 fb_beat=0 vblur=0`) for a **smooth UI**.
- **Wallpapers & Settings Picker** — 20 crisp `750x1624` wallpapers (including iOS 16 `WWDC 2022` rendered from `.HEIC` layers + 19 Apple-style gradients/arcs) rendered into `/redwood/share/wallpapers/` at build time, displayed by `SpringBoard` (`userland/src/inject_sb.c`), and selectable live from **Settings → Wallpaper** (`userland/src/inject.c`, persisted to `/var/mobile/Library/Redwood/wallpaper.jpg`).
- **Input** — Novatek touchscreen (`bsd/dev/redwood_nvt.c`, `touchscreen=3`), power and volume buttons (`buttons=2`, `bsd/dev/redwood_mtkkeys.c`).
- **Wi-Fi & Time Sync** — CONNSYS (`connsys_step=17`, `WIFID=1`) + `com.apple.timed`: scans, joins, and authenticates on both Open and WPA2 networks (2.4 GHz & 5 GHz, fixed management TX rate), acquires DHCP on `en0`, passes DNS/TCP/UDP traffic, and syncs system time over NTP.
- **Bluetooth** — `connsys_bt=2` + `bluetoothd` + `BlueTool` + `securityd`/`keybagd` (`aks_keys=1`, `rc.launchd.aks`): Skywalk kernel pipe + HCI up, Settings → Bluetooth opens and stays on.
- **Rear Camera** — `s5kgm1sp` (`mtkcam_skel=1`, `bsd/dev/redwood_mtkcam_skel.c`): streams live frames in `Camera.app` (currently laggy/tearing; front camera not yet ported).
- **Battery** — Real battery percentage in the status bar (`mtkbatt=1`, `bsd/dev/redwood_mtkbatt.c`).
- **Apps & Shell** — Settings, Clock, Calendar, Photos, Notes, Camera, widgets, SSH by key.

**Known limitations / not yet working:**
- **Live dynamic `PosterBoard` wallpapers** are not enabled (uses fast static `750x1624` wallpapers + **Settings → Wallpaper** picker instead)
- **Safari / WebKit** stays blank/unresponsive despite working Wi-Fi/DNS/TCP
- **Extra USB services** (`afcd`, `os_trace_relay`, `mobile_installation_proxy`) not yet enabled in `MORE_JOBS`
- **`Settings → General → About`** crashes `Preferences`
- **Not ported / unsupported:** front camera, speakers/mic audio, cellular/baseband, `ext4` (`iosdata` is `ext2`), and Apple online services (activation, Apple ID, iCloud, App Store — no Secure Enclave).

## Repositories

| Repository | Contents |
|---|---|
| [IOS-for-Merlinx](https://github.com/Yousef13710/IOS-for-Merlinx) | Xiaomi Redmi Note 9 (`merlinx`) port, `ext2` storage, `mtkloader`, userland, installer, and docs (branch `mtk`) |
| [hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel) | Upstream HakiOS repository (Infinix Hot 20 `X6826B`) |

## What you bring, and what is never here

Nothing of Apple's and no vendor firmware is in this repository. You fetch the official iOS 16.5 (20F66) IPSW yourself using `./ios165/fetch.sh` (or automatically via `./setup-merlinx.sh`). **Installing onto `merlinx` completely nukes Android (`super`, `cache`, `userdata`) — all data on the phone will be lost.**

## Quick Build & Install (for Beginners)

Run the interactive ASCII installer from a Linux host with your `merlinx` booted into any modern custom recovery with root ADB (TWRP, OrangeFox, PitchBlack, SHRP, etc.):

```bash
./setup-merlinx.sh
```

It checks host dependencies (and Theos + `iPhoneOS16.5.sdk`), guides you if anything is missing, fetches iOS 16.5, builds XNU + Mesa Panfrost + userland, packs `boot-ios.img`, repartitions, formats `ext2`, and flashes everything automatically. See **[docs/merlinx-install.md](docs/merlinx-install.md)** for manual step-by-step commands.

> [!TIP]
> Always use `idevicediagnostics restart` (over USB) for clean reboots instead of holding Power/Volume keys so the persistent `ext2` (`iosdata`) partition stays clean.

## License and legal

The XNU patch and the files derived from it are under the Apple Public Source License 2.0; everything else is under MIT, as the file
headers say. None of this is legal advice.

HakiOS is an independent research project. It is **not affiliated with or endorsed by Apple**; "Darwin", "iPhone", "XNU", "iOS" and
"Apple" are named only to describe what the code works with. If you believe a file here infringes anyone's rights, open an issue and it
will be removed.

## Team

An experimental project bringing Apple's Darwin kernel to Android hardware. The goal is to keep HakiOS free and open to everyone.
