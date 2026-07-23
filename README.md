# Haiku USB Audio Class 2.0 (UAC2) support

The companion / distribution repo for a set of Haiku kernel add-on changes that add
working support for **class-compliant USB Audio Class 2.0** interfaces. Stock Haiku
enumerates UAC2 devices but deliberately refuses to publish an audio node for them, so
USB-2 audio interfaces are "seen but unusable." These changes make them work as real
`/dev/audio/hmulti` devices — which is what lets JACK and the JackDAW stack use a USB
audio interface on Haiku.

The actual source changes live in a fork of the Haiku tree —
[`rations/haiku`, branch `usb-audio-uac2`](https://github.com/rations/haiku/tree/usb-audio-uac2).
**This** repo holds the patches, the xHCI reference material, and the build/install
scripts (`dist/`) that turn those changes into a reversible non-packaged driver override;
it is not itself a checkout of the OS source.

## What changed

Three add-ons, in `src/add-ons/kernel/`:

- **`busses/usb/xhci`** — enlarged the xHCI event ring (it previously overflowed
  deterministically under isochronous audio capture and killed every completion on the
  bus) and added a variable-length isochronous **OUT** transfer path for asynchronous
  feedback (e.g. 24 audio frames in one microframe, 25 in the next).
- **`drivers/audio/usb` (`usb_audio`)** — lifted the `bcdUSB >= 0x200` guard and
  implemented UAC2 descriptor parsing (clock source/selector units, UAC2 format types,
  feedback endpoints), sample-buffer headroom, input validation on user requests, and
  clean reattach after replug.
- **`media/media-add-ons/multi_audio` (`hmulti_audio.media_addon`)** — survives device
  failure and recovers the audio node when the device returns.

## Verified hardware

Tested on real hardware with two class-compliant UAC2 interfaces:

- **Behringer UMC204HD** (USB ID `1397:0508`)
- **XMOS-based NUX USB Audio 2.0** (USB ID `20b1:2018`)

Other class-compliant UAC2 devices are expected to work but are **untested** — reports
welcome.

## Status / caveats

- **Not upstreamed.** These are carried as a local fork; install them as a reversible
  non-packaged override (see below).
- **ABI-tied to the hrev.** Kernel add-ons must be built on the same Haiku revision you run
  them on, so they must be rebuilt after every nightly update. The distribution scripts are
  not pinned to a revision: they detect it, stamp it into the build, refuse to install a
  build that does not match the running system, and remove the previous override when
  re-run. Developed against **hrev59846 x86_64** and up.

## Install

Full procedure — build, install, the blocklist rationale, verification, and rollback — is in
**[dist/INSTALL.md](dist/INSTALL.md)**. The short version, starting from a fresh Haiku install:

**1. Get the tools and sources.** A stock Haiku nightly has neither `git` nor the build
tools, and the driver is compiled from a Haiku source checkout on the `usb-audio-uac2`
branch — so fetch all of it first (the machine must be online):

```sh
# git; build-driver.sh installs the Haiku build tools it finds missing
pkgman install -y git

# the Haiku source with the driver changes (shallow clone; still a few GB)
git clone -b usb-audio-uac2 --depth 1 https://github.com/rations/haiku.git ~/haiku

# this repo (the build/install scripts)
git clone https://github.com/rations/haiku-kernel-usb ~/haiku-kernel-usb
```

**2. Build and install the overrides** (on the nightly you will run — the binaries are
ABI-tied to it). The revision, architecture and paths are all detected; re-running after a
nightly update rebuilds and replaces the previous override:

```sh
cd ~/haiku-kernel-usb/dist
./build-driver.sh
./install-driver.sh          # --uninstall removes the overrides again
```

**3. Make the `xhci` override load.** The `usb_audio` driver override loads on a plain
reboot, but `xhci` is a bus-manager module that the boot loader **preloads** before the
packagefs blocklist applies — so a plain reboot keeps running the stock `xhci`. Pick one:

- **Per boot (evaluation):** reboot and tap **Space** at power-on to reach the boot menu →
  *Select safe mode options → Disable system components → `add-ons` → `kernel` → `boot`* → toggle
  **`xhci`** → boot. Applied before the preload, so your override wins. Repeat each boot.
- **Persistent:** bake the patched `xhci` into the `haiku` system package
  (`jam -q -sHAIKU_REVISION=$(uname -v | awk '{print $1}') haiku.hpkg`, then swap it in —
  never `cp` onto the live package). See INSTALL.md Option B.

**4. Verify:**

```sh
listusb                       # your UAC2 interface enumerates
ls /dev/audio/hmulti/          # an audio node appears
listimage | grep xhci          # the override shows a PATH (stock preload shows none)
```

This is the foundation of the audio stack; once USB audio works, install the JACK server
and the rest (jack-port-haiku → plugins → JackDAW).
