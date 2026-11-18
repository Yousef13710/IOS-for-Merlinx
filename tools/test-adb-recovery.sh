#!/bin/bash
# Exercise both installer entry points with ADB output as emitted by Windows/WSL.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/adb" <<'EOF'
#!/bin/bash
if [ "${1:-}" = -s ]; then shift 2; fi
case "${1:-}" in
  devices) printf 'List of devices attached\r\ntestserial\trecovery\r\n\r\n' ;;
  get-state) printf '%s\r\n' "${TEST_ADB_STATE:-recovery}" ;;
  shell)
    case "${2:-}" in
      'id -u') printf '0\r\n' ;;
      'getprop ro.product.device') printf 'merlin\r\n' ;;
    esac ;;
esac
EOF
chmod +x "$tmp/adb"
touch "$tmp/boot.img"
export PATH="$tmp:$PATH"

# Option 5 runs the real interactive connection check; q then exits the menu.
printf '5\n\nq\n' | bash "$root/setup-merlinx.sh" > "$tmp/interactive.log" 2>&1
grep -q 'Connected to root recovery ADB: testserial (device: merlin)' "$tmp/interactive.log"
bash "$root/tools/install-merlinx.sh" --serial testserial --boot "$tmp/boot.img" --dry-run > "$tmp/install.log" 2>&1
grep -q 'Connected recovery device: merlin' "$tmp/install.log"

# Invalid states must remain rejected after normalization.
if TEST_ADB_STATE=offline bash "$root/tools/install-merlinx.sh" --serial testserial --boot "$tmp/boot.img" --dry-run > "$tmp/offline.log" 2>&1; then
  echo 'offline device was accepted' >&2
  exit 1
fi
grep -q "got state: 'offline'" "$tmp/offline.log"
echo 'ADB recovery checks passed (CRLF recovery and offline)'
