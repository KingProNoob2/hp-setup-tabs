# HP setup tab unlocker - EFI applications (x86_64, GNU-EFI-free, -nostdlib)
#
#   make            build the working variants
#   make dump       the AMITSE dumper
#   make experiment the 7-tab attempt (does NOT work, see docs/)
#
CC      = x86_64-w64-mingw32-gcc
CFLAGS  := -O2 -nostdlib -ffreestanding -fno-stack-protector \
           -fno-asynchronous-unwind-tables -Wall -Wextra \
           -Wno-unused-parameter -Wno-unused-function
LDFLAGS := -Wl,--subsystem,10 -Wl,-e,efi_main -Wl,--enable-reloc-section \
           -Wl,--no-insert-timestamp

# VARIANT selects the FormId written into the free tab slot:
#   5 = 0x2712 Advanced      3 = 0x2713 Chipset
#   7 = 0x2771 PCH-IO        8 = 0x2757 System Agent      9 = 0x273F CPU
TARGETS := SpfTab-pchio.efi SpfTab-cpu.efi SpfTab-sa.efi \
           SpfTab-advanced.efi SpfTab-chipset.efi \
           SpfTab-cpu-sa.efi SpfTab-pchio-cpu.efi

all: $(TARGETS)

%.efi: src/spftab.c
	$(CC) $(CFLAGS) $(LDFLAGS) -DVARIANT=$(VARIANT_$@) $(EXTRA_$@) -o $@ $<

VARIANT_SpfTab-pchio.efi      := 7
VARIANT_SpfTab-cpu.efi        := 9
VARIANT_SpfTab-sa.efi         := 8
VARIANT_SpfTab-advanced.efi   := 5
VARIANT_SpfTab-chipset.efi    := 3
VARIANT_SpfTab-cpu-sa.efi     := 5
EXTRA_SpfTab-cpu-sa.efi       := -DREPLACE_MAIN=0x2757
VARIANT_SpfTab-pchio-cpu.efi  := 7
EXTRA_SpfTab-pchio-cpu.efi    := -DREPLACE_MAIN=0x273f

dump: SpfDump.efi
SpfDump.efi: src/spfdump.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

experiment: SpfTabs7.efi
SpfTabs7.efi: src/spftabs.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f *.efi

.PHONY: all dump experiment clean
