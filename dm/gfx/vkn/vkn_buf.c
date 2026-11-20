// [DOS2] VK-NATIVE buffers (vkn_buf.h): engine vertex / index buffers as blocks of the static arena, the constant ring.
// No game addresses, no NVN. One mutex for the tables and counters (loader threads create, the render thread destroys and draws),
// one for the ring (constant flushes may come from several command-list threads).
#ifdef DM_GAME_DOS2
#include "vkn_buf.h"
#include <pthread.h>
#include <string.h>

#define VKN_BUF_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"   // the red-line mark as a literal of its own (the next letter may be a hex digit)

typedef struct vkn_buf_rec { vkn_buf_info i; } vkn_buf_rec;
static vkn_tab g_tab[VKN_BUF_KINDS]; static int g_tab_ok;
static vkn_buf_stats g_st;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
// chunk index -> its VkBuffer, as last seen by a create (a chunk with a live block of ours is never dropped, so the entry of every
// chunk a live pseudo address names is current).
enum { VKN_BUF_CHUNKS = 4096 };
static VkBuffer g_chunk_buf[VKN_BUF_CHUNKS];

static int tabs_locked(void) {   // g_mu held
    if (g_tab_ok) return g_tab_ok > 0;
    // 2 x 1M records of 64 bytes: calloc'd, so only the pages of indices in use are ever touched.
    if (vkn_tab_init(&g_tab[VKN_BUF_VB], VKN_BUF_TAB, sizeof(vkn_buf_rec)) || vkn_tab_init(&g_tab[VKN_BUF_IB], VKN_BUF_TAB, sizeof(vkn_buf_rec))) {
        vkn_logf(VKN_BUF_RED "buffers: no memory for the handle tables");
        g_tab_ok = -1; return 0;
    }
    g_tab_ok = 1;
    return 1;
}

// Forget a live record (g_mu held): counters, and its block to the retire list.
static void rec_drop_locked(int kind, vkn_buf_rec *r) {
    g_st.live[kind]--; g_st.live_bytes[kind] -= r->i.size;
    if (r->i.blk.size) vkn_retire_block(&r->i.blk);
    memset(r, 0, sizeof *r);
}

static int put(int kind, uint64_t handle, uint64_t size, unsigned index_size, unsigned usage, int has_init, const vkn_block *blk) {
    if (kind < 0 || kind >= VKN_BUF_KINDS) return -1;
    pthread_mutex_lock(&g_mu);
    if (!tabs_locked()) { g_st.fail[kind]++; pthread_mutex_unlock(&g_mu); return -1; }
    vkn_tab *t = &g_tab[kind];
    // An old record under the same index (a create over a live handle, or a destroy we never saw): retire it first.
    vkn_buf_rec *old = (vkn_buf_rec *)vkn_tab_at(t, vkn_h_idx(handle));
    if (old && t->handle[vkn_h_idx(handle)]) { g_st.replaced[kind]++; rec_drop_locked(kind, old); vkn_tab_kill(t, t->handle[vkn_h_idx(handle)]); }
    vkn_buf_rec *r = (vkn_buf_rec *)vkn_tab_put(t, handle, NULL);
    if (!r) { g_st.fail[kind]++; pthread_mutex_unlock(&g_mu); return -1; }
    memset(r, 0, sizeof *r);
    if (blk) { r->i.blk = *blk; if (blk->chunk < VKN_BUF_CHUNKS) g_chunk_buf[blk->chunk] = blk->buf; }
    r->i.size = size; r->i.index_size = index_size; r->i.usage = (uint8_t)usage; r->i.kind = (uint8_t)kind; r->i.has_init = (uint8_t)!!has_init;
    g_st.created[kind]++; g_st.live[kind]++; g_st.live_bytes[kind] += size;
    if (usage == VKN_BUF_USAGE_DYNAMIC) g_st.bytes_dynamic[kind] += size; else g_st.bytes_static[kind] += size;
    if (has_init) g_st.with_init[kind]++; else g_st.without_init[kind]++;
    pthread_mutex_unlock(&g_mu);
    return 0;
}

int vkn_buf_create(int kind, uint64_t handle, uint64_t size, unsigned index_size, unsigned usage, const void *init, vkn_block *out) {
    if (kind < 0 || kind >= VKN_BUF_KINDS || vkn_h_idx(handle) >= VKN_BUF_TAB) {
        if (kind >= 0 && kind < VKN_BUF_KINDS) { pthread_mutex_lock(&g_mu); g_st.fail[kind]++; pthread_mutex_unlock(&g_mu); }
        return -1;
    }
    vkn_block b;
    // At least 16 bytes: a size-0 buffer still gets a valid, unique CPU pointer and pseudo address (NVN maps size 0 as well).
    // Alignment 256: every use the engine makes of a buffer (vertex / index / uniform / storage offset) is satisfied.
    if (vkn_block_alloc(VKN_ARENA_STATIC, size ? size : 16, 256, &b)) {
        pthread_mutex_lock(&g_mu); g_st.fail[kind]++; pthread_mutex_unlock(&g_mu);
        vkn_logf(VKN_BUF_RED "buffers: no arena memory for %s %#llx (%llu bytes)", kind == VKN_BUF_VB ? "VB" : "IB", (unsigned long long)handle, (unsigned long long)size);
        return -1;
    }
    // Without init data the block is zeroed: arena blocks are reused (stale bytes of an old buffer), and zeros make the native
    // path's first frame deterministic (fresh NVN pool memory is zero too).
    if (init && size) memcpy(b.cpu, init, size); else memset(b.cpu, 0, b.size);
    if (put(kind, handle, size, index_size, usage, init && size, &b)) { vkn_block_free(&b); return -1; }
    if (out) *out = b;
    return 0;
}

int vkn_buf_note(int kind, uint64_t handle, uint64_t size, unsigned index_size, unsigned usage, int has_init) {
    return put(kind, handle, size, index_size, usage, has_init, NULL);
}

void vkn_buf_destroy(int kind, uint64_t handle) {
    if (kind < 0 || kind >= VKN_BUF_KINDS) return;
    pthread_mutex_lock(&g_mu);
    vkn_buf_rec *r = g_tab_ok > 0 ? (vkn_buf_rec *)vkn_tab_get(&g_tab[kind], handle) : NULL;
    if (r) { rec_drop_locked(kind, r); vkn_tab_kill(&g_tab[kind], handle); g_st.destroyed[kind]++; }
    else g_st.unknown_destroy[kind]++;
    pthread_mutex_unlock(&g_mu);
}

const vkn_block *vkn_buf_get(int kind, uint64_t handle) {
    if (kind < 0 || kind >= VKN_BUF_KINDS) return NULL;
    pthread_mutex_lock(&g_mu);
    const vkn_buf_rec *r = g_tab_ok > 0 ? (const vkn_buf_rec *)vkn_tab_get(&g_tab[kind], handle) : NULL;
    const vkn_block *b = r && r->i.blk.size ? &r->i.blk : NULL;
    pthread_mutex_unlock(&g_mu);
    return b;
}

int vkn_buf_lookup(int kind, uint64_t handle, vkn_buf_info *out) {
    if (kind < 0 || kind >= VKN_BUF_KINDS) return -1;
    pthread_mutex_lock(&g_mu);
    const vkn_buf_rec *r = g_tab_ok > 0 ? (const vkn_buf_rec *)vkn_tab_get(&g_tab[kind], handle) : NULL;
    if (r && out) *out = r->i;
    pthread_mutex_unlock(&g_mu);
    return r ? 0 : -1;
}

VkIndexType vkn_buf_index_type(unsigned index_size) { return index_size == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16; }

int vkn_buf_copy_in(int kind, uint64_t handle, uint64_t dst_off, const void *src, uint64_t size) {
    if (kind < 0 || kind >= VKN_BUF_KINDS) return -1;
    int rc;
    pthread_mutex_lock(&g_mu);
    const vkn_buf_rec *r = g_tab_ok > 0 ? (const vkn_buf_rec *)vkn_tab_get(&g_tab[kind], handle) : NULL;
    if (!r || !r->i.blk.size) rc = -1;
    else if (!src || dst_off > r->i.blk.size || size > r->i.blk.size - dst_off) rc = -2;
    else { memcpy((uint8_t *)r->i.blk.cpu + dst_off, src, size); rc = 0; }   // under the lock: a concurrent destroy cannot retire it mid-copy
    pthread_mutex_unlock(&g_mu);
    return rc;
}

int vkn_buf_resolve(uint64_t va, VkBuffer *buf, uint64_t *off) {
    if ((va & 0xffff000000000000ull) != VKN_BUF_VA_TAG) return -1;
    const uint32_t c = (uint32_t)(va >> 36) & 0xfffu;
    pthread_mutex_lock(&g_mu);
    const VkBuffer b = c < VKN_BUF_CHUNKS ? g_chunk_buf[c] : VK_NULL_HANDLE;
    pthread_mutex_unlock(&g_mu);
    if (!b) return -1;
    if (buf) *buf = b;
    if (off) *off = va & 0xfffffffffull;
    return 0;
}

void vkn_buf_stats_get(vkn_buf_stats *s) { pthread_mutex_lock(&g_mu); *s = g_st; pthread_mutex_unlock(&g_mu); }
void vkn_buf_count_copy(unsigned kind, uint64_t bytes, int ok) {
    if (kind > 2) kind = 2;
    pthread_mutex_lock(&g_mu);
    g_st.copies[kind]++; g_st.copy_bytes[kind] += bytes; if (!ok) g_st.copy_fail++;
    pthread_mutex_unlock(&g_mu);
}

// ---- the constant ring ----
static vkn_block g_ring; static int g_ring_state;   // 0 not tried, 1 live, -1 failed (logged once; retried on the next call)
static uint32_t g_ring_head; static pthread_mutex_t g_ring_mu = PTHREAD_MUTEX_INITIALIZER;
static int ring_locked(void) {   // g_ring_mu held
    if (g_ring_state > 0) return 1;
    if (vkn_block_alloc(VKN_ARENA_RING, VKN_RING_BYTES, 256, &g_ring)) {
        if (!g_ring_state) vkn_logf(VKN_BUF_RED "constant ring: no memory for %u bytes", VKN_RING_BYTES);
        g_ring_state = -1; return 0;
    }
    g_ring_state = 1; g_ring_head = 0;
    vkn_logf("constant ring: %u bytes at offset %llu of its buffer", VKN_RING_BYTES, (unsigned long long)g_ring.off);
    return 1;
}
int vkn_ring_alloc(uint32_t size, uint32_t *off) {
    pthread_mutex_lock(&g_ring_mu);
    if (!ring_locked() || size >= VKN_RING_BYTES) { pthread_mutex_unlock(&g_ring_mu); pthread_mutex_lock(&g_mu); g_st.ring_fail++; pthread_mutex_unlock(&g_mu); return -1; }
    uint32_t h = (g_ring_head + 255u) & ~255u, wrap = 0;
    if ((uint64_t)h + size >= VKN_RING_BYTES) { h = 0; wrap = 1; }
    g_ring_head = h + size;
    pthread_mutex_unlock(&g_ring_mu);
    if (off) *off = h;
    pthread_mutex_lock(&g_mu); g_st.ring_allocs++; g_st.ring_wraps += wrap; g_st.ring_bytes += size; pthread_mutex_unlock(&g_mu);
    return 0;
}
VkBuffer vkn_ring_buffer(void) { pthread_mutex_lock(&g_ring_mu); const VkBuffer b = g_ring_state > 0 ? g_ring.buf : VK_NULL_HANDLE; pthread_mutex_unlock(&g_ring_mu); return b; }
void *vkn_ring_cpu(void) { pthread_mutex_lock(&g_ring_mu); void *p = ring_locked() ? g_ring.cpu : NULL; pthread_mutex_unlock(&g_ring_mu); return p; }
uint64_t vkn_ring_base(void) { pthread_mutex_lock(&g_ring_mu); const uint64_t o = g_ring_state > 0 ? g_ring.off : 0; pthread_mutex_unlock(&g_ring_mu); return o; }
#endif
