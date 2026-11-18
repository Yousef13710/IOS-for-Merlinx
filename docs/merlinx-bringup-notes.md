# HakiOS on Xiaomi Redmi Note 9 (`merlinx`, MT6768, 3 GB) — Bring-up & Fixes

Reproducible record of kernel, loader, and userland fixes applied on top of `hakios` for `merlinx`.

## 1. Safety & Milestone Backups
- `/home/devnull/IOS/working-safe` is never modified.
- Historical milestone: the step 5f backup mentioned in earlier notes has been removed.

## 2. Display & GPU Optimizations
- **Verbose console off (`-DXNU_VERBOSE=0`):** Disables `-v` text console over the UI (`v_display = 1` in `mtkloader.c`) while preserving `debug=0x8` and pstore `console-ramoops`.
- **Fast framebuffer copy (`fbscale=2 fbfit=1 fb_filter=0 fb_threads=4`):**
  - Dropped `MiniIOMobileFramebuffer` driver copy time from `~8.5 ms` to `~3.2 ms` per frame.
  - Dropped `fbxr=1` so the bottom-up home-bar swipe gesture works normally.
- **Double-buffered tear-free page flipping (`fb_flip=1`):**
  - `FLIP_LK_FB_BASE = 0x7cdb0000ULL`, `FLIP_LK_FB_SIZE = 0x017e8000ULL` in `xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp` (matching `mtkloader/mtkdbg.h`).
  - **Fix for `KERN_NO_SPACE` panic (`kmem_alloc(0x9b8000): failed with 3`):** `ml_io_map_wcomb()` allocates from `io_submap` which is only 8 MB (`IO_MAP_SIZE = 8ul << 20` in `osfmk/arm/io_map.c`), while `merlinx`'s 1080x2340x32 framebuffer (`picture = 4352 * 2340 = 0x9b8000`, ~9.7 MB) exceeds 8 MB. Switched `flipStart()` to `ml_io_map_unmappable(scanned + picture, (vm_size_t) picture, VM_WIMG_WCOMB)` (allocates from `kernel_map`). Verified zero tearing and no panic.
- **VSync pacing (`fb_beat=0`), blur (`vblur=0`), & Mesa Panfrost (`-O3` + shader cache):**
  - `fb_beat=1` slept `9.2–12.7 ms` per frame waiting on a software beat, dropping animations to 14–32 fps. Setting `fb_beat=0` + `vblur=0` (skipping heavy variable-radius blur passes in `eagl.c`) + tightening `wait_job()` sleep-poll in `xnu/files/bsd/dev/redwood_mali.c` from `200 us` to `100 us` noticeably improved UI & animation responsiveness.
  - Rebuilt Mesa Panfrost (`tools/mk-mesa.sh`) with `-Doptimization=3` (instead of `-Os`) and `-Dshader-cache=enabled` (with `tools/mesa/panfrost-disk-cache-darwin.patch` and `MESA_SHADER_CACHE_DIR=/private/var/tmp/mesa_cache` in `userland/src/eagl.c`), and built userland with `OSLOG=""`.
- **Big-core `backboardd` render thread (`userland/src/eagl.c`):**
  - Pinned `backboardd`'s OpenGL/Panfrost render thread to Cortex-A75 big core `1` (`kern.redwood.bind_cpu = 1`), cutting CPU frame time roughly in half (heavy ~170-draw animations improved from `22–35 ms` / `15–32 FPS` to `11–15 ms` / `45–56 FPS`, medium ~120-draw scenes up to `60.6 FPS`).
- **CPU & GPU Clocks (`-DXNU_CORES=8`, `cpus=8 small_cores_after=0 mtk_cpufreq=3 mali=1 mali_mhz=1050`):**
  - Verified A75 big cores at `2000 MHz` and A55 little cores at `1700 MHz`.
  - Updated `gpu_raise()` in `xnu/files/bsd/dev/redwood_mtkkeys.c` to allow `mali_mhz` up to `1150` using `/2` `GPUPLL` post-divider above `950 MHz` (`1050 MHz` at `VGPU = 1000 mV`, `VSRAM_GPU = 1100 mV`).

## 3. AKS / Keybagd (`rc.launchd.aks`)
- **Fix for `/redwood/bin/tolog: No such file or directory`:** Built and copied `obj/tolog` unconditionally in `userland/build.sh` (previously gated on `METAL=1`). Fixed `aks_restore` and `keybagd --init` (`-25291` Identity Root error).

## 4. Wi-Fi & Bluetooth (`connsys_step=17`, `connsys_bt=2`, `bluetoothd`, `BlueTool`, `tccd`, `keybagd`)
- **Wi-Fi (`connsys_step=17`, `WIFID=1`):** Scans and connects to Wi-Fi networks; association was later verified on device.
- **`bluetoothd` HCI fixes (`xnu/files/bsd/dev/redwood_connsys.c`):**
  - `OI_HCICMD_WriteDefaultErroneousDataReporting` (`0x0c5b`) failed locally with `status=628` because `Read_Local_Supported_Commands` octet 18 was `0x83` (missing bits 2 & 3). Added `packet[7 + 18] |= 0x0c;` and expanded the `0x0c5b` quirk to match any non-zero `0x0e`/`0x0f` response.
  - `0x200b` (`LE_Set_Scan_Parameters`) and `0x200c` (`LE_Set_Scan_Enable`) returned `0x0c` (`Command Disallowed`). Rewrote `04 0e 04 01 0b/0c 20 0c` to `status=0x00`.
  - Result: `bluetoothd` stays alive (`Scanning started successfully`) and Bluetooth stays **On** in Settings.
- **Settings → Bluetooth freeze (FIXED in `step5t`):**
  - `com.redwood.wifi` was `pending spawn, domain in on-demand-only mode` in `user/501`, so `net_test` never ran to set `ComputerName`. Added `/redwood/bin/net_test name &` to `userland/rc.jobs`.
  - Moved `Preferences` from `libredwood-tweaks.dylib` to `libredwood.dylib` in `userland/build.sh`, and added `pref_say()` (via `kern.redwood.say`) + `pref_hang_watch` in `userland/src/inject.c`.
  - Root cause found via `pref_hang_watch` stack trace: `BluetoothSettings` calls `NanoRegistry` on the main thread, deadlocking in `semaphore_wait_trap` when `com.apple.nanoregistryd` is absent.
  - **Fix:** Add `com.apple.nanoregistryd` and `com.apple.nanoprefsyncd` to `MORE_JOBS`. Verified in `step5t`: Settings → Bluetooth opens and works. (Note: `DROP_JOBS="com.apple.chronod"` removes home screen widgets without significant UI smoothness improvement, so keep `chronod`.)

## 5. Rear Camera (`s5kgm1sp_mipi_raw`, `mtkcam_skel=1`)
- **Userland (`step6a`):** Added `com.apple.mediaserverd` to `MORE_JOBS` (and `com.apple.chronod` restored) so `Camera.app` opens cleanly.
- **Sensor & I2C2 Probe (`step6b`):** Verified `s5kgm1sp` power-up (`VCAMIO` PMIC `0x1cb0` bit 0, `GPIO178` AVDD, `GPIO179` DVDD, `GPIO100` MCLK, `GPIO102` RST) and I2C2 bit-bang (`GPIO103` SCL, `GPIO104` SDA) -> `addr7=0x10 id=08d1`.
- **Live Streaming (`step6c`):**
  - Dumped live Android camera registers (`11c10000_6000` MIPI RX `csi1a`/`csi1b`, `1a040000_8000` `SENINF3_CSI2` + `MUX3` routing VC0 to `CAMSV_1`, `1a051000_1000` `CAMSV1`) and extracted `gm1sp_init[645]` + `gm1sp_preview[342]` (`4000x3000`) into `xnu/files/bsd/dev/redwood_mtkcam_gm1sp.h`.
  - Implemented `merlinx` `mtkcam_platform` in `xnu/files/bsd/dev/redwood_mtkcam_skel.c` (`kvtophys` for DMA phys, `kmem_alloc_contig(..., max_pnum=(ppnum_t)(0x100000000ULL >> PAGE_SHIFT), pnum_mask=0, ...)` called at boot in `redwood_mtkcam_skel_init()`).
  - Verified in `step6c`: rear camera streams live frames in `Camera.app` (front camera not yet ported; preview is laggy/tearing, to be tuned later).

## 6. Persistent Read/Write Storage (`step7`) & Android Removal
- **Install & Bring-up Guides:** See [merlinx-install.md](merlinx-install.md) for requirements, partition layout, build steps, and [../tools/install-merlinx.sh](../tools/install-merlinx.sh) for automated TWRP installation.
- **eMMC Repartitioning (`/dev/block/mmcblk0` via TWRP, GPT backup in `logs/gpt_orig_merlinx.bin`):**
  - Kept `recovery` (`mmcblk0p1`, TWRP for root & `/sys/fs/pstore`), `boot` (`p33`), `xnusys` (`p48`, `113631232..122021823`, 4.0 GiB), and firmware/bootloader partitions.
  - Deleted Android `super` (`p43`), `cache` (`p44`), and `userdata` (`p45`); created `iosdata` (`p43`, `4767744..113631231`, ~51.9 GiB, code `8300`) formatted as `ext2` (`mke2fs -t ext2 -b 4096 -L iosdata /dev/block/mmcblk0p43`).
- **eMMC Driver (`xnu/files/bsd/dev/redwood_emmc.c`):**
  - Added `/dev/emmc1` + `/dev/remmc1` (minor 1) mapped to GPT `iosdata` with read/write support.
  - Uses single-block `CMD17` (`blocks <= 8`) and `CMD24` (`write_one_block`); `CMD25` (`write_blocks`) hits `INT_CMDTMO (0x200)` on `merlinx`'s MSDC0 PIO controller.
- **Writable `ext2` VFS (`xnu/files/bsd/miscfs/ext2/ext2.c`, registered in `bsd/conf/files` and `bsd/vfs/vfs_conf.c`):**
  - Note: `xnu/files/...` are copied (not symlinked) into `xnu-src/xnu-8796.121.2/`; sync modified files into `xnu-src/xnu-8796.121.2/` before running `./xnu-build.sh -j8 -k`.
  - Omitted `vfs_setextendedsecurity(mp)` so `mkdir`/`chmod`/`chown` do not fail with `ENOTSUP`.
  - Added `vnode_startwrite(em->em_devvp)` before `VNOP_STRATEGY(bp)` on `B_WRITE` (fixes `vnode_writedone: numoutput < 0`).
  - Added `if (ap->a_flags & (VNODE_READDIR_EXTENDED | VNODE_READDIR_REQSEEKOFF)) return EINVAL;` in `ext2_readdir` so `getdirentries64()` falls back to `struct dirent`.
  - Deferred `ext2_sync_node`/`ext2_flush_meta` in `ext2_write` (only on `IO_SYNC`, `fsync`, `sync`, `inactive`, `reclaim`, `unmount`) and `ext2_pageout`.
  - Fixed `ext2_vget_locked()` `em_lock` race condition, added 32 MB (`8192`-block) write-back cache, 128 KB `ext2_bcache_flush_run` coalescing, `bc_ino` per-inode `fsync`, 32 KB `em_ra_buf` read-ahead, and `em_grp_hint` O(1) block allocator.
  - Dynamically allocated `em_bcache_hash` (`Z_WAITOK | Z_ZERO`), `em_io_buf` (128 KB), `em_ra_buf` (32 KB), `em_grp_hint`, and `em_bmap_buf` via `kalloc_data` to avoid `KALLOC_SAFE_ALLOC_SIZE` + `Z_NOFAIL` early-boot panics.
  - Verified on device: boots in **~2.5–3 min** (`~2 min 40 s` measured, down from ~30 min), un-chopped icons; currently no wallpaper, laggy UI/apps, at that time, and `Settings → General → About` crashed in `Preferences`. About now opens, though free-space reporting remains incorrect.
- **Userland (`userland/src/init.c`):**
  - `mount_writable()` mounts `/dev/emmc1` (`ext2`) on `/private/var` (falling back to `ramfs`, or `no_ext2=1` boot-arg to force `ramfs`), treats `EEXIST` on `mkdir` as success, and skips overwriting existing files.
- **Bootloader (`mtkloader/mtkloader.c`):**
  - Removed `chain_android()` from `mtkloader_main()` and `el_exception()` (`reset_board()` instead), and updated `MARK_RUNNING` to clear the marker (`marker_set(0)`), rescue logs to pstore, and continue booting XNU instead of halting in `wfi`.

## 7. Build & Flash Commands
```sh
# Userland
cd /home/devnull/IOS/hakios/userland
DRIVERS="softpipe,panfrost" GL=1 WIFID=1 RC=rc.launchd.aks MORE_JOBS="com.apple.bluetoothd com.apple.BlueTool com.apple.tccd com.apple.mobile.keybagd com.apple.nanoregistryd com.apple.nanoprefsyncd com.apple.mediaserverd" OSLOG="" bash build.sh

# Kernel (sync xnu/files/* into xnu-src/xnu-8796.121.2/ if modified)
cd /home/devnull/IOS/hakios && ./xnu-build.sh -j8 -k

# Payload, mtkloader, boot.img
python3 /home/devnull/IOS/hakios/xnuloader/mkpayload.py -o /home/devnull/IOS/hakios/payload.bin kernel=/home/devnull/IOS/hakios/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 loader=/home/devnull/IOS/working-safe/images/loader.efi ramdisk=/home/devnull/IOS/hakios/userland/root.apfs
bash /home/devnull/IOS/hakios/mtkloader/build.sh -DXNU_CORES=8 -DXNU_VERBOSE=0 -D'XNU_EXTRA_ARGS="cpus=8 small_cores_after=0 buttons=2 mtk_cpufreq=3 mali=1 mali_mhz=1050 fbscale=2 fbfit=1 fb_filter=0 fb_threads=4 fb_flip=1 fb_beat=0 vblur=0 aks_keys=1 mtkbatt=1 connsys_step=17 connsys_bt=2 touchscreen=3 touch_poll_us=2000 mtkcam_skel=1"'
python3 /home/devnull/IOS/hakios/tools/chainboot.py /home/devnull/IOS/working-safe/images/boot_shas_oc_ksu.img /home/devnull/IOS/hakios/mtkloader/mtkloader.bin /home/devnull/IOS/hakios/boot.img /home/devnull/IOS/hakios/payload.bin

# Install onto merlinx in TWRP
./tools/install-merlinx.sh --boot boot.img --reboot
```
