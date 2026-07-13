# Installing the UAC2 USB-audio driver override

These add-ons make **class-compliant USB Audio Class 2.0 (UAC2)** interfaces work on
Haiku, which the stock driver refuses to publish. They are a **prerequisite** for the
JACK / JackDAW audio stack — without them `jackd -d hmulti` has no USB audio device to
open.

> **ABI warning.** Kernel add-ons are tied to the exact Haiku revision they are built
> on. Build and install them on the **same nightly you will run** (these instructions
> target **hrev59846 x86_64**). For a different nightly, rebuild with `build-driver.sh`.

Three add-ons are involved:

| Add-on | Kind | Why |
|---|---|---|
| `xhci` | bus-manager **module** | enlarged event ring + variable-length isochronous OUT (async feedback) |
| `usb_audio` | device **driver** | actual UAC2 descriptor/clock/stream support |
| `hmulti_audio.media_addon` | media add-on | recover the audio node after device unplug/replug (enhancement) |

They install as **non-packaged overrides** so the base system package is left intact and
the change is fully reversible.

## 0. Prerequisites (on the running nightly)

- The Haiku source on the **`usb-audio-uac2`** branch at `$HOME/haiku` (rsync or clone).
- Build tools (the build script installs these): `jam nasm gcc_syslibs_devel zlib_devel zstd_devel`.

## 1. Build

```sh
cd ~/haiku-kernel-usb/dist
HREV=hrev59846 HAIKU_SRC=$HOME/haiku ./build-driver.sh
```

Stages the three linked binaries into `$HOME/uac2-driver-staged`.

## 2. Install

```sh
./install-driver.sh
```

This copies the overrides into `~/config/non-packaged/add-ons/...` and writes the
packagefs blocklist (next section). Then **reboot**:

```sh
python3 -c "import ctypes; ctypes.CDLL('libroot.so')._kern_shutdown(1)"
```

(A plain `shutdown -r` frequently fails from a non-desktop ssh session; the `_kern_shutdown`
call kills user teams, syncs, and reboots reliably. `python3` lives in its own package so it
keeps working even if the system package is mid-swap.)

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
the `settings/packages` block before that preload (verified on hrev59846: after installing and
rebooting, `listimage` still shows `xhci` with *no* path = the stock preloaded binary, and the
controller-start line lacks the `[OVERRIDE-BUILD]` marker). The kernel cannot unload an
already-preloaded module, so the file block — which the *runtime* packagefs does apply — comes
too late for `xhci`. The `usb_audio` **driver** override still loads correctly regardless
(different loader, prefers non-packaged); only `xhci` is affected.

Two ways to actually run the override, from easiest-per-boot to permanent:

### Option A — Boot-menu blacklist (per boot)

Reboot and, from the moment of power-on, **tap the Space bar repeatedly** until the boot menu
appears. (On EFI, *holding Shift does nothing*; the loader only polls for a Space keystroke in
a brief window. Avoid Esc — that selects debug output.) Then:

**Select safe mode options → Blacklist entries → `add-ons` → `kernel` → `boot` →** toggle
**`xhci`**, return to the main menu, and boot.

This is applied via `kernel_args` **before** the preload, so the stock `xhci` is never loaded
and the runtime module manager picks up your non-packaged override. It must be repeated **each
boot** — good for evaluation, not for a permanent install.

### Option B — Rebuild the system package (persistent)

Bake the patched `xhci` into the `haiku` package itself so the boot loader preloads *your*
build with no blocklist and no menu:

```sh
cd ~/haiku
jam -q -sHAIKU_REVISION=hrev59846 haiku.hpkg   # needs the LIBRARY_PATH export build-driver.sh sets
# then swap it in — NEVER cp onto the live package file (it corrupts the running packagefs):
cp generated/objects/haiku/x86_64/packaging/packages/haiku.hpkg \
   /boot/system/packages/.haiku-new.tmp
mv /boot/system/packages/.haiku-new.tmp \
   /boot/system/packages/haiku-*-x86_64.hpkg     # rename keeps the old inode live until reboot
```

Reboot: the preloaded `xhci` is now the patched one (`listimage` shows a path / the
`[OVERRIDE-BUILD]` marker appears), persistently. This pins the machine to the hrev the
package was built for; keep the replaced package as a backup and do not let a Haiku update
overwrite it. This is the route for a lasting install; the `settings/packages` block and the
non-packaged `xhci` are not needed once the package carries the patch.

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

- To undo everything: `rm /boot/system/settings/packages` and reboot. The overrides in
  `~/config/non-packaged` are then simply ignored (stock modules load again).
- If the override fails to load, xHCI USB may be dead (USB mouse/interface gone), **but the
  system still boots** — an Ethernet/PCIe ssh session and the laptop's built-in keyboard
  survive, so you can revert over ssh.
- The boot menu's **"previous state"** also restores the prior configuration (tap Space at
  power-on on EFI to reach it).
- **Never `cp` directly onto a live packaged file.** These instructions only write to the
  non-packaged tree and the `settings/packages` file, never the base package.

## Rebuilding for another nightly

Re-run `build-driver.sh` with the new `HREV=` on that nightly (the source tree must match
that hrev), then `install-driver.sh` again. The blocklist file does not need to change.
