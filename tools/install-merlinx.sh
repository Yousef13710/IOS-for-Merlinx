#!/bin/bash
# Installer for HakiOS on Xiaomi Redmi Note 9 (merlinx, MT6768) via TWRP ADB.
set -euo pipefail

BOOT_IMG=""
XNUSYS_IMG=""
DO_REPART=0
DO_FORMAT=0
DO_REBOOT=0
DRY_RUN=0
YES_CONFIRMED=0
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
  --yes                 Skip interactive --repartition confirmation
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
		--yes) YES_CONFIRMED=1; shift ;;
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
	# Read the GPT straight off the eMMC (the first 1 MiB) instead of parsing `sgdisk -p`: Android recoveries ship
	# different sgdisk builds (some do not even know -p), but dd and adb pull always work.
	GPT_HEAD=$(mktemp)
	"${ADB[@]}" shell "dd if=/dev/block/mmcblk0 of=/tmp/gpt_head.bin bs=512 count=2048 2>/dev/null"
	"${ADB[@]}" pull /tmp/gpt_head.bin "$GPT_HEAD" >/dev/null
	"${ADB[@]}" shell "rm -f /tmp/gpt_head.bin"
	GPT_TOTAL=$("${ADB[@]}" shell "cat /sys/block/mmcblk0/size 2>/dev/null" | tr -d '\r\n ')
	GPT_INFO=""
	if ! read -r IOSDATA_START IOSDATA_END XNUSYS_START XNUSYS_END XNUSYS_PART DEL_LIST DISK_DESC < <(python3 - "$GPT_HEAD" "${GPT_TOTAL:-0}" <<'PY'
import struct, sys

data = open(sys.argv[1], "rb").read()
total = int(sys.argv[2] or 0)

def fail(msg):
    sys.stderr.write("Error: " + msg + "\n")
    sys.exit(1)

if data[512:520] != b"EFI PART":
    fail("no GPT found on /dev/block/mmcblk0 (is this really a merlin/merlinx in recovery?)")
first_usable, last_usable = struct.unpack_from("<QQ", data, 512 + 40)
entries_lba, count, size = struct.unpack_from("<QII", data, 512 + 72)
if size < 128 or count == 0 or (entries_lba * 512 + count * size) > len(data):
    fail("unexpected GPT layout (entries at LBA %d, %d x %d bytes)" % (entries_lba, count, size))

parts = {}
for i in range(count):
    e = data[entries_lba * 512 + i * size: entries_lba * 512 + (i + 1) * size]
    if e[:16] == b"\0" * 16:
        continue
    first, last = struct.unpack_from("<QQ", e, 32)
    name = e[56:128].decode("utf-16le", "ignore").split("\0")[0]
    parts[i + 1] = (first, last, name)

sys.stderr.write("== Current partition table of the phone (%d slots, usable sectors %d..%d) ==\n" % (count, first_usable, last_usable))
for n in sorted(parts):
    f, l, name = parts[n]
    sys.stderr.write("  p%-3d %12d %12d  %7.2f GiB  %s\n" % (n, f, l, (l - f + 1) * 512 / 2**30, name))

OURS = {43, 44, 45, 48}
expected = {"super", "cache", "userdata", "iosdata", "xnusys"}
for n in (43, 44, 45):
    if n in parts and parts[n][2] not in expected:
        fail("p%d is '%s', not Android's super/cache/userdata. Refusing to touch it. Nothing was changed." % (n, parts[n][2]))

present = [n for n in sorted(OURS) if n in parts]
if 43 not in parts and not present:
    fail("none of p43/p44/p45/p48 exist; this does not look like a merlin/merlinx layout. Nothing was changed.")
region_start = parts[43][0] if 43 in parts else min(parts[n][0] for n in present)
# the space those partitions occupy now is ours to reuse, up to the next foreign partition
region_end = max(parts[n][1] for n in present)
tail = [f for n, (f, l, nm) in parts.items() if n not in OURS and f > region_start]
if 45 not in parts:
    # a half-done earlier run (userdata already gone): take the free space up to the next foreign partition,
    # or to the end of the disk less ~59 MiB, which is where stock userdata ended
    region_end = (min(tail) - 1) if tail else ((last_usable - 120000 + 1) // 2048) * 2048 - 1 - 64
if tail and min(tail) - 1 < region_end:
    region_end = min(tail) - 1
if region_end > last_usable:
    region_end = last_usable

xnusys_sectors = 8390592  # 4.0 GiB
xnusys_end = region_end
xnusys_start = xnusys_end - xnusys_sectors + 1
iosdata_start = region_start
iosdata_end = xnusys_start - 1
if iosdata_end <= iosdata_start + 2097152:
    fail("not enough room: iosdata %d..%d, xnusys %d..%d. Nothing was changed." % (iosdata_start, iosdata_end, xnusys_start, xnusys_end))
# the partition number is only a label (the kernel and installer look partitions up by name)
xnusys_part = 48 if count >= 48 else 44
dels = ",".join(str(n) for n in present) or "-"
desc = "%.1f GiB eMMC (%d sectors, %d GPT slots)" % ((total or last_usable + 34) * 512 / 2**30, total or last_usable + 34, count)
print(iosdata_start, iosdata_end, xnusys_start, xnusys_end, xnusys_part, dels, desc)
PY
); then
		rm -f "$GPT_HEAD"
		echo "Error: could not compute the partition layout (see above). Nothing was changed on the phone." >&2
		exit 1
	fi
	rm -f "$GPT_HEAD"

	IOSDATA_GIB=$(python3 -c "print(f'{($IOSDATA_END - $IOSDATA_START + 1) * 512 / (1024**3):.1f}')")
	echo ""
	echo "Detected eMMC: $DISK_DESC"
	echo "WARNING: --repartition will DELETE Android 'super' (p43), 'cache' (p44), and 'userdata' (p45)"
	echo "and create 'iosdata' (p43, ~${IOSDATA_GIB} GiB, sectors ${IOSDATA_START}..${IOSDATA_END}) and 'xnusys' (p${XNUSYS_PART}, 4.0 GiB, sectors ${XNUSYS_START}..${XNUSYS_END})."
	if [ "$DRY_RUN" -eq 0 ] && [ "$YES_CONFIRMED" -eq 0 ]; then
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
	# One sgdisk call: it saves the table only if EVERY operation succeeds, so a failure leaves Android's partitions untouched.
	SG_ARGS=""
	for n in ${DEL_LIST//,/ }; do [ "$n" = "-" ] || SG_ARGS="$SG_ARGS --delete=$n"; done
	SG_ARGS="$SG_ARGS --new=43:${IOSDATA_START}:${IOSDATA_END} --change-name=43:iosdata --typecode=43:8300"
	SG_ARGS="$SG_ARGS --new=${XNUSYS_PART}:${XNUSYS_START}:${XNUSYS_END} --change-name=${XNUSYS_PART}:xnusys --typecode=${XNUSYS_PART}:8300"
	if ! run_adb shell "sgdisk $SG_ARGS /dev/block/mmcblk0"; then
		echo "Error: sgdisk could not create the partitions; the partition table was NOT changed." >&2
		echo "Please send the table printed above when asking for help." >&2
		exit 1
	fi
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
		XNUSYS_DEV=$("${ADB[@]}" shell "[ -e /dev/block/by-name/xnusys ] && echo /dev/block/by-name/xnusys || echo /dev/block/mmcblk0p${XNUSYS_PART:-48}" | tr -d '\r')
		TMP_SPLIT=$(mktemp -d)
		split -b 512M -d -a 3 "$XNUSYS_IMG" "$TMP_SPLIT/part_"
		SEEK_4M=0
		for part in "$TMP_SPLIT"/part_*; do
			echo "  flashing $(basename "$part") at offset $((SEEK_4M * 4)) MiB..."
			"${ADB[@]}" push "$part" /tmp/xnusys.part
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
	run_adb push "$BOOT_IMG" /tmp/boot.img
	run_adb shell "dd if=/tmp/boot.img of=/dev/block/by-name/boot bs=4M && sync && rm -f /tmp/boot.img"
fi

if [ "$DO_REBOOT" -eq 1 ]; then
	echo "== Rebooting device =="
	run_adb reboot
fi

echo "Done."
