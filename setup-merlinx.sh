#!/bin/bash
# Interactive ASCII TUI Installer for HakiOS (iOS 16.5 / XNU) on Xiaomi Redmi Note 9 (merlinx)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

LOG_DIR="$ROOT/.setup-logs"
mkdir -p "$LOG_DIR"
SESSION_LOG="$LOG_DIR/session-$(date +%Y%m%d-%H%M%S)-$$.log"
: > "$SESSION_LOG"

# Colors & ASCII UI helpers
if [ -t 1 ] && command -v tput >/dev/null 2>&1 && [ "$(tput colors 2>/dev/null || echo 0)" -ge 8 ]; then
	C_RESET=$'\033[0m'
	C_BOLD=$'\033[1m'
	C_DIM=$'\033[2m'
	C_RED=$'\033[31m'
	C_GREEN=$'\033[32m'
	C_YELLOW=$'\033[33m'
	C_BLUE=$'\033[34m'
	C_CYAN=$'\033[36m'
else
	C_RESET="" C_BOLD="" C_DIM="" C_RED="" C_GREEN="" C_YELLOW="" C_BLUE="" C_CYAN=""
fi

THEOS_DIR="${THEOS:-$HOME/theos}"
SDK_PATH="$THEOS_DIR/sdks/iPhoneOS16.5.sdk"
if [ -z "${LDID:-}" ] && ! command -v ldid >/dev/null 2>&1 && [ -x "$THEOS_DIR/toolchain/linux/iphone/bin/ldid" ]; then
	export LDID="$THEOS_DIR/toolchain/linux/iphone/bin/ldid"
fi
JOBS="$(nproc 2>/dev/null || echo 8)"
ADB_SERIAL=""
STOCK_BOOT_CONTAINER=""
TMP_BOOT_CONTAINER=""

cleanup() {
	if [ -n "$TMP_BOOT_CONTAINER" ] && [ -f "$TMP_BOOT_CONTAINER" ]; then
		rm -f "$TMP_BOOT_CONTAINER"
	fi
}
trap cleanup EXIT

clear_screen() {
	[ -t 1 ] && printf '\033[2J\033[H' || true
}

banner() {
	echo "${C_CYAN}${C_BOLD}"
	cat <<'EOF'
+----------------------------------------------------------------------+
|          HakiOS for Xiaomi Redmi Note 9 (merlinx / MT6768)           |
|              Interactive Build & Recovery Installer                  |
+----------------------------------------------------------------------+
EOF
	echo "${C_RESET}"
}

ok()   { echo "${C_GREEN}[ OK ]${C_RESET} $*"; }
info() { echo "${C_CYAN}[INFO]${C_RESET} $*"; }
warn() { echo "${C_YELLOW}[WARN]${C_RESET} $*"; }
fail() { echo "${C_RED}[FAIL]${C_RESET} $*"; }

pause_enter() {
	echo ""
	read -r -p "Press [Enter] to continue... " _
}

run_step() {
	local title="$1"
	local logfile="$2"
	shift 2
	echo ""
	echo "${C_BOLD}==> $title${C_RESET}"
	echo "${C_DIM}    Log: $logfile${C_RESET}"
	local progress_file pid rc elapsed=0 progress
	progress_file=$(mktemp "$LOG_DIR/progress.XXXXXX")
	printf '\n== %s ==\n' "$title" >> "$SESSION_LOG"
	HAKIOS_PROGRESS_FILE="$progress_file" "$@" > "$logfile" 2>&1 &
	pid=$!
	if [ -t 1 ]; then
		while kill -0 "$pid" 2>/dev/null; do
			progress=$(cat "$progress_file" 2>/dev/null || true)
			printf '\r\033[K    %s  %s' "${progress:-Working...}" "(${elapsed}s)"
			sleep 1
			elapsed=$((elapsed + 1))
		done
		printf '\r\033[K'
	fi
	if wait "$pid"; then rc=0; else rc=$?; fi
	cat "$logfile" >> "$SESSION_LOG"
	rm -f "$progress_file"
	if [ "$rc" -eq 0 ]; then
		ok "$title"
		return 0
	else
		echo ""
		fail "$title failed (exit $rc)"
		echo "${C_YELLOW}--- Last 20 lines of $logfile ---${C_RESET}"
		tail -n 20 "$logfile" || true
		echo "${C_YELLOW}----------------------------------${C_RESET}"
		return $rc
	fi
}

adb_cmd() {
	if [ -n "$ADB_SERIAL" ]; then
		adb -s "$ADB_SERIAL" "$@"
	else
		adb "$@"
	fi
}

# 1. Host & Toolchain Preflight
check_host() {
	local missing_pkgs=()
	local cmds=(adb python3 git curl tar xz make cc clang flex bison m4 perl autoconf automake meson ninja llvm-ar)
	local c
	for c in "${cmds[@]}"; do
		if ! command -v "$c" >/dev/null 2>&1; then
			missing_pkgs+=("$c")
		fi
	done
	if ! command -v "${LDID:-ldid}" >/dev/null 2>&1; then
		missing_pkgs+=("${LDID:-ldid} (iOS code signer)")
	fi

	if ! command -v 7zz >/dev/null 2>&1 && ! command -v 7z >/dev/null 2>&1; then
		missing_pkgs+=("7zz/p7zip")
	fi

	echo "${C_BOLD}Host Environment Check:${C_RESET}"
	if [ ${#missing_pkgs[@]} -gt 0 ]; then
		fail "Missing host tools: ${missing_pkgs[*]}"
		echo ""
		echo "Install them first:"
		echo "  Debian/Ubuntu: sudo apt update && sudo apt install -y adb python3 python3-venv git curl tar xz-utils build-essential clang lld flex bison m4 perl autoconf automake meson ninja-build llvm 7zip"
		echo "  Arch Linux:    sudo pacman -S --needed android-tools python git curl tar xz base-devel clang lld flex bison m4 perl autoconf automake meson ninja llvm 7zip"
		echo "  Install ldid, or set LDID to a compatible signing executable."
		return 1
	fi
	ok "Core build tools present"

	if ! python3 -c "import venv" >/dev/null 2>&1; then
		fail "python3 venv module is missing (needed by tools/mk-mesa.sh)"
		echo "  Debian/Ubuntu: sudo apt install -y python3-venv"
		return 1
	fi
	ok "python3 venv available"

	if [ ! -x "$ROOT/xnu-deps/host/zig/zig" ] && ! command -v ld.lld >/dev/null 2>&1; then
		warn "Neither ld.lld nor xnu-deps/host/zig/zig found (needed for mtkloader)."
		read -r -p "Run ./tools/get-zig.sh now to fetch zig? [Y/n] " ans
		if [[ "${ans:-Y}" =~ ^[Yy]$ ]]; then
			run_step "Fetching zig (ld.lld)" "$LOG_DIR/get-zig.log" bash "$ROOT/tools/get-zig.sh"
		else
			fail "Cannot build mtkloader without ld.lld or zig."
			return 1
		fi
	else
		ok "ELF linker (ld.lld / zig) ready"
	fi

	if [ ! -d "$THEOS_DIR" ] || [ ! -d "$SDK_PATH" ]; then
		fail "Theos or iPhoneOS16.5.sdk not found!"
		echo "  Expected:"
		echo "    THEOS: $THEOS_DIR"
		echo "    SDK:   $SDK_PATH"
		echo "  Fix: Install Theos with its Linux iOS toolchain and place iPhoneOS16.5.sdk at $SDK_PATH"
		echo "       (or export THEOS=/path/to/theos before running this script)."
		return 1
	fi
	ok "Theos & iPhoneOS16.5.sdk found ($SDK_PATH)"
	return 0
}

# 2. Smart Recovery ADB Connection (works with any modern recovery with root ADB)
connect_recovery() {
	info "Looking for a phone booted into recovery with root ADB (TWRP, OrangeFox, PitchBlack, SHRP, Lineage, etc.)..."
	while true; do
		local lines
		lines=$(adb devices 2>/dev/null | awk 'NR>1 && NF>=2 {print $1 "\t" $2}')
		if [ -z "$lines" ]; then
			warn "No ADB device detected."
			echo "  1. Boot your Redmi Note 9 (merlinx) into your custom recovery (hold Vol+ + Power)."
			echo "  2. Connect the USB cable (make sure ADB is enabled in recovery)."
			read -r -p "Press [Enter] to retry, or type 'q' to cancel: " ans
			[ "$ans" = "q" ] && return 1
			continue
		fi

		local count
		count=$(printf '%s\n' "$lines" | wc -l)
		if [ "$count" -gt 1 ] && [ -z "$ADB_SERIAL" ]; then
			echo "Multiple ADB devices detected:"
			local i=1
			while IFS=$'\t' read -r ser st; do
				echo "  $i) $ser ($st)"
				i=$((i + 1))
			done <<< "$lines"
			read -r -p "Select device number: " pick
			ADB_SERIAL=$(printf '%s\n' "$lines" | sed -n "${pick}p" | cut -f1)
			[ -z "$ADB_SERIAL" ] && continue
		elif [ -z "$ADB_SERIAL" ]; then
			ADB_SERIAL=$(printf '%s\n' "$lines" | head -n1 | cut -f1)
		fi

		local state
		state=$(adb_cmd get-state 2>/dev/null | tr -d '\r' || true)
		if [ "$state" = "unauthorized" ]; then
			warn "Device $ADB_SERIAL is 'unauthorized'. Accept the ADB authorization prompt in recovery (or re-enable ADB)."
			read -r -p "Press [Enter] to retry, or 'q' to cancel: " ans
			[ "$ans" = "q" ] && return 1
			continue
		elif [ "$state" = "offline" ]; then
			warn "Device $ADB_SERIAL is 'offline'. Unplug and replug the USB cable."
			read -r -p "Press [Enter] to retry, or 'q' to cancel: " ans
			[ "$ans" = "q" ] && return 1
			continue
		elif [ "$state" != "recovery" ] && [ "$state" != "device" ]; then
			warn "Device $ADB_SERIAL state is '${state:-unknown}'. Make sure it is booted into recovery."
			read -r -p "Press [Enter] to retry, or 'q' to cancel: " ans
			[ "$ans" = "q" ] && return 1
			continue
		fi

		local uid
		uid=$(adb_cmd shell "id -u" 2>/dev/null | tr -d '\r' || true)
		if [ "$uid" != "0" ]; then
			fail "Recovery ADB is NOT running as root (id -u = '${uid:-?}')."
			echo "  Enable 'Root ADB' in your recovery settings, or use TWRP / OrangeFox / PitchBlack / SHRP."
			read -r -p "Press [Enter] to retry, or 'q' to cancel: " ans
			[ "$ans" = "q" ] && return 1
			continue
		fi

		local dev
		dev=$(adb_cmd shell "getprop ro.product.device" 2>/dev/null | tr -d '\r' || true)
		if [ -n "$dev" ] && [ "$dev" != "merlin" ] && [ "$dev" != "merlinx" ] && [ "$dev" != "merlinnfc" ]; then
			warn "Connected device reports '$dev' (expected merlin/merlinx)."
			read -r -p "Continue anyway? [y/N] " ans
			[[ "$ans" =~ ^[Yy]$ ]] || return 1
		fi

		local missing_rec=()
		adb_cmd shell "command -v sgdisk >/dev/null 2>&1" || missing_rec+=("sgdisk")
		adb_cmd shell "command -v mke2fs >/dev/null 2>&1 || command -v mkfs.ext2 >/dev/null 2>&1" || missing_rec+=("mke2fs/mkfs.ext2")
		adb_cmd shell "command -v dd >/dev/null 2>&1" || missing_rec+=("dd")
		if [ ${#missing_rec[@]} -gt 0 ]; then
			fail "This recovery is missing required tools: ${missing_rec[*]}"
			echo "  Please boot a recovery that includes them (e.g. TWRP or OrangeFox for merlinx)."
			return 1
		fi

		ok "Connected to root recovery ADB: $ADB_SERIAL (device: ${dev:-unknown})"
		return 0
	done
}

# Fetch Android v2 boot container from device (temporary only, no user backup)
prepare_boot_container() {
	if [ -n "$STOCK_BOOT_CONTAINER" ] && [ -f "$STOCK_BOOT_CONTAINER" ]; then
		ok "Using boot container: $STOCK_BOOT_CONTAINER"
		return 0
	fi
	if [ -f "$ROOT/backup/boot_shas_oc_ksu.img" ]; then
		STOCK_BOOT_CONTAINER="$ROOT/backup/boot_shas_oc_ksu.img"
		ok "Using existing Android boot container: $STOCK_BOOT_CONTAINER"
		return 0
	fi

	info "Need an Android v2 boot.img header/DTB container for chainboot.py (pulled temporarily from phone, not saved as backup)."
	connect_recovery || return 1
	TMP_BOOT_CONTAINER=$(mktemp /tmp/hakios_boot_container.XXXXXX.img)
	info "Reading the phone's boot container..."
	if ! adb_cmd shell "dd if=/dev/block/by-name/boot of=/tmp/dev_boot.img bs=4M" >> "$SESSION_LOG" 2>&1 ||
	   ! adb_cmd pull /tmp/dev_boot.img "$TMP_BOOT_CONTAINER" >> "$SESSION_LOG" 2>&1; then
		fail "Could not read the boot container. Details: $SESSION_LOG"
		return 1
	fi
	adb_cmd shell "rm -f /tmp/dev_boot.img" >> "$SESSION_LOG" 2>&1 || true

	if python3 -c "
import sys
d = open(sys.argv[1], 'rb').read()
assert d[:8] == b'ANDROID!', 'Not an Android boot image'
if b'XNUP' in d:
    sys.exit(2)
" "$TMP_BOOT_CONTAINER" 2>/dev/null; then
		STOCK_BOOT_CONTAINER="$TMP_BOOT_CONTAINER"
		ok "Pulled temporary Android boot container from device"
		return 0
	else
		local rc=$?
		rm -f "$TMP_BOOT_CONTAINER"
		TMP_BOOT_CONTAINER=""
		if [ "$rc" -eq 2 ]; then
			warn "The phone's boot partition already has HakiOS installed (cannot re-wrap it)."
		else
			warn "The phone's boot partition is not a valid Android v2 boot.img."
		fi
		read -r -p "Enter path to a stock merlinx Android boot.img: " custom_boot
		if [ -f "$custom_boot" ]; then
			STOCK_BOOT_CONTAINER="$custom_boot"
			return 0
		fi
		fail "Valid Android boot.img not found."
		return 1
	fi
}

# Build steps
xnu_dependencies_ready() {
	local sdk="$ROOT/xnu-deps/sdk/iPhoneOS16.5.xnu.sdk"
	local host="$ROOT/xnu-deps/host/bin" tool
	[ -f "$sdk/SDKSettings.plist" ] || return 1
	[ -x "$sdk/usr/local/libexec/availability.pl" ] || return 1
	[ -f "$sdk/usr/local/include/kernel/os/firehose_buffer_private.h" ] || return 1
	[ -f "$sdk/usr/local/include/kernel/os/firehose_types.defs" ] || return 1
	[ -f "$sdk/usr/local/lib/kernel/libfirehose_kernel.a" ] || return 1
	[ -f "$sdk/usr/local/lib/kernel/libpthread_kext.a" ] || return 1
	for tool in mig migcom unifdef tcsh csh ld64; do
		[ -x "$host/$tool" ] || return 1
	done
}

step_fetch_ios() {
	if [ -d "$ROOT/ios165/rootfs/usr/sbin" ] && [ -f "$ROOT/ios165/rootfs/sbin/launchd" ] &&
	   [ -f "$ROOT/ios165/.done-rootfs" ] && [ ! "$ROOT/ios165/select-system.py" -nt "$ROOT/ios165/.done-rootfs" ] &&
	   [ -f "$ROOT/ios165/system.manifest" ] && [ -f "$ROOT/ios165/var.skel" ] &&
	   [ -f "$ROOT/ios165/.done-cryptex" ] && [ -d "$ROOT/ios165/cryptex/usr/lib" ] &&
	   [ -f "$ROOT/ios165/20F66__iPhone10,3_6/dyld_shared_cache_arm64" ] &&
	   [ "$(find "$ROOT/ios165/20F66__iPhone10,3_6" -maxdepth 1 -type f 2>/dev/null | wc -l)" -eq 43 ] &&
	   [ -f "$ROOT/ios165/.done-cryptex-app" ] && [ -f "$ROOT/ios165/cryptex-app/usr/libexec/webbookmarksd" ]; then
		ok "iOS 16.5 (20F66) already extracted in ios165/rootfs (skipping)"
		return 0
	fi
	local rc=0 detail
	run_step "Fetching & extracting iOS 16.5 (20F66)" "$LOG_DIR/fetch-ios165.log" bash "$ROOT/ios165/fetch.sh" || rc=$?
	for detail in "$ROOT"/ios165/extract-{system,cryptex,dyld,cryptex-app}.log; do
		if [ -f "$detail" ]; then
			printf '\n== Extraction detail: %s ==\n' "$detail" >> "$SESSION_LOG"
			cat "$detail" >> "$SESSION_LOG"
		fi
	done
	return "$rc"
}

step_build_xnu() {
	if [ ! -d "$ROOT/xnu-src/xnu-8796.121.2" ]; then
		run_step "Applying XNU patches (xnu/apply.sh)" "$LOG_DIR/xnu-apply.log" bash "$ROOT/xnu/apply.sh" || return 1
	else
		info "Syncing xnu/files/ into existing xnu-src/xnu-8796.121.2/"
		cp -a "$ROOT/xnu/files/." "$ROOT/xnu-src/xnu-8796.121.2/"
	fi
	if ! xnu_dependencies_ready; then
		info "XNU dependencies are missing or incomplete; setting them up."
		run_step "Setting up XNU dependencies (xnu-deps/setup.sh)" "$LOG_DIR/xnu-deps.log" bash "$ROOT/xnu-deps/setup.sh" || return 1
		if ! xnu_dependencies_ready; then
			fail "XNU dependency setup did not produce all required SDK files and tools. See $LOG_DIR/xnu-deps.log."
			return 1
		fi
	else
		ok "xnu-deps already set up (skipping)"
	fi
	run_step "Building XNU kernel (-j$JOBS)" "$LOG_DIR/xnu-build.log" bash "$ROOT/xnu-build.sh" "-j$JOBS" -k
}

step_build_mesa() {
	if [ -f "$ROOT/xnu-deps/build/mesa/out/libOSMesa.8.dylib" ]; then
		ok "Mesa Panfrost (libOSMesa.8.dylib) already built (skipping)"
		return 0
	fi
	run_step "Building Mesa Panfrost (softpipe,panfrost)" "$LOG_DIR/mk-mesa.log" env DRIVERS="softpipe,panfrost" bash "$ROOT/tools/mk-mesa.sh"
}

step_build_userland() {
	run_step "Building userland (root.apfs + system.apfs)" "$LOG_DIR/userland-build.log" \
		bash -c 'cd "$1/userland" && DRIVERS="softpipe,panfrost" GL=1 WIFID=1 RC=rc.launchd.aks MORE_JOBS="com.apple.bluetoothd com.apple.BlueTool com.apple.tccd com.apple.mobile.keybagd com.apple.nanoregistryd com.apple.nanoprefsyncd com.apple.mediaserverd com.apple.medialibraryd com.apple.FileProvider com.apple.searchd com.apple.assetsd com.apple.mobile.lockdown com.apple.timed" OSLOG="" bash build.sh' _ "$ROOT"
}

step_pack_boot() {
	prepare_boot_container || return 1
	run_step "Packing payload.bin" "$LOG_DIR/mkpayload.log" \
		python3 "$ROOT/xnuloader/mkpayload.py" -o "$ROOT/payload.bin" \
		"kernel=$ROOT/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837" \
		"loader=$ROOT/prebuilt/loader.efi" \
		"ramdisk=$ROOT/userland/root.apfs" || return 1

	run_step "Building mtkloader.bin" "$LOG_DIR/mtkloader.log" \
		bash "$ROOT/mtkloader/build.sh" -DXNU_CORES=8 -DXNU_VERBOSE=0 \
		'-DXNU_EXTRA_ARGS="cpus=8 small_cores_after=0 buttons=2 mtk_cpufreq=3 a75_mhz=2000 a55_mhz=1800 mali=1 mali_mhz=1050 fbscale=2 fbfit=1 fb_filter=0 fb_threads=4 fb_flip=1 fb_beat=0 vblur=0 aks_keys=1 mtkbatt=1 connsys_step=17 connsys_bt=2 touchscreen=3 touch_poll_us=2000 mtkcam_skel=1 mtk_usb=1"' || return 1

	run_step "Creating boot-ios.img" "$LOG_DIR/chainboot.log" \
		python3 "$ROOT/tools/chainboot.py" "$STOCK_BOOT_CONTAINER" "$ROOT/mtkloader/mtkloader.bin" "$ROOT/boot-ios.img" "$ROOT/payload.bin"
}

build_all() {
	check_host || return 1
	step_fetch_ios || return 1
	step_build_xnu || return 1
	step_build_mesa || return 1
	step_build_userland || return 1
	step_pack_boot || return 1
}

action_full_install() {
	clear_screen
	banner
	echo "${C_RED}${C_BOLD}"
	cat <<'EOF'
  !====================================================================!
  !  WARNING: THIS WILL PERMANENTLY WIPE ALL DATA ON YOUR PHONE!       !
  !  Android (super, cache, userdata) is DELETED and replaced with     !
  !  iOS (xnusys + ext2 iosdata). NO backup is created.                !
  !====================================================================!
EOF
	echo "${C_RESET}"
	read -r -p "Type YES to confirm you understand all data will be lost: " confirm
	if [ "$confirm" != "YES" ]; then
		warn "Cancelled."
		return 0
	fi

	connect_recovery || return 1
	build_all || return 1

	echo ""
	info "Flashing & repartitioning device $ADB_SERIAL..."
	HAKIOS_FLASH_LOG="$LOG_DIR/flash.log" bash "$ROOT/tools/install-merlinx.sh" \
		--serial "$ADB_SERIAL" \
		--repartition \
		--yes \
		--format-iosdata \
		--xnusys "$ROOT/userland/system.apfs" \
		--boot "$ROOT/boot-ios.img" \
		--reboot || { cat "$LOG_DIR/flash.log" >> "$SESSION_LOG"; fail "Flashing/repartitioning failed! Details: $LOG_DIR/flash.log"; return 1; }
	cat "$LOG_DIR/flash.log" >> "$SESSION_LOG"
	echo ""
	ok "Installation complete! First boot takes ~2 minutes."
	echo "  Tip: For future reboots, use 'idevicediagnostics restart' over USB so ext2 stays clean."
	echo ""
	prompt_cleanup_artifacts
}

prompt_cleanup_artifacts() {
	local ans
	read -r -p "Free ~8 GB of disk space by removing extracted iOS 16.5 files and system.apfs? [y/N]: " ans
	case "$ans" in
		y|Y|yes|YES) action_cleanup_artifacts ;;
		*) info "Keeping build/fetch files for faster future rebuilds." ;;
	esac
}

action_cleanup_artifacts() {
	info "Removing extracted iOS 16.5 files, system.apfs, and temporary build artifacts..."
	rm -rf \
		"$ROOT/ios165/rootfs" \
		"$ROOT/ios165/20F66__iPhone10,3_6" \
		"$ROOT/ios165/cryptex" \
		"$ROOT/ios165/cryptex-app" \
		"$ROOT/ios165/rootfs.slt" \
		"$ROOT/ios165/rootfs.dmg" \
		"$ROOT/ios165/cryptex-os.dmg" \
		"$ROOT/ios165/cryptex-app.dmg" \
		"$ROOT/ios165/.done-rootfs" \
		"$ROOT/ios165/.done-cryptex" \
		"$ROOT/ios165/.done-cryptex-app" \
		"$ROOT/userland/system.apfs" \
		"$ROOT/userland/sysroot" \
		"$ROOT/payload.bin"
	ok "Cleanup complete! Repo footprint reduced (boot-ios.img and source kept)."
}

action_update_boot() {
	clear_screen
	banner
	info "Update Boot Only (keeps existing iosdata & xnusys)"
	connect_recovery || return 1
	check_host || return 1
	step_build_xnu || return 1
	step_build_userland || return 1
	step_pack_boot || return 1
	HAKIOS_FLASH_LOG="$LOG_DIR/flash.log" bash "$ROOT/tools/install-merlinx.sh" --serial "$ADB_SERIAL" --boot "$ROOT/boot-ios.img" --reboot || { cat "$LOG_DIR/flash.log" >> "$SESSION_LOG"; fail "Flashing failed! Details: $LOG_DIR/flash.log"; return 1; }
	cat "$LOG_DIR/flash.log" >> "$SESSION_LOG"
	ok "Updated boot-ios.img and rebooted!"
}

action_update_system() {
	clear_screen
	banner
	info "Update System and Boot (replaces xnusys and boot; keeps iosdata)"
	connect_recovery || return 1
	build_all || return 1
	HAKIOS_FLASH_LOG="$LOG_DIR/flash.log" bash "$ROOT/tools/install-merlinx.sh" --serial "$ADB_SERIAL" \
		--xnusys "$ROOT/userland/system.apfs" --boot "$ROOT/boot-ios.img" --reboot || {
		cat "$LOG_DIR/flash.log" >> "$SESSION_LOG"
		fail "System/boot update failed. Details: $LOG_DIR/flash.log"
		return 1
	}
	cat "$LOG_DIR/flash.log" >> "$SESSION_LOG"
	ok "Updated system and boot images and rebooted."
}

action_repair_iosdata() {
	clear_screen
	banner
	info "Repairing ext2 filesystem on iosdata (/dev/block/by-name/iosdata)..."
	connect_recovery || return 1
	adb_cmd shell "umount /dev/block/by-name/iosdata 2>/dev/null || true; e2fsck -y /dev/block/by-name/iosdata"
	ok "e2fsck finished."
}

main_menu() {
	while true; do
		clear_screen
		banner
		echo "  Detailed session log: $SESSION_LOG"
		echo "  ${C_BOLD}Select an action:${C_RESET}"
		echo "    1) Full Install (Fresh merlinx — ${C_RED}WIPES ALL DATA${C_RESET}, fetches, builds & flashes)"
		echo "    2) Update Boot Image Only (rebuilds kernel/userland & flashes boot-ios.img)"
		echo "    3) Build Everything Only (no phone needed)"
		echo "    4) Repair iosdata (run e2fsck -y in recovery)"
		echo "    5) Run Host & Recovery Diagnostics"
		echo "    6) Clean Up Build/Fetch Artifacts (free ~8 GB disk space)"
		echo "    7) Update System and Boot (keeps iosdata; rebuilds and flashes xnusys)"
		echo "    q) Quit"
		echo ""
		read -r -p "Choice [1-7/q]: " choice
		case "$choice" in
			1) action_full_install || true; pause_enter ;;
			2) action_update_boot || true; pause_enter ;;
			3) build_all || true; pause_enter ;;
			4) action_repair_iosdata || true; pause_enter ;;
			5) check_host || true; echo ""; connect_recovery || true; pause_enter ;;
			6) action_cleanup_artifacts || true; pause_enter ;;
			7) action_update_system || true; pause_enter ;;
			q|Q) exit 0 ;;
			*) warn "Invalid choice"; sleep 1 ;;
		esac
	done
}

main_menu
