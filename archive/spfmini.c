/* SpfMini.efi  v1   -- no file I/O, no key wait, ~5 lines on screen.
 *
 * Finds AMITSE, reads its live setup-database pointer (DAT_0008ecc8), validates
 * the six tab records of form 0x2710 and clears the condition pointer
 * (item+0x30) of the Advanced / Chipset tab records.
 *
 * It deliberately touches the filesystem not at all and prints only a few
 * short lines, because the earlier builds (big console dump + 12 KB log file)
 * were followed by a boot-manager freeze even when they wrote nothing.
 *
 * Workflow: press F10 once (this loads the database), leave setup, run this
 * from F9, then press F10 again.
 *
 * Variants (-DVARIANT=): 5 = Advanced+Chipset (default), 4 = Security canary,
 * 6 = tracer only, 0 = no writes (control).
 */
typedef unsigned long long u64;
typedef unsigned int       u32;
typedef unsigned short     u16;
typedef unsigned char      u8;
typedef u64                UINTN;
typedef u64                EFI_STATUS;
typedef void              *EFI_HANDLE;
typedef u16                CHAR16;

#define EFI_SUCCESS 0ULL

#ifndef VARIANT
#define VARIANT 5
#endif

typedef struct { u32 d1; u16 d2; u16 d3; u8 d4[8]; } EFI_GUID;
static const EFI_GUID gLoadedImageGuid = { 0x5B1B31A1,0x9562,0x11D2,{0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B} };
static const u8 gStoreGuid[16] = { 0x43,0xD6,0x87,0xEC,0xA4,0xEB,0xB5,0x4B,0xA1,0xE5,0x3F,0x3E,0x36,0xB2,0x0D,0xA9 };

static void      *gST, *gBS;
static EFI_HANDLE gImage;
static int        gOut = 1;

EFI_STATUS efi_main(EFI_HANDLE image, void *st);
static void *const gAnchor __attribute__((used)) = (void *)&efi_main;

static int mem_eq(const u8 *a, const u8 *b, u32 n)
{ u32 i; for (i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }
static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u64 rd64(const u8 *p) { return (u64)rd32(p) | ((u64)rd32(p + 4) << 32); }
static void wr32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static void wr64(u8 *p, u64 v) { wr32(p, (u32)v); wr32(p + 4, (u32)(v >> 32)); }

static void say(const CHAR16 *s)
{
    void *co;
    if (!gOut) return;
    co = *(void **)((u8 *)gST + 0x40);
    if (!co) { gOut = 0; return; }
    {
        EFI_STATUS (*OutputString)(void *, const CHAR16 *) = *(void **)((u8 *)co + 8);
        OutputString(co, s);
    }
}
static void say_hexn(u64 v, int digits)
{
    CHAR16 b[19]; int i;
    b[0] = '0'; b[1] = 'x';
    for (i = 0; i < digits; i++) {
        u32 n = (u32)((v >> (4 * (digits - 1 - i))) & 0xF);
        b[2 + i] = (CHAR16)(n < 10 ? '0' + n : 'A' + n - 10);
    }
    b[2 + digits] = 0;
    say(b);
}
static void say_dec(u32 v)
{
    CHAR16 b[11]; int i = 10;
    b[10] = 0;
    do { b[--i] = (CHAR16)('0' + v % 10); v /= 10; } while (v && i > 0);
    say(b + i);
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

EFI_STATUS efi_main(EFI_HANDLE image, void *st)
{
    (void)gAnchor;
    gST = st; gImage = image;
    gBS = *(void **)((u8 *)st + 0x60);
    if (!gBS) return 1;

    {
        EFI_STATUS (*HandleProtocol)(EFI_HANDLE, const EFI_GUID *, void **) = *(void **)((u8 *)gBS + 0x98);
        EFI_STATUS (*LocateHandleBuffer)(u32, const EFI_GUID *, void *, UINTN *, EFI_HANDLE **) = *(void **)((u8 *)gBS + 0x138);
        void (*FreePool)(void *) = *(void **)((u8 *)gBS + 0x48);
        UINTN nh = 0;
        EFI_HANDLE *hs = 0;
        u8 *amitse = 0;
        u32 i;

        if (LocateHandleBuffer(2, &gLoadedImageGuid, 0, &nh, &hs) != EFI_SUCCESS || !hs) {
            say(L"SpfMini: cannot list images\r\n");
            return 0;
        }
        for (i = 0; i < nh; i++) {
            void *li = 0;
            u8 *base;
            if (HandleProtocol(hs[i], &gLoadedImageGuid, &li) != EFI_SUCCESS || !li) continue;
            base = *(u8 **)((u8 *)li + 0x40);
            if (!base) continue;
            if (rd16(base) == 0x5A4D) {
                u32 pe = rd32(base + 0x3c);
                if (rd32(base + pe) == 0x00004550u &&
                    rd64(base + pe + 24 + 24) == 0 &&
                    rd32(base + pe + 24 + 56) == 0xde680u &&
                    rd32(base + pe + 24 + 16) == 0x4a0u) { amitse = base; break; }
            }
        }
        FreePool(hs);
        if (!amitse) { say(L"SpfMini: AMITSE not found\r\n"); return 0; }

        {
            u64 live = rd64(amitse + 0x8ecc8);
            SPF s;
            if (!live || rd32((u8 *)(UINTN)live) != 0x46505324u || !spf_prepare(&s, (u8 *)(UINTN)live)) {
                say(L"DB not loaded yet: press F10, leave setup with ESC,\r\n");
                say(L"run this again, then press F10.\r\n");
                return 0;
            }
            say(L"DB "); say_hexn(live, 12);
            say(L"  tabs 9/1E/1F/39/20/4C  f30: ");
            say_dec((u32)(rd64(s.kid[1] + 0x30) != 0));
            say_dec((u32)(rd64(s.kid[2] + 0x30) != 0));
            say_dec((u32)(rd64(s.kid[3] + 0x30) != 0));
            say(L"\r\n");

#if VARIANT != 0
            {
                int k;
#if VARIANT == 4
                int apply[6] = { 0, -1, -1, 0, -1, -1 };
#elif VARIANT == 6
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
            say(L"now f30: ");
            say_dec((u32)(rd64(s.kid[1] + 0x30) != 0));
            say_dec((u32)(rd64(s.kid[2] + 0x30) != 0));
            say_dec((u32)(rd64(s.kid[3] + 0x30) != 0));
            say(L"  last tab id ");
            say_hexn(rd32(s.kid[5] + 0x3c), 4);
#if VARIANT == 0
            say(L"   (control)\r\n");
#else
            say(L"   -> press F10 now\r\n");
#endif
        }
    }
    return 0;
}
