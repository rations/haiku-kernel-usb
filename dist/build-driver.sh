#!/bin/sh
# build-driver.sh — build the UAC2 USB-audio driver override add-ons.
#
# Run this ON a running Haiku system (e.g. over ssh to the laptop booted into the
# target nightly), NOT on a Linux host — these are native Haiku kernel add-ons and
# their ABI is tied to the exact hrev they are built on. Build on the same hrev you
# will run them on.
#
# It builds three add-ons from a Haiku source checkout on the usb-audio-uac2 branch
# and stages the linked binaries for install-driver.sh:
#   xhci                     - xHCI bus-manager (event-ring + variable-length iso OUT)
#   usb_audio                - USB Audio Class 2.0 device driver
#   hmulti_audio.media_addon - multi_audio media add-on (device unplug/replug recovery)
#
# Usage:  HREV=hrev59846 HAIKU_SRC=$HOME/haiku ./build-driver.sh
set -e

HREV="${HREV:-hrev59846}"                 # revision string baked into the build
HAIKU_SRC="${HAIKU_SRC:-$HOME/haiku}"     # the Haiku source tree (usb-audio-uac2 branch)
STAGE="${STAGE:-$HOME/uac2-driver-staged}"

# runtime_loader uses ONLY $LIBRARY_PATH when it is set, and a non-interactive ssh
# shell never sources SetupEnvironment, so the system lib dirs must be added by hand
# or jam's host build tools fail to load (libbe_build.so / libbsd.so) and the build
# dies silently. %A is expanded by runtime_loader to each tool's own directory.
export LIBRARY_PATH="%A/lib:/boot/system/non-packaged/lib:/boot/system/lib"

echo ">> Haiku source : $HAIKU_SRC"
echo ">> Revision     : $HREV"
echo ">> Stage dir    : $STAGE"

if [ ! -d "$HAIKU_SRC" ]; then
	echo "!! Haiku source tree not found at $HAIKU_SRC" >&2
	echo "   Put the usb-audio-uac2 branch there (rsync/clone) or set HAIKU_SRC." >&2
	exit 1
fi

# Build prerequisites (idempotent; -y is non-interactive). jam is the build driver.
pkgman install -y jam nasm gcc_syslibs_devel zlib_devel zstd_devel || true

cd "$HAIKU_SRC"
[ -d generated ] || ./configure

# The tree is typically rsynced without .git, so jam cannot read the revision; pass
# it explicitly (any value works, but use the real hrev for a truthful build stamp).
for target in "<usb>xhci" usb_audio hmulti_audio.media_addon; do
	echo ">> jam -q -sHAIKU_REVISION=$HREV $target"
	jam -q -sHAIKU_REVISION="$HREV" "$target"
done

# Locate the linked add-ons (object-dir path varies with build profile; find by name
# and by their location under the add-ons tree, excluding intermediate .o files).
find_one() {
	found=$(find generated/objects -type f -name "$1" -path "$2" 2>/dev/null | head -1)
	if [ -z "$found" ]; then
		echo "!! could not find built $1 under generated/objects" >&2
		exit 1
	fi
	echo "$found"
}
XHCI=$(find_one xhci '*busses/usb*')
USB_AUDIO=$(find_one usb_audio '*drivers*')
MEDIA=$(find_one hmulti_audio.media_addon '*media*')

mkdir -p "$STAGE"
cp -f "$XHCI"      "$STAGE/xhci"
cp -f "$USB_AUDIO" "$STAGE/usb_audio"
cp -f "$MEDIA"     "$STAGE/hmulti_audio.media_addon"

echo ">> Staged:"
ls -l "$STAGE"
echo ">> Done. Next: run install-driver.sh (reads STAGE=$STAGE)."
