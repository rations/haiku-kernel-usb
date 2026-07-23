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
# Nothing is pinned to a particular Haiku revision: the revision is detected from the
# source tree (or, failing that, from the running system) and stamped into the staged
# build, so re-running after a nightly update just works.
#
# Usage:  ./build-driver.sh
#         HAIKU_SRC=$HOME/haiku ./build-driver.sh      # non-default source tree
#         HREV=hrev60123 ./build-driver.sh             # force the revision stamp
set -e

HAIKU_SRC="${HAIKU_SRC:-$HOME/haiku}"     # the Haiku source tree (usb-audio-uac2 branch)
STAGE="${STAGE:-$HOME/uac2-driver-staged}"
# Jamrules: HAIKU_OUTPUT_DIR defaults to <tree>/generated, HAIKU_BUILD_OUTPUT_DIR to
# <output>/build. Respect an override, but do not export it — jam reads it itself.
OUTPUT_DIR="${HAIKU_OUTPUT_DIR:-$HAIKU_SRC/generated}"

ADDONS="xhci usb_audio hmulti_audio.media_addon"

# runtime_loader uses ONLY $LIBRARY_PATH when it is set, and a non-interactive ssh
# shell never sources SetupEnvironment, so the system lib dirs must be added by hand
# or jam's host build tools fail to load (libbe_build.so / libbsd.so) and the build
# dies silently. %A is expanded by runtime_loader to each tool's own directory.
export LIBRARY_PATH="%A/lib:/boot/system/non-packaged/lib:/boot/system/lib"

# --- sanity: this must run on the target OS ---------------------------------
# uname(1) reports the utsname fields filled in by Haiku's libroot
# (src/system/libroot/posix/sys/uname.c): sysname is always "Haiku".
if [ "$(uname -s)" != "Haiku" ]; then
	echo "!! this script builds native Haiku kernel add-ons and must run ON Haiku" >&2
	echo "   (uname -s says '$(uname -s)')" >&2
	exit 1
fi

if [ ! -d "$HAIKU_SRC" ]; then
	echo "!! Haiku source tree not found at $HAIKU_SRC" >&2
	echo "   Clone the usb-audio-uac2 branch there, or set HAIKU_SRC=/path/to/tree." >&2
	exit 1
fi

# --- detect the Haiku revision ----------------------------------------------
# uname -v is "<revision> <build date> <build time>", so field 1 is the hrev string
# of the RUNNING kernel. A self-built system may carry a "+N [branch]" suffix on the
# revision; field 1 is still the hrev token.
RUNNING_HREV=$(uname -v 2>/dev/null | awk '{print $1}')

# The source tree's own revision, determined exactly the way Haiku's build does it
# (build/scripts/determine_haiku_revision): describe HEAD against the hrev* tags.
# A shallow clone carries no tags, so this legitimately comes up empty.
SOURCE_HREV=$(git -C "$HAIKU_SRC" describe --dirty --tags --match='hrev*' --abbrev=1 \
	2>/dev/null | sed 's/-g[0-9a-z]\{1,\}//' | sed 's/-/+/g')

if [ -n "$HREV" ]; then
	HREV_FROM="HREV environment variable"
elif [ -n "$SOURCE_HREV" ]; then
	HREV="$SOURCE_HREV"
	HREV_FROM="git describe in $HAIKU_SRC"
else
	HREV="$RUNNING_HREV"
	HREV_FROM="uname -v (the running system)"
fi

case "$HREV" in
hrev[0-9]*)
	;;
*)
	echo "!! could not determine a Haiku revision (got '$HREV')" >&2
	echo "   The source tree has no hrev tags (shallow clone) and 'uname -v' did not" >&2
	echo "   start with one. Pass it explicitly: HREV=hrev60123 ./build-driver.sh" >&2
	exit 1
	;;
esac

# The packaging architecture. getarch(1) prints the primary packaging architecture
# ("x86_64"); uname -m prints the CPU platform, which is "BePC" on 32-bit x86 and so
# is only a last resort.
ARCH="${ARCH:-$(getarch -p 2>/dev/null || true)}"
[ -n "$ARCH" ] || ARCH=$(uname -m)

echo ">> Haiku source : $HAIKU_SRC"
echo ">> Output dir   : $OUTPUT_DIR"
echo ">> Revision     : $HREV   (from $HREV_FROM)"
echo ">> Running hrev : $RUNNING_HREV"
echo ">> Architecture : $ARCH"
echo ">> Stage dir    : $STAGE"

# Kernel add-ons are ABI-tied to the kernel they load into, so the source tree really
# has to match the running nightly. Say so loudly rather than producing add-ons that
# fail to load — or worse, load and misbehave.
if [ -n "$SOURCE_HREV" ] && [ "$SOURCE_HREV" != "$RUNNING_HREV" ]; then
	echo "!! WARNING: source tree is $SOURCE_HREV but the running system is $RUNNING_HREV."
	echo "   Kernel add-ons are ABI-tied to the revision they run on. Update the branch"
	echo "   (git -C $HAIKU_SRC pull / rebase onto the matching upstream) before trusting"
	echo "   this build."
elif [ -z "$SOURCE_HREV" ]; then
	echo ">> note: $HAIKU_SRC has no hrev tags (shallow clone), so the source revision"
	echo "   cannot be verified. The stamp above assumes the checkout matches the running"
	echo "   system. After a nightly update, update the branch before rebuilding."
fi

# --- build prerequisites ----------------------------------------------------
# Only install what is actually missing. Haiku is mid-transition to R1/beta6 and a
# blanket 'pkgman install' can pull an unwanted partial upgrade — which would move the
# system off the hrev these add-ons are being built for.
# 'pkgman search -i' consults only the installed repositories (no network, so this is
# safe while the remote repos are in flux). '-D' prints Repository/Name/Version/Arch,
# columns that are never blank, so field 2 is reliably the package name; the match is a
# case-insensitive substring, hence the exact-name filter.
have_package() {
	pkgman search -i -D -s name "$1" 2>/dev/null | awk '{print $2}' | grep -qxF "$1"
}

MISSING=
for pkg in jam nasm gcc_syslibs_devel zlib_devel zstd_devel; do
	have_package "$pkg" || MISSING="$MISSING $pkg"
done
# Haiku's configure needs a Python interpreter >= 3.10 for its generated sources; any
# python3 on PATH satisfies that, whatever package provided it.
command -v python3 >/dev/null 2>&1 || command -v python3.10 >/dev/null 2>&1 \
	|| MISSING="$MISSING python3.10"

if [ -n "$MISSING" ]; then
	echo ">> installing missing build prerequisites:$MISSING"
	# shellcheck disable=SC2086
	pkgman install -y $MISSING
else
	echo ">> build prerequisites already installed — not touching pkgman"
fi

cd "$HAIKU_SRC"

# Jamrules refuses to run without generated/build/BuildConfig, which configure writes.
if [ ! -f "$OUTPUT_DIR/build/BuildConfig" ]; then
	# configure looks for python3/python on PATH; pass HOST_PYTHON explicitly so it works
	# even when the package installed only a versioned "python3.10" binary.
	HOST_PYTHON="$(command -v python3 || command -v python3.10 || command -v python)"
	echo ">> configuring ($OUTPUT_DIR/build/BuildConfig missing)"
	HOST_PYTHON="$HOST_PYTHON" ./configure
fi

# jam caches the revision string in generated/build/haiku-revision. When HAIKU_REVISION
# is passed on the command line that target has no dependencies (build/jam/FileRules,
# DetermineHaikuRevision2), so a file left over from a previous nightly would never be
# rewritten. Drop it when it disagrees.
REVFILE="$OUTPUT_DIR/build/haiku-revision"
if [ -f "$REVFILE" ] && [ "$(cat "$REVFILE")" != "$HREV" ]; then
	echo ">> stale revision stamp ($(cat "$REVFILE")) — removing $REVFILE"
	rm -f "$REVFILE"
fi

# --- drop anything a previous run staged -------------------------------------
# Those binaries were linked against whatever hrev was current then. Clearing them
# first means a build that fails partway cannot leave old add-ons behind for
# install-driver.sh to pick up and install onto a newer kernel.
if [ -d "$STAGE" ]; then
	for f in $ADDONS BUILT-FOR; do
		if [ -e "$STAGE/$f" ]; then
			rm -f "$STAGE/$f"
			echo ">> cleared previously staged $f"
		fi
	done
fi

for target in "<usb>xhci" usb_audio hmulti_audio.media_addon; do
	echo ">> jam -q -sHAIKU_REVISION=$HREV $target"
	jam -q -sHAIKU_REVISION="$HREV" "$target"
done

# Locate the linked add-ons (the object-dir path varies with build profile and
# architecture; find by name and by their location under the add-ons tree, excluding
# intermediate .o files). If several architectures have been built in this tree,
# prefer the one we are building for rather than picking arbitrarily.
find_one() {
	found=$(find "$OUTPUT_DIR/objects" -type f -name "$1" -path "$2" 2>/dev/null)
	if [ -z "$found" ]; then
		echo "!! could not find built $1 under $OUTPUT_DIR/objects" >&2
		exit 1
	fi
	if [ "$(echo "$found" | wc -l)" -gt 1 ]; then
		narrowed=$(echo "$found" | grep "/$ARCH/" || true)
		if [ "$(echo "$narrowed" | grep -c .)" -eq 1 ]; then
			found="$narrowed"
		else
			echo "!! ambiguous build products for $1:" >&2
			echo "$found" >&2
			echo "   Clean the stale ones (or set ARCH=) and re-run." >&2
			exit 1
		fi
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

# Stamp the staged build so install-driver.sh can refuse to install add-ons built for
# a different nightly than the one currently running.
cat > "$STAGE/BUILT-FOR" <<EOF
revision=$HREV
architecture=$ARCH
kernel_version=$(uname -v)
source_tree=$HAIKU_SRC
source_head=$(git -C "$HAIKU_SRC" rev-parse --short HEAD 2>/dev/null || echo unknown)
built=$(date)
EOF

echo ">> Staged:"
ls -l "$STAGE"
echo ">> Done. Next: run install-driver.sh (reads STAGE=$STAGE)."
