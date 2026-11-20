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
CPU_CORES="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 8)"
JOBS="${JOBS:-$CPU_CORES}"
ADB_SERIAL=""
STOCK_BOOT_CONTAINER=""
TMP_BOOT_CONTAINER=""
VERBOSE=0
ASSUME_YES=0
DRY_RUN=0
NON_INTERACTIVE_ACTION=""

cleanup() {
	[ -t 1 ] && printf '\033[?25h' || true # restore cursor
	if [ -n "$TMP_BOOT_CONTAINER" ] && [ -f "$TMP_BOOT_CONTAINER" ]; then
		rm -f "$TMP_BOOT_CONTAINER"
	fi
}
trap cleanup EXIT INT TERM

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

# Distro package manager detection for helpful install instructions
detect_pm() {
	if [ -f /etc/os-release ]; then
		# shellcheck disable=SC1091
		. /etc/os-release
		case "${ID:-}" in
			ubuntu|debian|linuxmint|pop) echo "apt" ;;
			arch|manjaro|endeavouros)     echo "pacman" ;;
			fedora|rhel|centos|rocky|alma) echo "dnf" ;;
			opensuse*|suse)               echo "zypper" ;;
			alpine)                       echo "apk" ;;
			*)
				case "${ID_LIKE:-}" in
					*debian*|*ubuntu*) echo "apt" ;;
					*arch*)            echo "pacman" ;;
					*fedora*|*rhel*)   echo "dnf" ;;
					*suse*)            echo "zypper" ;;
					*)                 echo "unknown" ;;
				esac
				;;
		esac
	elif [ "$(uname -s)" = "Darwin" ]; then
		echo "brew"
	else
		echo "unknown"
	fi
}

# Intelligent failure diagnosis and recovery handler
handle_step_failure() {
	local title="$1"
	local logfile="$2"
	local rc="$3"
	shift 3
	local cmd=("$@")

	echo ""
	fail "$title failed (exit $rc)"

	# Highlight root cause if detectable in log
	if grep -iqE "killed|out of memory|exit code 137" "$logfile" 2>/dev/null; then
		echo "  ${C_RED}* Out of Memory (OOM):${C_RESET} Process was killed due to insufficient RAM/swap."
		echo "    ${C_CYAN}Remedy:${C_RESET} Rerun with fewer parallel jobs (e.g. -j 2 or -j 4) or add swap space:"
		echo "            sudo fallocate -l 4G /swapfile && sudo chmod 600 /swapfile && sudo mkswap /swapfile && sudo swapon /swapfile"
	elif grep -iqE "no space left on device|disk full" "$logfile" 2>/dev/null; then
		echo "  ${C_RED}* Disk Full:${C_RESET} No space left on device. Free up disk space (~15-20 GB needed)."
	elif grep -iqE "device offline|device not found|error: closed|device unauthorized" "$logfile" 2>/dev/null; then
		echo "  ${C_RED}* ADB Connection Lost:${C_RESET} Device disconnected. Check USB cable and recovery status."
	elif grep -iqE "fatal error: '.*' file not found" "$logfile" 2>/dev/null; then
		local missing_hdr
		missing_hdr=$(grep -iE "fatal error: '.*' file not found" "$logfile" 2>/dev/null | head -n 3 | sed -e 's/^[[:space:]]*//' || true)
		echo "  ${C_RED}* Missing Header:${C_RESET} $missing_hdr"
	elif grep -iqE "command not found" "$logfile" 2>/dev/null; then
		local missing_cmd
		missing_cmd=$(grep -iE "command not found" "$logfile" 2>/dev/null | head -n 3 | sed -e 's/^[[:space:]]*//' || true)
		echo "  ${C_RED}* Missing Command:${C_RESET} $missing_cmd"
	fi

	local err_lines
	err_lines=$(grep -n -iE 'error:|fatal error:|cannot find|failed to' "$logfile" 2>/dev/null | grep -v "error: 0" | head -n 8 || true)
	if [ -n "$err_lines" ]; then
		echo ""
		echo "  ${C_YELLOW}Key Error Indicators:${C_RESET}"
		echo "${C_RED}$err_lines${C_RESET}"
	fi

	echo ""
	echo "${C_YELLOW}--- Last 20 lines of $logfile ---${C_RESET}"
	tail -n 20 "$logfile" 2>/dev/null || true
	echo "${C_YELLOW}----------------------------------${C_RESET}"
	echo ""

	# If non-interactive mode or stdin not a terminal, return exit code immediately
	if [ -n "$NON_INTERACTIVE_ACTION" ] || [ ! -t 0 ]; then
		return "$rc"
	fi

	# Interactive Recovery Prompt
	while true; do
		echo "${C_BOLD}Recovery Options:${C_RESET}"
		echo "  ${C_GREEN}1) Retry${C_RESET} this step"
		echo "  ${C_CYAN}2) View full log${C_RESET} ($logfile)"
		echo "  ${C_BLUE}3) Open interactive subshell${C_RESET} at project root (to fix manually)"
		echo "  ${C_YELLOW}4) Skip${C_RESET} this step and continue (advanced)"
		echo "  ${C_RED}5) Abort${C_RESET} to main menu"
		echo ""
		read -r -p "Choice [1-5]: " act
		case "$act" in
			1)
				info "Retrying: $title..."
				run_step "$title" "$logfile" "${cmd[@]}"
				return $?
				;;
			2)
				if command -v less >/dev/null 2>&1; then
					less "$logfile" || true
				elif command -v more >/dev/null 2>&1; then
					more "$logfile" || true
				else
					cat "$logfile" | head -n 100 || true
				fi
				continue
				;;
			3)
				info "Spawning subshell at $ROOT. Type 'exit' to return."
				( cd "$ROOT" && "${SHELL:-bash}" ) || true
				continue
				;;
			4)
				warn "Skipping step '$title'. Dependent steps may fail!"
				return 0
				;;
			5|q|Q)
				return "$rc"
				;;
			*)
				warn "Invalid choice."
				;;
		esac
	done
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

	local start_time
	start_time=$(date +%s 2>/dev/null || echo 0)
	local spin_chars=('⠋' '⠙' '⠹' '⠸' '⠼' '⠴' '⠦' '⠧' '⠇' '⠏')
	local spin_idx=0

	if [ "$VERBOSE" -eq 1 ]; then
		# Verbose mode: stream command output directly to terminal and log
		HAKIOS_PROGRESS_FILE="$progress_file" "$@" 2>&1 | tee -a "$logfile" "$SESSION_LOG"
		rc=${PIPESTATUS[0]}
	else
		HAKIOS_PROGRESS_FILE="$progress_file" "$@" > "$logfile" 2>&1 &
		pid=$!
		if [ -t 1 ]; then
			printf '\033[?25l' || true # hide cursor
			while kill -0 "$pid" 2>/dev/null; do
				progress=$(cat "$progress_file" 2>/dev/null || true)
				# If no explicit progress reported, extract latest activity from log tail
				if [ -z "$progress" ] && [ -f "$logfile" ]; then
					progress=$(tail -n 20 "$logfile" 2>/dev/null | grep -E '(\[[0-9]+/[0-9]+\]|Compiling|Building|Linking|Fetching|Extracting|Creating|Signing|CC |CXX |AR |LD )' | tail -n 1 | tr -d '\r\n' || true)
					progress=$(echo "$progress" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
				fi

				# Truncate cleanly to terminal width
				local term_cols
				term_cols=$(tput cols 2>/dev/null || echo 80)
				local max_prog_len=$((term_cols - 25))
				[ "$max_prog_len" -lt 15 ] && max_prog_len=15
				if [ ${#progress} -gt $max_prog_len ]; then
					progress="${progress:0:$((max_prog_len - 3))}..."
				fi

				local now duration_str
				now=$(date +%s 2>/dev/null || echo $((start_time + elapsed)))
				elapsed=$((now - start_time))
				local mins=$((elapsed / 60))
				local secs=$((elapsed % 60))
				printf -v duration_str '%02d:%02d' "$mins" "$secs"

				local spin="${spin_chars[$spin_idx]}"
				spin_idx=$(( (spin_idx + 1) % ${#spin_chars[@]} ))

				printf '\r\033[K    %s%s%s [%s] %s' "${C_CYAN}" "$spin" "${C_RESET}" "$duration_str" "${progress:-Working...}"
				sleep 0.5
			done
			printf '\033[?25h\r\033[K' || true # restore cursor
		fi
		if wait "$pid"; then rc=0; else rc=$?; fi
	fi

	local end_time total_secs
	end_time=$(date +%s 2>/dev/null || echo 0)
	total_secs=$((end_time - start_time))
	local total_mins=$((total_secs / 60))
	local rem_secs=$((total_secs % 60))
	local duration_formatted="${total_mins}m ${rem_secs}s"
	[ "$total_mins" -eq 0 ] && duration_formatted="${total_secs}s"

	if [ "$VERBOSE" -ne 1 ]; then
		cat "$logfile" >> "$SESSION_LOG"
	fi
	rm -f "$progress_file"
	if [ "$rc" -eq 0 ]; then
		ok "$title (completed in $duration_formatted)"
		return 0
	else
		handle_step_failure "$title" "$logfile" "$rc" "$@"
		return $?
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
	local cmds=(adb python3 git curl tar xz make cc clang flex bison m4 perl autoconf automake meson ninja llvm-ar patch)
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

	# Check WSL2 environment
	if [ -f /proc/sys/fs/binfmt_misc/WSLInterop ] || uname -r | grep -qi "microsoft"; then
		info "Running in Windows Subsystem for Linux (WSL2)."
		info "Ensure your phone is attached from Windows PowerShell via: usbipd wsl attach --busid <BUSID>"
	fi

	# Disk space check (~18-20 GB needed for full build)
	local free_kb free_gb
	free_kb=$(df -Pk "$ROOT" 2>/dev/null | awk 'NR==2 {print $4}' || echo 0)
	free_gb=$((free_kb / 1024 / 1024))
	if [ "$free_gb" -gt 0 ]; then
		if [ "$free_gb" -lt 12 ]; then
			fail "Low disk space: only ${free_gb} GB free on $(df -h "$ROOT" 2>/dev/null | awk 'NR==2 {print $6}')."
			echo "  Building HakiOS requires at least 15-20 GB free space during compilation."
			return 1
		elif [ "$free_gb" -lt 18 ]; then
			warn "Available disk space: ${free_gb} GB (~18-20 GB recommended for full build)."
		else
			ok "Available disk space: ${free_gb} GB"
		fi
	fi

	# RAM check & jobs advisory
	local mem_kb=0 mem_gb=0
	if [ -f /proc/meminfo ]; then
		mem_kb=$(awk '/MemTotal:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)
		mem_gb=$((mem_kb / 1024 / 1024))
		if [ "$mem_gb" -gt 0 ] && [ "$mem_gb" -lt 4 ]; then
			warn "System RAM is ${mem_gb} GB. If compilation fails with OOM, run with -j 2 or add swap."
		fi
	fi

	if [ ${#missing_pkgs[@]} -gt 0 ]; then
		fail "Missing host tools: ${missing_pkgs[*]}"
		echo ""
		echo "Install them first:"
		local pm
		pm=$(detect_pm)
		case "$pm" in
			fedora|rhel|centos|rocky|alma)
				echo "  Fedora/RHEL:   sudo dnf install -y android-tools python3 python3-pip git curl tar xz gcc gcc-c++ clang lld flex bison m4 perl autoconf automake meson ninja-build llvm patch p7zip p7zip-plugins"
				;;
			opensuse*|suse)
				echo "  openSUSE:      sudo zypper install -y android-tools python3 python3-pip git curl tar xz gcc gcc-c++ clang lld flex bison m4 perl autoconf automake meson ninja llvm patch p7zip"
				;;
			brew)
				echo "  macOS (Homebrew): brew install android-platform-tools python git curl xz make clang-format flex bison m4 perl autoconf automake meson ninja llvm patch sevenzip"
				;;
			*)
				echo "  Debian/Ubuntu: sudo apt update && sudo apt install -y adb python3 python3-venv git curl tar xz-utils build-essential clang lld flex bison m4 perl autoconf automake meson ninja-build llvm patch 7zip"
				echo "  Arch Linux:    sudo pacman -S --needed android-tools python git curl tar xz base-devel clang lld flex bison m4 perl autoconf automake meson ninja llvm patch 7zip"
				;;
		esac
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
		if [ -z "$NON_INTERACTIVE_ACTION" ]; then
			read -r -p "Run ./tools/get-zig.sh now to fetch zig? [Y/n] " ans
			if [[ "${ans:-Y}" =~ ^[Yy]$ ]]; then
				run_step "Fetching zig (ld.lld)" "$LOG_DIR/get-zig.log" bash "$ROOT/tools/get-zig.sh"
			else
				fail "Cannot build mtkloader without ld.lld or zig."
				return 1
			fi
		else
			fail "Cannot build mtkloader without ld.lld or zig. Run ./tools/get-zig.sh first."
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
			echo "  3. In WSL2, attach phone via: usbipd wsl attach --busid <BUSID>"
			read -r -p "Press [Enter] to retry, [r] to run 'adb reboot recovery', or type 'q' to cancel: " ans
			if [ "$ans" = "q" ] || [ "$ans" = "Q" ]; then
				return 1
			elif [ "$ans" = "r" ] || [ "$ans" = "R" ]; then
				info "Sending 'adb reboot recovery' command..."
				adb reboot recovery 2>/dev/null || true
				sleep 5
			fi
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

		# Check battery level if exposed by recovery kernel
		local batt
		batt=$(adb_cmd shell "cat /sys/class/power_supply/battery/capacity 2>/dev/null || cat /sys/class/power_supply/bms/capacity 2>/dev/null" | tr -d '\r\n ' || true)
		if [[ "$batt" =~ ^[0-9]+$ ]]; then
			if [ "$batt" -lt 25 ]; then
				warn "Device battery is LOW (${batt}%). Please charge to >= 30% before flashing!"
				read -r -p "Continue anyway? [y/N] " ans
				[[ "$ans" =~ ^[Yy]$ ]] || return 1
			else
				ok "Device battery: ${batt}%"
			fi
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
		custom_boot="${custom_boot/#\~/$HOME}"
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
	local mesa="$ROOT/xnu-deps/build/mesa/out" patch_hash
	patch_hash=$(cat "$ROOT"/tools/mesa/*.patch "$ROOT"/tools/mesa/redwood-drm/* | sha256sum)
	if [ -s "$mesa/libOSMesa.8.dylib" ] && [ -s "$mesa/gl.xml" ] && [ -s "$mesa/exported-gl.txt" ] &&
	   [ "$(cat "$mesa/built-drivers" 2>/dev/null)" = "softpipe,panfrost" ] &&
	   [ "$(cat "$mesa/built-from" 2>/dev/null)" = "$patch_hash" ]; then
		ok "Mesa Panfrost (libOSMesa.8.dylib) already built (skipping)"
		return 0
	fi
	run_step "Building Mesa Panfrost (softpipe,panfrost)" "$LOG_DIR/mk-mesa.log" env DRIVERS="softpipe,panfrost" bash "$ROOT/tools/mk-mesa.sh"
}

step_build_userland() {
	run_step "Building userland (root.apfs + system.apfs)" "$LOG_DIR/userland-build.log" \
		bash -c 'cd "$1/userland" && DRIVERS="softpipe,panfrost" GL=1 WIFID=1 RC=rc.launchd.aks MORE_JOBS="com.apple.bluetoothd com.apple.BlueTool com.apple.tccd com.apple.mobile.keybagd com.apple.nanoregistryd com.apple.nanoprefsyncd com.apple.mediaserverd com.apple.medialibraryd com.apple.FileProvider com.apple.searchd com.apple.assetsd com.apple.mobile.lockdown com.apple.timed" OSLOG="" bash build.sh' _ "$ROOT" || return 1
	local file
	for file in lib/libOSMesa.8.dylib lib/libredwood-gl.dylib \
		fw/connsys/WIFI_RAM_CODE_soc1_0_1a_1.bin fw/connsys/soc1_0_patch_mcu_1a_1_hdr.bin \
		fw/connsys/soc1_0_ram_bt_1a_1_hdr.bin fw/connsys/soc1_0_ram_mcu_1a_1_hdr.bin \
		fw/connsys/soc1_0_ram_wifi_1a_1_hdr.bin fw/connsys/BT_FW.cfg fw/connsys/WMT_SOC.cfg \
		fw/connsys/wifi.cfg fw/connsys/txpowerctrl.cfg; do
		if [ ! -s "$ROOT/userland/root/$file" ] ||
		   ! cmp -s "$ROOT/userland/root/$file" <(python3 "$ROOT/tools/apfs/apfs.py" cat "$ROOT/userland/root.apfs" "/$file"); then
			fail "Required GPU or radio file is missing or differs in root.apfs: $file"
			return 1
		fi
	done
	ok "GPU libraries and radio firmware verified in boot image"
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

# Pre-flash validation helper
verify_flash_artifacts() {
	local need_sys="${1:-0}"
	if [ ! -s "$ROOT/boot-ios.img" ]; then
		fail "Missing boot image: $ROOT/boot-ios.img"
		return 1
	fi
	local boot_size
	boot_size=$(wc -c < "$ROOT/boot-ios.img")
	if [ "$boot_size" -gt $((64 * 1024 * 1024)) ]; then
		fail "boot-ios.img exceeds 64 MiB partition limit (${boot_size} bytes)."
		return 1
	fi
	if [ "$need_sys" -eq 1 ]; then
		if [ ! -s "$ROOT/userland/system.apfs" ]; then
			fail "Missing system image: $ROOT/userland/system.apfs"
			return 1
		fi
		local sys_size
		sys_size=$(wc -c < "$ROOT/userland/system.apfs")
		if [ "$sys_size" -gt $((8390592 * 512)) ]; then
			fail "system.apfs exceeds 4.0 GiB partition capacity (${sys_size} bytes)."
			return 1
		fi
	fi
	ok "Build artifacts verified and within partition limits"
	return 0
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
	if [ "$ASSUME_YES" -eq 0 ]; then
		read -r -p "Type YES to confirm you understand all data will be lost: " confirm
		if [ "$confirm" != "YES" ]; then
			warn "Cancelled."
			return 0
		fi
	fi

	connect_recovery || return 1
	build_all || return 1
	verify_flash_artifacts 1 || return 1

	echo ""
	info "Flashing & repartitioning device $ADB_SERIAL..."
	local flash_args=(
		--serial "$ADB_SERIAL"
		--repartition
		--yes
		--format-iosdata
		--xnusys "$ROOT/userland/system.apfs"
		--boot "$ROOT/boot-ios.img"
		--reboot
	)
	[ "$DRY_RUN" -eq 1 ] && flash_args+=(--dry-run)

	HAKIOS_FLASH_LOG="$LOG_DIR/flash.log" bash "$ROOT/tools/install-merlinx.sh" "${flash_args[@]}" || {
		cat "$LOG_DIR/flash.log" >> "$SESSION_LOG"
		fail "Flashing/repartitioning failed! Details: $LOG_DIR/flash.log"
		return 1
	}
	cat "$LOG_DIR/flash.log" >> "$SESSION_LOG"
	echo ""
	ok "Installation complete! First boot takes ~2 minutes."
	echo "  Tip: For future reboots, use 'idevicediagnostics restart' over USB so ext2 stays clean."
	echo ""
	if [ "$ASSUME_YES" -eq 0 ]; then
		prompt_cleanup_artifacts
	fi
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
	verify_flash_artifacts 0 || return 1

	local flash_args=(
		--serial "$ADB_SERIAL"
		--boot "$ROOT/boot-ios.img"
		--reboot
	)
	[ "$DRY_RUN" -eq 1 ] && flash_args+=(--dry-run)

	HAKIOS_FLASH_LOG="$LOG_DIR/flash.log" bash "$ROOT/tools/install-merlinx.sh" "${flash_args[@]}" || {
		cat "$LOG_DIR/flash.log" >> "$SESSION_LOG"; fail "Flashing failed! Details: $LOG_DIR/flash.log"; return 1; }
	cat "$LOG_DIR/flash.log" >> "$SESSION_LOG"
	ok "Updated boot-ios.img and rebooted!"
}

action_update_system() {
	clear_screen
	banner
	info "Update System and Boot (replaces xnusys and boot; keeps iosdata)"
	connect_recovery || return 1
	build_all || return 1
	verify_flash_artifacts 1 || return 1

	local flash_args=(
		--serial "$ADB_SERIAL"
		--xnusys "$ROOT/userland/system.apfs"
		--boot "$ROOT/boot-ios.img"
		--reboot
	)
	[ "$DRY_RUN" -eq 1 ] && flash_args+=(--dry-run)

	HAKIOS_FLASH_LOG="$LOG_DIR/flash.log" bash "$ROOT/tools/install-merlinx.sh" "${flash_args[@]}" || {
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

log_viewer_menu() {
	while true; do
		clear_screen
		banner
		echo "${C_BOLD}Log & Diagnostics Viewer:${C_RESET}"
		echo "  Log Directory: $LOG_DIR"
		echo ""
		echo "    1) View Active Session Log ($(basename "$SESSION_LOG"))"
		echo "    2) View iOS Fetch Log (fetch-ios165.log)"
		echo "    3) View XNU Kernel Build Log (xnu-build.log)"
		echo "    4) View Mesa Panfrost Log (mk-mesa.log)"
		echo "    5) View Userland Build Log (userland-build.log)"
		echo "    6) View Flashing Log (flash.log)"
		echo "    b) Back to main menu"
		echo ""
		read -r -p "Choice [1-6/b]: " lchoice
		local target=""
		case "$lchoice" in
			1) target="$SESSION_LOG" ;;
			2) target="$LOG_DIR/fetch-ios165.log" ;;
			3) target="$LOG_DIR/xnu-build.log" ;;
			4) target="$LOG_DIR/mk-mesa.log" ;;
			5) target="$LOG_DIR/userland-build.log" ;;
			6) target="$LOG_DIR/flash.log" ;;
			b|B|q|Q) return 0 ;;
			*) warn "Invalid choice"; sleep 1; continue ;;
		esac

		if [ -n "$target" ] && [ -f "$target" ]; then
			if command -v less >/dev/null 2>&1; then
				less "$target" || true
			elif command -v more >/dev/null 2>&1; then
				more "$target" || true
			else
				cat "$target" | tail -n 50 || true
				pause_enter
			fi
		else
			warn "Log file does not exist yet: $target"
			pause_enter
		fi
	done
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
		echo "    8) View Logs"
		echo "    q) Quit"
		echo ""
		read -r -p "Choice [1-8/q]: " choice
		case "$choice" in
			1) action_full_install || true; pause_enter ;;
			2) action_update_boot || true; pause_enter ;;
			3) build_all || true; pause_enter ;;
			4) action_repair_iosdata || true; pause_enter ;;
			5) check_host || true; echo ""; connect_recovery || true; pause_enter ;;
			6) action_cleanup_artifacts || true; pause_enter ;;
			7) action_update_system || true; pause_enter ;;
			8) log_viewer_menu ;;
			q|Q) exit 0 ;;
			*) warn "Invalid choice"; sleep 1 ;;
		esac
	done
}

show_help() {
	cat <<EOF
HakiOS setup-merlinx.sh - Interactive & Scriptable Installer

Usage: ./setup-merlinx.sh [options]

Options:
  -h, --help            Show this help message
  -c, --check           Run host and recovery environment checks
  -b, --build           Build all components (kernel, Mesa, userland, boot-ios.img)
  -i, --install         Full install (repartition + flash; WIPES DATA)
  --update-boot         Rebuild and flash boot-ios.img only
  --update-system       Rebuild and flash xnusys and boot-ios.img (preserves iosdata)
  --repair-data         Run e2fsck on device /dev/block/by-name/iosdata
  --clean               Clean temporary build and iOS extraction artifacts
  -j, --jobs <N>        Set build concurrency (default: $CPU_CORES)
  -s, --serial <SERIAL> Target specific ADB device serial
  -v, --verbose         Stream full command output in real time
  -y, --yes             Skip confirmation prompts
  --dry-run             Simulate flashing without modifying device

Without options, launches the interactive ASCII TUI menu.
EOF
}

parse_args() {
	while [ $# -gt 0 ]; do
		case "$1" in
			-h|--help)
				show_help
				exit 0
				;;
			-c|--check)
				NON_INTERACTIVE_ACTION="check"
				shift
				;;
			-b|--build|--build-only)
				NON_INTERACTIVE_ACTION="build"
				shift
				;;
			-i|--install|--full-install)
				NON_INTERACTIVE_ACTION="install"
				shift
				;;
			--update-boot)
				NON_INTERACTIVE_ACTION="update_boot"
				shift
				;;
			--update-system)
				NON_INTERACTIVE_ACTION="update_system"
				shift
				;;
			--repair-data)
				NON_INTERACTIVE_ACTION="repair_data"
				shift
				;;
			--clean)
				NON_INTERACTIVE_ACTION="clean"
				shift
				;;
			-j|--jobs)
				JOBS="$2"
				shift 2
				;;
			-s|--serial)
				ADB_SERIAL="$2"
				shift 2
				;;
			-v|--verbose)
				VERBOSE=1
				shift
				;;
			-y|--yes)
				ASSUME_YES=1
				shift
				;;
			--dry-run)
				DRY_RUN=1
				shift
				;;
			*)
				echo "Error: Unknown argument: $1" >&2
				show_help
				exit 1
				;;
		esac
	done
}

main() {
	parse_args "$@"

	if [ -n "$NON_INTERACTIVE_ACTION" ]; then
		case "$NON_INTERACTIVE_ACTION" in
			check)
				check_host
				connect_recovery
				;;
			build)
				build_all
				;;
			install)
				action_full_install
				;;
			update_boot)
				action_update_boot
				;;
			update_system)
				action_update_system
				;;
			repair_data)
				action_repair_iosdata
				;;
			clean)
				action_cleanup_artifacts
				;;
		esac
		exit $?
	fi

	main_menu
}

main "$@"
