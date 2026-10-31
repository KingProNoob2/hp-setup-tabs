# HP setup tabs - UEFI applications that add a page to the F10 setup tab bar
#   make        build the three tab tools
#   make dump   build the AMITSE dumper
#
CC      = x86_64-w64-mingw32-gcc
CFLAGS  := -O2 -nostdlib -ffreestanding -fno-stack-protector \
           -fno-asynchronous-unwind-tables -Wall -Wextra \
           -Wno-unused-parameter -Wno-unused-function
LDFLAGS := -Wl,--subsystem,10 -Wl,-e,efi_main -Wl,--enable-reloc-section \
           -Wl,--no-insert-timestamp

# VARIANT is the FormId written into the one free tab slot
VARIANT_SpfTab-pchio.efi := 7     # 0x2771 PCH-IO Configuration
VARIANT_SpfTab-cpu.efi   := 9     # 0x273F CPU Configuration
VARIANT_SpfTab-sa.efi    := 8     # 0x2757 System Agent

TARGETS := SpfTab-pchio.efi SpfTab-cpu.efi SpfTab-sa.efi

all: $(TARGETS)

%.efi: src/spftab.c
	$(CC) $(CFLAGS) $(LDFLAGS) -DVARIANT=$(VARIANT_$@) -o $@ $<

dump: SpfDump.efi
SpfDump.efi: src/spfdump.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f *.efi

.PHONY: all dump clean
