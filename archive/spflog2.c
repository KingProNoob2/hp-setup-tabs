/* SpfLog2.efi  v2  -- checkpointed diagnostic + patch + setup trigger.
 *
 * Writes \SPFHOOK.LOG after every step, so whatever line the file ends on tells
 * us exactly where it stopped.  Console output is limited to three short lines.
 *
 * Steps: image base -> memory map -> firmware volumes -> resident copy ->
 * hook ReadFile/ReadSection -> per-volume "does it carry AMITSESetupData?" ->
 * install the setup trigger protocol 451EAC81-0D7D-4B4D-A3D0-1CB3FE1D6666.
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
static const EFI_GUID gSdGuid  = { 0xFE612B72,0x203C,0x47B1,{0x85,0x60,0xA6,0x6D,0x94,0x6E,0xB3,0x71} };
static const EFI_GUID gTrigGuid= { 0x451EAC81,0x0D7D,0x4B4D,{0xA3,0xD0,0x1C,0xB3,0xFE,0x1D,0x66,0x66} };
static const u8 gStoreGuid[16] = { 0x43,0xD6,0x87,0xEC,0xA4,0xEB,0xB5,0x4B,0xA1,0xE5,0x3F,0x3E,0x36,0xB2,0x0D,0xA9 };

typedef struct { u32 Type; u32 Pad; u64 PhysicalStart; u64 VirtualStart; u64 NumberOfPages; u64 Attribute; } MEM_DESC;
#define MAXFV 32
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
static CHAR16     gLog[8000];
static UINTN      gLogLen;

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

static void lg(const CHAR16 *s)
{
    UINTN i = 0;
    while (s[i] && gLogLen + 2 < sizeof(gLog) / sizeof(CHAR16)) gLog[gLogLen++] = s[i++];
    gLog[gLogLen] = 0;
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

/* rewrite \SPFHOOK.LOG */
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
        s = FOpen(gRoot, &file, L"\\SPFHOOK.LOG", 3, 0);
        if (s == EFI_SUCCESS && file) { FDelete(file); file = 0; }
        s = FOpen(gRoot, &file, L"\\SPFHOOK.LOG", 3 | 0x8000000000000000ULL, 0);
        if (s != EFI_SUCCESS || !file) return;
        n = gLogLen * sizeof(CHAR16);
        FWrite(file, &n, gLog);
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

static void spf_try_patch(u8 *buf, u32 size, const CHAR16 *tag)
{
    u32 off;
    SPF s;
    if (!buf || size < 0x1000) return;
    for (off = 0; off < 0x40 && off + 0x1000 < size; off += 4) {
        if (rd32(buf + off) != 0x46505324u) continue;
        lg(tag); lg(L" read size="); lg_dec(size); lg(L" magic@"); lg_hexn(off, 2);
        if (!spf_prepare(&s, buf + off)) { lg(L" VALID=0\r\n"); commit(); return; }
        lg(L" VALID=1");
#if VARIANT != 0
        {
            int k;
#if VARIANT == 6
            int apply[6] = { 0, -1, -1, -1, -1, -1 };
#else
            int apply[6] = { 0, 0, 0, -1, -1, -1 };
#endif
            for (k = 1; k < 6; k++) {
                if (apply[k] != 0) continue;
                if (rd64(s.kid[k] + 0x30) != 0) wr64(s.kid[k] + 0x30, 0);
            }
#if VARIANT != 6
            if (rd32(s.kid[5] + 0x3c) == 0x4c) wr32(s.kid[5] + 0x3c, 0x1e);
#endif
        }
        lg(L" PATCHED");
#endif
        lg(L"\r\n");
        commit();
        return;
    }
    lg(tag); lg(L" read size="); lg_dec(size); lg(L" no magic\r\n");
    commit();
}

typedef EFI_STATUS (*RF_FN)(void *, const EFI_GUID *, u32, UINTN, void **, UINTN *, u32 *);

static EFI_STATUS hook_ReadFile(void *This, const EFI_GUID *Name, u32 T, UINTN I, void **B, UINTN *SZ, u32 *A)
{
    RF_FN orig = 0; EFI_STATUS s; int i;
    for (i = 0; i < gFvN; i++) if (gFv[i].proto == This) { orig = (RF_FN)gFv[i].origRF; break; }
    if (!orig) return 0x8000000000000007ULL;
    s = orig(This, Name, T, I, B, SZ, A);
    if (s == EFI_SUCCESS && B && *B && SZ) spf_try_patch((u8 *)*B, (u32)*SZ, L"ReadFile");
    return s;
}
static EFI_STATUS hook_ReadSection(void *This, const EFI_GUID *Name, u32 T, UINTN I, void **B, UINTN *SZ, u32 *A)
{
    RF_FN orig = 0; EFI_STATUS s; int i;
    for (i = 0; i < gFvN; i++) if (gFv[i].proto == This) { orig = (RF_FN)gFv[i].origRS; break; }
    if (!orig) return 0x8000000000000007ULL;
    s = orig(This, Name, T, I, B, SZ, A);
    if (s == EFI_SUCCESS && B && *B && SZ) spf_try_patch((u8 *)*B, (u32)*SZ, L"ReadSection");
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

EFI_STATUS efi_main(EFI_HANDLE image, void *st)
{
    (void)gAnchor;
    gST = st; gImage = image;
    gBS = *(void **)((u8 *)st + 0x60);
    if (!gBS) return 1;

    open_root();
    lg(L"SpfLog2 v2  variant "); lg_dec(VARIANT); lg(L"\r\n");
    commit();

    /* 1. own image */
    {
        EFI_STATUS (*HP)(EFI_HANDLE, const EFI_GUID *, void **) = bsf(0x98);
        void *li = 0;
        if (HP(image, &gLiGuid, &li) == EFI_SUCCESS && li) {
            gImgBase = *(u8 **)((u8 *)li + 0x40);
            gImgSize = *(u64 *)((u8 *)li + 0x48);
        }
        lg(L"1 image "); lg_hexn((u64)gImgBase, 12); lg(L" size "); lg_dec((u32)gImgSize);
        lg(L"\r\n"); commit();
        if (!gImgBase) { say(L"no image base\r\n"); goto done; }
    }

    /* 2. memory map (only to skip read-only protocol pages) */
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
        lg(L"2 memmap entries "); lg_dec((u32)gMapN); lg(L"\r\n"); commit();
    }

    /* 3. firmware volumes */
    {
        EFI_STATUS (*HP)(EFI_HANDLE, const EFI_GUID *, void **) = bsf(0x98);
        EFI_STATUS (*LHB)(u32, const EFI_GUID *, void *, UINTN *, EFI_HANDLE **) = bsf(0x138);
        void (*FreePool)(void *) = bsf(0x48);
        UINTN nh = 0; EFI_HANDLE *hs = 0; u32 i;
        if (LHB(2, &gFv2Guid, 0, &nh, &hs) != EFI_SUCCESS || !hs) {
            say(L"no firmware volumes\r\n"); CKPT(L"3 no fv handles\r\n"); goto done;
        }
        lg(L"3 fv handles "); lg_dec((u32)nh); lg(L"\r\n"); commit();
        for (i = 0; i < nh && gFvN < MAXFV; i++) {
            void *fv = 0;
            if (HP(hs[i], &gFv2Guid, &fv) != EFI_SUCCESS || !fv) continue;
            gFv[gFvN].proto  = fv;
            gFv[gFvN].origRF = *(void **)((u8 *)fv + 0x18);
            gFv[gFvN].origRS = *(void **)((u8 *)fv + 0x20);
            lg(L"   fv "); lg_dec((u32)gFvN); lg(L" @"); lg_hexn((u64)fv, 10);
            lg(L" rf "); lg_hexn((u64)gFv[gFvN].origRF, 10);
            lg(L" rs "); lg_hexn((u64)gFv[gFvN].origRS, 10);
            lg(L"\r\n");
            gFvN++;
            commit();
        }
        FreePool(hs);
        if (!gFvN) { say(L"no FV protocol\r\n"); CKPT(L"3 no fv proto\r\n"); goto done; }
    }

    /* 4. resident copy */
    if (!clone_self()) { say(L"resident copy failed\r\n"); CKPT(L"4 clone FAILED\r\n"); goto done; }
    lg(L"4 clone @"); lg_hexn((u64)gClone, 10); lg(L"\r\n"); commit();

    /* 5. hooks */
    {
        u64 off1 = (u64)((u8 *)&hook_ReadFile - gImgBase);
        u64 off2 = (u64)((u8 *)&hook_ReadSection - gImgBase);
        void *h1 = (void *)(gClone + off1);
        void *h2 = (void *)(gClone + off2);
        int ok = 0, i;
        for (i = 0; i < gFvN; i++) {
            u8 *p1 = (u8 *)gFv[i].proto + 0x18;
            u8 *p2 = (u8 *)gFv[i].proto + 0x20;
            if (!page_writable((u64)gFv[i].proto)) {
                lg(L"   fv "); lg_dec((u32)i); lg(L" page RO - skipped\r\n");
                continue;
            }
            wr64(p1, (u64)h1);
            wr64(p2, (u64)h2);
            if (rd64(p1) == (u64)h1 && rd64(p2) == (u64)h2) ok++;
        }
        lg(L"5 hooked "); lg_dec((u32)ok); lg(L" of "); lg_dec((u32)gFvN); lg(L"\r\n"); commit();
        say(L"Shim armed: FVs "); say_dec((u32)gFvN); say(L" hooked "); say_dec((u32)ok); say(L"\r\n");
        if (!ok) { say(L"could not hook\r\n"); goto done; }
    }

    /* 6. ask each volume for AMITSESetupData ourselves.  This both finds the
     *    volume and exercises the hook (our call goes through it). */
    {
        int i;
        for (i = 0; i < gFvN; i++) {
            void *fv = gFv[i].proto;
            /* go through the protocol so our hook runs (and patches) */
            RF_FN ReadFile = (RF_FN)(*(void **)((u8 *)fv + 0x18));
            void *buf = 0; UINTN sz = 0; u32 auth = 0;
            EFI_STATUS s;
            lg(L"6 fv "); lg_dec((u32)i);
            if (!ReadFile) { lg(L" no ReadFile\r\n"); commit(); continue; }
            s = ReadFile(fv, &gSdGuid, 0x18, 0, &buf, &sz, &auth);
            lg(L" read status "); lg_hexn(s, 16);
            lg(L" size "); lg_dec((u32)sz);
            if (s == EFI_SUCCESS && buf) {
                lg(L" HAS-SETUP-DATA b0-3 ");
                { int k; for (k = 0; k < 4; k++) lg_hexn(((u8 *)buf)[k], 2); }
                { void (*FreePool)(void *) = bsf(0x48); FreePool(buf); }
            }
            lg(L"\r\n");
            commit();
        }
    }

    /* 7. enter setup ourselves */
    {
        EFI_STATUS (*Install)(EFI_HANDLE *, const EFI_GUID *, u32, void *) = bsf(0x80);
        EFI_STATUS (*Uninstall)(EFI_HANDLE, const EFI_GUID *, void *) = bsf(0x90);
        EFI_HANDLE h = 0;
        EFI_STATUS s;
        say(L"Entering setup...\r\n");
        CKPT(L"7 installing trigger\r\n");
        stall_ms(600);
        s = Install(&h, &gTrigGuid, 0, 0);
        lg(L"7 trigger status "); lg_hexn(s, 16); lg(L" handle "); lg_hexn((u64)h, 10); lg(L"\r\n"); commit();
        say(L"trigger -> "); say_dec((u32)(s == 0 ? 1 : 0)); say(L"\r\n");
        if (s == EFI_SUCCESS && h) {
            Uninstall(h, &gTrigGuid, 0);
            CKPT(L"7 uninstalled\r\n");
        }
    }

done:
    CKPT(L"done\r\n");
    stall_ms(1200);
    return 0;
}
