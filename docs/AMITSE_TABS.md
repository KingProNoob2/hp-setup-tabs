# AMITSE setup tabs — reverse-engineering notes

Everything here was derived from the firmware image (`sp158736.exe` -> UCP ->
`@R00`/`@R01` -> AMI `@UAF` container -> EFI-compressed FVs -> inner LZMA FV) and
confirmed on hardware.

## Where the tab bar comes from

AMITSE (the setup UI, FV file `b1da0adf-4f77-4070-a88e-bffe1c60529a`) builds the
tab row in `FUN_000275c8`, which walks a table of `0x20`-byte entries:

```
+0x00  EFI_GUID  formset GUID   7B59104A-C00D-4158-87FF-F04D6396A915
+0x10  UINT16    FormId
+0x12  UINT16    padding
+0x14  UINT32    padding
+0x18  UINT64    extra match key (0 = wildcard)
```

For every *form* in the compiled setup database it scans the table; the first
entry whose GUID and FormId match the form is used to append a 15-byte
`EFI_IFR_REF` to the menu node, labelled with the form's own title string id.
The node is `AllocatePool(0x5A)` = **six** refs, and the scan is bounded by
`cmp rdi, 0xC0` = **six** entries.

Live table on the tested machine (AMITSE at 0x4EB11000, table at +0x6D470):

```
+0x6D470  {7B59104A-…, 0x2718}  Main
+0x6D490  {7B59104A-…, 0x2719}  Security
+0x6D4B0  {7B59104A-…, 0x271A}  Configuration
+0x6D4D0  {7B59104A-…, 0x2A09}  Boot Options
+0x6D4F0  {7B59104A-…, 0x271B}  Exit
+0x6D510  00 …  (free, and still inside the window)
+0x6D530  start of the next table
```

So `src/spftab.c` writes one entry at `+0x6D510` (and, optionally, overwrites the
Main entry at `+0x6D470` to buy a second tab).

## What does NOT work

* **Patching the IFR suppression** (`Suppress If {True}` around `Ref Advanced` in
  the Setup module) or the HII package list — the UI does not render from HII.
* **Patching `$SPF`**, the compiled database in FV file `AMITSESetupData`: the item
  condition field at `+0x30` was the first guess, but the tab bar never comes from
  form `0x2710` (the form that holds those suppressed refs), so it changed nothing.
* **Hooking the FV protocol** to catch the database read: setup never re-reads it
  at F10 — the pointer is restored from AMITSE's saved-state stack
  (`FUN_0002009c` push / `FUN_0002056c` pop, pointer at `ctx[0]`).
* **Widening the tab limit** (`src/spftabs.c`): relocating the table into pool
  memory, repointing the two `lea` instructions, raising the scan bound to 0xE0
  and the node to 0x69 reports success but adds nothing — the bound took effect
  while the repoint did not, so the builder scanned foreign entries.
* **Advanced / Chipset as tabs**: those forms are absent from this build's setup
  database, so no entry can ever match them.

## Build differences between the two images in the SoftPaq

| | `@R00` extraction | live machine |
|---|---|---|
| AMITSE `SizeOfImage` | 0xDE680 | 0xDE2E0 |
| tab table | +0x6D430 | +0x6D470 |
| `ImageBase` in header | 0 | rewritten by the loader to the load address |

That last row is why a PE-header identity test (`ImageBase == 0`) fails on the
running image while the code signatures at RVA 0x2C0/0x4A0 still match.

## Reproducing on another BIOS build

1. `make dump`, run `SpfDump.efi`, copy `\AMITSE.BIN` off the card.
2. Open it in Ghidra at its RVAs, find the function that does
   `lea rcx,[rip+…]` + `cmp rdi,0xC0` + `memcmp(guid, entry, 0x10)`.
3. Read the table address and the entry layout from there.
4. Adjust `RVA_TAB` (and the FormIds) in `src/spftab.c`.

Or just run it: it verifies the five known FormIds at the hardcoded address and
refuses to write if they are not there.
