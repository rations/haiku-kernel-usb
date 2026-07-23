#!/bin/sh
# install-driver.sh — install the UAC2 USB-audio override add-ons and arrange for
# the stock xHCI bus-manager to be bypassed. Run ON the Haiku system, after
# build-driver.sh has staged the binaries. See INSTALL.md for the full rationale.
#
# Nothing is pinned to a particular Haiku revision. The staged build carries a
# BUILT-FOR stamp; this script checks it against the running system and refuses to
# install add-ons built for a different nightly. Any override left by a previous run
# is removed first, so re-running after a rebuild replaces the old add-ons cleanly.
#
# Usage:  ./install-driver.sh
#         STAGE=$HOME/uac2-driver-staged ./install-driver.sh
#         ./install-driver.sh --uninstall    # remove the overrides again
#         FORCE=1 ./install-driver.sh        # install despite a revision mismatch
set -e

STAGE="${STAGE:-$HOME/uac2-driver-staged}"
NPK="$HOME/config/non-packaged/add-ons/kernel"     # user non-packaged kernel add-ons
NPM="$HOME/config/non-packaged/add-ons/media"       # user non-packaged media add-ons
# packagefs BlockedEntries lives here; overridable so the script can be exercised
# against a scratch file instead of the live one.
SETTINGS="${SETTINGS:-/boot/system/settings/packages}"

ADDONS="xhci usb_audio hmulti_audio.media_addon"

# A record of what was installed, kept outside the add-on directories so nothing ever
# tries to load it. Lets a later run (or a bug report) tell which revision the loaded
# add-ons were built for.
RECORD="$HOME/config/settings/uac2-driver-installed"

# The packagefs blocklist that makes the module manager fall through to our xhci (see
# "Why the xHCI blocklist is required" in INSTALL.md). Revision-independent: it names
# paths inside the haiku package, so nightly updates leave it valid.
BLOCK='Package haiku {
	BlockedEntries {
		add-ons/kernel/boot/xhci
		add-ons/kernel/busses/usb/xhci
	}
}'

# uname(1) reports the utsname fields filled in by Haiku's libroot
# (src/system/libroot/posix/sys/uname.c): sysname is always "Haiku", and version is
# "<revision> <build date> <build time>", so field 1 is the running hrev.
if [ "$(uname -s)" != "Haiku" ]; then
	echo "!! these are native Haiku kernel add-ons; run this ON Haiku" >&2
	echo "   (uname -s says '$(uname -s)')" >&2
	exit 1
fi
RUNNING_HREV=$(uname -v 2>/dev/null | awk '{print $1}')
# getarch(1) prints the primary packaging architecture; uname -m is the CPU platform
# ("BePC" on 32-bit x86) and is only a fallback.
RUNNING_ARCH=$(getarch -p 2>/dev/null || true)
[ -n "$RUNNING_ARCH" ] || RUNNING_ARCH=$(uname -m)

# --- remove whatever a previous run installed --------------------------------
# Exactly the paths this script writes, so a re-run replaces the old add-ons instead of
# leaving a build for the previous nightly lying around, and removes nothing it did not
# install itself.
remove_installed() {
	for path in \
		"$NPK/busses/usb/xhci" \
		"$NPK/drivers/bin/usb_audio" \
		"$NPK/drivers/dev/audio/hmulti/usb_audio" \
		"$NPM/hmulti_audio.media_addon" \
		"$RECORD"; do
		# -e is false for a dangling symlink, so test -L as well.
		if [ -e "$path" ] || [ -L "$path" ]; then
			rm -f "$path"
			echo "   removed $path"
		fi
	done
}

if [ "$1" = "--uninstall" ]; then
	echo ">> removing installed overrides"
	remove_installed
	# The blocklist must go too. Leaving it with the override gone means the packaged
	# xhci is blocked and no replacement exists — the system boots with NO xHCI driver
	# at all, i.e. dead USB. Only remove the file if it is exactly the block this
	# script writes; anything else is the user's and must not be clobbered.
	if [ -f "$SETTINGS" ]; then
		if [ "$(cat "$SETTINGS")" = "$BLOCK" ]; then
			rm -f "$SETTINGS"
			echo "   removed $SETTINGS (the xhci blocklist this script wrote)"
		elif grep -q 'busses/usb/xhci' "$SETTINGS"; then
			echo "!! WARNING: $SETTINGS has been edited, so it was left alone — but it still"
			echo "   blocks the packaged xhci and the override is now gone. That boots with NO"
			echo "   xHCI driver and dead USB. Remove these two lines BEFORE rebooting:"
			echo "        add-ons/kernel/boot/xhci"
			echo "        add-ons/kernel/busses/usb/xhci"
		fi
	fi
	echo ">> the stock add-ons load again after a reboot."
	exit 0
fi

for f in $ADDONS; do
	[ -f "$STAGE/$f" ] || { echo "!! missing $STAGE/$f — run build-driver.sh first" >&2; exit 1; }
done

# --- check the staged build against the running system -----------------------
STAMP="$STAGE/BUILT-FOR"
if [ -f "$STAMP" ]; then
	BUILT_HREV=$(sed -n 's/^revision=//p' "$STAMP")
	BUILT_ARCH=$(sed -n 's/^architecture=//p' "$STAMP")
else
	BUILT_HREV=
	BUILT_ARCH=
fi

echo ">> Stage dir    : $STAGE"
echo ">> Built for    : ${BUILT_HREV:-unknown} ${BUILT_ARCH:-unknown}"
echo ">> Running      : $RUNNING_HREV $RUNNING_ARCH"

MISMATCH=
if [ -z "$BUILT_HREV" ]; then
	echo "!! WARNING: $STAMP is missing — this staging directory predates the revision"
	echo "   stamp, so the add-ons cannot be matched to the running kernel. Re-run"
	echo "   build-driver.sh to be sure."
else
	[ "$BUILT_HREV" = "$RUNNING_HREV" ] || MISMATCH="revision"
	[ -z "$BUILT_ARCH" ] || [ "$BUILT_ARCH" = "$RUNNING_ARCH" ] || MISMATCH="architecture"
fi

if [ -n "$MISMATCH" ]; then
	echo "!! $MISMATCH mismatch: these add-ons were built for ${BUILT_HREV} ${BUILT_ARCH}," >&2
	echo "   but this system is $RUNNING_HREV $RUNNING_ARCH." >&2
	echo "   Kernel add-ons are ABI-tied to the revision they load into; installing them" >&2
	echo "   across a nightly update can fail to load or destabilise the USB stack." >&2
	echo "   Rebuild first:  ./build-driver.sh" >&2
	echo "   To install anyway (you know the ABI is compatible):  FORCE=1 $0" >&2
	[ -n "$FORCE" ] || exit 1
	echo ">> FORCE set — installing anyway."
fi

# --- remove the previous install before laying down the new one ---------------
echo ">> removing any previously installed override"
remove_installed

# --- xHCI: a bus-manager MODULE. The module manager searches PACKAGED add-ons first,
#     so a non-packaged xhci is never chosen while the stock one exists. We drop the
#     override here and block the packaged file below.
mkdir -p "$NPK/busses/usb"
cp -f "$STAGE/xhci" "$NPK/busses/usb/xhci"

# --- usb_audio: a device DRIVER. The driver loader searches user non-packaged first
#     (kernel/device_manager/legacy_drivers.cpp, kDriverPaths), so this override wins
#     with no blocklist needed. Binary in drivers/bin + a dev symlink.
mkdir -p "$NPK/drivers/bin" "$NPK/drivers/dev/audio/hmulti"
cp -f "$STAGE/usb_audio" "$NPK/drivers/bin/usb_audio"
ln -sf ../../../bin/usb_audio "$NPK/drivers/dev/audio/hmulti/usb_audio"

# --- multi_audio media add-on (device recovery on replug). Enhancement, not required
#     for basic UAC2 audio. If its behavior does not take effect, the packaged copy may
#     be winning — see INSTALL.md for the optional extra BlockedEntries line.
mkdir -p "$NPM"
cp -f "$STAGE/hmulti_audio.media_addon" "$NPM/hmulti_audio.media_addon"

if [ -f "$STAMP" ]; then
	mkdir -p "$(dirname "$RECORD")"
	cp -f "$STAMP" "$RECORD"
	printf 'installed=%s\n' "$(date)" >> "$RECORD"
fi

# --- warn about copies in the other override locations ------------------------
# kDriverPaths order is: user non-packaged, user packaged, system non-packaged, system
# packaged. Ours (user non-packaged) wins, but a stale copy elsewhere is confusing and
# will be loaded if this one is ever removed. Report, do not delete: this script did
# not put them there.
for other in "/boot/system/non-packaged/add-ons/kernel/drivers/bin/usb_audio" \
	"/boot/system/non-packaged/add-ons/kernel/busses/usb/xhci" \
	"/boot/system/non-packaged/add-ons/media/hmulti_audio.media_addon" \
	"$HOME/config/add-ons/kernel/drivers/bin/usb_audio"; do
	if [ -e "$other" ]; then
		echo "!! note: another copy exists at $other"
		echo "   It is shadowed by the override just installed, but remove it to avoid"
		echo "   confusion (it was not installed by this script)."
	fi
done

# --- Block the packaged xHCI so the module manager falls through to our override.
#     BOTH paths must be blocked: the bus file AND the boot-loader preload symlink
#     (add-ons/kernel/boot/xhci -> ../busses/usb/xhci), or the stock binary is
#     preloaded before packagefs applies the blocklist and your override never wins.
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

>> Rollback: ./install-driver.sh --uninstall (removes the overrides), or
   rm /boot/system/settings/packages  then reboot, or pick "previous state" from the
   boot menu. USB may be dead if an override fails to load, but ssh + the built-in
   keyboard survive.
EOF
