/* SpfDump.efi  v1
 *
 * Writes the *running* AMITSE image to \AMITSE.BIN on the volume it was started
 * from, plus a small \AMITSE.TXT with base / size / key RVAs.
 *
 * Why: the machine runs a different AMITSE build than the one extracted from
 * "sp158736.exe" @R00 (their tab tables sit at 0x6D430 vs 0x6D470), so every
 * address derived from that file is suspect.  Writing the live image out lets
 * the whole analysis be redone against the real build instead of guessing.
 *
 * Read-only with respect to the target: it only reads AMITSE's memory.
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

#ifndef MAX_MB
#define MAX_MB 4
#endif

typedef struct { u32 d1; u16 d2; u16 d3; u8 d4[8]; } EFI_GUID;
static const EFI_GUID gLiGuid  = { 0x5B1B31A1,0x9562,0x11D2,{0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B} };
static const EFI_GUID gSfsGuid = { 0x964E5B22,0x6459,0x11D2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B} };

static const u8 gSig1[32] = {
    0x56,0x57,0x48,0x89,0xD6,0x48,0x89,0xCF,0x4E,0x8D,0x4C,0x06,0xFF,0x48,0x39,0xFE,
    0x48,0x89,0xF8,0x73,0x05,0x49,0x39,0xF9,0x73,0x10,0x4C,0x89,0xC1,0x49,0x83,0xE0 };
static const u8 gSig2[32] = {
    0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xDA,0x48,0x8B,0xF9,
    0xE8,0x17,0x00,0x00,0x00,0x48,0x8B,0xD3,0x48,0x8B,0xCF,0xE8,0xC0,0x03,0x00,0x00 };

static void *gST, *gBS;
static EFI_HANDLE gImage;

EFI_STATUS efi_main(EFI_HANDLE image, void *st);
static void *const gAnchor __attribute__((used)) = (void *)&efi_main;

static int mem_eq(const u8 *a, const u8 *b, u32 n) { u32 i; for (i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }
static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u64 rd64(const u8 *p) { return (u64)rd32(p) | ((u64)rd32(p + 4) << 32); }
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

/* ------------------------------------------------------------------ */
EFI_STATUS efi_main(EFI_HANDLE image, void *st)
{
    (void)gAnchor;
    gST = st; gImage = image;
    gBS = *(void **)((u8 *)st + 0x60);
    if (!gBS) return 1;

    {
        EFI_STATUS (*HP)(EFI_HANDLE, const EFI_GUID *, void **) = bsf(0x98);
        void *li = 0, *sfs = 0, *root = 0;
        EFI_HANDLE dev;
        u8 *amitse;
        u32 sizeimg = 0;

        if (HP(gImage, &gLiGuid, &li) != EFI_SUCCESS || !li) { say(L"no loaded image\r\n"); return 0; }
        dev = *(EFI_HANDLE *)((u8 *)li + 0x18);
        if (!dev || HP(dev, &gSfsGuid, &sfs) != EFI_SUCCESS || !sfs) { say(L"no volume\r\n"); return 0; }
        {
            EFI_STATUS (*OpenVolume)(void *, void **) = *(void **)((u8 *)sfs + 8);
            if (OpenVolume(sfs, &root) != EFI_SUCCESS || !root) { say(L"OpenVolume failed\r\n"); return 0; }
        }

        amitse = find_amitse();
        if (!amitse) { say(L"AMITSE not found\r\n"); stall_ms(3000); return 0; }
        {
            u32 pe = rd32(amitse + 0x3c);
            sizeimg = rd32(amitse + pe + 24 + 56);
            if (sizeimg < 0x1000 || sizeimg > (MAX_MB * 1024u * 1024u)) {
                say(L"implausible size\r\n"); stall_ms(3000); return 0;
            }
        }

        say(L"AMITSE "); say_hex((u64)amitse); say(L" size "); say_dec(sizeimg); say(L"\r\n");

        {
            void **fv = (void **)root;
            EFI_STATUS (*FOpen)(void *, void **, const CHAR16 *, u64, u64) = fv[1];
            EFI_STATUS (*FClose)(void *) = fv[2];
            EFI_STATUS (*FDelete)(void *) = fv[3];
            EFI_STATUS (*FWrite)(void *, UINTN *, void *) = fv[5];
            void *file = 0;
            EFI_STATUS s;
            u32 off = 0, chunk = 0x10000;

            s = FOpen(root, &file, L"\\AMITSE.BIN", 3, 0);
            if (s == EFI_SUCCESS && file) { FDelete(file); file = 0; }
            s = FOpen(root, &file, L"\\AMITSE.BIN", 3 | 0x8000000000000000ULL, 0);
            if (s != EFI_SUCCESS || !file) { say(L"cannot create AMITSE.BIN\r\n"); stall_ms(3000); return 0; }
            while (off < sizeimg) {
                UINTN n = sizeimg - off;
                if (n > chunk) n = chunk;
                FWrite(file, &n, amitse + off);
                off += (u32)n;
                if (!n) break;
            }
            FClose(file);

            /* small text summary */
            file = 0;
            s = FOpen(root, &file, L"\\AMITSE.TXT", 3, 0);
            if (s == EFI_SUCCESS && file) { FDelete(file); file = 0; }
            s = FOpen(root, &file, L"\\AMITSE.TXT", 3 | 0x8000000000000000ULL, 0);
            if (s == EFI_SUCCESS && file) {
                CHAR16 buf[96]; int i; u64 vals[4]; const CHAR16 *names[4] = { L"base ", L"size ", L"tab  ", L"d8ecc8 " };
                vals[0] = (u64)amitse; vals[1] = sizeimg; vals[2] = (u64)rd64(amitse + 0x8ecc8); vals[3] = (u64)rd64(amitse + 0x8ead8);
                for (i = 0; i < 4; i++) {
                    int j = 0; const CHAR16 *p = names[i];
                    while (*p) buf[j++] = *p++;
                    { CHAR16 h[19]; int k; h[0] = '0'; h[1] = 'x';
                      for (k = 0; k < 16; k++) { u32 n = (u32)((vals[i] >> (60 - 4 * k)) & 0xF); h[2 + k] = (CHAR16)(n < 10 ? '0' + n : 'A' + n - 10); }
                      h[18] = 0; for (k = 0; k < 18; k++) buf[j++] = h[k]; }
                    buf[j++] = '\r'; buf[j++] = '\n';
                    { UINTN n = (UINTN)j * sizeof(CHAR16); FWrite(file, &n, buf); }
                }
                FClose(file);
            }
        }

        say(L"Wrote AMITSE.BIN ("); say_dec(sizeimg); say(L" bytes).\r\n");
        stall_ms(4000);
    }
    return 0;
}
