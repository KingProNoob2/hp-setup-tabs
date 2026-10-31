/* SpfLive.efi  v0.3
 *
 * Same idea as SpfUnlock, but it patches *only the live copy* of the compiled
 * setup database: it locates AMITSE's loaded image, reads the global
 * DAT_0008ecc8 (= pointer to the "$SPF" magic of the buffer AMITSE keeps),
 * validates it and patches in place.  No RAM scan, no second (possibly stale or
 * freed) copy touched.
 *
 * Why: the v0.1 scan found TWO $SPF copies in RAM (0x4D3CA02C and 0x4EBFA028)
 * and patched both; the boot manager froze right after the app returned.  One of
 * those buffers is very likely dead/reused memory, so writing into it corrupts
 * someone else's data.  This build removes that variable and also removes the
 * whole-memory scan.
 *
 * Variants:
 *   0  no-op control: locate + validate + log, write nothing
 *   6  tracer only: Save&Exit label 0x4c -> 0x1e
 *   4  canary: tracer + clear item+0x30 on Security only
 *   5  tracer + clear item+0x30 on Advanced + Chipset
 *   2  tracer + clear item+0x30 on Advanced + Chipset + Security
 *   9  tracer + clone Main data into Advanced + Chipset (the risky one)
 *
 * Build: see README.md
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
#define EfiBootServicesData  4
#define FILE_MODE_READ       0x0000000000000001ULL
#define FILE_MODE_WRITE      0x0000000000000002ULL
#define FILE_MODE_CREATE     0x8000000000000000ULL

#ifndef VARIANT
#define VARIANT 0
#endif

typedef struct { u32 d1; u16 d2; u16 d3; u8 d4[8]; } EFI_GUID;

static const EFI_GUID gLoadedImageGuid = { 0x5B1B31A1,0x9562,0x11D2,{0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B} };
static const EFI_GUID gSimpleFsGuid    = { 0x964E5B22,0x6459,0x11D2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B} };

static const u8 gStoreGuid[16] = { 0x43,0xD6,0x87,0xEC,0xA4,0xEB,0xB5,0x4B,0xA1,0xE5,0x3F,0x3E,0x36,0xB2,0x0D,0xA9 };

/* AMITSE .text signatures (file offsets 0x2c0 / 0x4a0 == RVAs) */
static const u8 gSig1[32] = {
    0x56,0x57,0x48,0x89,0xD6,0x48,0x89,0xCF,0x4E,0x8D,0x4C,0x06,0xFF,0x48,0x39,0xFE,
    0x48,0x89,0xF8,0x73,0x05,0x49,0x39,0xF9,0x73,0x10,0x4C,0x89,0xC1,0x49,0x83,0xE0 };
static const u8 gSig2[32] = {
    0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xDA,0x48,0x8B,0xF9,
    0xE8,0x17,0x00,0x00,0x00,0x48,0x8B,0xD3,0x48,0x8B,0xCF,0xE8,0xC0,0x03,0x00,0x00 };

#define RVA_DB_PTR   0x8ecc8u   /* DAT_0008ecc8 : -> "$SPF" magic      */
#define RVA_DB_BASE  0x8eb68u   /* DAT_0008eb68 : == DB ptr            */
#define RVA_FORMTAB  0x8eb58u   /* DAT_0008eb58 : == DB ptr + 0x60     */

static void      *gST, *gBS;
static EFI_HANDLE gImage;
static CHAR16     gLog[40000];
static UINTN      gLogLen;

EFI_STATUS efi_main(EFI_HANDLE image, void *st);
static void *const gAnchor __attribute__((used)) = (void *)&efi_main;

/* ---- helpers ----------------------------------------------------------- */
static int mem_eq(const u8 *a, const u8 *b, u32 n)
{ u32 i; for (i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }
static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u64 rd64(const u8 *p) { return (u64)rd32(p) | ((u64)rd32(p + 4) << 32); }
static void wr32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static void wr64(u8 *p, u64 v) { wr32(p, (u32)v); wr32(p + 4, (u32)(v >> 32)); }

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
    if (!gImage) return;
    if (HandleProtocol(gImage, &gLoadedImageGuid, &li) != EFI_SUCCESS || !li) return;
    EFI_HANDLE dev = *(EFI_HANDLE *)((u8 *)li + 0x18);
    if (!dev || HandleProtocol(dev, &gSimpleFsGuid, &sfs) != EFI_SUCCESS || !sfs) return;
    EFI_STATUS (*OpenVolume)(void *, void **) = *(void **)((u8 *)sfs + 8);
    if (OpenVolume(sfs, &root) != EFI_SUCCESS || !root) return;

    void **fv = (void **)root;
    EFI_STATUS (*FOpen)(void *, void **, const CHAR16 *, u64, u64) = fv[1];
    EFI_STATUS (*FClose)(void *)   = fv[2];
    EFI_STATUS (*FDelete)(void *)  = fv[3];
    EFI_STATUS (*FWrite)(void *, UINTN *, void *) = fv[5];

    EFI_STATUS s = FOpen(root, &file, L"\\SPFLIVE.LOG", FILE_MODE_READ | FILE_MODE_WRITE, 0);
    if (s == EFI_SUCCESS && file) { FDelete(file); file = 0; }
    s = FOpen(root, &file, L"\\SPFLIVE.LOG", FILE_MODE_READ | FILE_MODE_WRITE | FILE_MODE_CREATE, 0);
    if (s != EFI_SUCCESS || !file) { app(L"cannot create \\SPFLIVE.LOG\r\n"); return; }
    UINTN n = gLogLen * sizeof(CHAR16);
    FWrite(file, &n, gLog);
    FClose(file);
    FClose(root);
}

/* ---- $SPF patch engine -------------------------------------------------- */
#define SPF_MAX_ITEMS 8

typedef struct {
    u8 *base; u8 *root; u8 *item;
    int nkids;
    u8 *kid[SPF_MAX_ITEMS];
} SPF;

static const u32 g_tab_prompt[6] = { 0x09, 0x1e, 0x1f, 0x39, 0x20, 0x4c };

static int spf_prepare(SPF *s, u8 *base)
{
    u32 forms_off, item_off, cnt, i, prev;
    if (!base) return 0;
    if (rd32(base) != 0x46505324u) return 0;
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
        if (n < 6 || n > SPF_MAX_ITEMS) return 0;
        s->nkids = (int)n;
        for (i = 0; i < n; i++) s->kid[i] = s->item + rd32(s->root + 0x20 + 4 * i);
    }
    for (i = 0; i < 6; i++) {
        if (rd32(s->kid[i] + 0x3c) != g_tab_prompt[i]) return 0;
        if (rd16(s->kid[i] + 0x14) != 1) return 0;
    }
    return 1;
}

static void log_tabs(SPF *s, const CHAR16 *when)
{
    int i;
    app(L"  tabs "); app(when); app(L":\r\n");
    for (i = 0; i < s->nkids; i++) {
        u8 *it = s->kid[i];
        app(L"   ["); app_dec((u32)i);
        app(L"] prompt="); app_hexn(rd32(it + 0x3c), 4);
        app(L" qid="); app_dec(rd32(it + 0x0c));
        app(L" op="); app_hexn(rd16(it + 0x14), 2);
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
        app(L"    ["); app_dec((u32)idx); app(L"] +");
        app_hexn((u64)(row * 16), 2); app(L": ");
        for (k = 0; k < 16; k++) {
            u8 b = it[row * 16 + k];
            line[0] = hx[b >> 4]; line[1] = hx[b & 0xF]; line[2] = ' '; line[3] = 0;
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

    app(L"SpfLive v0.3  variant "); app_dec(VARIANT); app(L"\r\n");
#if VARIANT == 0
    app(L"  plan: NO-OP control (locate + validate + log only)\r\n");
#elif VARIANT == 2
    app(L"  plan: clear f30 on Advanced+Chipset+Security, tracer\r\n");
#elif VARIANT == 4
    app(L"  plan: canary - clear f30 on SECURITY only, tracer\r\n");
#elif VARIANT == 5
    app(L"  plan: clear f30 on Advanced+Chipset, tracer\r\n");
#elif VARIANT == 9
    app(L"  plan: clone Main data into Advanced+Chipset, tracer\r\n");
#else
    app(L"  plan: tracer only (no f30 write)\r\n");
#endif
    if (!gBS) return 1;

    EFI_STATUS (*HandleProtocol)(EFI_HANDLE, const EFI_GUID *, void **) = bs_fn(0x98);
    EFI_STATUS (*LocateHandleBuffer)(u32, const EFI_GUID *, void *, UINTN *, EFI_HANDLE **) = bs_fn(0x138);
    void (*FreePool)(void *) = bs_fn(0x48);

    u8 *amitse = 0;
    {
        UINTN nh = 0;
        EFI_HANDLE *hs = 0;
        u32 i;
        if (LocateHandleBuffer(2, &gLoadedImageGuid, 0, &nh, &hs) != EFI_SUCCESS || !hs) {
            app(L"cannot enumerate loaded images\r\n"); goto finish;
        }
        app(L"  loaded images "); app_dec((u32)nh); app(L"\r\n");
        for (i = 0; i < nh; i++) {
            void *li = 0;
            u8 *base;
            if (HandleProtocol(hs[i], &gLoadedImageGuid, &li) != EFI_SUCCESS || !li) continue;
            base = *(u8 **)((u8 *)li + 0x40);
            if (!base) continue;
            /* AMITSE: PE32+ with ImageBase 0, SizeOfImage 0xDE680,
             * entry 0x4A0 - or, as a fallback, both .text signatures */
            {
                u32 pe, entry = 0, so = 0;
                u64 ib = 0;
                int hdr_ok = 0;
                if (rd16(base) == 0x5A4D) {                 /* "MZ" */
                    pe = rd32(base + 0x3c);
                    if (rd32(base + pe) == 0x00004550u) {
                        entry = rd32(base + pe + 24 + 16);
                        ib    = rd64(base + pe + 24 + 24);
                        so    = rd32(base + pe + 24 + 56);
                        hdr_ok = (ib == 0 && so == 0xde680u && entry == 0x4a0u);
                    }
                }
                if (!hdr_ok &&
                    !(mem_eq(base + 0x2c0, gSig1, 32) && mem_eq(base + 0x4a0, gSig2, 32)))
                    continue;
                app(L"  match by "); app(hdr_ok ? L"PE header" : L"code signature");
                app(L"\r\n");
            }
            amitse = base;
            app(L"  AMITSE @ "); app_hex((u64)base); app(L"\r\n");
            break;
        }
        FreePool(hs);
    }
    if (!amitse) { app(L"AMITSE image not found\r\n"); goto finish; }

    {
        u64 live    = rd64(amitse + RVA_DB_PTR);
        u64 basecmp = rd64(amitse + RVA_DB_BASE);
        u64 fmtab   = rd64(amitse + RVA_FORMTAB);
        app(L"  DAT_8ecc8 (live DB) = "); app_hex(live); app(L"\r\n");
        app(L"  DAT_8eb68           = "); app_hex(basecmp);
        app(live == basecmp ? L"  (match)\r\n" : L"  (MISMATCH)\r\n");
        app(L"  DAT_8eb58 (fmtab)   = "); app_hex(fmtab);
        app(fmtab == live + 0x60 ? L"  (== live+0x60)\r\n" : L"  (unexpected)\r\n");
        if (!live) { app(L"setup database not loaded yet (pointer is NULL)\r\n"); goto finish; }
        if (rd32((u8 *)(UINTN)live) != 0x46505324u) {
            app(L"live pointer does not start with $SPF\r\n");
            goto finish;
        }
        {
            SPF s;
            if (!spf_prepare(&s, (u8 *)(UINTN)live)) {
                app(L"live $SPF failed validation - NOT patching\r\n");
                goto finish;
            }
            log_tabs(&s, L"BEFORE");
            {
                int k;
                for (k = 0; k < s.nkids; k++) dump_item_hex(&s, k);
            }
#if VARIANT != 0
            {
                u8 *main_item = s.kid[0];
                int k;
#if VARIANT == 2
                int apply[6] = { 0, 0, 0, 0, -1, -1 };
#elif VARIANT == 4
                int apply[6] = { 0, -1, -1, 0, -1, -1 };
#elif VARIANT == 5
                int apply[6] = { 0, 0, 0, -1, -1, -1 };
#elif VARIANT == 9
                int apply[6] = { 0, 1, 1, -1, -1, -1 };
#else
                int apply[6] = { 0, -1, -1, -1, -1, -1 };
#endif
                for (k = 1; k < 6; k++) {
                    u8 *it = s.kid[k];
                    if (apply[k] < 0) continue;
                    if (apply[k] == 0) {
                        u64 old = rd64(it + 0x30);
                        if (old == 0) {
                            app(L"  item "); app_hexn(rd32(it + 0x3c), 4);
                            app(L" already 0\r\n");
                            continue;
                        }
                        wr64(it + 0x30, 0);
                        app(L"  CLEAR f30 of "); app_hexn(rd32(it + 0x3c), 4);
                        app(L" (was "); app_hex(old); app(L")\r\n");
                    } else {
                        app(L"  CLONE "); app_hexn(rd32(it + 0x3c), 4);
                        app(L" f28 "); app_hexn(rd32(it + 0x28), 6);
                        app(L" <- "); app_hexn(rd32(main_item + 0x28), 6);
                        app(L" f30 "); app_hex(rd64(it + 0x30));
                        app(L" <- "); app_hex(rd64(main_item + 0x30));
                        wr32(it + 0x28, rd32(main_item + 0x28));
                        wr32(it + 0x2c, rd32(main_item + 0x2c));
                        wr64(it + 0x30, rd64(main_item + 0x30));
                        app(L" ok\r\n");
                    }
                }
#if VARIANT != 6
                if (rd32(s.kid[5] + 0x3c) == 0x4c) {
                    wr32(s.kid[5] + 0x3c, 0x1e);
                    app(L"  tracer: Save&Exit label 0x4C -> 0x1E\r\n");
                }
#endif
            }
#endif
            log_tabs(&s, L"AFTER");
            app(L"  verify adv.f30="); app_hex(rd64(s.kid[1] + 0x30));
            app(L" chi.f30=");       app_hex(rd64(s.kid[2] + 0x30));
            app(L" sec.f30=");       app_hex(rd64(s.kid[3] + 0x30));
            app(L" last.prompt=");   app_hexn(rd32(s.kid[5] + 0x3c), 4);
            app(L"\r\n");
        }
    }

finish:
    write_log();
    app(L"done. Press any key.\r\n");
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
