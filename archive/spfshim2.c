/* SpfShim2.efi  v2
 *
 * Why this exists: exiting setup reboots the machine, so the decompressed
 * $SPF database only ever exists *inside* a setup session and can never be
 * patched by an app running from the boot manager.  The only place to catch it
 * is the moment it is produced: AMITSE reads it with
 *     FV->ReadFile(Fv, &FE612B72-..., EFI_SECTION_FREEFORM_SUBTYPE_GUID, ...)
 * and DXE decompresses the LZMA section into a pool buffer, which AMITSE then
 * builds the UI from.
 *
 * So: arm a hook on EFI_FIRMWARE_VOLUME2_PROTOCOL->ReadFile (+0x18) for every
 * firmware volume, then return.  Press F10; the hook patches the buffer before
 * AMITSE sees it.
 *
 * The hook has to survive this app's unload, so the whole image is copied
 * (and relocated) into AllocatePages(EfiBootServicesCode) memory and the
 * protocol pointers are pointed there.
 *
 * Patch (variant 5, default): clear item+0x30 (condition ref) on the Advanced
 * and Chipset tab records of form 0x2710, and rename the Save&Exit label
 * 0x4C -> 0x1E as a tracer.   Variant 6 = tracer only.  Variant 0 = hook that
 * validates but writes nothing.
 *
 * No file I/O and at most three short lines on screen.
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
static const u8 gStoreGuid[16] = { 0x43,0xD6,0x87,0xEC,0xA4,0xEB,0xB5,0x4B,0xA1,0xE5,0x3F,0x3E,0x36,0xB2,0x0D,0xA9 };

typedef struct { u32 Type; u32 Pad; u64 PhysicalStart; u64 VirtualStart; u64 NumberOfPages; u64 Attribute; } MEM_DESC;

#define MAXFV 48
typedef struct { void *proto; void *orig; } FVENT;

static void      *gST, *gBS;
static EFI_HANDLE gImage;
static u8        *gImgBase;
static u64        gImgSize;
static u8        *gClone;
static FVENT      gFv[MAXFV];
static int        gFvN;
static u64        gDbHits;

/* startup-only */
static MEM_DESC  *gMapPtr;
static u64        gMapN, gMapDs;

EFI_STATUS efi_main(EFI_HANDLE image, void *st);
static void *const gAnchor __attribute__((used)) = (void *)&efi_main;

/* ---- helpers ------------------------------------------------------------ */
static void mem_copy(void *d, const void *s, UINTN n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; }
static void mem_zero(void *d, UINTN n) { u8 *a = d; while (n--) *a++ = 0; }
static int mem_eq(const u8 *a, const u8 *b, u32 n) { u32 i; for (i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }
static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u64 rd64(const u8 *p) { return (u64)rd32(p) | ((u64)rd32(p + 4) << 32); }
static void wr32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static void wr64(u8 *p, u64 v) { wr32(p, (u32)v); wr32(p + 4, (u32)(v >> 32)); }

static void say(const CHAR16 *s)
{
    void *co;
    if (!gST) return;
    co = *(void **)((u8 *)gST + 0x40);
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

static void stall_ms(u32 ms)
{
    EFI_STATUS (*Stall)(UINTN) = *(void **)((u8 *)gBS + 0xF8);
    if (Stall) Stall((UINTN)ms * 1000);
}

static int page_writable(u64 addr)
{
    u64 i;
    for (i = 0; i < gMapN; i++) {
        MEM_DESC *md = (MEM_DESC *)((u8 *)gMapPtr + i * gMapDs);
        u64 lo = md->PhysicalStart;
        u64 hi = lo + md->NumberOfPages * 4096;
        if (addr >= lo && addr < hi) return (md->Attribute & EFI_MEMORY_RO) ? 0 : 1;
    }
    return 0;
}

/* ---- $SPF patch engine -------------------------------------------------- */
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

/* called by the hook on every ReadFile result */
static void spf_try_patch(u8 *buf, u32 size)
{
    u32 off;
    SPF s;
    if (!buf || size < 0x4000) return;
    for (off = 0; off < 0x40 && off + 0x1000 < size; off += 4) {
        if (rd32(buf + off) != 0x46505324u) continue;
        if (!spf_prepare(&s, buf + off)) continue;
        gDbHits++;
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
#endif
        return;
    }
}

typedef EFI_STATUS (*READFILE_FN)(void *, const EFI_GUID *, u32, UINTN, void **, UINTN *, u32 *);

/* runs from the resident clone */
static EFI_STATUS hook_ReadFile(void *This, const EFI_GUID *Name, u32 SecType, UINTN SecInst,
                                void **Buf, UINTN *BufSize, u32 *Auth)
{
    READFILE_FN orig = 0;
    EFI_STATUS s;
    int i;
    for (i = 0; i < gFvN; i++)
        if (gFv[i].proto == This) { orig = (READFILE_FN)gFv[i].orig; break; }
    if (!orig) return 0x8000000000000007ULL;
    s = orig(This, Name, SecType, SecInst, Buf, BufSize, Auth);
    if (s == EFI_SUCCESS && Buf && *Buf && BufSize) spf_try_patch((u8 *)*Buf, (u32)*BufSize);
    return s;
}

/* ---- clone + relocate self ---------------------------------------------- */
static int clone_self(void)
{
    EFI_STATUS (*AllocatePages)(u32, u32, UINTN, void **) = *(void **)((u8 *)gBS + 0x28);
    u32 pe, relocrva, relocsz, sizeimg;
    u8 *opt;
    u64 pages, sz;
    u8 *dst = 0;
    s64 delta;

    if (!gImgBase) return 0;
    pe = rd32(gImgBase + 0x3c);
    if (rd32(gImgBase + pe) != 0x00004550u) return 0;
    opt      = gImgBase + pe + 24;
    sizeimg  = rd32(opt + 56);
    relocrva = rd32(opt + 112 + 5 * 8);
    relocsz  = rd32(opt + 112 + 5 * 8 + 4);
    if (sizeimg < 0x1000 || sizeimg > 0x800000) return 0;

    sz = sizeimg;
    if (gImgSize > sz) sz = gImgSize;
    pages = (sz + 4095) / 4096;
    if (AllocatePages(AllocateAnyPages, EfiBootServicesCode, pages, (void **)&dst) != EFI_SUCCESS || !dst)
        return 0;
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
                u32 type = v >> 12, o = v & 0xFFF;
                u8 *p = dst + page + o;
                if (type == 10) wr64(p, (u64)((s64)rd64(p) + delta));
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

    {
        EFI_STATUS (*HandleProtocol)(EFI_HANDLE, const EFI_GUID *, void **) = *(void **)((u8 *)gBS + 0x98);
        EFI_STATUS (*LocateHandleBuffer)(u32, const EFI_GUID *, void *, UINTN *, EFI_HANDLE **) = *(void **)((u8 *)gBS + 0x138);
        EFI_STATUS (*GetMemoryMap)(UINTN *, void *, UINTN *, UINTN *, u32 *) = *(void **)((u8 *)gBS + 0x38);
        EFI_STATUS (*AllocatePool)(u32, UINTN, void **) = *(void **)((u8 *)gBS + 0x40);
        void (*FreePool)(void *) = *(void **)((u8 *)gBS + 0x48);
        UINTN nh = 0;
        EFI_HANDLE *hs = 0;
        void *map = 0;
        u32 i;

        /* our own image */
        {
            EFI_STATUS (*HP)(EFI_HANDLE, const EFI_GUID *, void **) = HandleProtocol;
            static const EFI_GUID liGuid = { 0x5B1B31A1,0x9562,0x11D2,{0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B} };
            void *li = 0;
            if (HP(image, &liGuid, &li) == EFI_SUCCESS && li) {
                gImgBase = *(u8 **)((u8 *)li + 0x40);
                gImgSize = *(u64 *)((u8 *)li + 0x48);
            }
        }
        if (!gImgBase) { say(L"Shim: no image base\r\n"); goto done; }

        /* memory map, only to avoid writing into a read-only page */
        {
            UINTN msize = 0, mkey = 0, ds = 0; u32 dv = 0;
            GetMemoryMap(&msize, 0, &mkey, &ds, &dv);
            msize += 8 * ds + 4096;
            if (AllocatePool(EfiBootServicesData, msize, &map) == EFI_SUCCESS && map &&
                GetMemoryMap(&msize, map, &mkey, &ds, &dv) == EFI_SUCCESS) {
                gMapPtr = map; gMapN = msize / ds; gMapDs = ds;
            }
        }

        if (LocateHandleBuffer(2, &gFv2Guid, 0, &nh, &hs) != EFI_SUCCESS || !hs) {
            say(L"Shim: no firmware volumes\r\n");
            goto done;
        }
        for (i = 0; i < nh && gFvN < MAXFV; i++) {
            void *fv = 0;
            if (HandleProtocol(hs[i], &gFv2Guid, &fv) != EFI_SUCCESS || !fv) continue;
            gFv[gFvN].proto = fv;
            gFv[gFvN].orig  = *(void **)((u8 *)fv + 0x18);
            gFvN++;
        }
        FreePool(hs);
        if (!gFvN) { say(L"Shim: no FV protocol\r\n"); goto done; }

        if (!clone_self()) { say(L"Shim: resident copy failed\r\n"); goto done; }

        {
            u64 off = (u64)((u8 *)&hook_ReadFile - gImgBase);
            void *hook = (void *)(gClone + off);
            int ok = 0;
            for (i = 0; i < gFvN; i++) {
                u8 *p = (u8 *)gFv[i].proto + 0x18;
                if (!page_writable((u64)gFv[i].proto)) continue;
                wr64(p, (u64)hook);
                if (rd64(p) == (u64)hook) ok++;
            }
            if (gMapPtr) FreePool(gMapPtr);
            gMapPtr = 0; gMapN = 0;
            say(L"Shim armed: FVs "); say_dec((u32)gFvN);
            say(L" hooked "); say_dec((u32)ok);
            say(L"\r\n");
            if (!ok) { say(L"Shim: could not hook (read-only?)\r\n"); goto done; }
        }
    }
    say(L"Press F10 now (variant "); say_dec(VARIANT); say(L").\r\n");
done:
    stall_ms(4000);
    return 0;
}
