// [DOS2] A corrupt binary resource (.lsb) must not kill the game (phone 21.11, run1121f: CRASH sig=11 pc=main+0x886828 on level load
// in UI mode TOUCH, bt main+0x885498 <- 0x8824f0 <- 0x13ae16c <- 0x13aa74c <- 0xae49c0).
// What happened: the PC inventory class (type 0x74) reads its movie's numbers once after the load (main+0x13aa5b0: cSize, cSpacing,
// bgDiscrap, invColW, portraitH, portraitW) and then main+0x13ae110 loads the profile file <profile>/InventorySettings.lsb (the game writes
// it at every save, main+0x13aeb40) with the generic LSB reader main+0x885270. That reader maps the file and, after the 0x18-byte header
// (and the four version words when the header float is > 1.0), reads the string table count (main+0x885470) and sizes an array of 8-byte
// slots by it (main+0x886790) WITHOUT comparing the count with the bytes left: the file on the phone gave 0xa837000, the allocation of
// 8 x count + 8 bytes was cleared past its end and the game died. (When fewer than 4 bytes are left it even goes on with a stale stack
// value, main+0x885bcc.) The movie values are read before and are not part of that parse.
// Fix: the four words at main+0x885468 jump to a stub that does the original reads and fails the parse (main+0x885b24: the reader's own
// "return false" exit, which frees the mapping) when fewer than 4 bytes are left, when the "left" value is not a sane size (>= 2 GiB:
// a file shorter than its header), or when the count is larger than the bytes left / 4 (every entry of that table reads at least one u32,
// main+0x8854d0) - none of these can be a valid file. A failed parse is what a missing file gives: the caller (main+0x13ae168) skips
// the settings and the class keeps its defaults; the next save writes the file again.
// Every reject is counted by the stub and logged from the pad poll ("[lsb] ..."). DM_DOS2_LSB_GUARD=0 = off (the game's reader as it was).
// main = main.raw of update v786432, offsets = main.raw. Static only when written (not run on a phone).
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

enum { LG_SITE = 0x885468, LG_BACK = 0x885478, LG_FAIL = 0x885b24, LG_RESIZE = 0x885494, LG_HDR = 0x885344 };
// the site (cmp x8,#4 / b.lo main+0x885bcc / ldr w8,[x9],#4 / stur w8,[x29,#-0x4c]: the count), the words after it, the "return false"
// exit, the resize call by the count, the header read (ldr x10,[sp,#0x18]: the mapped cursor)
static const uint32_t k_lg_site[6] = {0xf100111f, 0x54003b03, 0xb8404528, 0xb81b43a8, 0xf9000fe9 /* str x9,[sp,#0x18] */, 0xb9402e69 /* ldr w9,[x19,#0x2c] */};
static const uint32_t k_lg_fail[2] = {0x2a1f03f3 /* mov w19,wzr */, 0x17fffef9 /* b main+0x88570c: the cleanup */};
static const uint32_t k_lg_resize[2] = {0xaa1403e0 /* mov x0,x20 */, 0x940004bf /* bl main+0x886790 */};
static const uint32_t k_lg_hdr[3] = {0xf9400fea /* ldr x10,[sp,#0x18] */, 0x9100614b /* add x11,x10,#0x18 */, 0xf9000feb /* str x11,[sp,#0x18] */};

static module_t *g_main;
static struct { volatile uint32_t n, pad; volatile uint64_t val; } g_rej __attribute__((aligned(16)));
static uint32_t g_logged;

static int lg_same(const uint8_t *base, uint32_t off, const uint32_t *w, int n) {
    return (uint64_t)off + 4ull * (uint64_t)n <= g_main->text_end && !memcmp(base + off, w, 4 * (size_t)n);
}

void dm_dos2_lsb_guard_poll(void);   // hle_dos2_uimode.c, every pad poll (the game thread)
void dm_dos2_lsb_guard_poll(void) {
    const uint32_t n = g_rej.n;
    if (n == g_logged) return;
    g_logged = n;
    LOG("【红】[lsb] a binary resource file failed the size check of the LSB reader main+0x885270 (reject #%u, the value read %#llx): parse "
        "failed instead of the crash main+0x886828 (21.11, a corrupt <profile>/InventorySettings.lsb); its settings are left at the defaults",
        n, (unsigned long long)g_rej.val);
}

void dm_dos2_lsb_guard_install(void);   // main.c host_preload
void dm_dos2_lsb_guard_install(void) {
    const char *e = getenv("DM_DOS2_LSB_GUARD");
    if (e && *e == '0') { LOG("[lsb] DM_DOS2_LSB_GUARD=0: the LSB reader main+0x885270 sizes its string table by the file's count unchecked (as the game)"); return; }
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) g_main = &g_mod[i];
    if (!g_main) return;
    const uint8_t *b = g_main->base;
    if (!lg_same(b, LG_SITE, k_lg_site, 6) || !lg_same(b, LG_FAIL, k_lg_fail, 2) || !lg_same(b, LG_RESIZE - 4, k_lg_resize, 2) || !lg_same(b, LG_HDR, k_lg_hdr, 3)) {
        LOG("【红】[lsb] main+%#x differs (the LSB reader's string table count): no size check, a corrupt .lsb file can still crash the game", LG_SITE); return; }
    uint32_t *t = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t == MAP_FAILED) { LOG("【红】[lsb] mmap failed: no size check"); return; }
    // x8 = the bytes left, x9 = the cursor; x16 / x17 are free (IP0 / IP1)
    static const uint32_t code[20] = {
        0xf100111f,   //  0 cmp x8,#4
        0x54000183,   //  1 b.lo 13 (bad)
        0xd35ffd10,   //  2 lsr x16,x8,#31
        0xb5000150,   //  3 cbnz x16, 13 (bad: not a sane size)
        0xd1001110,   //  4 sub x16,x8,#4: left after the count
        0xb8404528,   //  5 ldr w8,[x9],#4: the count (the game's own word)
        0xeb50091f,   //  6 cmp x8, x16, lsr #2
        0x540000c8,   //  7 b.hi 13 (bad: more entries than the file can hold)
        0xb81b43a8,   //  8 stur w8,[x29,#-0x4c] (the game's own word)
        0x58000000,   //  9 ldr x16, =main+0x885478 (offset set below)
        0xd61f0200,   // 10 br x16
        0xd503201f,   // 11 nop
        0xd503201f,   // 12 nop
        0x58000000,   // 13 bad: ldr x16, =&g_rej (offset set below)
        0xb9400211,   // 14 ldr w17,[x16]
        0x11000631,   // 15 add w17,w17,#1
        0xb9000211,   // 16 str w17,[x16]
        0xf9000608,   // 17 str x8,[x16,#8]
        0x58000000,   // 18 ldr x16, =main+0x885b24 (offset set below)
        0xd61f0200};  // 19 br x16
    memcpy(t, code, sizeof code);
    // literals at words 20.. (8-byte aligned): 20-21 = main+0x885478, 22-23 = &g_rej, 24-25 = main+0x885b24
    const uint64_t back = (uint64_t)(uintptr_t)(b + LG_BACK), fail = (uint64_t)(uintptr_t)(b + LG_FAIL), rej = (uint64_t)(uintptr_t)(void *)&g_rej;
    t[9] = 0x58000000u | ((20u - 9u) << 5) | 16u;    // ldr x16, [pc + 11 words]
    t[13] = 0x58000000u | ((22u - 13u) << 5) | 16u;  // ldr x16, [pc + 9 words]
    t[18] = 0x58000000u | ((24u - 18u) << 5) | 16u;  // ldr x16, [pc + 6 words]
    t[20] = (uint32_t)back; t[21] = (uint32_t)(back >> 32); t[22] = (uint32_t)rej; t[23] = (uint32_t)(rej >> 32); t[24] = (uint32_t)fail; t[25] = (uint32_t)(fail >> 32);
    if (mprotect(t, 0x4000, PROT_READ | PROT_EXEC)) { munmap(t, 0x4000); LOG("【红】[lsb] mprotect RX failed: no size check"); return; }
    __builtin___clear_cache((char *)t, (char *)(t + 32));
    const uint64_t to = (uint64_t)(uintptr_t)t;
    const uint32_t jump[4] = {0x58000050 /* ldr x16,#8 */, 0xd61f0200 /* br x16 */, (uint32_t)to, (uint32_t)(to >> 32)};
    uint32_t *at = (uint32_t *)(void *)(g_main->base + LG_SITE);
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF);
    const size_t len = (size_t)(((uintptr_t)(at + 4) - (uintptr_t)pg + 0x3FFF) & ~(uintptr_t)0x3FFF);
    if (mprotect(pg, len, PROT_READ | PROT_WRITE)) { LOG("【红】[lsb] mprotect RW failed at main+%#x: no size check", LG_SITE); return; }
    memcpy(at, jump, sizeof jump);
    if (mprotect(pg, len, PROT_READ | PROT_EXEC)) die("[lsb] mprotect RX");
    __builtin___clear_cache((char *)at, (char *)(at + 4));
    LOG("[lsb] LSB reader main+0x885270: the string table count is checked against the bytes left (a corrupt file = a failed parse, the "
        "caller keeps its defaults; crash main+0x886828 of 21.11, InventorySettings.lsb); DM_DOS2_LSB_GUARD=0 = off");
}
#endif
