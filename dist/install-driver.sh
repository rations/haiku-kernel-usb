#!/bin/sh
# install-driver.sh — install the UAC2 USB-audio override add-ons and arrange for
# the stock xHCI bus-manager to be bypassed. Run ON the Haiku system, after
# build-driver.sh has staged the binaries. See INSTALL.md for the full rationale.
#
# Usage:  STAGE=$HOME/uac2-driver-staged ./install-driver.sh
set -e

STAGE="${STAGE:-$HOME/uac2-driver-staged}"
NPK="$HOME/config/non-packaged/add-ons/kernel"     # user non-packaged kernel add-ons
NPM="$HOME/config/non-packaged/add-ons/media"       # user non-packaged media add-ons
SETTINGS="/boot/system/settings/packages"           # packagefs BlockedEntries lives here

for f in xhci usb_audio hmulti_audio.media_addon; do
	[ -f "$STAGE/$f" ] || { echo "!! missing $STAGE/$f — run build-driver.sh first" >&2; exit 1; }
done

# --- xHCI: a bus-manager MODULE. The module manager searches PACKAGED add-ons first,
#     so a non-packaged xhci is never chosen while the stock one exists. We drop the
#     override here and block the packaged file below.
mkdir -p "$NPK/busses/usb"
cp -f "$STAGE/xhci" "$NPK/busses/usb/xhci"

# --- usb_audio: a device DRIVER. The driver loader prefers non-packaged, so this
#     override wins with no blocklist needed. Binary in drivers/bin + a dev symlink.
mkdir -p "$NPK/drivers/bin" "$NPK/drivers/dev/audio/hmulti"
cp -f "$STAGE/usb_audio" "$NPK/drivers/bin/usb_audio"
ln -sf ../../../bin/usb_audio "$NPK/drivers/dev/audio/hmulti/usb_audio"

# --- multi_audio media add-on (device recovery on replug). Enhancement, not required
#     for basic UAC2 audio. If its behavior does not take effect, the packaged copy may
#     be winning — see INSTALL.md for the optional extra BlockedEntries line.
mkdir -p "$NPM"
cp -f "$STAGE/hmulti_audio.media_addon" "$NPM/hmulti_audio.media_addon"

# --- Block the packaged xHCI so the module manager falls through to our override.
#     BOTH paths must be blocked: the bus file AND the boot-loader preload symlink
#     (add-ons/kernel/boot/xhci -> ../busses/usb/xhci), or the stock binary is
#     preloaded before packagefs applies the blocklist and your override never wins.
BLOCK='Package haiku {
	BlockedEntries {
		add-ons/kernel/boot/xhci
		add-ons/kernel/busses/usb/xhci
	}
}'

if [ ! -e "$SETTINGS" ]; then
	printf '%s\n' "$BLOCK" > "$SETTINGS"
	echo ">> wrote $SETTINGS"
elif grep -q 'busses/usb/xhci' "$SETTINGS" && grep -q 'boot/xhci' "$SETTINGS"; then
	echo ">> $SETTINGS already blocks both xhci paths — left unchanged"
else
	echo "!! $SETTINGS already exists and does not block xhci."
	echo "   Merge these entries into its BlockedEntries block by hand (do not clobber it):"
	printf '%s\n' "$BLOCK"
fi

cat <<'EOF'

>> Installed. Now REBOOT for packagefs to apply the blocklist:
     python3 -c "import ctypes; ctypes.CDLL('libroot.so')._kern_shutdown(1)"
   (plain `shutdown -r` often fails from a non-desktop ssh session).

>> After reboot, verify the OVERRIDE build is the one running:
     grep OVERRIDE-BUILD /var/log/syslog     # controller-start line = override loaded
     listusb                                  # your UAC2 interface should enumerate
     ls /dev/audio/hmulti/                     # an audio node should appear

>> Rollback (USB may be dead if the override fails to load, but ssh + built-in
   keyboard survive): rm /boot/system/settings/packages  then reboot, or pick
   "previous state" from the boot menu.
EOF
