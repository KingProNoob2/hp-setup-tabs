/* SpfUnlock.efi  v0.1
 *
 * Finds the compiled AMI setup database ("$SPF" in FV file AMITSESetupData,
 * GUID FE612B72-203C-47B1-8560-A66D946EB371) as it lives in RAM, and clears the
 * per-item "condition" pointer (item +0x30, qword) of the hidden Advanced /
 * Chipset tab refs in the top-level form 0x2710.
 *
 * Rationale (see ../RESEARCH_BRIEF.md):
 *   - $SPF is 1:1 with the Setup module IFR (207 forms == 207 kind-1 records).
 *   - In form 0x2710 the six tab refs are structurally identical except:
 *         Advanced  (prompt 0x1e) : item+0x30 = -4
 *         Chipset   (prompt 0x1f) : item+0x30 = -4
 *         Security  (prompt 0x39) : item+0x30 = -4
 *         Main      (prompt 0x09) : item+0x30 =  0
 *         Boot      (prompt 0x20) : item+0x30 =  0
 *         Save&Exit (prompt 0x4c) : item+0x30 =  0
 *     The same holds for every other item that the IFR wraps in
 *     "Suppress If {True}" (e.g. the two Access Level texts in form 0x2711 have
 *     item+0x30 = -8, all other texts 0).  So item+0x30 is the condition /
 *     expression reference and 0 means "no condition".
 *
 * Variants (build with -DVARIANT=n):
 *   1  clear item+0x30 of Advanced+Chipset                        (minimal)
 *   2  same, but copy Main's (item+0x28, item+0x30) into them      (clone)
 *   3  variant 1 + also Security
 *
 * In every variant a TRACER is applied: the visible Boot tab's prompt string id
 * (item+0x3c) is changed 0x20 -> 0x1e.  If the setup UI then shows the Boot tab
 * labelled "Advanced", the write really reaches the renderer; if nothing
 * changes, the $SPF copy we patch is not the one the UI uses.
 *
 * Build:
 *   x86_64-w64-mingw32-gcc -O2 -nostdlib -ffreestanding -fno-stack-protector \
 *     -fno-asynchronous-unwind-tables -Wl,--subsystem,10 -Wl,-e,efi_main \
 *     -Wl,--enable-reloc-section -Wl,--no-insert-timestamp \
 *     -DVARIANT=1 -o SpfUnlock1.efi spfunlock.c
 */
typedef unsigned long long u64;
typedef unsigned int       u32;
typedef unsigned short     u16;
typedef unsigned char      u8;
typedef u64                UINTN;
typedef u64                EFI_STATUS;
typedef void              *EFI_HANDLE;
typedef u16                CHAR16;

#define EFI_SUCCESS          0ULL
#define EFI_BUFFER_TOO_SMALL 0x8000000000000005ULL
#define EfiBootServicesData  4
#define FILE_MODE_READ       0x0000000000000001ULL
#define FILE_MODE_WRITE      0x0000000000000002ULL
#define FILE_MODE_CREATE     0x8000000000000000ULL

#define EFI_MEMORY_RO        0x0000000000020000ULL

#ifndef VARIANT
#define VARIANT 1
#endif

typedef struct { u32 d1; u16 d2; u16 d3; u8 d4[8]; } EFI_GUID;

static const EFI_GUID gLoadedImageGuid = { 0x5B1B31A1,0x9562,0x11D2,{0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B} };
static const EFI_GUID gSimpleFsGuid    = { 0x964E5B22,0x6459,0x11D2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B} };

/* Setup varstore GUID that sits at $SPF+0x14 */
static const u8 gStoreGuid[16] = { 0x43,0xD6,0x87,0xEC,0xA4,0xEB,0xB5,0x4B,0xA1,0xE5,0x3F,0x3E,0x36,0xB2,0x0D,0xA9 };

typedef struct {
    u32 Type;
    u32 Pad;
    u64 PhysicalStart;
    u64 VirtualStart;
    u64 NumberOfPages;
    u64 Attribute;
} MEM_DESC;

EFI_STATUS efi_main(EFI_HANDLE image, void *st);
static void *const gAnchor __attribute__((used)) = (void *)&efi_main;

static void      *gST, *gBS;
static EFI_HANDLE gImage;
static CHAR16     gLog[60000];
static UINTN      gLogLen;

/* ---- output ------------------------------------------------------------- */
static void *con_out(void) { return *(void **)((u8 *)gST + 0x40); }
static void *con_in(void)  { return *(void **)((u8 *)gST + 0x30); }
static void *bs_fn(u32 o)  { return *(void **)((u8 *)gBS + o); }

static void app(const CHAR16 *s)
{
    void *co = con_out();
    UINTN i = 0;
    if (co) {
        EFI_STATUS (*OutputString)(void *, const CHAR16 *) = *(void **)((u8 *)co + 8);
        OutputString(co, s);
    }
    while (s[i] && gLogLen + 2 < sizeof(gLog) / sizeof(CHAR16)) gLog[gLogLen++] = s[i++];
    gLog[gLogLen] = 0;
}

static void app_hex(u64 v)
{
    CHAR16 b[19]; int i;
    b[0] = '0'; b[1] = 'x';
    for (i = 0; i < 16; i++) {
        u32 n = (u32)((v >> (60 - 4 * i)) & 0xF);
        b[2 + i] = (CHAR16)(n < 10 ? '0' + n : 'A' + n - 10);
    }
    b[18] = 0;
    app(b);
}

static void app_hexn(u64 v, int digits)
{
    CHAR16 b[19]; int i;
    b[0] = '0'; b[1] = 'x';
    for (i = 0; i < digits; i++) {
        u32 n = (u32)((v >> (4 * (digits - 1 - i))) & 0xF);
        b[2 + i] = (CHAR16)(n < 10 ? '0' + n : 'A' + n - 10);
    }
    b[2 + digits] = 0;
    app(b);
}

static void app_dec(u32 v)
{
    CHAR16 b[11]; int i = 10;
    b[10] = 0;
    do { b[--i] = (CHAR16)('0' + v % 10); v /= 10; } while (v && i > 0);
    app(b + i);
}

static void write_log(void)
{
    void *li = 0, *sfs = 0, *root = 0, *file = 0;
    EFI_STATUS (*HandleProtocol)(EFI_HANDLE, const EFI_GUID *, void **) = bs_fn(0x98);
    if (!gImage) { app(L"no image handle\r\n"); return; }
    if (HandleProtocol(gImage, &gLoadedImageGuid, &li) != EFI_SUCCESS || !li) {
        app(L"no LoadedImage\r\n"); return;
    }
    EFI_HANDLE dev = *(EFI_HANDLE *)((u8 *)li + 0x18);
    if (!dev || HandleProtocol(dev, &gSimpleFsGuid, &sfs) != EFI_SUCCESS || !sfs) {
        app(L"no SimpleFileSystem\r\n"); return;
    }
    EFI_STATUS (*OpenVolume)(void *, void **) = *(void **)((u8 *)sfs + 8);
    if (OpenVolume(sfs, &root) != EFI_SUCCESS || !root) { app(L"OpenVolume failed\r\n"); return; }

    void **fv = (void **)root;
    EFI_STATUS (*FOpen)(void *, void **, const CHAR16 *, u64, u64) = fv[1];
    EFI_STATUS (*FClose)(void *)   = fv[2];
    EFI_STATUS (*FDelete)(void *)  = fv[3];
    EFI_STATUS (*FWrite)(void *, UINTN *, void *) = fv[5];

    EFI_STATUS s = FOpen(root, &file, L"\\SPFUNLOCK.LOG", FILE_MODE_READ | FILE_MODE_WRITE, 0);
    if (s == EFI_SUCCESS && file) { FDelete(file); file = 0; }
    s = FOpen(root, &file, L"\\SPFUNLOCK.LOG", FILE_MODE_READ | FILE_MODE_WRITE | FILE_MODE_CREATE, 0);
    if (s != EFI_SUCCESS || !file) {
        app(L"cannot create \\SPFUNLOCK.LOG\r\n");
        return;
    }
    UINTN n = gLogLen * sizeof(CHAR16);
    FWrite(file, &n, gLog);
    FClose(file);
    if (root) FClose(root);
}

/* ---- little helpers ----------------------------------------------------- */
static int mem_eq(const u8 *a, const u8 *b, u32 n)
{ u32 i; for (i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }

static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u64 rd64(const u8 *p) { return (u64)rd32(p) | ((u64)rd32(p + 4) << 32); }
static void wr32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static void wr64(u8 *p, u64 v) { wr32(p, (u32)v); wr32(p + 4, (u32)(v >> 32)); }

/* ---- $SPF handling ------------------------------------------------------ */
#define SPF_MAX_ITEMS 8

typedef struct {
    u8 *base;
    u8 *root;       /* form 0x2710 record      */
    u8 *itemtab;    /* base + u32(+0x30)       */
    int nkids;
    u8 *kid[SPF_MAX_ITEMS];
} SPF;

static int spf_validate(u8 *base, u32 *formcount_out)
{
    u32 forms_off, item_off, cnt, i, prev;
    if (!base) return 0;
    if (rd32(base) != 0x46505324u) return 0;                 /* "$SPF" */
    if (!mem_eq(base + 0x14, gStoreGuid, 16)) return 0;
    forms_off = rd32(base + 0x2c);
    item_off  = rd32(base + 0x30);
    if (forms_off < 0x40 || forms_off > 0x800000) return 0;
    if (item_off  < 0x40 || item_off  > 0x800000) return 0;
    cnt = rd32(base + forms_off);
    if (cnt < 2 || cnt > 4096) return 0;
    /* the item table follows the form table, and the first form record starts
     * right after the u32 offset array (this build: 0x60+4+4*220 == 0x3D4,
     * first record at 0x3D8, i.e. one spare u32) */
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
    *formcount_out = cnt;
    return 1;
}

static int spf_find_root(SPF *s)
{
    u32 cnt = 0, i;
    u32 forms_off, item_off;
    if (!spf_validate(s->base, &cnt)) return 0;
    forms_off = rd32(s->base + 0x2c);
    item_off  = rd32(s->base + 0x30);
    s->itemtab = s->base + item_off;
    s->root = 0;
    for (i = 0; i < cnt; i++) {
        u8 *rec = s->base + rd32(s->base + forms_off + 4 + 4 * i);
        if (rd16(rec + 8) != 1) continue;            /* kind 1 = form */
        if (rd16(rec + 0x0a) != 0x2710) continue;    /* top-level form */
        s->root = rec;
        break;
    }
    if (!s->root) return 0;
    {
        u32 n = rd32(s->root + 0x1c);
        if (n < 1 || n > SPF_MAX_ITEMS) return 0;
        s->nkids = (int)n;
        for (i = 0; i < n; i++) s->kid[i] = s->itemtab + rd32(s->root + 0x20 + 4 * i);
    }
    return 1;
}

static u32 item_prompt(const u8 *it) { return rd32(it + 0x3c); }
static u16 item_op     (const u8 *it) { return rd16(it + 0x14); }
static u32 item_qid    (const u8 *it) { return rd32(it + 0x0c); }

/* expected tab order of form 0x2710 (prompt string ids) */
static const u32 g_tab_prompt[6] = { 0x09, 0x1e, 0x1f, 0x39, 0x20, 0x4c };

static int spf_check_tabs(SPF *s)
{
    int i;
    if (s->nkids != 6) return 0;
    for (i = 0; i < 6; i++) {
        if (item_prompt(s->kid[i]) != g_tab_prompt[i]) return 0;
        if (item_op(s->kid[i]) != 1) return 0;        /* EFI ref item */
    }
    return 1;
}

static void log_tabs(SPF *s)
{
    int i;
    app(L"  tabs:\r\n");
    for (i = 0; i < s->nkids; i++) {
        u8 *it = s->kid[i];
        app(L"   ["); app_dec((u32)i); app(L"] prompt="); app_hexn(item_prompt(it), 4);
        app(L" qid="); app_dec(item_qid(it));
        app(L" op="); app_hexn(item_op(it), 2);
        app(L" f28="); app_hexn(rd32(it + 0x28), 6);
        app(L" f30="); app_hex(rd64(it + 0x30));
        app(L"\r\n");
    }
}

static void dump_item_hex(SPF *s, int idx)
{
    static const CHAR16 hx[] = L"0123456789ABCDEF";
    u8 *it = s->kid[idx];
    CHAR16 line[3 * 16 + 4];
    int row, k;
    for (row = 0; row < 4; row++) {
        app(L"    [");
        app_dec((u32)idx); app(L"] +");
        app_hexn((u64)(row * 16), 2);
        app(L": ");
        for (k = 0; k < 16; k++) {
            u8 b = it[row * 16 + k];
            line[0] = hx[b >> 4];
            line[1] = hx[b & 0xF];
            line[2] = ' ';
            line[3] = 0;
            app(line);
        }
        app(L"\r\n");
    }
}

EFI_STATUS efi_main(EFI_HANDLE image, void *st)
{
    (void)gAnchor;
    gST = st; gImage = image;
    gBS = *(void **)((u8 *)st + 0x60);
    gLog[0] = 0; gLogLen = 0;

    app(L"SpfUnlock v0.2  variant "); app_dec(VARIANT); app(L"\r\n");
#if VARIANT == 1
    app(L"  plan: Advanced clear f30, Chipset clone Main data, tracer\r\n");
#elif VARIANT == 2
    app(L"  plan: clear f30 on Advanced+Chipset+Security, tracer\r\n");
#elif VARIANT == 3
    app(L"  plan: clone Main data into Advanced+Chipset, tracer\r\n");
#elif VARIANT == 4
    app(L"  plan: canary - clear f30 on SECURITY only, tracer\r\n");
#elif VARIANT == 5
    app(L"  plan: clear f30 on Advanced+Chipset only, tracer\r\n");
#else
    app(L"  plan: tracer only (no unlock write)\r\n");
#endif
    if (!gBS) { app(L"no BootServices\r\n"); return 1; }

    EFI_STATUS (*GetMemoryMap)(UINTN *, void *, UINTN *, UINTN *, u32 *) = bs_fn(0x38);
    EFI_STATUS (*AllocatePool)(u32, UINTN, void **) = bs_fn(0x40);

    UINTN msize = 0, mkey = 0, dsize = 0; u32 dver = 0;
    GetMemoryMap(&msize, 0, &mkey, &dsize, &dver);
    msize += 8 * dsize + 4096;
    void *map = 0;
    if (AllocatePool(EfiBootServicesData, msize, &map) != EFI_SUCCESS || !map) {
        app(L"GetMemoryMap alloc failed\r\n"); goto finish;
    }
    if (GetMemoryMap(&msize, map, &mkey, &dsize, &dver) != EFI_SUCCESS) {
        app(L"GetMemoryMap failed\r\n"); goto finish;
    }

    u32 ndesc = (u32)(msize / dsize);
    app(L"memmap entries "); app_dec(ndesc); app(L"\r\n");

    /* log the big writable regions we will scan (bounded) */
    {
        u32 k, shown = 0;
        for (k = 0; k < ndesc && shown < 24; k++) {
            MEM_DESC *md = (MEM_DESC *)((u8 *)map + (UINTN)k * dsize);
            u32 t = md->Type;
            u64 pages = md->NumberOfPages;
            if (t != 2 && t != 3 && t != 4) continue;
            if (pages < 256) continue;
            app(L"  region type="); app_dec(t);
            app(L" base="); app_hex(md->PhysicalStart);
            app(L" pages="); app_dec((u32)pages);
            app(L" attr="); app_hex(md->Attribute);
            app(L"\r\n");
            shown++;
        }
    }

    u64 budget = 0xC0000000ULL;   /* 3 GiB scan budget */

    /* ---- pass 1: count $SPF candidates -------------------------------- */
    int found = 0, patched = 0, skipped_ro = 0;
    u32 d;
    for (d = 0; d < ndesc; d++) {
        MEM_DESC *md = (MEM_DESC *)((u8 *)map + (UINTN)d * dsize);
        u8 *p, *end;
        u32 type = md->Type;
        u64 want;
        if (type != 2 && type != 3 && type != 4) continue;  /* LoaderData / BS code / BS data */
        if (md->NumberOfPages < 2) continue;
        want = md->NumberOfPages * 4096;
        if (want > budget) {
            app(L"scan budget exhausted, skipping "); app_hex(md->PhysicalStart); app(L"\r\n");
            continue;
        }
        budget -= want;
        p = (u8 *)(UINTN)md->PhysicalStart;
        end = p + (UINTN)want;
        /* 4-byte aligned scan for "$SPF" */
        while (p + 0x80 < end) {
            u32 w = rd32(p);
            if (w == 0x46505324u) {
                u32 fc = 0;
                if (spf_validate(p, &fc)) {
                    SPF s;
                    s.base = p; s.nkids = 0;
                    app(L"$SPF @ "); app_hex((u64)p);
                    app(L" type="); app_dec(type);
                    app(L" forms="); app_dec(fc);
                    app(L" attr="); app_hex(md->Attribute);
                    app(L"\r\n");
                    found++;
                    if (!spf_find_root(&s)) {
                        app(L"  root form 0x2710 not found / bad\r\n");
                        p += 4; continue;
                    }
                    if (!spf_check_tabs(&s)) {
                        app(L"  unexpected tab list, NOT patching\r\n");
                        log_tabs(&s);
                        p += 4; continue;
                    }
                    log_tabs(&s);
                    {
                        int k;
                        app(L"  items BEFORE:\r\n");
                        for (k = 0; k < s.nkids; k++) dump_item_hex(&s, k);
                    }
                    if (md->Attribute & EFI_MEMORY_RO) {
                        app(L"  region is EFI_MEMORY_RO -> skip\r\n");
                        skipped_ro++;
                        p += 4; continue;
                    }
                    /* per-variant plan:
                     *   V1: Advanced = clear f30      Chipset = clone Main data
                     *   V2: Advanced+Chipset+Security = clear f30
                     *   V3: Advanced+Chipset = clone Main data
                     */
                    {
                        u8 *main = s.kid[0];
                        int k;
#if VARIANT == 1
                        int apply[6] = { 0, 0, 1, -1, -1, -1 };   /* 0=clear, 1=clone */
#elif VARIANT == 2
                        int apply[6] = { 0, 0, 0, 0, -1, -1 };
#elif VARIANT == 3
                        int apply[6] = { 0, 1, 1, -1, -1, -1 };
#elif VARIANT == 4
                        int apply[6] = { 0, -1, -1, 0, -1, -1 };  /* canary: Security only */
#elif VARIANT == 5
                        int apply[6] = { 0, 0, 0, -1, -1, -1 };   /* Advanced + Chipset */
#else
                        int apply[6] = { 0, -1, -1, -1, -1, -1 }; /* tracer only */
#endif
                        for (k = 1; k < 6; k++) {
                            u8 *it = s.kid[k];
                            if (apply[k] < 0) continue;
                            if (apply[k] == 0) {
                                u64 old = rd64(it + 0x30);
                                if (old == 0) {
                                    app(L"  item "); app_hexn(item_prompt(it), 4);
                                    app(L" already unconditioned\r\n");
                                    continue;
                                }
                                wr64(it + 0x30, 0);
                                app(L"  CLEAR f30 of "); app_hexn(item_prompt(it), 4);
                                app(L" (was "); app_hex(old); app(L")\r\n");
                            } else {
                                app(L"  CLONE ");
                                app_hexn(item_prompt(it), 4);
                                app(L"  f28 "); app_hexn(rd32(it + 0x28), 6);
                                app(L" <- "); app_hexn(rd32(main + 0x28), 6);
                                app(L"  f30 "); app_hex(rd64(it + 0x30));
                                app(L" <- "); app_hex(rd64(main + 0x30));
                                wr32(it + 0x28, rd32(main + 0x28));
                                wr32(it + 0x2c, rd32(main + 0x2c));
                                wr64(it + 0x30, rd64(main + 0x30));
                                app(L"  ok\r\n");
                            }
                        }
                    }
                    /* TRACER: Save & Exit tab label 0x4c -> 0x1e ("Advanced").
                     * Proves the UI really renders the $SPF copy we patched. */
                    {
                        u8 *last = s.kid[5];
                        if (item_prompt(last) == 0x4c) {
                            wr32(last + 0x3c, 0x1e);
                            app(L"  tracer: Save&Exit tab prompt 0x4C -> 0x1E\r\n");
                        }
                    }
                    log_tabs(&s);
                    {
                        int k;
                        app(L"  items AFTER:\r\n");
                        for (k = 0; k < s.nkids; k++) dump_item_hex(&s, k);
                    }
                    app(L"  verify: adv.f30="); app_hex(rd64(s.kid[1] + 0x30));
                    app(L" chi.f30="); app_hex(rd64(s.kid[2] + 0x30));
                    app(L" sec.f30="); app_hex(rd64(s.kid[3] + 0x30));
                    app(L" save.prompt="); app_hexn(item_prompt(s.kid[5]), 4);
                    app(L"\r\n");
                    patched++;
                }
            }
            p += 4;
        }
    }

    app(L"done: found="); app_dec((u32)found);
    app(L" patched="); app_dec((u32)patched);
    app(L" ro_skipped="); app_dec((u32)skipped_ro);
    app(L"\r\n");
    if (!found) app(L"no $SPF in RAM (inner FV not decompressed?)\r\n");

finish:
    write_log();
    app(L"Press any key.\r\n");
    {
        void *ci = con_in();
        if (ci) {
            void *ev = *(void **)((u8 *)ci + 0x10);
            EFI_STATUS (*WaitForEvent)(UINTN, void **) = bs_fn(0x60);
            if (ev && WaitForEvent) { void *evs[1]; evs[0] = ev; WaitForEvent(1, evs); }
        }
    }
    return 0;
}
