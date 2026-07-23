# Installing the UAC2 USB-audio driver override

These add-ons make **class-compliant USB Audio Class 2.0 (UAC2)** interfaces work on
Haiku, which the stock driver refuses to publish. They are a **prerequisite** for the
JACK / JackDAW audio stack — without them `jackd -d hmulti` has no USB audio device to
open.

> **ABI warning.** Kernel add-ons are tied to the exact Haiku revision they are built
> on. Build and install them on the **same nightly you will run**. Nothing here is
> pinned to a particular revision — `build-driver.sh` detects the revision and stamps
> it into the staged build, and `install-driver.sh` refuses to install a build that
> does not match the running system. After a nightly update, just re-run both.

Three add-ons are involved:

| Add-on | Kind | Why |
|---|---|---|
| `xhci` | bus-manager **module** | enlarged event ring + variable-length isochronous OUT (async feedback) |
| `usb_audio` | device **driver** | actual UAC2 descriptor/clock/stream support |
| `hmulti_audio.media_addon` | media add-on | recover the audio node after device unplug/replug (enhancement) |

They install as **non-packaged overrides** so the base system package is left intact and
the change is fully reversible.

## 0. Prerequisites — get the sources (on the running nightly)

A stock Haiku nightly has no `git`, and the driver is compiled from a Haiku source checkout
on the **`usb-audio-uac2`** branch. Fetch both, plus this repo (the machine must be online):

```sh
# git (the Haiku build tools are installed by build-driver.sh in step 1)
pkgman install -y git

# the Haiku source with the driver changes (shallow clone; still a few GB)
git clone -b usb-audio-uac2 --depth 1 https://github.com/rations/haiku.git ~/haiku

# this repo (build/install scripts)
git clone https://github.com/rations/haiku-kernel-usb ~/haiku-kernel-usb
```

The build tools (`jam nasm gcc_syslibs_devel zlib_devel zstd_devel`) do not need a separate
install — `build-driver.sh` checks which of them are present and installs only the missing
ones. (It deliberately does **not** run a blanket `pkgman install`: while the HaikuPorts
repositories are mid-transition to R1/beta6, that can drag in a partial upgrade and move the
system off the very revision you are building for.)

## 1. Build

```sh
cd ~/haiku-kernel-usb/dist
./build-driver.sh
```

Stages the three linked binaries into `$HOME/uac2-driver-staged`, together with a
`BUILT-FOR` stamp recording the revision and architecture they were built for.

The revision is detected, in this order:

1. `HREV=` in the environment, if you set it;
2. `git describe --tags --match=hrev*` in the source tree — the same method Haiku's own
   build uses (`build/scripts/determine_haiku_revision`). A `--depth 1` clone has no tags,
   so this is usually skipped;
3. the running kernel's revision, which is the first field of `uname -v`.

If the source tree *does* carry tags and its revision differs from the running system, the
script says so — that combination builds add-ons against headers from a different nightly
than the one they will load into.

Everything else is detected too: the packaging architecture comes from `getarch -p`, and
the build directory from `$HAIKU_OUTPUT_DIR` (default `<tree>/generated`). Overridable:

```sh
HAIKU_SRC=$HOME/haiku STAGE=$HOME/uac2-driver-staged HREV=hrev60123 ./build-driver.sh
```

## 2. Install

```sh
./install-driver.sh
```

This first compares the staged `BUILT-FOR` stamp against the running system and **stops if
they disagree** (rebuild, or `FORCE=1 ./install-driver.sh` if you are sure). It then
**removes any override a previous run installed** — so re-running after a nightly update
replaces the old add-ons rather than leaving a stale build behind — copies the new ones into
`~/config/non-packaged/add-ons/...`, and writes the packagefs blocklist (next section).
Then **reboot**.

To take the overrides back out again:

```sh
./install-driver.sh --uninstall
```

At the machine, reboot with **Deskbar → Shut Down → Restart System**, or in a Terminal:

```sh
shutdown -r
```

Over a headless ssh session `shutdown -r` frequently fails; there, use the kernel call
instead (needs the `python3` package, which the build already pulled in):

```sh
python3 -c "import ctypes; ctypes.CDLL('libroot.so')._kern_shutdown(1)"
```

It kills user teams, syncs, and reboots reliably, and `python3` lives in its own package so
it keeps working even if the system package is mid-swap.

## Why the xHCI blocklist is required (the trap)

`xhci` is a bus-manager **module**, and the module manager searches **packaged** add-ons
(`/boot/system/add-ons/kernel`) **before** non-packaged ones. So a non-packaged `xhci`
override is *never* chosen while the stock packaged file exists — you would silently keep
running the stock controller. (`usb_audio`, a device *driver*, uses a different loader that
*does* prefer non-packaged, which is why it needs no blocklist.)

The fix is to block the packaged file so the search falls through to the override, in
`/boot/system/settings/packages` (driver-settings format; `/boot/system/settings` is
writable even though `/boot/system` is read-only packagefs):

```
Package haiku {
	BlockedEntries {
		add-ons/kernel/boot/xhci
		add-ons/kernel/busses/usb/xhci
	}
}
```

**Both** paths matter. `xhci` is also **preloaded by the boot loader** via the symlink
`add-ons/kernel/boot/xhci -> ../busses/usb/xhci`. `install-driver.sh` writes this file if it
does not exist (and prints the block to merge by hand if you already have one).

**However — the file blocklist alone is not enough for `xhci` on current nightlies.**
The boot loader preloads `xhci` into memory very early, and in practice it does **not** honor
the `settings/packages` block before that preload (first verified on hrev59846, and unchanged
since: after installing and rebooting, `listimage` still shows `xhci` with *no* path = the
stock preloaded binary, and the controller-start line lacks the
`[OVERRIDE-BUILD]` marker). The kernel cannot unload an
already-preloaded module, so the file block — which the *runtime* packagefs does apply — comes
too late for `xhci`. The `usb_audio` **driver** override still loads correctly regardless
(different loader, prefers non-packaged); only `xhci` is affected.

Two ways to actually run the override, from easiest-per-boot to permanent:

### Option A — Boot-menu blacklist (per boot)

Reboot and, from the moment of power-on, **tap the Space bar repeatedly** until the boot menu
appears. (On EFI, *holding Shift does nothing*; the loader only polls for a Space keystroke in
a brief window. Avoid Esc — that selects debug output.) Then:

**Select safe mode options → Disable system components → `add-ons` → `kernel` → `boot` →** toggle
**`xhci`**, return to the main menu, and boot.

This is applied via `kernel_args` **before** the preload, so the stock `xhci` is never loaded
and the runtime module manager picks up your non-packaged override. It must be repeated **each
boot** — good for evaluation, not for a permanent install.

### Option B — Rebuild the system package (persistent)

Bake the patched `xhci` into the `haiku` package itself so the boot loader preloads *your*
build with no blocklist and no menu:

Derive the revision and architecture the same way the scripts do, so nothing is typed in
by hand:

```sh
cd ~/haiku
HREV=$(uname -v | awk '{print $1}')   # running revision (uname -v field 1)
ARCH=$(getarch -p)                    # primary packaging architecture, e.g. x86_64
export LIBRARY_PATH="%A/lib:/boot/system/non-packaged/lib:/boot/system/lib"

jam -q -sHAIKU_REVISION="$HREV" haiku.hpkg

# then swap it in — NEVER cp onto the live package file (it corrupts the running packagefs):
LIVE=$(ls /boot/system/packages/haiku-*-"$ARCH".hpkg)   # exactly one match expected
cp "$LIVE" ~/haiku-system-package-backup.hpkg           # keep the stock one
cp generated/objects/haiku/"$ARCH"/packaging/packages/haiku.hpkg \
   /boot/system/packages/.haiku-new.tmp
mv /boot/system/packages/.haiku-new.tmp "$LIVE"         # rename keeps the old inode live until reboot
```

(The `packaging/packages` path is where the build puts system packages — see
`build/jam/BuildSetup`, `HAIKU_PACKAGES_DIR_$(architecture)`.)

Reboot: the preloaded `xhci` is now the patched one (`listimage` shows a path / the
`[OVERRIDE-BUILD]` marker appears), persistently. This pins the machine to the hrev the
package was built for — after a system update, redo this step (and the driver build) on the
new revision. Keep the backup copy above and do not let a Haiku update overwrite it silently.
This is the route for a lasting install; the `settings/packages` block and the non-packaged
`xhci` are not needed once the package carries the patch.

## 3. Verify (after reboot)

```sh
grep OVERRIDE-BUILD /var/log/syslog   # the controller-start line carries this marker
                                       # ONLY when your override is the loaded binary
listusb                                # your UAC2 interface should now enumerate
ls /dev/audio/hmulti/                   # an audio node should appear
```

If `grep OVERRIDE-BUILD` finds nothing, the stock `xhci` is still preloaded — the file
blocklist did not stop the preload (expected on current nightlies). Use **Option A**
(boot-menu blacklist) for this boot, or **Option B** (rebuild the system package) for a
permanent fix. `usb_audio` and the audio node work either way; without the `xhci` override,
isochronous capture is unstable under load.

Then a JACK smoke test:

```sh
jackd -d hmulti -d /dev/audio/hmulti/usb/1 -r 48000 -p 128 -n 3
```

## multi_audio (optional precedence note)

If device recovery after unplug/replug does not take effect, the **packaged**
`hmulti_audio.media_addon` may be winning over the non-packaged copy. In that case add its
path to the same blocklist and reboot:

```
add-ons/media/hmulti_audio.media_addon
```

Basic UAC2 audio does not require this add-on; it is a resilience enhancement only.

## Rollback / safety

- To remove the overrides: `./install-driver.sh --uninstall`, then reboot.
- To undo everything including the blocklist: `rm /boot/system/settings/packages` and
  reboot. The overrides in `~/config/non-packaged` are then simply ignored (stock modules
  load again).
- If the override fails to load, xHCI USB may be dead (USB mouse/interface gone), **but the
  system still boots** — an Ethernet/PCIe ssh session and the laptop's built-in keyboard
  survive, so you can revert over ssh.
- The boot menu's **"previous state"** also restores the prior configuration (tap Space at
  power-on on EFI to reach it).
- **Never `cp` directly onto a live packaged file.** These instructions only write to the
  non-packaged tree and the `settings/packages` file, never the base package.

## Rebuilding after a nightly update

Haiku nightlies move fast. On the updated system:

```sh
cd ~/haiku-kernel-usb/dist
./build-driver.sh                 # detects the new revision itself
./install-driver.sh               # removes the old override, installs the new one
```

Then reboot. Nothing needs editing: the revision and architecture are detected, the
previously installed add-ons are removed before the new ones are laid down, and the
blocklist file is revision-independent (it names paths inside the `haiku` package) so it
stays as it is.

Rebasing the source tree onto the new nightly is a **separate** decision, and not
automatic — Haiku does not version-check kernel modules, so add-ons built a few hundred
revisions back generally still load. Rebase when the kernel or USB stack changes something
the driver depends on, not on a schedule. When you do:

```sh
cd ~/haiku
git status                        # FIRST: driver work is often uncommitted here
git commit -am "wip"              # or git stash — never pull into a dirty driver tree
git pull
```

If you took **Option B** (patched system package), redo that step as well — a system
update replaces `haiku*.hpkg` with the stock one, taking the patched `xhci` with it.
