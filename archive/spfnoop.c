/* SPFNOOP.EFI - does nothing at all, just returns.
 * Control test: if THIS freezes the boot manager, the problem is not our code. */
typedef unsigned long long u64;
typedef unsigned long long EFI_STATUS;
typedef void *EFI_HANDLE;
EFI_STATUS efi_main(EFI_HANDLE image, void *st);
static void *const gAnchor __attribute__((used)) = (void *)&efi_main;
EFI_STATUS efi_main(EFI_HANDLE image, void *st) { (void)image; (void)st; (void)gAnchor; return 0; }
