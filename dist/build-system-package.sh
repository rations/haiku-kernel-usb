#!/bin/sh
# Build the system packages for the JackDAW stack: haiku and haiku_devel from
# the rations/haiku release branch (UAC2 USB-audio driver + 32-bit Windows
# support), as revision 2 so that pkgman installs them as an update over the
# official packages of the same Haiku revision.
#
# Run it on Haiku, on the nightly the packages are for, in a checkout of the
# release branch:
#
#   git clone -b jackdaw-stack-0.5-hrev60256 https://github.com/rations/haiku.git ~/haiku
#   sh build-system-package.sh [~/haiku]
#
# The packages land in <tree>/generated/objects/haiku/x86_64/packaging/packages.
# The log is ~/system-package-build.log.
TREE=${1:-$HOME/haiku}
REVISION=2
LOG="$HOME/system-package-build.log"
HREV=$(uname -v | awk '{print $1}')

case "$HREV" in
hrev[0-9]*) ;;
*)
	echo "!! cannot read the Haiku revision from uname -v: $HREV"
	exit 1
	;;
esac
cd "$TREE" || exit 1
if [ ! -f build/jam/PackageRules ] \
		|| ! grep -q HAIKU_PACKAGE_REVISION build/jam/PackageRules; then
	echo "!! $TREE is not a Haiku tree with the package revision change"
	exit 1
fi

# Build prerequisites, as build-driver.sh installs them: only what is missing,
# so pkgman cannot move the system off the revision being built for.
have_package() {
	pkgman search -i -D -s name "$1" 2>/dev/null | awk '{print $2}' | grep -qxF "$1"
}
MISSING=
for pkg in jam nasm gcc_syslibs_devel zlib_devel zstd_devel; do
	have_package "$pkg" || MISSING="$MISSING $pkg"
done
command -v python3 > /dev/null 2>&1 || command -v python3.10 > /dev/null 2>&1 \
	|| MISSING="$MISSING python3.10"
if [ -n "$MISSING" ]; then
	echo ">> installing missing build prerequisites:$MISSING"
	# shellcheck disable=SC2086
	pkgman install -y $MISSING || exit 1
fi

# runtime_loader uses only $LIBRARY_PATH when it is set, and a non-interactive
# shell may set it without the system library directories.
export LIBRARY_PATH="%A/lib:/boot/system/non-packaged/lib:/boot/system/lib"

# Jamrules refuses to run without generated/build/BuildConfig.
if [ ! -f generated/build/BuildConfig ]; then
	HOST_PYTHON="$(command -v python3 || command -v python3.10 || command -v python)"
	echo ">> configuring"
	HOST_PYTHON="$HOST_PYTHON" ./configure || exit 1
fi

# With HAIKU_REVISION given, the cached revision stamp is never rewritten, and
# the package infos made from it are only remade when it changes. Drop both,
# so the revision and the package revision below are applied.
rm -f generated/build/haiku-revision
rm -f generated/objects/haiku/x86_64/packaging/packages_build/regular/*/*-package-info

echo ">> building haiku and haiku_devel for $HREV, revision $REVISION (log: $LOG)"
: > "$LOG"
for attempt in 1 2 3 4 5 6; do
	echo "=== attempt $attempt" >> "$LOG"
	# SSL is only enabled for a full image by default; without it libbnetapi
	# is built without HTTPS and pkgman cannot reach the repositories.
	jam -j"$(nproc)" -q -sHAIKU_REVISION="$HREV" \
		-sHAIKU_BUILD_FEATURE_SSL=1 -sHAIKU_PACKAGE_REVISION=$REVISION \
		haiku.hpkg haiku_devel.hpkg >> "$LOG" 2>&1
	rc=$?
	sync
	[ $rc = 0 ] && break
	# Large builds on Haiku sometimes fail with a spurious "No space left on
	# device" from ld; the next attempt continues where it stopped.
	grep -q "No space left on device" "$LOG" || break
done
if [ $rc != 0 ]; then
	tail -20 "$LOG"
	echo "!! the build failed (attempt $attempt), see $LOG"
	exit 1
fi

OUT=generated/objects/haiku/x86_64/packaging/packages
for p in haiku haiku_devel; do
	version=$(package list -i "$OUT/$p.hpkg" | awk '/^\tversion:/ {print $2}')
	cp "$OUT/$p.hpkg" "$OUT/$p-$version-x86_64.hpkg" || exit 1
	echo "   $OUT/$p-$version-x86_64.hpkg"
done

# pkgman reaches the repositories through libbnetapi, which needs OpenSSL.
CHECK=$(mktemp -d) || exit 1
package extract -C "$CHECK" "$OUT/haiku.hpkg" lib/libbnetapi.so > /dev/null 2>&1
if ! readelf -d "$CHECK/lib/libbnetapi.so" 2>/dev/null | grep -q 'libssl\.so'; then
	rm -rf "$CHECK"
	echo "!! libbnetapi.so is not linked with libssl: pkgman would have no HTTPS"
	exit 1
fi
rm -rf "$CHECK"
echo "== done"
