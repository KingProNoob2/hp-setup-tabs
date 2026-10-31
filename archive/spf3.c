/* Spf3.efi  v3
 *
 * Answers: where do the visible tabs come from?
 *
 *  1. writes \SPFHOOK.LOG from a *shared* pool buffer, so the resident hook and
 *     the main body append to the same log (the v2 build let the clone overwrite
 *     it with its own stale snapshot).
 *  2. logs AMITSE's own database pointers before touching anything:
 *     DAT_0008ecc8 (live $SPF), DAT_0008eb68, DAT_0008eb58.
 *  3. if DAT_0008ecc8 already points at a valid $SPF, patches THAT buffer
 *     directly (the live copy only - never a scanned one).
 *  4. arms the FV read hooks and lets the trigger protocol start setup, logging
 *     every read the hook sees.
 *
 * If neither the direct patch nor the hook changes the tracer (Save&Exit label
 * 0x4C -> 0x1E), the tab list is built before any of our code runs.
 */
typedef unsigned long long u64;
typedef long long          s64;
typedef unsigned int       u32;
typedef unsigned short     u16;
typedef unsigned char      u8;
typedef u64                UINTN;
typedef u64                EFI_STATUS;
typedef void              *EFI_HANDLE;
typedef u16                CHAR16;

#define EFI_SUCCESS          0ULL
#define EfiBootServicesData  4
#define EfiBootServicesCode  3
#define AllocateAnyPages     0
#define EFI_MEMORY_RO        0x0000000000020000ULL

#ifndef VARIANT
#define VARIANT 5
#endif

typedef struct { u32 d1; u16 d2; u16 d3; u8 d4[8]; } EFI_GUID;
static const EFI_GUID gFv2Guid = { 0x220E73B6,0x6BDB,0x4413,{0x84,0x05,0xB9,0x74,0xB1,0x08,0x61,0x9A} };
static const EFI_GUID gLiGuid  = { 0x5B1B31A1,0x9562,0x11D2,{0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B} };
static const EFI_GUID gSfsGuid = { 0x964E5B22,0x6459,0x11D2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B} };
/* AMITSE .text bytes at RVA 0x2c0 / 0x4a0; the relocation table has no entries
 * in these ranges, so they are runtime-stable. */
static const u8 gSig1[32] = {
    0x56,0x57,0x48,0x89,0xD6,0x48,0x89,0xCF,0x4E,0x8D,0x4C,0x06,0xFF,0x48,0x39,0xFE,
    0x48,0x89,0xF8,0x73,0x05,0x49,0x39,0xF9,0x73,0x10,0x4C,0x89,0xC1,0x49,0x83,0xE0 };
static const u8 gSig2[32] = {
    0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xDA,0x48,0x8B,0xF9,
    0xE8,0x17,0x00,0x00,0x00,0x48,0x8B,0xD3,0x48,0x8B,0xCF,0xE8,0xC0,0x03,0x00,0x00 };

static const EFI_GUID gTrigGuid= { 0x451EAC81,0x0D7D,0x4B4D,{0xA3,0xD0,0x1C,0xB3,0xFE,0x1D,0x66,0x66} };
static const u8 gStoreGuid[16] = { 0x43,0xD6,0x87,0xEC,0xA4,0xEB,0xB5,0x4B,0xA1,0xE5,0x3F,0x3E,0x36,0xB2,0x0D,0xA9 };

typedef struct { u32 Type; u32 Pad; u64 PhysicalStart; u64 VirtualStart; u64 NumberOfPages; u64 Attribute; } MEM_DESC;
#define MAXFV 32
#define LOGCHARS 12000

typedef struct { void *proto; void *origRF; void *origRS; } FVENT;

static void      *gST, *gBS;
static EFI_HANDLE gImage;
static void      *gRoot;
static u8        *gImgBase;
static u64        gImgSize;
static u8        *gClone;
static FVENT      gFv[MAXFV];
static int        gFvN;
static MEM_DESC  *gMapPtr;
static u64        gMapN, gMapDs;
static u8        *gLogMem;          /* [u64 length][CHAR16 data] in pool memory */

EFI_STATUS efi_main(EFI_HANDLE image, void *st);
static void *const gAnchor __attribute__((used)) = (void *)&efi_main;

static void mem_copy(void *d, const void *s, UINTN n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; }
static void mem_zero(void *d, UINTN n) { u8 *a = d; while (n--) *a++ = 0; }
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
static void say_dec(u32 v)
{
    CHAR16 b[11]; int i = 10;
    b[10] = 0;
    do { b[--i] = (CHAR16)('0' + v % 10); v /= 10; } while (v && i > 0);
    say(b + i);
}
static void stall_ms(u32 ms) { EFI_STATUS (*S)(UINTN) = bsf(0xF8); if (S) S((UINTN)ms * 1000); }

/* ---- shared log --------------------------------------------------------- */
static void log_init(void)
{
    EFI_STATUS (*AllocatePool)(u32, UINTN, void **) = bsf(0x40);
    void *p = 0;
    if (gLogMem) return;
    if (AllocatePool(EfiBootServicesData, 8 + LOGCHARS * 2, &p) == EFI_SUCCESS && p) {
        mem_zero(p, 8 + LOGCHARS * 2);
        gLogMem = p;
    }
}
static void lg(const CHAR16 *s)
{
    UINTN n;
    CHAR16 *b;
    if (!gLogMem) return;
    n = *(u64 *)gLogMem;
    b = (CHAR16 *)(gLogMem + 8);
    while (*s && n + 2 < LOGCHARS) b[n++] = *s++;
    b[n] = 0;
    *(u64 *)gLogMem = n;
}
static void lg_hexn(u64 v, int digits)
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
    if (!gRoot || !gLogMem) return;
    fv = (void **)gRoot;
    {
        EFI_STATUS (*FOpen)(void *, void **, const CHAR16 *, u64, u64) = fv[1];
        EFI_STATUS (*FClose)(void *) = fv[2];
        EFI_STATUS (*FDelete)(void *) = fv[3];
        EFI_STATUS (*FWrite)(void *, UINTN *, void *) = fv[5];
        s = FOpen(gRoot, &file, L"\\SPFHOOK.LOG", 3, 0);
        if (s == EFI_SUCCESS && file) { FDelete(file); file = 0; }
        s = FOpen(gRoot, &file, L"\\SPFHOOK.LOG", 3 | 0x8000000000000000ULL, 0);
        if (s != EFI_SUCCESS || !file) return;
        n = *(u64 *)gLogMem * sizeof(CHAR16);
        FWrite(file, &n, gLogMem + 8);
        FClose(file);
    }
}
#define CKPT(t) do { lg(t); commit(); } while (0)

static int open_root(void)
{
    EFI_STATUS (*HP)(EFI_HANDLE, const EFI_GUID *, void **) = bsf(0x98);
    void *li = 0, *sfs = 0, *root = 0;
    EFI_HANDLE dev;
    if (HP(gImage, &gLiGuid, &li) != EFI_SUCCESS || !li) return 0;
    dev = *(EFI_HANDLE *)((u8 *)li + 0x18);
    if (!dev || HP(dev, &gSfsGuid, &sfs) != EFI_SUCCESS || !sfs) return 0;
    {
        EFI_STATUS (*OpenVolume)(void *, void **) = *(void **)((u8 *)sfs + 8);
        if (OpenVolume(sfs, &root) != EFI_SUCCESS || !root) return 0;
    }
    gRoot = root;
    return 1;
}

static int addr_mapped(u64 addr)
{
    u64 i;
    for (i = 0; i < gMapN; i++) {
        MEM_DESC *md = (MEM_DESC *)((u8 *)gMapPtr + i * gMapDs);
        if (addr >= md->PhysicalStart && addr < md->PhysicalStart + md->NumberOfPages * 4096) return 1;
    }
    return 0;
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

/* ---- $SPF --------------------------------------------------------------- */
static const u32 g_tab_prompt[6] = { 0x09, 0x1e, 0x1f, 0x39, 0x20, 0x4c };
typedef struct { u8 *base; u8 *root; u8 *item; int nkids; u8 *kid[8]; } SPF;

static int spf_prepare(SPF *s, u8 *base)
{
    u32 forms_off, item_off, cnt, i, prev;
    if (!base || rd32(base) != 0x46505324u) return 0;
    if (!mem_eq(base + 0x14, gStoreGuid, 16)) return 0;
    forms_off = rd32(base + 0x2c);
    item_off  = rd32(base + 0x30);
    if (forms_off < 0x40 || forms_off > 0x800000) return 0;
    if (item_off  < 0x40 || item_off  > 0x800000) return 0;
    cnt = rd32(base + forms_off);
    if (cnt < 2 || cnt > 4096) return 0;
    if (forms_off + 4 + 4 * cnt > item_off) return 0;
    {
        u32 first = rd32(base + forms_off + 4);
        if (first < forms_off + 4 + 4 * cnt) return 0;
        if (first > forms_off + 4 + 4 * cnt + 0x20) return 0;
    }
    prev = 0;
    for (i = 0; i < cnt; i++) {
        u32 v = rd32(base + forms_off + 4 + 4 * i);
        if (v < prev || v > 0x800000) return 0;
        prev = v;
    }
    s->base = base;
    s->item = base + item_off;
    s->root = 0;
    for (i = 0; i < cnt; i++) {
        u8 *rec = base + rd32(base + forms_off + 4 + 4 * i);
        if (rd16(rec + 8) != 1) continue;
        if (rd16(rec + 0x0a) != 0x2710) continue;
        s->root = rec; break;
    }
    if (!s->root) return 0;
    {
        u32 n = rd32(s->root + 0x1c);
        if (n < 6 || n > 8) return 0;
        s->nkids = (int)n;
        for (i = 0; i < n; i++) s->kid[i] = s->item + rd32(s->root + 0x20 + 4 * i);
    }
    for (i = 0; i < 6; i++) {
        if (rd32(s->kid[i] + 0x3c) != g_tab_prompt[i]) return 0;
        if (rd16(s->kid[i] + 0x14) != 1) return 0;
    }
    return 1;
}

static void spf_apply(SPF *s, const CHAR16 *tag)
{
#if VARIANT != 0
    int k;
#if VARIANT == 6
    int apply[6] = { 0, -1, -1, -1, -1, -1 };
#elif VARIANT == 4
    int apply[6] = { 0, -1, -1, 0, -1, -1 };
#else
    int apply[6] = { 0, 0, 0, -1, -1, -1 };
#endif
    lg(tag); lg(L": ");
    for (k = 1; k < 6; k++) {
        if (apply[k] != 0) continue;
        if (rd64(s->kid[k] + 0x30) != 0) { wr64(s->kid[k] + 0x30, 0); lg(L"clear"); lg_dec((u32)(rd32(s->kid[k] + 0x3c))); lg(L" "); }
    }
#if VARIANT != 6
    if (rd32(s->kid[5] + 0x3c) == 0x4c) { wr32(s->kid[5] + 0x3c, 0x1e); lg(L"tracer "); }
#endif
    lg(L"\r\n");
#else
    (void)s; (void)tag;
#endif
}

static u32 gCalls;
static void note_call(const CHAR16 *tag, const EFI_GUID *name, u32 type, EFI_STATUS st, u8 *buf, u32 size)
{
    u64 g0 = name ? rd64((const u8 *)name) : 0;
    u32 off; SPF s; int magic = -1;
    if (gCalls >= 90) return;
    gCalls++;
    lg(tag); lg(L" guid "); lg_hexn(g0, 16);
    lg(L" type "); lg_hexn(type, 2);
    lg(L" st "); lg_hexn(st, 8);
    lg(L" size "); lg_dec(size);
    if (st == EFI_SUCCESS && buf && size >= 0x1000) {
        for (off = 0; off < 0x40 && off + 0x1000 < size; off += 4)
            if (rd32(buf + off) == 0x46505324u) { magic = (int)off; break; }
        if (magic >= 0) {
            lg(L" magic@"); lg_hexn((u64)magic, 2);
            if (spf_prepare(&s, buf + magic)) { lg(L" VALID=1"); spf_apply(&s, L" hook"); }
            else lg(L" VALID=0");
        }
    }
    lg(L"\r\n");
    commit();
}

typedef EFI_STATUS (*RF_FN)(void *, const EFI_GUID *, u32, UINTN, void **, UINTN *, u32 *);
static EFI_STATUS hook_ReadFile(void *This, const EFI_GUID *N, u32 T, UINTN I, void **B, UINTN *SZ, u32 *A)
{
    RF_FN orig = 0; EFI_STATUS s; int i;
    for (i = 0; i < gFvN; i++) if (gFv[i].proto == This) { orig = (RF_FN)gFv[i].origRF; break; }
    if (!orig) return 0x8000000000000007ULL;
    s = orig(This, N, T, I, B, SZ, A);
    note_call(L"RF", N, T, s, (s == EFI_SUCCESS && B && *B) ? (u8 *)*B : 0, (s == EFI_SUCCESS && SZ) ? (u32)*SZ : 0);
    return s;
}
static EFI_STATUS hook_ReadSection(void *This, const EFI_GUID *N, u32 T, UINTN I, void **B, UINTN *SZ, u32 *A)
{
    RF_FN orig = 0; EFI_STATUS s; int i;
    for (i = 0; i < gFvN; i++) if (gFv[i].proto == This) { orig = (RF_FN)gFv[i].origRS; break; }
    if (!orig) return 0x8000000000000007ULL;
    s = orig(This, N, T, I, B, SZ, A);
    note_call(L"RS", N, T, s, (s == EFI_SUCCESS && B && *B) ? (u8 *)*B : 0, (s == EFI_SUCCESS && SZ) ? (u32)*SZ : 0);
    return s;
}

static int clone_self(void)
{
    EFI_STATUS (*AllocatePages)(u32, u32, UINTN, void **) = bsf(0x28);
    u32 pe, relocrva, relocsz, sizeimg;
    u8 *opt; u64 pages, sz; u8 *dst = 0; s64 delta;
    if (!gImgBase) return 0;
    pe = rd32(gImgBase + 0x3c);
    if (rd32(gImgBase + pe) != 0x00004550u) return 0;
    opt      = gImgBase + pe + 24;
    sizeimg  = rd32(opt + 56);
    relocrva = rd32(opt + 112 + 5 * 8);
    relocsz  = rd32(opt + 112 + 5 * 8 + 4);
    if (sizeimg < 0x1000 || sizeimg > 0x800000) return 0;
    sz = gImgSize > sizeimg ? gImgSize : sizeimg;
    pages = (sz + 4095) / 4096;
    if (AllocatePages(AllocateAnyPages, EfiBootServicesCode, pages, (void **)&dst) != EFI_SUCCESS || !dst) return 0;
    mem_zero(dst, pages * 4096);
    mem_copy(dst, gImgBase, gImgSize && gImgSize < sz ? gImgSize : sz);
    gClone = dst;
    delta = (s64)(dst - gImgBase);
    if (relocrva && relocsz) {
        u8 *r = dst + relocrva, *rend = r + relocsz;
        while (r + 8 <= rend) {
            u32 page = rd32(r), bsz = rd32(r + 4);
            u16 *e;
            if (bsz < 8 || r + bsz > rend) break;
            e = (u16 *)(r + 8);
            while ((u8 *)e < r + bsz) {
                u16 v = *e++;
                u8 *p = dst + page + (v & 0xFFF);
                if ((v >> 12) == 10) wr64(p, (u64)((s64)rd64(p) + delta));
            }
            r += bsz;
        }
    }
    return 1;
}

/* AMITSE identifiers */
static u8 *find_amitse(void)
{
    EFI_STATUS (*HP)(EFI_HANDLE, const EFI_GUID *, void **) = bsf(0x98);
    EFI_STATUS (*LHB)(u32, const EFI_GUID *, void *, UINTN *, EFI_HANDLE **) = bsf(0x138);
    void (*FreePool)(void *) = bsf(0x48);
    UINTN nh = 0; EFI_HANDLE *hs = 0; u8 *found = 0; u32 i, shown = 0;
    if (LHB(2, &gLiGuid, 0, &nh, &hs) != EFI_SUCCESS || !hs) return 0;
    lg(L"   images "); lg_dec((u32)nh); lg(L"\r\n");
    for (i = 0; i < nh; i++) {
        void *li = 0; u8 *base; int hdr = 0, sig = 0;
        u32 entry = 0, so = 0; u64 ib = 1;
        if (HP(hs[i], &gLiGuid, &li) != EFI_SUCCESS || !li) continue;
        base = *(u8 **)((u8 *)li + 0x40);
        if (!base) continue;
        if (rd16(base) == 0x5A4D) {
            u32 pe = rd32(base + 0x3c);
            if (rd32(base + pe) == 0x00004550u) {
                entry = rd32(base + pe + 24 + 16);
                ib    = rd64(base + pe + 24 + 24);
                so    = rd32(base + pe + 24 + 56);
                hdr   = (ib == 0 && so == 0xde680u && entry == 0x4a0u);
                sig   = (mem_eq(base + 0x2c0, gSig1, 32) && mem_eq(base + 0x4a0, gSig2, 32));
            }
        }
        if (shown < 10) {
            shown++;
            lg(L"   img "); lg_hexn((u64)base, 12);
            lg(L" entry "); lg_hexn(entry, 6);
            lg(L" ib "); lg_hexn(ib, 8);
            lg(L" soi "); lg_hexn(so, 7);
            lg(hdr ? L" HDR" : L" -");
            lg(sig ? L" SIG" : L" -");
            lg(L"\r\n");
        }
        if (!found && (hdr || sig)) found = base;
    }
    FreePool(hs);
    return found;
}

EFI_STATUS efi_main(EFI_HANDLE image, void *st)
{
    u8 *amitse;
    (void)gAnchor;
    gST = st; gImage = image;
    gBS = *(void **)((u8 *)st + 0x60);
    if (!gBS) return 1;

    open_root();
    log_init();
    lg(L"Spf3 v3  variant "); lg_dec(VARIANT); lg(L"\r\n");
    commit();

    {
        EFI_STATUS (*HP)(EFI_HANDLE, const EFI_GUID *, void **) = bsf(0x98);
        void *li = 0;
        if (HP(image, &gLiGuid, &li) == EFI_SUCCESS && li) {
            gImgBase = *(u8 **)((u8 *)li + 0x40);
            gImgSize = *(u64 *)((u8 *)li + 0x48);
        }
        lg(L"1 image "); lg_hexn((u64)gImgBase, 12); lg(L"\r\n"); commit();
        if (!gImgBase) { say(L"no image base\r\n"); goto done; }
    }
    {
        EFI_STATUS (*GetMemoryMap)(UINTN *, void *, UINTN *, UINTN *, u32 *) = bsf(0x38);
        EFI_STATUS (*AllocatePool)(u32, UINTN, void **) = bsf(0x40);
        UINTN msize = 0, mkey = 0, ds = 0; u32 dv = 0; void *map = 0;
        GetMemoryMap(&msize, 0, &mkey, &ds, &dv);
        msize += 8 * ds + 4096;
        if (AllocatePool(EfiBootServicesData, msize, &map) == EFI_SUCCESS && map &&
            GetMemoryMap(&msize, map, &mkey, &ds, &dv) == EFI_SUCCESS) {
            gMapPtr = map; gMapN = msize / ds; gMapDs = ds;
        }
    }

    /* ---- the database lives in AMITSE's saved-state stack ----
     * FUN_0002009c pushes: ctx[0] = DAT_0008ecc8, then zeroes it, and links the
     * context at DAT_0008ead8.  FUN_0002056c pops: DAT_0008ecc8 = ctx[0].
     * So while a child app runs, the live database pointer is at
     *     *(u64*)(DAT_0008ead8 + 0)
     * and setup pops it back on entry (which is why it never re-reads the FV).
     */
    amitse = find_amitse();
    lg(L"2 amitse "); lg_hexn((u64)amitse, 12); lg(L"\r\n"); commit();
    if (!amitse) { say(L"AMITSE not found\r\n"); goto done; }
    {
        u64 live = rd64(amitse + 0x8ecc8);
        u64 ctx  = rd64(amitse + 0x8ead8);
        int  n   = 0, done_any = 0;
        lg(L"   DAT_8ecc8 "); lg_hexn(live, 12);
        lg(L"  DAT_8ead8 "); lg_hexn(ctx, 12); lg(L"\r\n"); commit();

        if (live && addr_mapped(live) && addr_mapped(live + 0x1000)) {
            SPF s;
            lg(L"   live ptr -> ");
            if (rd32((u8 *)(UINTN)live) == 0x46505324u && spf_prepare(&s, (u8 *)(UINTN)live)) {
                lg(L"VALID=1"); spf_apply(&s, L" direct"); done_any = 1;
            } else lg(L"not $SPF");
            lg(L"\r\n"); commit();
        }
        while (ctx && n < 8 && addr_mapped(ctx) && addr_mapped(ctx + 0x19c)) {
            u64 db = rd64((u8 *)(UINTN)ctx);
            int ok_db = (db && addr_mapped(db) && addr_mapped(db + 0x1000));
            lg(L"   ctx ");  lg_dec((u32)n);
            lg(L" @");       lg_hexn(ctx, 12);
            lg(L" db ");     lg_hexn(db, 12);
            lg(L" next ");   lg_hexn(rd64((u8 *)(UINTN)ctx + 0x195), 12);
            if (ok_db && rd32((u8 *)(UINTN)db) == 0x46505324u) {
                SPF s;
                if (spf_prepare(&s, (u8 *)(UINTN)db)) {
                    lg(L" VALID=1"); spf_apply(&s, L" patch"); done_any = 1;
                } else lg(L" VALID=0");
            } else {
                lg(L" -");
            }
            lg(L"\r\n");
            commit();
            ctx = rd64((u8 *)(UINTN)ctx + 0x195);
            n++;
        }
        if (!done_any) lg(L"   nothing patched yet\r\n");
        commit();
        say(L"DB patched: "); say_dec((u32)done_any); say(L"\r\n");
    }

    /* ---- arm the read hooks ---- */
    {
        EFI_STATUS (*HP)(EFI_HANDLE, const EFI_GUID *, void **) = bsf(0x98);
        EFI_STATUS (*LHB)(u32, const EFI_GUID *, void *, UINTN *, EFI_HANDLE **) = bsf(0x138);
        void (*FreePool)(void *) = bsf(0x48);
        UINTN nh = 0; EFI_HANDLE *hs = 0; u32 i;
        if (LHB(2, &gFv2Guid, 0, &nh, &hs) != EFI_SUCCESS || !hs) {
            say(L"no firmware volumes\r\n"); CKPT(L"3 no fv\r\n"); goto done;
        }
        for (i = 0; i < nh && gFvN < MAXFV; i++) {
            void *fv = 0;
            if (HP(hs[i], &gFv2Guid, &fv) != EFI_SUCCESS || !fv) continue;
            gFv[gFvN].proto  = fv;
            gFv[gFvN].origRF = *(void **)((u8 *)fv + 0x18);
            gFv[gFvN].origRS = *(void **)((u8 *)fv + 0x20);
            gFvN++;
        }
        FreePool(hs);
        lg(L"3 fvs "); lg_dec((u32)gFvN); lg(L"\r\n"); commit();
        if (!gFvN) { say(L"no FV protocol\r\n"); goto done; }
        if (!clone_self()) { say(L"resident copy failed\r\n"); CKPT(L"4 clone FAILED\r\n"); goto done; }
        lg(L"4 clone "); lg_hexn((u64)gClone, 12); lg(L"\r\n"); commit();
        {
            u64 off1 = (u64)((u8 *)&hook_ReadFile - gImgBase);
            u64 off2 = (u64)((u8 *)&hook_ReadSection - gImgBase);
            void *h1 = (void *)(gClone + off1);
            void *h2 = (void *)(gClone + off2);
            int ok = 0;
            for (i = 0; i < (u32)gFvN; i++) {
                u8 *p1 = (u8 *)gFv[i].proto + 0x18;
                u8 *p2 = (u8 *)gFv[i].proto + 0x20;
                if (!page_writable((u64)gFv[i].proto)) continue;
                wr64(p1, (u64)h1);
                wr64(p2, (u64)h2);
                if (rd64(p1) == (u64)h1 && rd64(p2) == (u64)h2) ok++;
            }
            lg(L"5 hooked "); lg_dec((u32)ok); lg(L" of "); lg_dec((u32)gFvN); lg(L"\r\n"); commit();
            say(L"Armed "); say_dec((u32)ok); say(L" hooks\r\n");
            if (!ok) { say(L"could not hook\r\n"); goto done; }
        }
    }

    /* ---- start setup ---- */
    {
        EFI_STATUS (*Install)(EFI_HANDLE *, const EFI_GUID *, u32, void *) = bsf(0x80);
        EFI_STATUS (*Uninstall)(EFI_HANDLE, const EFI_GUID *, void *) = bsf(0x90);
        EFI_HANDLE h = 0;
        EFI_STATUS s;
        say(L"Starting setup...\r\n");
        CKPT(L"6 installing trigger\r\n");
        s = Install(&h, &gTrigGuid, 0, 0);
        lg(L"6 trigger "); lg_hexn(s, 16); lg(L"\r\n"); commit();
        if (s == EFI_SUCCESS && h) { Uninstall(h, &gTrigGuid, 0); CKPT(L"6 uninstalled\r\n"); }
    }

done:
    CKPT(L"done\r\n");
    stall_ms(1000);
    return 0;
}
