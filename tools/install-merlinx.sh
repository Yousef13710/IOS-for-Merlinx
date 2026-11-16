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

ADB=(adb)
if [ -n "$ADB_SERIAL" ]; then
	ADB=(adb -s "$ADB_SERIAL")
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
	echo ""
	echo "WARNING: --repartition will DELETE Android 'super' (p43), 'cache' (p44), and 'userdata' (p45)"
	echo "and create 'iosdata' (p43, ~51.9 GiB, sectors 4767744..113631231) and 'xnusys' (p48, 4.0 GiB, sectors 113631232..122021823)."
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
	run_adb pull "/tmp/$BACKUP" "./$BACKUP"
	run_adb shell "rm -f /tmp/$BACKUP"

	echo "== Repartitioning /dev/block/mmcblk0 =="
	run_adb shell "umount /data /cache /sdcard /iosdata 2>/dev/null || true"
	run_adb shell "sgdisk --delete=43 --delete=44 --delete=45 --delete=48 /dev/block/mmcblk0 2>/dev/null || true"
	run_adb shell "sgdisk --new=43:4767744:113631231 --change-name=43:iosdata --typecode=43:8300 /dev/block/mmcblk0"
	run_adb shell "sgdisk --new=48:113631232:122021823 --change-name=48:xnusys --typecode=48:8300 /dev/block/mmcblk0"
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
	run_adb push "$XNUSYS_IMG" /tmp/xnusys.img
	run_adb shell "XNUSYS_DEV=/dev/block/by-name/xnusys; [ -e \"\$XNUSYS_DEV\" ] || XNUSYS_DEV=/dev/block/mmcblk0p48; dd if=/tmp/xnusys.img of=\"\$XNUSYS_DEV\" bs=4M && sync && rm -f /tmp/xnusys.img"
fi

if [ -n "$BOOT_IMG" ]; then
	if [ ! -f "$BOOT_IMG" ]; then
		echo "Error: boot image not found: $BOOT_IMG" >&2
		exit 1
	fi
	echo "== Flashing $BOOT_IMG to /dev/block/by-name/boot =="
	run_adb push "$BOOT_IMG" /tmp/boot.img
	run_adb shell "dd if=/tmp/boot.img of=/dev/block/by-name/boot bs=4M && sync && rm -f /tmp/boot.img"
fi

if [ "$DO_REBOOT" -eq 1 ]; then
	echo "== Rebooting device =="
	run_adb reboot
fi

echo "Done."
