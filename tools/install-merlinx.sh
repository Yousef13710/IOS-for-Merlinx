#!/bin/bash
# Installer for HakiOS on Xiaomi Redmi Note 9 (merlinx, MT6768) via TWRP ADB.
set -euo pipefail

BOOT_IMG=""
XNUSYS_IMG=""
DO_REPART=0
DO_FORMAT=0
DO_REBOOT=0
DRY_RUN=0
ADB_SERIAL=""

usage() {
	cat <<'EOF'
Usage: install-merlinx.sh [options]

Options:
  --boot <path>         Flash boot image (e.g. boot-step7-storage.img) to /dev/block/by-name/boot
  --xnusys <path>       Flash xnusys packfs image (4 GiB) to /dev/block/by-name/xnusys
  --repartition         DESTRUCTIVE: delete super/cache/userdata and create iosdata (p43) + xnusys (p48)
  --format-iosdata      Format /dev/block/by-name/iosdata as ext2 (-b 4096 -L iosdata)
  --serial <id>         Target specific adb device serial
  --reboot              Reboot device after flashing
  --dry-run             Print actions without modifying the device
  -h, --help            Show this help

Examples:
  # First-time full setup (in TWRP):
  bash tools/install-merlinx.sh --repartition --format-iosdata --xnusys xnusys.img --boot boot-step7-storage.img --reboot

  # Update boot.img only (preserves iosdata and xnusys):
  bash tools/install-merlinx.sh --boot boot-step7-storage.img --reboot
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
		--boot) BOOT_IMG="$2"; shift 2 ;;
		--xnusys) XNUSYS_IMG="$2"; shift 2 ;;
		--repartition) DO_REPART=1; shift ;;
		--format-iosdata) DO_FORMAT=1; shift ;;
		--serial) ADB_SERIAL="$2"; shift 2 ;;
		--reboot) DO_REBOOT=1; shift ;;
		--dry-run) DRY_RUN=1; shift ;;
		-h|--help) usage; exit 0 ;;
		*) echo "Unknown option: $1" >&2; usage; exit 1 ;;
	esac
done

if [ -z "$BOOT_IMG" ] && [ -z "$XNUSYS_IMG" ] && [ "$DO_REPART" -eq 0 ] && [ "$DO_FORMAT" -eq 0 ]; then
	usage
	exit 1
fi

ADB_BIN="${ADB_BIN:-}"
if [ -z "$ADB_BIN" ]; then
	if command -v adb >/dev/null 2>&1 && { ! grep -qi microsoft /proc/version 2>/dev/null || adb devices 2>/dev/null | awk 'NR>1 && NF>=2' | grep -q .; }; then
		ADB_BIN="adb"
	elif grep -qi microsoft /proc/version 2>/dev/null && command -v adb.exe >/dev/null 2>&1; then
		ADB_BIN="adb.exe"
	else
		ADB_BIN="adb"
	fi
fi

adb_host_path() {
	if [[ "$ADB_BIN" == *.exe ]] && command -v wslpath >/dev/null 2>&1; then
		wslpath -w "$1"
	else
		printf '%s' "$1"
	fi
}

ADB=("$ADB_BIN")
if [ -n "$ADB_SERIAL" ]; then
	ADB=("$ADB_BIN" -s "$ADB_SERIAL")
fi

run_adb() {
	if [ "$DRY_RUN" -eq 1 ]; then
		echo "[dry-run] ${ADB[*]} $*"
	else
		"${ADB[@]}" "$@"
	fi
}

echo "== Checking Recovery ADB connection =="
STATE=$("${ADB[@]}" get-state 2>/dev/null || true)
if [ "$STATE" != "recovery" ] && [ "$STATE" != "device" ]; then
	echo "Error: device must be booted into recovery with root ADB (got state: '${STATE:-none}')." >&2
	exit 1
fi

UID_CHECK=$("${ADB[@]}" shell "id -u" 2>/dev/null | tr -d '\r' || true)
if [ "$UID_CHECK" != "0" ]; then
	echo "Error: recovery ADB is not running as root (id -u = '${UID_CHECK:-unknown}'). Enable root ADB in your recovery." >&2
	exit 1
fi

DEVICE=$("${ADB[@]}" shell "getprop ro.product.device" | tr -d '\r')
echo "Connected recovery device: ${DEVICE:-unknown}"
if [ -n "$DEVICE" ] && [ "$DEVICE" != "merlin" ] && [ "$DEVICE" != "merlinx" ] && [ "$DEVICE" != "merlinnfc" ]; then
	echo "Warning: expected merlin/merlinx, got '$DEVICE'. Proceeding carefully..."
fi

if [ "$DO_REPART" -eq 1 ]; then
	GPT_INFO=$("${ADB[@]}" shell "sgdisk -p /dev/block/mmcblk0" 2>/dev/null | tr -d '\r')
	read -r IOSDATA_START IOSDATA_END XNUSYS_START XNUSYS_END DISK_DESC < <(python3 -c '
import re, sys
text = sys.stdin.read()
disk_m = re.search(r"Disk /dev/block/mmcblk0:\s*(\d+)\s*sectors,\s*([^\n]+)", text)
last_m = re.search(r"last usable sector is\s*(\d+)", text)
if not last_m:
    sys.exit("Could not parse last usable sector from sgdisk -p")
last_usable = int(last_m.group(1))
parts = {}
for line in text.splitlines():
    m = re.match(r"^\s*(\d+)\s+(\d+)\s+(\d+)\s+", line)
    if m:
        parts[int(m.group(1))] = (int(m.group(2)), int(m.group(3)))

if 43 in parts:
    iosdata_start = parts[43][0]
else:
    prev_ends = [end for num, (_, end) in parts.items() if num < 43]
    iosdata_start = ((max(prev_ends) + 1 + 2047) // 2048) * 2048 if prev_ends else 4767744

tail_starts = [start for num, (start, _) in parts.items() if num > 45 and num != 48]
limit_end = min(tail_starts) - 1 if tail_starts else last_usable
# Match 64 GB layout (122021823 on 122142686-sector disk): align down to 2048-sector boundary,
# leaving ~59 MiB margin at the end of usable space when no tail partitions are listed.
if not tail_starts and limit_end == last_usable:
    max_end = ((last_usable - 120000 + 1) // 2048) * 2048 - 1
else:
    max_end = ((limit_end + 1) // 2048) * 2048 - 1

xnusys_sectors = 8390592  # 4.0 GiB (matches 113631232..122021823)
xnusys_end = max_end
xnusys_start = xnusys_end - xnusys_sectors + 1
iosdata_end = xnusys_start - 1
if iosdata_end <= iosdata_start + 2097152:
    sys.exit(f"Invalid computed layout: iosdata {iosdata_start}..{iosdata_end}, xnusys {xnusys_start}..{xnusys_end}")
desc = f"{disk_m.group(2).strip()} ({disk_m.group(1)} sectors)" if disk_m else f"{last_usable} sectors"
print(iosdata_start, iosdata_end, xnusys_start, xnusys_end, desc)
' <<< "$GPT_INFO")

	IOSDATA_GIB=$(python3 -c "print(f'{($IOSDATA_END - $IOSDATA_START + 1) * 512 / (1024**3):.1f}')")
	echo ""
	echo "Detected eMMC: $DISK_DESC"
	echo "WARNING: --repartition will DELETE Android 'super' (p43), 'cache' (p44), and 'userdata' (p45)"
	echo "and create 'iosdata' (p43, ~${IOSDATA_GIB} GiB, sectors ${IOSDATA_START}..${IOSDATA_END}) and 'xnusys' (p48, 4.0 GiB, sectors ${XNUSYS_START}..${XNUSYS_END})."
	if [ "$DRY_RUN" -eq 0 ]; then
		read -r -p "Type YES to continue: " CONFIRM
		if [ "$CONFIRM" != "YES" ]; then
			echo "Aborted."
			exit 1
		fi
	fi
	BACKUP="gpt_backup_merlinx_$(date +%Y%m%d_%H%M%S).bin"
	echo "== Backing up GPT to $BACKUP =="
	run_adb shell "sgdisk --backup=/tmp/$BACKUP /dev/block/mmcblk0"
	run_adb pull "/tmp/$BACKUP" "$(adb_host_path "./$BACKUP")"
	run_adb shell "rm -f /tmp/$BACKUP"

	echo "== Repartitioning /dev/block/mmcblk0 =="
	run_adb shell "umount /data /cache /sdcard /iosdata 2>/dev/null || true"
	run_adb shell "sgdisk --delete=43 --delete=44 --delete=45 --delete=48 /dev/block/mmcblk0 2>/dev/null || true"
	run_adb shell "sgdisk --new=43:${IOSDATA_START}:${IOSDATA_END} --change-name=43:iosdata --typecode=43:8300 /dev/block/mmcblk0"
	run_adb shell "sgdisk --new=48:${XNUSYS_START}:${XNUSYS_END} --change-name=48:xnusys --typecode=48:8300 /dev/block/mmcblk0"
	run_adb shell "partprobe /dev/block/mmcblk0 2>/dev/null || blockdev --rereadpt /dev/block/mmcblk0 2>/dev/null || true"
	sleep 1
fi

if [ "$DO_FORMAT" -eq 1 ]; then
	echo "== Formatting /dev/block/by-name/iosdata as ext2 =="
	run_adb shell "IOSDATA_DEV=/dev/block/by-name/iosdata; [ -e \"\$IOSDATA_DEV\" ] || IOSDATA_DEV=/dev/block/mmcblk0p43; umount \"\$IOSDATA_DEV\" 2>/dev/null || true; (command -v mke2fs >/dev/null && mke2fs -t ext2 -b 4096 -F -L iosdata \"\$IOSDATA_DEV\") || mkfs.ext2 -b 4096 -F -L iosdata \"\$IOSDATA_DEV\""
fi

if [ -n "$XNUSYS_IMG" ]; then
	if [ ! -f "$XNUSYS_IMG" ]; then
		echo "Error: xnusys image not found: $XNUSYS_IMG" >&2
		exit 1
	fi
	echo "== Flashing $XNUSYS_IMG to /dev/block/by-name/xnusys =="
	if [ "$DRY_RUN" -eq 1 ]; then
		echo "[dry-run] split 512M + ${ADB[*]} push /tmp/xnusys.part + dd of=/dev/block/by-name/xnusys bs=4M seek=..."
	else
		XNUSYS_DEV=$("${ADB[@]}" shell '[ -e /dev/block/by-name/xnusys ] && echo /dev/block/by-name/xnusys || echo /dev/block/mmcblk0p48' | tr -d '\r')
		TMP_SPLIT=$(mktemp -d)
		split -b 512M -d -a 3 "$XNUSYS_IMG" "$TMP_SPLIT/part_"
		SEEK_4M=0
		for part in "$TMP_SPLIT"/part_*; do
			echo "  flashing $(basename "$part") at offset $((SEEK_4M * 4)) MiB..."
			"${ADB[@]}" push "$(adb_host_path "$part")" /tmp/xnusys.part
			"${ADB[@]}" shell "dd if=/tmp/xnusys.part of=\"$XNUSYS_DEV\" bs=4M seek=$SEEK_4M conv=notrunc && rm -f /tmp/xnusys.part"
			SEEK_4M=$((SEEK_4M + 128))
		done
		"${ADB[@]}" shell "sync"
		rm -rf "$TMP_SPLIT"
	fi
fi

if [ -n "$BOOT_IMG" ]; then
	if [ ! -f "$BOOT_IMG" ]; then
		echo "Error: boot image not found: $BOOT_IMG" >&2
		exit 1
	fi
	echo "== Flashing $BOOT_IMG to /dev/block/by-name/boot =="
	run_adb push "$(adb_host_path "$BOOT_IMG")" /tmp/boot.img
	run_adb shell "dd if=/tmp/boot.img of=/dev/block/by-name/boot bs=4M && sync && rm -f /tmp/boot.img"
fi

if [ "$DO_REBOOT" -eq 1 ]; then
	echo "== Rebooting device =="
	run_adb reboot
fi

echo "Done."
