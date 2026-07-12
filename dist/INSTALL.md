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
`add-ons/kernel/boot/xhci -> ../busses/usb/xhci`; if only the bus path is blocked, the stock
binary is preloaded before packagefs applies the blocklist and the override never wins.
Blocking `boot/xhci` stops that preload (USB is dead only at the boot *menu*, which is fine
when booting from the internal disk with a built-in keyboard).

`install-driver.sh` writes this file if it does not exist. If you already have a
`packages` settings file, it prints the block for you to merge by hand rather than
clobbering your settings.

## 3. Verify (after reboot)

```sh
grep OVERRIDE-BUILD /var/log/syslog   # the controller-start line carries this marker
                                       # ONLY when your override is the loaded binary
listusb                                # your UAC2 interface should now enumerate
ls /dev/audio/hmulti/                   # an audio node should appear
```

If `grep OVERRIDE-BUILD` finds nothing, the stock `xhci` is still loaded — recheck the
blocklist (both paths) and that the override is at
`~/config/non-packaged/add-ons/kernel/busses/usb/xhci`.

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
