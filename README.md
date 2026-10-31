# hp-setup-tabs

Two small UEFI applications for **HP laptops whose F10 setup hides pages** — they
add a page to the setup tab bar at runtime, from RAM, with no flash write and no
signed-image problems.

Built and tested on an HP Spectre x360 15-eb0xxx (BIOS F.23, AMI Aptio), where the
stock tab bar is `Main | Security | Configuration | Boot Options | Exit` and the
*Advanced* / *Chipset* pages exist in the firmware but are never linked anywhere.

```
F9 (boot device menu) -> Boot From EFI File -> SpfTab-pchio.efi
   "table 0x6D470 formids 2718 2719 271A 2A09 271B  slot5 0x6D510 formid 0000"
   "wrote slot5 -> 0x2771  readback 0x2771"
F10 -> the tab row now has a sixth tab: "PCH-IO Configuration"
```

## How the tab bar is built

The setup UI (AMITSE) builds the tab row from a **six-slot table in its .data**,
each entry `0x20` bytes:

```
+0x00  EFI_GUID  formset GUID   7B59104A-C00D-4158-87FF-F04D6396A915
+0x10  UINT16    FormId of the page to show as a tab
+0x18  UINT64    extra match key (0 = wildcard)
```

`FUN_000275c8` walks that table and, for every form that matches, appends one
15-byte `EFI_IFR_REF` to a menu node allocated for **six** refs, using the form's
own title string as the label.

On this model the table is at **AMITSE+0x6D470** and holds five entries
(`0x2718` Main, `0x2719` Security, `0x271A` Configuration, `0x2A09` Boot Options,
`0x271B` Exit) — so **slot 5 at 0x6D510 is free**, and it is still inside the
builder's `0xC0`-byte scan window. One 0x20-byte write = one extra tab.

`Advanced` (0x2712) and `Chipset` (0x2713) themselves **cannot** be added: those
container forms are not in this build's setup database, so the per-form loop never
matches them. Every page *inside* them can be, though — that is what the variants
below do.

## Variants

| file | tab added | notes |
|---|---|---|
| `SpfTab-pchio.efi` | PCH-IO Configuration (0x2771) | 42 items, **verified working** |
| `SpfTab-cpu.efi` | CPU Configuration (0x273F) | 41 items, incl. Power Limit 1-4 |
| `SpfTab-sa.efi` | System Agent (0x2757) | 24 items |
| `SpfTab-advanced.efi` | Advanced (0x2712) | only on a build whose database has it |
| `SpfTab-chipset.efi` | Chipset (0x2713) | same |
| `SpfTab-cpu-sa.efi` | CPU + SA | gives up the **Main** tab for the second slot |
| `SpfTab-pchio-cpu.efi` | PCH-IO + CPU | same |

Six tabs is the ceiling: the menu node holds six refs and the table has six slots.
`SpfTab-*.efi` all byte-verify the five known FormIds before writing anything, and
refuse to touch a slot that already looks like an entry.

`SpfDump.efi` (`make dump`) writes the **running** AMITSE image to `\AMITSE.BIN` on
the ESP — use it if you want to re-derive the offsets for a different BIOS build.

## Build

```sh
make            # needs x86_64-w64-mingw32-gcc, no EDK2 required
make dump
```

Copy the `.efi` files to `EFI/` somewhere on an internal ESP or an SD card, boot
the firmware's file browser (F9 -> Boot From EFI File), run one, then press F10.

## Why the offsets can differ between machines

The same F.23 SoftPaq ships two complete 16 MB images, and the AMITSE build in
them differs (`SizeOfImage` 0xDE680 vs 0xDE2E0; the tab table sits at 0x6D430 in
one and 0x6D470 in the other). That is why the tool **verifies** the table
contents instead of trusting a hardcoded address, and why `SpfDump.efi` exists.

## Caveats

* RAM only — a power cycle restores the stock tab bar; nothing is flashed.
* One extra tab per boot. The first (failed) attempt to widen the node and
  relocate the table is kept in `src/spftabs.c` with an explanation in
  [docs/AMITSE_TABS.md](docs/AMITSE_TABS.md).
* Writing the wrong entry removes a tab (harmless) — it cannot brick anything,
  the tool only ever writes inside AMITSE's `.data`.
* `archive/` holds the earlier experiments (HII patching, `$SPF` item patching,
  FV hooks, resident shims). They all failed; they are kept because the reasons
  are instructive, documented in docs.

## License

MIT
