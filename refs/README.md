# Reference material

The xHCI work in this repo (`patches/`, the driver changes in the
[`rations/haiku` `usb-audio-uac2`](https://github.com/rations/haiku/tree/usb-audio-uac2)
fork) is written against:

- **eXtensible Host Controller Interface for Universal Serial Bus (xHCI) Requirements
  Specification, Revision 1.2** — published by Intel Corporation.

The spec itself is **not vendored here**: it is a large, copyrighted third-party document,
so this directory is git-ignored except for this note. Download it from Intel's USB /
xHCI documentation pages (search "Intel xHCI 1.2 specification") and drop the PDF in this
directory locally if you want it alongside the source.

Section references in the driver comments and in `../notes/` are to that revision.
