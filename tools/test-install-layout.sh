#!/bin/bash
# Exercise GPT planning without writing to a device.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
python3 - "$tmp" <<'PY'
import pathlib, struct, sys
root = pathlib.Path(sys.argv[1])
for case in ('stock', 'installed', 'foreign48', 'overlap'):
    data = bytearray(1024 * 1024)
    data[512:520] = b'EFI PART'
    struct.pack_into('<QQ', data, 512 + 40, 34, 125000000)
    struct.pack_into('<QII', data, 512 + 72, 2, 128, 128)
    parts = {43: (10000000, 30000000, 'super'), 44: (30000001, 31000000, 'cache'), 45: (31000001, 120000000, 'userdata')}
    if case == 'installed':
        parts = {43: (10000000, 110000000, 'iosdata'), 48: (110000001, 120000000, 'xnusys')}
    if case == 'foreign48':
        parts[48] = (9000000, 9001000, 'important')
    if case == 'overlap':
        parts[40] = (20000000, 20001000, 'important')
    for n, (start, end, name) in parts.items():
        at = 1024 + (n - 1) * 128
        data[at:at+16] = b'X' * 16
        struct.pack_into('<QQ', data, at + 32, start, end)
        name = name.encode('utf-16le')
        data[at+56:at+56+len(name)] = name
    (root / case).write_bytes(data)
PY
cat > "$tmp/adb" <<'MOCK'
#!/bin/bash
if [ "${1:-}" = -s ]; then shift 2; fi
case "${1:-}" in
  get-state) echo recovery ;;
  shell)
    case "${2:-}" in
      'id -u') echo 0 ;;
      'getprop ro.product.device') echo merlin ;;
      'cat /sys/block/mmcblk0/size 2>/dev/null') echo 125000034 ;;
    esac ;;
  pull) cp "$TEST_GPT" "$3" ;;
esac
MOCK
chmod +x "$tmp/adb"
export PATH="$tmp:$PATH"
for case in stock installed foreign48 overlap; do
  export TEST_GPT="$tmp/$case"
  if bash "$root/tools/install-merlinx.sh" --serial test --repartition --yes --dry-run > "$tmp/$case.log" 2>&1; then
    case "$case" in stock|installed) ;; *) echo "unsafe layout accepted: $case" >&2; exit 1 ;; esac
    grep -q '\[dry-run\].*sgdisk.*--new=43' "$tmp/$case.log"
  else
    case "$case" in foreign48|overlap) ;; *) cat "$tmp/$case.log" >&2; exit 1 ;; esac
    grep -q 'Nothing was changed' "$tmp/$case.log"
  fi
done
echo 'GPT layout checks passed (stock, installed, foreign partition, overlap)'
