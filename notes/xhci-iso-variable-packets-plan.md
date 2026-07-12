# Plan: variable-length isochronous OUT packets in Haiku xHCI, for UAC2 async feedback

Status: DESIGN (research complete; not yet implemented)
Restore point before this work: `git -C ~/src/haiku reset --hard 26cbb1f0`

## 1. Root cause (verified from source)

Asynchronous USB-audio playback drifts and pops on Haiku (regular underrun gaps,
~2/s at 192 kHz, ~1/2 s at 48 kHz, inaudible on digital silence, on *every* async
interface). The device runs on its own crystal; we feed exactly-nominal frames, so
its FIFO slips and periodically starves. The standard cure is sample-rate feedback:
vary the frames per microframe (e.g. 24 then 25) so the average tracks the device.

That requires **variable `request_length` per packet within one isochronous transfer.**
Haiku's xHCI does not support it. In `XHCI::SubmitNormalRequest`
(`src/add-ons/kernel/busses/usb/xhci.cpp`):

- ~983: `trbSize = DataLength / packet_count` — a single uniform size.
- ~984-986: rejects the transfer (`B_BAD_VALUE`) unless `trbSize == packet_descriptors[0].request_length`.
- ~1016-1053: lays out `trbCount` TRBs of uniform `trbSize`; only `[0]` is ever read.

So per-packet `request_length[1..N]` are ignored, and a genuinely non-uniform OUT
transfer is refused outright. The driver's existing `_FillPlaybackPackets` per-packet
sizing has therefore always been a no-op. This is the true blocker; it lives in the
shared host-controller driver, not in `usb_audio`.

What already works and MUST be preserved:
- Uniform-`request_length` transfers (all current callers): audio nominal playback,
  and INPUT iso where every packet requests `maxpkt` and only the *received*
  `actual_length` varies (webcams via usb_raw, audio capture). The completion path
  already records per-packet `actual_length` for short packets
  (xhci.cpp ~2752-2774), so INPUT variability is not part of this change.

## 2. Design principles (RULES.md)

- **Gate on non-uniformity.** New code runs only when the caller supplies packets of
  differing `request_length`. Every existing caller keeps the exact current path,
  byte-for-byte. This bounds regression risk to the one brand-new caller (usb_audio
  async feedback).
- **Untrusted input.** `usb_raw` copies `packet_descriptors` from userspace
  (usb_raw.cpp ~828, `user_memcpy`). The new path must bounds-check every field
  before use: `packet_count` range, each `request_length <= maxPacketSize`,
  `sum(request_length) == DataLength`, and the contiguous-buffer size cap. Fail
  closed (`B_BAD_VALUE`), never size an allocation or index from an unchecked count.
- **Kernel context.** No new blocking/allocation in the completion/callback path.
  Allocation stays in the submit path exactly as today.
- **Minimal & surgical.** Reuse the existing descriptor/buffer machinery; add the
  smallest new branch. No reformatting; match xhci.cpp style (tabs, `B_`-returns).

## 3. xHCI change (Phase 1)

Scope: OUTPUT (device-OUT) isochronous only. INPUT unchanged.

In `SubmitNormalRequest`, isochronous branch:

1. Compute `bool uniform` = all `packet_descriptors[i].request_length` equal AND
   `DataLength == packet_count * request_length[0]`. If `uniform`, keep today's code
   verbatim.

2. Non-uniform OUT path (new), with validation first (fail closed on any):
   - `packet_count > 0` and `<= XHCI_MAX_TRANSFERS`-derived ring capacity.
   - direction is OUT (defer IN-variable to a later change; INPUT stays uniform).
   - each `request_length[i] > 0` and `<= pipe->MaxPacketSize()`.
   - `sum(request_length[i]) == transfer->DataLength()` (guards buffer overrun; use a
     64-bit accumulator to avoid overflow).
   - `DataLength < 32 * B_PAGE_SIZE` (single-chunk contiguous buffer path; always true
     for audio: <= ~85 * 400 B ~= 34 KB. Otherwise reject — no current caller hits it,
     and a per-packet-buffer variant can be added later if a large-OUT user appears).

3. Allocate one contiguous buffer: `CreateDescriptor(packet_count, 1, DataLength)`.
   `WriteDescriptor` (bufferCount == 1) packs the contiguous source into it unchanged.

4. Build `packet_count` ISOCH TRBs, walking a running byte offset:
   - `td->trbs[i].address = buffer_addrs[0] + offset`
   - `TRB_2_BYTES(request_length[i])`, `TRB_2_TD_SIZE(...)` per spec
   - ISOCH type; `[0]` gets SIA/FRID; non-last get ISP|BEI with CHAIN cleared —
     identical flag handling to the current loop, so scheduling semantics are
     inherited (not redesigned).
   - `offset += request_length[i]`

5. Completion path is already per-packet (indexes `packet_descriptors[offset]` by TRB
   index) and needs no change: TRB i still corresponds to packet i.

FLAGGED UNKNOWNS (verify during implementation, per RULES "never guess"):
- xHCI isochronous scheduling of packets after `[0]` (frame assignment when only the
  first TRB carries SIA). We inherit the current behavior rather than change it, but
  confirm against the xHCI 1.2 spec (no local PDF found: obtain it) that variable
  per-TRB lengths within the multi-TRB iso layout are legal and do not need per-TRB
  frame IDs. If the spec requires one TD (with its own frame) per service interval,
  the layout may need each packet as a separate TD — re-scope accordingly.
- High-bandwidth (Mult/TBC/TLBPC) is already an unhandled TODO (xhci.cpp ~1070) and is
  out of scope: audio packets are < maxpkt so Mult == 0. Leave the TODO.

## 4. usb_audio changes (Phases 2-3)

Phase 2 - accurate capture (also fixes the babble bug), no feedback yet:
- For the INPUT stream, request `fMaxPacketSize` per packet instead of nominal, so the
  device's bursts fit and stop babbling (endpoint-0x88 babble seen in logs). Size the
  capture DMA/descriptors for maxpkt. This makes per-packet `actual_length` reflect the
  true received frame count.
- Add a throttled diagnostic that logs the measured capture rate (sum actual_length /
  packets, in frames-per-microframe) vs nominal. VALIDATE on hardware that it is stable
  and slightly off nominal (real drift), now that the feedback-storm chaos is gone
  (the earlier "unreliable" conclusion was reached under that storm). Do NOT feed it to
  playback until validated.

Phase 3 - implicit feedback drives variable OUT packets:
- Re-enable the implicit path (`fUseImplicitFeedback`) only for devices with an implicit
  source. Prefer Linux's GENERIC_IMPLICIT_FB shape: mirror each capture packet's
  delivered frame count onto the next playback packet's `request_length` (exact,
  shares one crystal), or use the averaged rate already plumbed
  (`_PublishImplicitFeedback` -> `fDevice->Feedback()` -> `_FillPlaybackPackets`).
- With the Phase 1 xHCI fix, the resulting variable OUT `request_length`s now take
  effect, correcting drift -> pops gone.
- Clamp the per-packet frame count to a sane window (>= 7/8 nominal, <= maxpkt/stride)
  before use — untrusted-device defense and overrun guard.

## 5. Testing / regression (real hardware, RULES gate 5)

- Build just the module: `jam -q -sHAIKU_REVISION=hrev59820 xhci` (host build needs the
  `LIBRARY_PATH=/boot/system/lib:.../generated/objects/haiku_host/lib` prefix).
- BACK UP the running xHCI module before installing the new one; a bad build can drop
  all USB. Keep a copy and the restore commit handy; recover by removing the override
  and rebooting (user is at the machine).
- Regression (uniform path unchanged, must still work):
  1. USB mouse/keyboard (interrupt) responsive.
  2. USB webcam capture (iso IN) — the primary iso regression check; confirm it is no
     worse (ideally unchanged) with the new xHCI.
  3. Audio nominal playback still plays (no queue rejections).
- New behavior:
  4. Behringer UMC204HD playback at 192 kHz and 48 kHz for several minutes: pops gone.
  5. Capture/record correctness on the Behringer.
  6. Full-duplex (playback + capture together), clean plug/unplug, no leaks/panics.
  7. UAC1 full-speed device: no regression.

## 6. Phasing (each phase builds clean, is independently testable, and reversible)

- P0 done: checkpoint commit 26cbb1f0 (known-good: plays with drift pops, no storm).
- P1: xHCI variable-length OUT (gated) + regression tests (webcam/mouse/audio-nominal).
- P2: usb_audio capture maxpkt + measurement diagnostic; validate on hardware.
- P3: usb_audio implicit feedback -> variable OUT; confirm pops gone.
- P4: full validation matrix + export patches to ~/haiku-kernel-usb/patches/
  (two upstream commits: one to xHCI, one to usb_audio).
