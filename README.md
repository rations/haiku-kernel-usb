# JackDAW stack system package (UAC2 USB audio + 32-bit Windows support)

> **Users:** install the whole stack from the
> [JackDAW release](https://github.com/rations/jackDAW-haiku/releases) (`sh install.sh`). It
> installs the system package built here. This repository has the build script, the tests and
> the notes for it.

The JackDAW stack needs three changes to Haiku's kernel and drivers:

- **USB Audio Class 2.0 interfaces.** Stock Haiku enumerates UAC2 devices but refuses to
  publish an audio node for them, so USB-2 audio interfaces are "seen but unusable". The
  driver changes make them work as `/dev/audio/hmulti` devices that JACK can use.
- **32-bit Windows code in 64-bit processes**, for Wine: per-team local descriptor tables and a
  per-thread FS segment that survives context switches and signals.
- **Self-debugging**, which copy protection such as iLok/PACE relies on. With no debugger
  attached, a program's own trap-flag steps, `int3` and hardware breakpoints arrive as
  `SIGTRAP`, and a thread can set its own debug registers.

The source changes are in a fork of the Haiku tree:
[`rations/haiku`](https://github.com/rations/haiku), branch **`jackdaw-stack-0.5-hrev60256`**
(the driver alone is also on `usb-audio-uac2`). They are not submitted upstream.

The USB stack in it (`bus_managers/usb`, `busses/usb` and `drivers/audio/usb`) is the one of
hrev59901 with the UAC2 work, from branch `usb-audio-uac2`, not hrev60256's own. With the USB
stack of hrev60256 a UAC2 interface crackles after about a minute of playback, without xruns.
That version is kept on `jackdaw-stack-0.5-hrev60256-upstream-usb`.

## What changed

USB audio, in `src/add-ons/kernel/` and `src/add-ons/media/`:
- **`busses/usb/xhci`:**
  - A larger event ring. It used to overflow under isochronous capture and kill every
    completion on the bus.
  - Variable-length isochronous OUT transfers, for asynchronous feedback.
  - Frame-ID scheduling for controllers that need it.
  - Per-endpoint error counting.
- **`drivers/audio/usb` (`usb_audio`):**
  - UAC2 descriptor parsing: clock source and selector units, UAC2 format types, feedback
    endpoints.
  - Feedback and implicit-feedback rate control, and sample-buffer headroom.
  - Validation of user requests.
  - Clean reattach after a replug.
- **`media-add-ons/multi_audio`:** survives device failure, and recovers the audio node when
  the device returns.

Kernel, in `src/system/kernel/arch/x86/`:
- **Per-team LDT:**
  - `THREAD_SET_LDT_ENTRY`, `THREAD_SET_FS_BASE`, `THREAD_GET_FS_BASE` and
    `THREAD_GET_GS_BASE`, next to the existing `THREAD_SET_GS_BASE`.
  - FS, DS and ES are kept per thread.
  - Signal handlers always run as 64-bit code. Sigreturn checks the segment selectors before
    `iret`, which also closes a privileged-CS hole.
- **User traps:** with no team debugger, `x86_handle_debug_exception` and the `int3` handler
  send `SIGTRAP`. They used to drop the trap or stop the team in the debug_server.
- **`THREAD_SET_DEBUG_REGISTERS`:** a thread's own hardware breakpoints. DR6 is reported in the
  signal context, and the resume flag survives sigreturn.

Build: `HAIKU_PACKAGE_REVISION` (default 1) sets the system packages' revision.

## The package

The changes ship as Haiku's own `haiku` and `haiku_devel` packages, rebuilt for one nightly
with revision `-2`. That makes them an ordinary update to the official `-1` packages of the
same nightly:
- pkgman installs them in a transaction.
- The boot menu's *Select state* goes back to the official packages.
- The Haiku repository must stay pinned to that nightly, because a newer official `haiku`
  package would replace this one.

Building, on the nightly the package is for:

```sh
pkgman install -y git
git clone -b jackdaw-stack-0.5-hrev60256 https://github.com/rations/haiku.git ~/haiku
git clone https://github.com/rations/haiku-kernel-usb ~/haiku-kernel-usb
sh ~/haiku-kernel-usb/dist/build-system-package.sh ~/haiku
```

`build-system-package.sh` installs the missing build tools and configures the tree. It builds
both packages with SSL enabled, which pkgman's HTTPS needs, and checks the result. The
packages end up in `~/haiku/generated/objects/haiku/x86_64/packaging/packages/`.

**For a newer nightly:**
1. Rebase the release branch onto the new `hrev` tag.
2. Build on that nightly.
3. Run the tests below.

The changes touch little outside the files listed above. Moving from hrev60247 to hrev60256
had no conflicts.

## Tests

- `tests/wow64_ldt_test.c` covers the kernel side: 32-bit code, context switches, signals,
  forged sigreturn frames, rejected descriptors, fork/exec, 32-bit syscall instructions, user
  traps and hardware breakpoints. All 11 pass on hrev60256.
- The Windows-side tests (traps, breakpoints, PACE's probe) are in
  [haiku-wine-port](https://github.com/rations/haiku-wine-port-jackdaw)'s `tests/`.

## Verified hardware

- Behringer UMC204HD (`1397:0508`)
- XMOS-based NUX USB Audio 2.0 (`20b1:2018`)

Other class-compliant UAC2 devices are expected to work but are untested. Reports are welcome.

## Known limitations

- The package leaves out the Haiku logo artwork and the Haiku3d demo, which only official
  builds include.
- It also leaves out the 32-bit BIOS and PXE boot loaders that Installer copies to new disks,
  because the native x86_64 toolchain cannot link them. The system itself boots normally on
  BIOS and UEFI.
- The system package fits one nightly only. Haiku updates wait for a rebuilt package.

## The older driver-only route

Before release 0.5 the driver was installed as a non-packaged override (`dist/build-driver.sh`,
`dist/install-driver.sh`, [dist/INSTALL.md](dist/INSTALL.md)). That route also needed `xhci` to
be disabled in the boot menu on every boot. It is kept for reference and for the driver alone.
The release installer uses `install-driver.sh --uninstall` to remove an old override before
installing the system package.
