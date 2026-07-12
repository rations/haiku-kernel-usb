# Shelved usb_audio pieces (NOT applied to the tree)

`post-8buffer-increments-full.diff` is the complete uncommitted diff that existed on the
`usb-audio-uac2` branch after commit `1a1e25fc` ("usb_audio: keep eight sample buffers in
flight"), captured 2026-07-04 before it was reverted during the silence-regression bisect.

## Status of each piece inside the diff

Already re-applied to the tree and hardware-verified (do NOT re-apply from here):
- **OnRemove run-summary** (Stream.cpp TEMP diagnostic) — committed separately.
- **UAC2 clock restore on reattach** (AudioControlInterface.h/.cpp remember last
  clockId/rate; Device.cpp CompareAndReattach restores it once; Stream::OnReattach gates
  per-stream rate restore to R1) — committed separately; fixed replug at 48 kHz.

Still shelved (the silence regression from 2026-07-04 lives somewhere in these; bisect was
stopped once the working state was restored — test ONE AT A TIME on top of the known-good
base if ever revisited):
1. **Stop()/OnRemove() reorder ("panic fix")** — Stream.cpp: mark stopped first, cancel,
   then wait for callbacks after the cancel. Was meant to fix the kernel panic on a live
   sample-rate change (real upstream teardown use-after-free; xhci delivers cancelled
   callbacks synchronously — see xhci.cpp:1376). Prime suspect for the silence regression
   per the hardware bisect. The panic itself is deferred: changing the rate while audio
   plays is not needed (JACK requires a server restart to change rates anyway).
2. **Duration-based buffer sizing** — Driver.h `kSamplesBufferDurationUs` (42667 µs)
   replacing the fixed 2048-sample `kSamplesBufferSize`; Stream.cpp `_SetupBuffers`
   computes samples per buffer from the rate. Arithmetically a no-op at 48 kHz stereo;
   makes 192 kHz as stall-tolerant as 48 kHz. Untested in isolation.
3. **fRemoved-late ordering in CompareAndReattach** — Device.cpp: keep `fRemoved` set
   until the reattach fully completes so a concurrent buffer exchange cannot start streams
   on half re-bound endpoints. Theoretically sound; untested in isolation.

The diff contexts predate the clock-restore commit, so hunks may need fuzz/manual
application on the current tree.
