/* SpfTab.efi  v1
 *
 * Adds a tab to HP's BIOS Setup Utility.
 *
 * The tab bar is not built from the setup form that contains the "Advanced" /
 * "Chipset" refs - it is built from a 6-slot table in AMITSE's .data at
 * RVA 0x6D430, 0x20 bytes per slot:
 *
 *     +0x00  EFI_GUID  formset GUID (7B59104A-C00D-4158-87FF-F04D6396A915)
 *     +0x10  UINT16    FormId of the page to show as a tab
 *     +0x12  UINT16    padding
 *     +0x14  UINT32    padding
 *     +0x18  UINT64    extra match key (0 = wildcard)
 *
 *   slot 0 0x2718 Main       slot 3 0x2A09 Boot Options
 *   slot 1 0x2719 Security   slot 4 0x271B Exit
 *   slot 2 0x271A Configuration
 *   slot 5 --- empty ---     <- this is what we fill in
 *
 * FUN_000275c8 walks that table (stride 0x20, bound 0xC0) and, for every form
 * that matches, appends one 15-byte EFI_IFR_REF to the menu node (allocated 0x5A
 * bytes == six refs), using the form's own title string id as the label.  So one
 * more entry == one more tab, labelled by the form itself.
 *
 * Variant = the FormId placed in slot 5:
 *   5 (default) 0x2712 Advanced
 *   3           0x2713 Chipset
 *
 * Run from the boot manager, then press F10.
 */
typedef unsigned long long u64;
typedef unsigned int       u32;
typedef unsigned short     u16;
typedef unsigned char      u8;
typedef u64                UINTN;
typedef u64                EFI_STATUS;
typedef void              *EFI_HANDLE;
typedef u16                CHAR16;

#define EFI_SUCCESS  0ULL
#define EFI_MEMORY_RO 0x0000000000020000ULL

#ifndef VARIANT
#define VARIANT 5
#endif

typedef struct { u32 d1; u16 d2; u16 d3; u8 d4[8]; } EFI_GUID;

static const EFI_GUID gLiGuid = { 0x5B1B31A1,0x9562,0x11D2,{0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B} };
/* Setup formset GUID (same value as every existing table entry) */
static const u8 gFormSet[16] = { 0x4A,0x10,0x59,0x7B,0x0D,0xC0,0x58,0x41,0x87,0xFF,0xF0,0x4D,0x63,0x96,0xA9,0x15 };

static const u8 gSig1[32] = {
    0x56,0x57,0x48,0x89,0xD6,0x48,0x89,0xCF,0x4E,0x8D,0x4C,0x06,0xFF,0x48,0x39,0xFE,
    0x48,0x89,0xF8,0x73,0x05,0x49,0x39,0xF9,0x73,0x10,0x4C,0x89,0xC1,0x49,0x83,0xE0 };
static const u8 gSig2[32] = {
    0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xDA,0x48,0x8B,0xF9,
    0xE8,0x17,0x00,0x00,0x00,0x48,0x8B,0xD3,0x48,0x8B,0xCF,0xE8,0xC0,0x03,0x00,0x00 };

#define RVA_TAB   0x6d430u
#define TAB_STRIDE 0x20u
#define TAB_SLOTS  6u

typedef struct { u32 Type; u32 Pad; u64 PhysicalStart; u64 VirtualStart; u64 NumberOfPages; u64 Attribute; } MEM_DESC;

static void *gST, *gBS;
static EFI_HANDLE gImage;
static MEM_DESC *gMapPtr;
static u64 gMapN, gMapDs;

EFI_STATUS efi_main(EFI_HANDLE image, void *st);
static void *const gAnchor __attribute__((used)) = (void *)&efi_main;

static int mem_eq(const u8 *a, const u8 *b, u32 n) { u32 i; for (i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }
static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u64 rd64(const u8 *p) { return (u64)rd32(p) | ((u64)rd32(p + 4) << 32); }
static void wr64(u8 *p, u64 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); p[4] = (u8)(v >> 32); p[5] = (u8)(v >> 40); p[6] = (u8)(v >> 48); p[7] = (u8)(v >> 56); }
static void *bsf(u32 o) { return *(void **)((u8 *)gBS + o); }

static void say(const CHAR16 *s)
{
    void *co = gST ? *(void **)((u8 *)gST + 0x40) : 0;
    if (!co) return;
    {
        EFI_STATUS (*OutputString)(void *, const CHAR16 *) = *(void **)((u8 *)co + 8);
        OutputString(co, s);
    }
}
static void say_dec(u32 v)
{
    CHAR16 b[11]; int i = 10;
    b[10] = 0;
    do { b[--i] = (CHAR16)('0' + v % 10); v /= 10; } while (v && i > 0);
    say(b + i);
}
static void say_hex(u64 v)
{
    CHAR16 b[19]; int i;
    b[0] = '0'; b[1] = 'x';
    for (i = 0; i < 16; i++) {
        u32 n = (u32)((v >> (60 - 4 * i)) & 0xF);
        b[2 + i] = (CHAR16)(n < 10 ? '0' + n : 'A' + n - 10);
    }
    b[18] = 0;
    say(b);
}
static void stall_ms(u32 ms) { EFI_STATUS (*S)(UINTN) = bsf(0xF8); if (S) S((UINTN)ms * 1000); }

static int page_writable(u64 addr)
{
    u64 i;
    for (i = 0; i < gMapN; i++) {
        MEM_DESC *md = (MEM_DESC *)((u8 *)gMapPtr + i * gMapDs);
        u64 lo = md->PhysicalStart, hi = lo + md->NumberOfPages * 4096;
        if (addr >= lo && addr < hi) return (md->Attribute & EFI_MEMORY_RO) ? 0 : 1;
    }
    return 0;
}

static u8 *find_amitse(void)
{
    EFI_STATUS (*HP)(EFI_HANDLE, const EFI_GUID *, void **) = bsf(0x98);
    EFI_STATUS (*LHB)(u32, const EFI_GUID *, void *, UINTN *, EFI_HANDLE **) = bsf(0x138);
    void (*FreePool)(void *) = bsf(0x48);
    UINTN nh = 0; EFI_HANDLE *hs = 0; u8 *found = 0; u32 i;
    if (LHB(2, &gLiGuid, 0, &nh, &hs) != EFI_SUCCESS || !hs) return 0;
    for (i = 0; i < nh && !found; i++) {
        void *li = 0; u8 *base;
        if (HP(hs[i], &gLiGuid, &li) != EFI_SUCCESS || !li) continue;
        base = *(u8 **)((u8 *)li + 0x40);
        if (!base) continue;
        if (rd16(base) != 0x5A4D) continue;
        {
            u32 pe = rd32(base + 0x3c);
            if (rd32(base + pe) != 0x00004550u) continue;
            if ((rd64(base + pe + 24 + 24) == 0 &&
                 rd32(base + pe + 24 + 56) == 0xde680u &&
                 rd32(base + pe + 24 + 16) == 0x4a0u) ||
                (mem_eq(base + 0x2c0, gSig1, 32) && mem_eq(base + 0x4a0, gSig2, 32)))
                found = base;
        }
    }
    FreePool(hs);
    return found;
}

/* ---- tiny log to \SPFTAB.LOG --------------------------------------- */
static void *gRoot;
static CHAR16 gLog[6000];
static UINTN gLogLen;
static void lg(const CHAR16 *t)
{
    UINTN i = 0;
    while (t[i] && gLogLen + 2 < sizeof(gLog) / sizeof(CHAR16)) gLog[gLogLen++] = t[i++];
    gLog[gLogLen] = 0;
}
static void lg_hex(u64 v, int digits)
{
    CHAR16 b[19]; int i;
    b[0] = '0'; b[1] = 'x';
    for (i = 0; i < digits; i++) {
        u32 n = (u32)((v >> (4 * (digits - 1 - i))) & 0xF);
        b[2 + i] = (CHAR16)(n < 10 ? '0' + n : 'A' + n - 10);
    }
    b[2 + digits] = 0;
    lg(b);
}
static void lg_dec(u32 v)
{
    CHAR16 b[11]; int i = 10;
    b[10] = 0;
    do { b[--i] = (CHAR16)('0' + v % 10); v /= 10; } while (v && i > 0);
    lg(b + i);
}
static void commit(void)
{
    void **fv; void *file = 0; EFI_STATUS s; UINTN n;
    if (!gRoot) return;
    fv = (void **)gRoot;
    {
        EFI_STATUS (*FOpen)(void *, void **, const CHAR16 *, u64, u64) = fv[1];
        EFI_STATUS (*FClose)(void *) = fv[2];
        EFI_STATUS (*FDelete)(void *) = fv[3];
        EFI_STATUS (*FWrite)(void *, UINTN *, void *) = fv[5];
        s = FOpen(gRoot, &file, L"\\SPFTAB.LOG", 3, 0);
        if (s == EFI_SUCCESS && file) { FDelete(file); file = 0; }
        s = FOpen(gRoot, &file, L"\\SPFTAB.LOG", 3 | 0x8000000000000000ULL, 0);
        if (s != EFI_SUCCESS || !file) return;
        n = gLogLen * sizeof(CHAR16);
        FWrite(file, &n, gLog);
        FClose(file);
    }
}
static void open_root(void)
{
    static const EFI_GUID sfsGuid = { 0x964E5B22,0x6459,0x11D2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B} };
    EFI_STATUS (*HP)(EFI_HANDLE, const EFI_GUID *, void **) = bsf(0x98);
    void *li = 0, *sfs = 0, *root = 0;
    EFI_HANDLE dev;
    if (HP(gImage, &gLiGuid, &li) != EFI_SUCCESS || !li) return;
    dev = *(EFI_HANDLE *)((u8 *)li + 0x18);
    if (!dev || HP(dev, &sfsGuid, &sfs) != EFI_SUCCESS || !sfs) return;
    {
        EFI_STATUS (*OpenVolume)(void *, void **) = *(void **)((u8 *)sfs + 8);
        if (OpenVolume(sfs, &root) != EFI_SUCCESS || !root) return;
    }
    gRoot = root;
}

EFI_STATUS efi_main(EFI_HANDLE image, void *st)
{
    (void)gAnchor;
    gST = st; gImage = image;
    gBS = *(void **)((u8 *)st + 0x60);
    if (!gBS) return 1;
    open_root();

    /* memory map: only to check we may write AMITSE's .data */
    {
        EFI_STATUS (*GetMemoryMap)(UINTN *, void *, UINTN *, UINTN *, u32 *) = bsf(0x38);
        EFI_STATUS (*AllocatePool)(u32, UINTN, void **) = bsf(0x40);
        UINTN msize = 0, mkey = 0, ds = 0; u32 dv = 0; void *map = 0;
        GetMemoryMap(&msize, 0, &mkey, &ds, &dv);
        msize += 8 * ds + 4096;
        if (AllocatePool(4, msize, &map) == EFI_SUCCESS && map &&
            GetMemoryMap(&msize, map, &mkey, &ds, &dv) == EFI_SUCCESS) {
            gMapPtr = map; gMapN = msize / ds; gMapDs = ds;
        }
    }

    {
        u8 *amitse = find_amitse();
        if (!amitse) { say(L"AMITSE not found\r\n"); stall_ms(2500); return 0; }
        {
            /* Verified layout of the running build (from \SPFTAB.LOG):
             *   0x6D470 +0x10 = 0x2718 Main
             *   0x6D490 +0x10 = 0x2719 Security
             *   0x6D4B0 +0x10 = 0x271A Configuration
             *   0x6D4D0 +0x10 = 0x2A09 Boot Options
             *   0x6D4F0 +0x10 = 0x271B Exit
             *   0x6D510        free  <- sixth slot, inside the 0xC0 scan window
             * The table base is checked before anything is written. */
            enum { TAB_OFF = 0x6d470, ENTRY = 0x20, SLOT5 = TAB_OFF + 5 * 0x20 };
            u8 *t = amitse + TAB_OFF;
            u8 *e = amitse + SLOT5;
            u16 want;
#if defined(FORMID)
            want = (u16)FORMID;                /* explicit: -DFORMID=0x2722 */
#elif VARIANT == 3
            want = 0x2713;          /* Chipset */
#elif VARIANT == 1
            want = 0x2718;          /* duplicate Main (cannot work: first match wins) */
#elif VARIANT == 7
            want = 0x2771;          /* PCH-IO Configuration */
#elif VARIANT == 8
            want = 0x2757;          /* System Agent (SA) Configuration */
#elif VARIANT == 9
            want = 0x273f;          /* CPU Configuration */
#else
            want = 0x2712;          /* Advanced */
#endif
            int i, k;

            say(L"AMITSE "); say_hex((u64)amitse); say(L"\r\n");
            lg(L"AMITSE "); lg_hex((u64)amitse, 12); lg(L"\r\n");
            lg(L"table "); lg_hex((u64)(TAB_OFF), 5);
            lg(L" formids "); lg_hex(rd16(t + 0x10), 4);
            lg(L" ");         lg_hex(rd16(t + 0x30), 4);
            lg(L" ");         lg_hex(rd16(t + 0x50), 4);
            lg(L" ");         lg_hex(rd16(t + 0x70), 4);
            lg(L" ");         lg_hex(rd16(t + 0x90), 4);
            lg(L"  slot5 "); lg_hex((u64)SLOT5, 5);
            lg(L" formid "); lg_hex(rd16(e + 0x10), 4); lg(L"\r\n");

            if (rd16(t + 0x10) != 0x2718 || rd16(t + 0x30) != 0x2719 ||
                rd16(t + 0x50) != 0x271a || rd16(t + 0x70) != 0x2a09 ||
                rd16(t + 0x90) != 0x271b) {
                say(L"tab table not where expected - nothing written\r\n");
                lg(L"table check FAILED, dumping window\r\n");
                for (i = 0; i + 0x20 <= 0x140; i += 0x20) {
                    lg(L"  "); lg_hex((u64)(TAB_OFF + i), 5); lg(L": ");
                    for (k = 0; k < 0x20; k++) lg_hex(t[i + k], 2);
                    lg(L"\r\n");
                }
                commit(); stall_ms(4000); return 0;
            }
            if (rd16(e + 0x10) != 0 || rd32(e) != 0) {
                say(L"sixth slot is not empty - nothing written\r\n");
                commit(); stall_ms(4000); return 0;
            }
            if (!page_writable((u64)e)) {
                say(L"table page read-only\r\n"); commit(); stall_ms(3500); return 0;
            }
            for (i = 0; i < 16; i++) e[i] = gFormSet[i];
            e[0x10] = (u8)(want & 0xFF);
            e[0x11] = (u8)(want >> 8);
            for (i = 0x12; i < 0x20; i++) e[i] = 0;

#if REPLACE_MAIN
            /* Second page: overwrite the "Main" entry (entry 0).  Main is only
             * system information, and this is the proven lever - an entry that
             * no longer matches its form simply loses its tab. */
            {
                u8 *m = t;                    /* entry 0 = 0x6D470 */
                u16 w2 = (u16)REPLACE_MAIN;
                lg(L"replace Main entry -> 0x"); lg_hex(w2, 4); lg(L"\r\n");
                for (i = 0; i < 16; i++) m[i] = gFormSet[i];
                m[0x10] = (u8)(w2 & 0xFF);
                m[0x11] = (u8)(w2 >> 8);
                for (i = 0x12; i < 0x20; i++) m[i] = 0;
                say(L"Main tab -> 0x"); say_hex(w2); say(L"\r\n");
            }
#endif

            lg(L"wrote entry5 -> "); lg_hex(want, 4);
            lg(L"  readback "); lg_hex(rd16(e + 0x10), 4); lg(L"\r\n");
            commit();
            say(L"wrote slot5 -> 0x"); say_hex(want);
            say(L"  readback 0x"); say_hex(rd16(e + 0x10));
            say(L"\r\n");
            say(L"Press F10 now.\r\n");
            stall_ms(4000);
        }
    }
    return 0;
}
