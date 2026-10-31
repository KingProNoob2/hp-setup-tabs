# HP setup tabs

**Three tiny UEFI apps that put a hidden page on your HP laptop's F10 setup tab
bar — at runtime, from RAM, with no flash write.**

Stock HP consumer BIOSes hide the interesting pages. On an HP Spectre x360
15-eb0xxx the tab bar is just:

```
Main | Security | Configuration | Boot Options | Exit
```

…while the firmware contains dozens of pages (CPU, PCH-IO, System Agent, PCIe,
NVMe, thermal, and more) that are simply never linked to a tab. Run one of these
from the firmware's own file browser and press F10:

```
PCH-IO Configuration | Main | Security | Configuration | Boot Options | Exit
```

Nothing is flashed. A power cycle restores the stock tab bar.

---

## Quick start

1. Copy the `.efi` you want to the **internal ESP** or an SD card, e.g.
   `\EFI\SREP\TAB.EFI`.
2. Power on → **F9** → *Boot From EFI File* → pick that file.
   It prints the table it found and what it wrote:
   ```
   AMITSE 0x4EB11000
   table 0x6D470 formids 2718 2719 271A 2A09 271B  slot5 0x6D510 formid 0000
   wrote slot5 -> 0x2771  readback 0x2771
   Press F10 now.
   ```
3. Press **F10** → the tab bar has one more page.

One page per boot: the menu holds six tabs and five are already used, so there is
exactly one free slot.

## The three pages

| file | adds | why you want it |
|---|---|---|
| `SpfTab-pchio.efi` | **PCH-IO Configuration** (0x2771, 42 items) | SATA/RST, USB, PCIe, HD audio, SerialIO, SCS, ISH, thermal throttling, FPRR |
| `SpfTab-cpu.efi` | **CPU Configuration** (0x273F, 41 items) | cores/HT, VT-x/VT-d, C-states, turbo, **Power Limit 1-4** |
| `SpfTab-sa.efi` | **System Agent** (0x2757, 24 items) | memory, graphics, PCIe root ports |
| `SpfTab-connectivity.efi` | **Connectivity Configuration** (0x2722, 112 items) | the biggest hidden page |
| `SpfTab-usb.efi` | **USB Configuration** (0x282F, 58 items) | per-port USB / xHCI settings |
| `SpfTab-thermal.efi` | **Thermal Configuration** (0x278C, 5 items) | thermal policy |
| `SpfDump.efi` | *(dumper)* | writes the running AMITSE image to `\AMITSE.BIN` — for other BIOS builds |

`Advanced` (0x2712) and `Chipset` (0x2713) themselves **cannot** be restored:
those container forms are not in this build's setup database, so the setup UI's
tab builder never matches them. The three pages above are the ones inside them
that are worth having.

## How it works (short version)

The setup UI builds the tab bar from a table of 0x20-byte entries
`{formset GUID, FormId, extra}`. On this model the table sits at
**AMITSE+0x6D470** with five entries used and **slot 5 free** at `0x6D510` —
still inside the tab builder's `0xC0`-byte scan window. Writing one entry there
adds one tab.

Every tool first byte-verifies the five known FormIds (`2718 2719 271A 2A09 271B`)
and refuses to write anything if they are not where they should be, so it can't
clobber an unrelated entry on a different BIOS build.

Full mechanism, the live table, and a list of every approach that *doesn't* work
(and why) are in **[docs/AMITSE_TABS.md](docs/AMITSE_TABS.md)**.

## Build

```sh
make          # the three tab tools   (needs x86_64-w64-mingw32-gcc)
make dump     # the AMITSE dumper
```

No EDK2 required — plain `-nostdlib` freestanding `efi_main`.

## Other BIOS builds

The same F.23 SoftPaq ships two different AMITSE builds (table at `0x6D430` vs
`0x6D470`, `SizeOfImage` `0xDE680` vs `0xDE2E0`), which is why the tools verify
instead of assuming. If yours differs, run `SpfDump.efi`, keep `\AMITSE.BIN`, and
re-derive the table address as described at the end of `docs/AMITSE_TABS.md`.

## Caveats

* RAM only, one extra tab per boot, gone after a power cycle.
* The tools only ever write inside AMITSE's `.data`; a wrong write removes a tab,
  it cannot brick anything.
* Verified on: HP Spectre x360 15-eb0xxx (BIOS F.23, AMI Aptio). Other HP models
  with the same setup formset should behave the same way.

## License

MIT
