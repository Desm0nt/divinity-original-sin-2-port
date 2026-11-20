// [DOS2] VK-NATIVE stage 4: reader of the setup ASTC texture pack (vkn_astc.h has the format and the key). The translator's reader
// (xbg_game.cpp astccache::open_setup_pack / lookup / upload, xbg_astc_cache.h) lives in C++ inside the translator; this is the same
// logic in C over the same file, without the translator's lazy encoder (out of scope here: a texture the pack lacks is decoded).
#ifdef DM_GAME_DOS2
#include "vkn_astc.h"
#include "vkn.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"
#define READ_CHUNK (256u << 10)   // the bounce buffer of NR-18 (XBG_AC_READ_CHUNK): small enough for L2

// CRC-32 + Adler-32 of guest storage (the pack key; the same function as xbg_game.cpp's xbg_astc_storage_key of the NR-05 worker, moved
// into the backend's shader library vkn_shlib.cpp so no mode calls xbg_game.cpp for it). Weak: the host tests of the other stages link
// vkn_tex.c without it (then the pack reports NONE).
extern int vkn_shlib_astc_key(const void *mem, uint64_t size, uint32_t *crc, uint32_t *adler) __attribute__((weak));

// ---- pure pieces ----
int vkn_astc_header_check(const vkn_astc_hdr *h, uint64_t file_bytes, const char **why) {
    if (memcmp(h->magic, "D3AC", 4)) { *why = "not a D3AC pack"; return -1; }
    if (h->version != 1) { *why = "pack version is not 1 (run setup again)"; return -1; }
    if (h->index_off < 64 || h->index_off + (uint64_t)h->entries * sizeof(vkn_astc_entry) != file_bytes) { *why = "index does not end the file (incomplete copy?)"; return -1; }
    if (h->data_off != 64 || h->data_off + h->data_bytes != h->index_off) { *why = "data range does not match the index"; return -1; }
    return 0;
}
uint64_t vkn_astc_level_bytes(const vkn_astc_entry *e, unsigned level) {
    const uint32_t w = e->width >> level ? e->width >> level : 1, h = e->height >> level ? e->height >> level : 1;
    return (uint64_t)((w + e->bw - 1) / e->bw) * ((h + e->bh - 1) / e->bh) * 16;
}
int vkn_astc_entry_ok(const vkn_astc_entry *e, const vkn_astc_hdr *h) {
    if (!((e->bw == 4 && e->bh == 4) || (e->bw == 5 && e->bh == 5) || (e->bw == 6 && e->bh == 6)) || !e->levels || e->levels > 16 || !e->width || !e->height) return 0;
    uint64_t n = 0;
    for (unsigned l = 0; l < e->levels; l++) n += vkn_astc_level_bytes(e, l);
    return n == e->data_bytes && e->data_off >= h->data_off && e->data_off + e->data_bytes <= h->data_off + h->data_bytes;
}
// The tool's key tuple order (xbg_ac_less): crc, adler, bytes, format, width, height, levels.
static int key_cmp(const vkn_astc_entry *a, uint32_t crc, uint32_t adler, uint32_t bytes, uint16_t format, uint16_t levels, uint16_t w, uint16_t h) {
    if (a->crc != crc) return a->crc < crc ? -1 : 1;
    if (a->adler != adler) return a->adler < adler ? -1 : 1;
    if (a->bytes != bytes) return a->bytes < bytes ? -1 : 1;
    if (a->format != format) return a->format < format ? -1 : 1;
    if (a->width != w) return a->width < w ? -1 : 1;
    if (a->height != h) return a->height < h ? -1 : 1;
    if (a->levels != levels) return a->levels < levels ? -1 : 1;
    return 0;
}
int64_t vkn_astc_find(const vkn_astc_entry *idx, uint32_t n, uint32_t crc, uint32_t adler, uint32_t bytes, uint16_t format, uint16_t levels, uint16_t w, uint16_t h) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) { const uint32_t mid = lo + (hi - lo) / 2; if (key_cmp(&idx[mid], crc, adler, bytes, format, levels, w, h) < 0) lo = mid + 1; else hi = mid; }
    return lo < n && !key_cmp(&idx[lo], crc, adler, bytes, format, levels, w, h) ? (int64_t)lo : -1;
}
int vkn_astc_srgb(uint32_t nvn) { return (nvn >= 0x45 && nvn <= 0x48) || nvn == 0x4e; }
VkFormat vkn_astc_vkformat(uint8_t bw, uint8_t bh, int srgb) {
    if (bw == 4 && bh == 4) return srgb ? VK_FORMAT_ASTC_4x4_SRGB_BLOCK : VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    if (bw == 5 && bh == 5) return srgb ? VK_FORMAT_ASTC_5x5_SRGB_BLOCK : VK_FORMAT_ASTC_5x5_UNORM_BLOCK;
    if (bw == 6 && bh == 6) return srgb ? VK_FORMAT_ASTC_6x6_SRGB_BLOCK : VK_FORMAT_ASTC_6x6_UNORM_BLOCK;
    return VK_FORMAT_UNDEFINED;
}
int vkn_astc_cacheable(uint32_t nvn, uint32_t target, uint32_t layers, uint32_t samples, uint64_t bytes, uint32_t w, uint32_t h) {
    return nvn >= 0x41 && nvn <= 0x48 && target == 1 && layers == 1 && samples <= 1 && bytes && bytes <= UINT32_MAX && w && h && w <= 65535 && h <= 65535;
}
unsigned vkn_astc_sanitize(uint8_t *blocks, uint64_t bytes) {
    static const uint8_t void_black[16] = {0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0};
    unsigned n = 0;
    for (uint64_t o = 0; o + 16 <= bytes; o += 16) {
        uint64_t a, b; memcpy(&a, blocks + o, 8); memcpy(&b, blocks + o + 8, 8);
        if (!(a | b)) { memcpy(blocks + o, void_black, 16); ++n; }
    }
    return n;
}

// ---- the pack ----
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;   // open / reset, the counters
static int g_state;                                        // VKN_ASTC_* (written under g_mu, read with acquire)
static int g_fd = -1;
static vkn_astc_entry *g_idx; static uint32_t g_n;
static uint64_t *g_shape; static uint32_t g_nshape;        // sorted shapes of the entries (the "can the pack have it" filter)
static vkn_astc_stats g_st;
static uint32_t g_read_red;

static uint64_t shape_of(uint32_t bytes, uint16_t format, uint16_t levels, uint16_t w, uint16_t h) {   // xbg_ac_shape
    return ((uint64_t)bytes * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)format << 48 | (uint64_t)levels << 40 | (uint64_t)w << 20 | (uint64_t)h);
}
static int u64_cmp(const void *a, const void *b) { const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static int shape_known(uint64_t s) {
    uint32_t lo = 0, hi = g_nshape;
    while (lo < hi) { const uint32_t mid = lo + (hi - lo) / 2; if (g_shape[mid] < s) lo = mid + 1; else hi = mid; }
    return lo < g_nshape && g_shape[lo] == s;
}
static int entry_less(const void *a, const void *b) {
    const vkn_astc_entry *y = (const vkn_astc_entry *)b;
    return key_cmp((const vkn_astc_entry *)a, y->crc, y->adler, y->bytes, y->format, y->levels, y->width, y->height);
}
static void path_of(char *out, size_t n) {
    const char *e = getenv("DM_ASTC_CACHE_PATH");
    if (e && *e) { snprintf(out, n, "%s", e); return; }
    if ((e = getenv("DM_ASTC_DATA_DIR")) && *e) { snprintf(out, n, "%s/astc/dos2-astc.pack", e); return; }
    char rt[1024]; e = getenv("DM_RUNTIME_DIR");
    snprintf(rt, sizeof rt, "%s", e && *e ? e : "./dm");
    size_t l = strlen(rt); while (l > 1 && rt[l - 1] == '/') rt[--l] = 0;   // <data>/dm -> <data>
    char *s = strrchr(rt, '/');
    if (s) *s = 0; else snprintf(rt, sizeof rt, ".");
    snprintf(out, n, "%s/astc/dos2-astc.pack", rt);
}
// The device samples (with linear filtering) and copies into both the UNORM and the sRGB format of this block size (astccache::init).
static int block_usable(uint8_t b) {
    const vkn_dev *d = vkn_device(); if (!d || !d->feat.textureCompressionASTC_LDR) return 0;
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    for (int s = 0; s < 2; s++) {
        VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(d->phys, vkn_astc_vkformat(b, b, s), &p);
        VkFormatFeatureFlags v = p.optimalTilingFeatures;
        if (d->props.apiVersion < VK_API_VERSION_1_1 && (v & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) v |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;   // (1.0: no transfer bits)
        if ((v & need) != need) return 0;
    }
    return 1;
}
static int open_locked(void) {
    char path[1200]; path_of(path, sizeof path);
    const char *sw = getenv("DM_VK_NATIVE_ASTC");
    const char *none = NULL;
    if (sw && *sw == '0') none = "DM_VK_NATIVE_ASTC=0";
    else if (!vkn_shlib_astc_key) none = "no key function in this build";
    if (!none) for (int k = 0; k < 3; k++) g_st.block_ok[k] = (uint8_t)block_usable((uint8_t)(4 + k));
    if (!none && !g_st.block_ok[0] && !g_st.block_ok[1] && !g_st.block_ok[2]) none = "the device samples no ASTC 4x4 / 5x5 / 6x6 (UNORM + sRGB, linear filter)";
    if (none) { vkn_logf("astc: pack not used (%s): BC textures are decoded on the CPU", none); return VKN_ASTC_NONE; }
    const int f = open(path, O_RDONLY | O_CLOEXEC);
    if (f < 0) {
        vkn_logf("astc: no pack at %s (errno %d; setup step \"astc\" makes it): BC textures are decoded on the CPU", path, errno);
        return VKN_ASTC_NONE;
    }
    const char *why = NULL; vkn_astc_hdr h; struct stat st;
    memset(&h, 0, sizeof h);
    if (fstat(f, &st) || pread(f, &h, sizeof h, 0) != (ssize_t)sizeof h) why = "header unreadable";
    else vkn_astc_header_check(&h, (uint64_t)st.st_size, &why);
    vkn_astc_entry *idx = NULL; uint64_t *shp = NULL;
    if (!why && h.entries) {
        idx = (vkn_astc_entry *)malloc(sizeof *idx * h.entries); shp = (uint64_t *)malloc(sizeof *shp * h.entries);
        if (!idx || !shp) why = "no memory for the index";
        else {
            const size_t n = sizeof *idx * h.entries;
            size_t got = 0;
            while (got < n) { const ssize_t k = pread(f, (uint8_t *)idx + got, n - got, (off_t)(h.index_off + got)); if (k <= 0) break; got += (size_t)k; }
            if (got != n) why = "index unreadable";
        }
    }
    uint64_t data = 0;
    for (uint32_t i = 0; !why && i < h.entries; i++) {
        if (!vkn_astc_entry_ok(&idx[i], &h)) { why = "corrupt index entry"; break; }
        data += idx[i].data_bytes; shp[i] = shape_of(idx[i].bytes, idx[i].format, idx[i].levels, idx[i].width, idx[i].height);
    }
    if (why) {
        vkn_logf(RED "astc: pack %s not used: %s (run setup again): BC textures are decoded on the CPU", path, why);
        free(idx); free(shp); close(f); return VKN_ASTC_BAD;
    }
    if (h.entries) { qsort(idx, h.entries, sizeof *idx, entry_less); qsort(shp, h.entries, sizeof *shp, u64_cmp); }   // (the tool writes it sorted already)
    uint32_t ns = 0;
    for (uint32_t i = 0; i < h.entries; i++) if (!ns || shp[ns - 1] != shp[i]) shp[ns++] = shp[i];
    g_fd = f; g_idx = idx; g_n = h.entries; g_shape = shp; g_nshape = ns;
    g_st.entries = h.entries; g_st.pack_bytes = data;
    vkn_logf("astc: pack %s: %u entries, %.1f MB of ASTC; blocks 4x4=%d 5x5=%d 6x6=%d", path, h.entries, data / 1048576.0, g_st.block_ok[0], g_st.block_ok[1], g_st.block_ok[2]);
    return VKN_ASTC_READY;
}
int vkn_astc_open(void) {
    int s = __atomic_load_n(&g_state, __ATOMIC_ACQUIRE);
    if (s) return s;
    if (!vkn_device()) return VKN_ASTC_NONE;   // (not latched: the device comes first in every caller anyway)
    pthread_mutex_lock(&g_mu);
    if (!(s = g_state)) { s = open_locked(); g_st.state = s; __atomic_store_n(&g_state, s, __ATOMIC_RELEASE); }
    pthread_mutex_unlock(&g_mu);
    return s;
}
static void count(uint64_t *c, uint64_t v) { __atomic_add_fetch(c, v, __ATOMIC_RELAXED); }

int vkn_astc_lookup(const void *storage, uint64_t bytes, uint32_t nvn, uint32_t levels, uint32_t w, uint32_t h, vkn_astc_entry *out, const char **why) {
    const char *dummy; if (!why) why = &dummy;
    *why = "off";
    if (vkn_astc_open() != VKN_ASTC_READY || !storage || !bytes || bytes > UINT32_MAX || w > 65535 || h > 65535 || !levels || levels > 0xffff) return 0;
    count(&g_st.lookups, 1);
    if (!shape_known(shape_of((uint32_t)bytes, (uint16_t)nvn, (uint16_t)levels, (uint16_t)w, (uint16_t)h))) { count(&g_st.miss_shape, 1); *why = "shape"; return 0; }
    uint32_t crc = 0, adler = 0; const uint64_t t0 = vkn_now_ns();
    vkn_shlib_astc_key(storage, bytes, &crc, &adler);
    count(&g_st.hash_ns, vkn_now_ns() - t0); count(&g_st.hash_bytes, bytes);
    const int64_t i = vkn_astc_find(g_idx, g_n, crc, adler, (uint32_t)bytes, (uint16_t)nvn, (uint16_t)levels, (uint16_t)w, (uint16_t)h);
    if (i < 0) { count(&g_st.miss_key, 1); *why = "key"; return 0; }
    const vkn_astc_entry *e = &g_idx[i];
    if (!g_st.block_ok[e->bw - 4]) { count(&g_st.miss_block, 1); *why = "block"; return 0; }
    *out = *e; count(&g_st.hits, 1); *why = NULL;
    return 1;
}

static __thread uint8_t *t_bounce;   // per thread, like the translator's thread_local bounce (kept for the thread's life)
int vkn_astc_read(const vkn_astc_entry *e, uint8_t *dst) {
    if (!t_bounce) t_bounce = (uint8_t *)malloc(READ_CHUNK);
    const int fd = g_fd; const uint64_t t0 = vkn_now_ns(), bytes = e->data_bytes;
    uint64_t got = 0; int err = 0, stop = fd < 0; unsigned fixed = 0;
    for (uint64_t o = 0; o < bytes;) {
        // NR-18: read and sanitize in the cached bounce buffer, one sequential copy into dst (dst only written); without a bounce
        // buffer (no memory) straight into dst
        const size_t n = bytes - o < READ_CHUNK ? (size_t)(bytes - o) : (size_t)READ_CHUNK;
        uint8_t *const w = t_bounce ? t_bounce : dst + o; size_t have = 0;
        while (!stop && have < n) {
            const ssize_t k = pread(fd, w + have, n - have, (off_t)(e->data_off + o + have));
            if (k <= 0) { stop = 1; err = k < 0 ? errno : 0; break; }
            have += (size_t)k;
        }
        got += have;
        if (have < n) memset(w + have, 0, n - have);
        fixed += vkn_astc_sanitize(w, n);
        if (t_bounce) memcpy(dst + o, t_bounce, n);
        o += n;
    }
    count(&g_st.read_ns, vkn_now_ns() - t0); count(&g_st.reads, 1); count(&g_st.read_bytes, bytes); count(&g_st.zero_fixed, fixed);
    if (got < bytes) {
        count(&g_st.read_fails, 1);
        if (__atomic_fetch_add(&g_read_red, 1, __ATOMIC_RELAXED) < 8)
            vkn_logf(RED "astc: read of the entry at %llu failed at %llu of %llu bytes (errno %d): the rest is transparent black", (unsigned long long)e->data_off,
                     (unsigned long long)got, (unsigned long long)bytes, err);
        return -1;
    }
    return 0;
}

void vkn_astc_get_stats(vkn_astc_stats *s) {
    pthread_mutex_lock(&g_mu);
    *s = g_st; s->state = g_state;
    pthread_mutex_unlock(&g_mu);
}
void vkn_astc_reset(void) {
    pthread_mutex_lock(&g_mu);
    if (g_fd >= 0) close(g_fd);
    g_fd = -1; free(g_idx); free(g_shape); g_idx = NULL; g_shape = NULL; g_n = g_nshape = 0; g_read_red = 0;
    memset(&g_st, 0, sizeof g_st);
    __atomic_store_n(&g_state, VKN_ASTC_UNOPENED, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_mu);
}
#endif
