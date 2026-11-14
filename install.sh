#!/usr/bin/env bash
#
# User-level installer for mchose-adv.
#
# Installs the binary into ~/.local/bin, a systemd *user* unit into
# ~/.config/systemd/user, and optionally the udev rule (which needs root).
# Nothing here requires root except the udev step.
#
#   ./install.sh              build, install, enable the user service
#   ./install.sh --no-udev    skip the udev rule (you manage access yourself)
#   ./install.sh --no-build   use an existing ./mchose-adv binary
#   ./install.sh --uninstall  remove everything this script installed
#
set -euo pipefail

SRC_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BIN_DIR="${XDG_BIN_HOME:-$HOME/.local/bin}"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
BIN="$BIN_DIR/mchose-adv"
UNIT="$UNIT_DIR/mchose-adv.service"
RULE_DST="/etc/udev/rules.d/70-mchose-adv.rules"

do_udev=1
do_build=1
do_uninstall=0

for arg in "$@"; do
    case "$arg" in
    --no-udev) do_udev=0 ;;
    --no-build) do_build=0 ;;
    --uninstall) do_uninstall=1 ;;
    -h | --help)
        sed -n '3,15p' "$0"
        exit 0
        ;;
    *)
        echo "unknown option: $arg" >&2
        exit 2
        ;;
    esac
done

if [[ $do_uninstall -eq 1 ]]; then
    echo "==> stopping and disabling the user service"
    systemctl --user disable --now mchose-adv.service 2>/dev/null || true
    rm -f "$UNIT" "$BIN"
    systemctl --user daemon-reload 2>/dev/null || true
    if [[ -f "$RULE_DST" ]]; then
        echo "==> removing $RULE_DST (needs root)"
        sudo rm -f "$RULE_DST"
        sudo udevadm control --reload-rules
        sudo udevadm trigger
    fi
    echo "==> done"
    exit 0
fi

if [[ $do_build -eq 1 ]]; then
    echo "==> building"
    make -C "$SRC_DIR"
fi

if [[ ! -x "$SRC_DIR/mchose-adv" ]]; then
    echo "error: $SRC_DIR/mchose-adv not found; run without --no-build" >&2
    exit 1
fi

echo "==> installing $BIN"
install -Dm755 "$SRC_DIR/mchose-adv" "$BIN"

echo "==> installing user unit $UNIT"
mkdir -p "$UNIT_DIR"
sed "s|^ExecStart=.*|ExecStart=$BIN --retry 2|" \
    "$SRC_DIR/packaging/mchose-adv.service" >"$UNIT"

if [[ $do_udev -eq 1 ]]; then
    if [[ -f "$RULE_DST" ]] && cmp -s "$SRC_DIR/packaging/70-mchose-adv.rules" "$RULE_DST"; then
        echo "==> udev rule already up to date"
    else
        echo "==> installing $RULE_DST (needs root)"
        sudo install -Dm644 "$SRC_DIR/packaging/70-mchose-adv.rules" "$RULE_DST"
        sudo udevadm control --reload-rules
        sudo udevadm trigger
    fi
fi

if ! id -nG "$USER" | tr ' ' '\n' | grep -qx input; then
    echo
    echo "NOTE: you are not in the 'input' group. The udev rule above grants"
    echo "      access via uaccess, which covers an active local session, but a"
    echo "      systemd user service may not see that ACL. Consider:"
    echo "          sudo usermod -aG input $USER    # then log out and back in"
    echo "      (note: 'input' membership exposes ALL input devices to your account)"
    echo
fi

echo "==> enabling the user service"
# Tolerate having no systemd user session yet: this is normal when the script
# runs from a TTY during machine bootstrap, and must not abort the caller.
if ! systemctl --user daemon-reload 2>/dev/null; then
    echo "    note: no systemd user session available yet."
    echo "          After logging in, run: systemctl --user enable --now mchose-adv"
else
    systemctl --user enable --now mchose-adv.service ||
        echo "    note: could not start it now; it will start on next login."
    sleep 1
    systemctl --user --no-pager --lines=0 status mchose-adv.service || true
fi

cat <<EOF

Installed.

  binary   $BIN
  service  $UNIT
  logs     journalctl --user -u mchose-adv -f

If the keyboard was already plugged in, the daemon will find it within a couple
of seconds. Verify with:

  ls /dev/input/by-id/ | grep -i 'advanced'      # the virtual keyboard
  evtest /dev/input/eventN                       # press an advanced key

EOF
