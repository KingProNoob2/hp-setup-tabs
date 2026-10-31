/* SpfTabs.efi  v1   --  add TWO pages to the setup tab bar (7 tabs total)
 *
 * Proven on hardware: writing one extra entry into the empty sixth slot of the
 * tab table adds one tab.  The table is scanned per form (first match wins), and
 * the menu node is AllocatePool(0x5A) = six 15-byte EFI_IFR_REFs, so six tabs
 * were the limit.  This build lifts that limit for the current boot:
 *
 *   1. allocate a new table in pool memory with the five existing entries plus
 *      our two new ones,
 *   2. repoint the builder's two `lea` instructions at it,
 *   3. widen the scan bound (0xC0 -> N*0x20),
 *   4. widen the menu-node allocation (0x5A -> N*0x0F).
 *
 * Two tabs, not more: the builder has a special case when its ref counter
 * reaches 7 (0xE668 == 7), so 7 refs is the safe maximum.
 *
 * Live-build patch sites (from work/amitse_live0.bin):
 *   0x27FFE  48 8D 0D <disp32>   lea rcx, [table]
 *   0x28055  48 8D 0D <disp32>   lea rcx, [table]   (loop tail)
 *   0x28060  48 81 FF <imm32>    cmp rdi, 0xC0      (scan bound)
 *   0x27766  B9 <imm32>          mov ecx, 0x5A      (node size)
 *
 * Every site is byte-checked before anything is written.
 */
typedef unsigned long long u64;
typedef unsigned int       u32;
typedef unsigned short     u16;
typedef unsigned char      u8;
typedef long long          s64;
typedef u64                UINTN;
typedef u64                EFI_STATUS;
typedef void              *EFI_HANDLE;
typedef u16                CHAR16;

#define EFI_SUCCESS   0ULL
#define EFI_MEMORY_RO 0x0000000000020000ULL

typedef struct { u32 d1; u16 d2; u16 d3; u8 d4[8]; } EFI_GUID;
static const EFI_GUID gLiGuid = { 0x5B1B31A1,0x9562,0x11D2,{0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B} };
static const u8 gFormSet[16] = { 0x4A,0x10,0x59,0x7B,0x0D,0xC0,0x58,0x41,0x87,0xFF,0xF0,0x4D,0x63,0x96,0xA9,0x15 };

static const u8 gSig1[32] = {
    0x56,0x57,0x48,0x89,0xD6,0x48,0x89,0xCF,0x4E,0x8D,0x4C,0x06,0xFF,0x48,0x39,0xFE,
    0x48,0x89,0xF8,0x73,0x05,0x49,0x39,0xF9,0x73,0x10,0x4C,0x89,0xC1,0x49,0x83,0xE0 };
static const u8 gSig2[32] = {
    0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xDA,0x48,0x8B,0xF9,
    0xE8,0x17,0x00,0x00,0x00,0x48,0x8B,0xD3,0x48,0x8B,0xCF,0xE8,0xC0,0x03,0x00,0x00 };

#define RVA_TABLE   0x6d470u
#define RVA_LEA1    0x27ffeu
#define RVA_LEA2    0x28055u
#define RVA_BOUND   0x28060u
#define RVA_NODESZ  0x27766u

#ifndef EXTRA1
#define EXTRA1 0x273f          /* CPU Configuration   */
#endif
#ifndef EXTRA2
#define EXTRA2 0x2771          /* PCH-IO Configuration */
#endif
#define NEXTRA 2
#define NTABS (5 + NEXTRA)

typedef struct { u32 Type; u32 Pad; u64 PhysicalStart; u64 VirtualStart; u64 NumberOfPages; u64 Attribute; } MEM_DESC;

static void *gST, *gBS;
static EFI_HANDLE gImage;
static MEM_DESC *gMapPtr;
static u64 gMapN, gMapDs;
static CHAR16 gLog[4000];
static UINTN gLogLen;
static void *gRoot;

EFI_STATUS efi_main(EFI_HANDLE image, void *st);
static void *const gAnchor __attribute__((used)) = (void *)&efi_main;

static int mem_eq(const u8 *a, const u8 *b, u32 n) { u32 i; for (i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }
static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u64 rd64(const u8 *p) { return (u64)rd32(p) | ((u64)rd32(p + 4) << 32); }
static void wr32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static void wr64(u8 *p, u64 v) { wr32(p, (u32)v); wr32(p + 4, (u32)(v >> 32)); }
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
static void say_dec(u32 v) { CHAR16 b[11]; int i = 10; b[10] = 0; do { b[--i] = (CHAR16)('0' + v % 10); v /= 10; } while (v && i > 0); say(b + i); }
static void say_hex(u64 v)
{
    CHAR16 b[19]; int i;
    b[0] = '0'; b[1] = 'x';
    for (i = 0; i < 16; i++) { u32 n = (u32)((v >> (60 - 4 * i)) & 0xF); b[2 + i] = (CHAR16)(n < 10 ? '0' + n : 'A' + n - 10); }
    b[18] = 0; say(b);
}
static void stall_ms(u32 ms) { EFI_STATUS (*S)(UINTN) = bsf(0xF8); if (S) S((UINTN)ms * 1000); }

static void lg(const CHAR16 *t) { UINTN i = 0; while (t[i] && gLogLen + 2 < sizeof(gLog) / sizeof(CHAR16)) gLog[gLogLen++] = t[i++]; gLog[gLogLen] = 0; }
static void lg_hex(u64 v, int digits)
{
    CHAR16 b[19]; int i;
    b[0] = '0'; b[1] = 'x';
    for (i = 0; i < digits; i++) { u32 n = (u32)((v >> (4 * (digits - 1 - i))) & 0xF); b[2 + i] = (CHAR16)(n < 10 ? '0' + n : 'A' + n - 10); }
    b[2 + digits] = 0; lg(b);
}
static void lg_dec(u32 v) { CHAR16 b[11]; int i = 10; b[10] = 0; do { b[--i] = (CHAR16)('0' + v % 10); v /= 10; } while (v && i > 0); lg(b + i); }
static void lg_tabs(void)
{
    lg(L"  tabs "); lg_hex((u64)NTABS, 2);
    lg(L"  extra1 0x"); lg_hex(EXTRA1, 4);
    lg(L"  extra2 0x"); lg_hex(EXTRA2, 4); lg(L"\r\n");
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
        s = FOpen(gRoot, &file, L"\\SPFTABS.LOG", 3, 0);
        if (s == EFI_SUCCESS && file) { FDelete(file); file = 0; }
        s = FOpen(gRoot, &file, L"\\SPFTABS.LOG", 3 | 0x8000000000000000ULL, 0);
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
        if (!base || rd16(base) != 0x5A4D) continue;
        {
            u32 pe = rd32(base + 0x3c);
            if (rd32(base + pe) != 0x00004550u) continue;
            if (mem_eq(base + 0x2c0, gSig1, 32) && mem_eq(base + 0x4a0, gSig2, 32)) found = base;
        }
    }
    FreePool(hs);
    return found;
}

EFI_STATUS efi_main(EFI_HANDLE image, void *st)
{
    (void)gAnchor;
    gST = st; gImage = image;
    gBS = *(void **)((u8 *)st + 0x60);
    if (!gBS) return 1;
    open_root();

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
        u8 *lea1, *lea2, *bound, *nodesz, *oldtab;
        u8 *nt = 0;
        int i;
        if (!amitse) { say(L"AMITSE not found\r\n"); stall_ms(2500); return 0; }
        lg(L"AMITSE "); lg_hex((u64)amitse, 12); lg(L"\r\n"); lg_tabs();

        lea1   = amitse + RVA_LEA1;
        lea2   = amitse + RVA_LEA2;
        bound  = amitse + RVA_BOUND;
        nodesz = amitse + RVA_NODESZ;
        oldtab = amitse + RVA_TABLE;

        /* ---- verify every site before touching anything ---- */
        {
            int ok = 1;
            u64 t1, t2;
            if (!(lea1[0] == 0x48 && lea1[1] == 0x8D && lea1[2] == 0x0D)) ok = 0;
            if (!(lea2[0] == 0x48 && lea2[1] == 0x8D && lea2[2] == 0x0D)) ok = 0;
            if (!(bound[0] == 0x48 && bound[1] == 0x81 && bound[2] == 0xFF && rd32(bound + 3) == 0xC0)) ok = 0;
            if (!(nodesz[0] == 0xB9 && rd32(nodesz + 1) == 0x5A)) ok = 0;
            t1 = (u64)(lea1 + 7) + (u64)(int)rd32(lea1 + 3);
            t2 = (u64)(lea2 + 7) + (u64)(int)rd32(lea2 + 3);
            if (t1 != (u64)oldtab || t2 != (u64)oldtab) ok = 0;
            if (!ok) {
                say(L"patch sites do not match - nothing written\r\n");
                lg(L"  site check FAILED\r\n"); commit(); stall_ms(4000); return 0;
            }
            lg(L"  sites verified, table "); lg_hex((u64)oldtab, 12); lg(L"\r\n");
        }
        if (!page_writable((u64)lea1) || !page_writable((u64)nodesz)) {
            say(L"code page read-only\r\n"); lg(L"  read-only code\r\n"); commit(); stall_ms(3500); return 0;
        }

        /* ---- build the bigger table ---- */
        {
            EFI_STATUS (*AllocatePool)(u32, UINTN, void **) = bsf(0x40);
            if (AllocatePool(4, (UINTN)NTABS * 0x20, (void **)&nt) != EFI_SUCCESS || !nt) {
                say(L"pool alloc failed\r\n"); lg(L"  pool alloc failed\r\n"); commit(); stall_ms(3000); return 0;
            }
            for (i = 0; i < 5 * 0x20; i++) nt[i] = oldtab[i];
            for (i = 0; i < NEXTRA; i++) {
                u8 *e = nt + (5 + i) * 0x20;
                u16 fid = (i == 0) ? (u16)EXTRA1 : (u16)EXTRA2;
                int k;
                for (k = 0; k < 16; k++) e[k] = gFormSet[k];
                e[0x10] = (u8)(fid & 0xFF);
                e[0x11] = (u8)(fid >> 8);
                for (k = 0x12; k < 0x20; k++) e[k] = 0;
                lg(L"  new entry "); lg_dec((u32)i);
                lg(L" form 0x"); lg_hex(fid, 4); lg(L"\r\n");
            }
            lg(L"  new table "); lg_hex((u64)nt, 12); lg(L"\r\n");
        }

        /* ---- repoint + resize ---- */
        wr32(lea1 + 3, (u32)(s64)((u8 *)nt - (lea1 + 7)));
        wr32(lea2 + 3, (u32)(s64)((u8 *)nt - (lea2 + 7)));
        wr32(bound + 3, (u32)(NTABS * 0x20));
        wr32(nodesz + 1, (u32)(NTABS * 0x0F));

        if (((u64)(lea1 + 7) + (u64)(int)rd32(lea1 + 3)) != (u64)nt ||
            rd32(bound + 3) != (u32)(NTABS * 0x20) ||
            rd32(nodesz + 1) != (u32)(NTABS * 0x0F)) {
            say(L"readback mismatch - not using it\r\n");
            lg(L"  readback MISMATCH\r\n"); commit(); stall_ms(4000); return 0;
        }
        lg(L"  patched: lea1 lea2 bound node size\r\n"); commit();
        say(L"Tabs "); say_dec((u32)NTABS); say(L" armed (2 extra). Press F10.\r\n");
        stall_ms(4000);
    }
    return 0;
}
