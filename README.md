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
  them on. The distribution scripts target **hrev59846 x86_64**; rebuild for any other
  nightly.

## Install

The stock `xhci` module takes precedence over a plain non-packaged copy, so installing the
override requires a specific packagefs blocklist step. The full procedure — build, install,
the blocklist rationale, verification, and rollback — is in **[dist/INSTALL.md](dist/INSTALL.md)**:

```sh
cd dist
HREV=hrev59846 HAIKU_SRC=$HOME/haiku ./build-driver.sh
./install-driver.sh
# then reboot and verify
```

This is the foundation of the audio stack; once USB audio works, install the JACK server
and the rest (jack-port-haiku → plugins → JackDAW).
