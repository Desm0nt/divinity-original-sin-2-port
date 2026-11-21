// [DOS2] VK-NATIVE core (vkn.h): mode, device, arenas, frames / deferred destruction, upload queue, one-shot command buffers.
// No game addresses, no NVN. Thread safety: one mutex per part (arenas, retire list, upload queue, one-shot command buffer).
#ifdef DM_GAME_DOS2
#include "vkn.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// The front end (android/vulkan_present.cpp). Weak: host tests and other front ends link without them (then there is no device).
extern int dm_vk_handles(VkInstance *i, VkPhysicalDevice *p, VkDevice *d, VkQueue *q, uint32_t *f) __attribute__((weak));
extern void dm_present_queue_lock(void) __attribute__((weak));
extern void dm_present_queue_unlock(void) __attribute__((weak));

static int g_mode;
int vkn_mode(void) { return g_mode; }
void vkn_set_mode(int mode) { g_mode = mode == VKN_NATIVE || mode == VKN_ALONGSIDE ? mode : VKN_OFF; }
void vkn_logf(const char *fmt, ...) {
    char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    const int red = !strncmp(b, "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91", 9);   // "【红】" first: the red-line scanners look at the line start
    if (red) fprintf(stderr, "[dm] \xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91[vkn] %s\n", b + 9); else fprintf(stderr, "[dm] [vkn] %s\n", b);
}
uint64_t vkn_now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }

// ---- device ----
static vkn_dev g_dev; static int g_dev_ok; static pthread_mutex_t g_dev_mu = PTHREAD_MUTEX_INITIALIZER;
const vkn_dev *vkn_device(void) {
    if (__atomic_load_n(&g_dev_ok, __ATOMIC_ACQUIRE)) return &g_dev;
    if (!dm_vk_handles) return NULL;
    pthread_mutex_lock(&g_dev_mu);
    if (!g_dev_ok && dm_vk_handles(&g_dev.instance, &g_dev.phys, &g_dev.dev, &g_dev.queue, &g_dev.family)) {
        vkGetPhysicalDeviceMemoryProperties(g_dev.phys, &g_dev.mem);
        vkGetPhysicalDeviceProperties(g_dev.phys, &g_dev.props);
        vkGetPhysicalDeviceFeatures(g_dev.phys, &g_dev.feat);
        __atomic_store_n(&g_dev_ok, 1, __ATOMIC_RELEASE);
        vkn_logf("device '%s' (vendor %#x, driver %#x), %u memory types, queue family %u", g_dev.props.deviceName, g_dev.props.vendorID, g_dev.props.driverVersion, g_dev.mem.memoryTypeCount, g_dev.family);
    }
    pthread_mutex_unlock(&g_dev_mu);
    return g_dev_ok ? &g_dev : NULL;
}
void vkn_queue_lock(void) { if (dm_present_queue_lock) dm_present_queue_lock(); }
void vkn_queue_unlock(void) { if (dm_present_queue_unlock) dm_present_queue_unlock(); }
int vkn_mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer) {
    const vkn_dev *d = vkn_device(); if (!d) return -1;
    for (int pass = 0; pass < 2; pass++) {
        const VkMemoryPropertyFlags need = pass ? want : want | prefer;
        for (uint32_t i = 0; i < d->mem.memoryTypeCount; i++)
            if ((bits >> i & 1) && (d->mem.memoryTypes[i].propertyFlags & need) == need) return (int)i;
    }
    return -1;
}
int vkn_image_mem(VkImage img, VkDeviceMemory *out, uint64_t *bytes) {
    const vkn_dev *d = vkn_device(); if (!d) return -1;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(d->dev, img, &mr);
    const int t = vkn_mem_type(mr.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); if (t < 0) return -1;
    const VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)t};
    if (vkAllocateMemory(d->dev, &ai, NULL, out) != VK_SUCCESS) return -1;
    if (vkBindImageMemory(d->dev, img, *out, 0) != VK_SUCCESS) { vkFreeMemory(d->dev, *out, NULL); *out = VK_NULL_HANDLE; return -1; }
    if (bytes) *bytes = mr.size;
    return 0;
}

// ---- arenas: chunks with a first-fit free list of extents (sorted by offset, merged on free) ----
enum { VKN_CHUNK = 16u << 20, VKN_CHUNKS = 256, VKN_EXTENTS = 4096 };
typedef struct vkn_ext { uint64_t off, size; } vkn_ext;
typedef struct vkn_chunk { VkBuffer buf; VkDeviceMemory mem; void *cpu; uint64_t size, used; vkn_ext *fr; uint32_t nfr, capfr; uint8_t cls, live; } vkn_chunk;
static vkn_chunk g_chunk[VKN_CHUNKS]; static pthread_mutex_t g_arena_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_arena_used[VKN_ARENAS], g_arena_res[VKN_ARENAS];

static int chunk_new(int cls, uint64_t size) {
    const vkn_dev *d = vkn_device(); if (!d) return -1;
    int ci = -1; for (int i = 0; i < VKN_CHUNKS; i++) if (!g_chunk[i].live) { ci = i; break; }
    if (ci < 0) return -1;
    vkn_chunk *c = &g_chunk[ci]; memset(c, 0, sizeof *c);
    const VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    if (vkCreateBuffer(d->dev, &bi, NULL, &c->buf) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(d->dev, c->buf, &mr);
    // Cached memory first: the engine reads its buffers back (buffer-to-buffer copies, opcode 0x2f); coherent so no flush calls are needed.
    const int t = vkn_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    const VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)(t < 0 ? 0 : t)};
    if (t < 0 || vkAllocateMemory(d->dev, &ai, NULL, &c->mem) != VK_SUCCESS) { vkDestroyBuffer(d->dev, c->buf, NULL); return -1; }
    if (vkBindBufferMemory(d->dev, c->buf, c->mem, 0) != VK_SUCCESS || vkMapMemory(d->dev, c->mem, 0, VK_WHOLE_SIZE, 0, &c->cpu) != VK_SUCCESS) {
        vkDestroyBuffer(d->dev, c->buf, NULL); vkFreeMemory(d->dev, c->mem, NULL); return -1; }
    c->capfr = 64; c->fr = (vkn_ext *)malloc(sizeof(vkn_ext) * c->capfr);
    if (!c->fr) { vkDestroyBuffer(d->dev, c->buf, NULL); vkFreeMemory(d->dev, c->mem, NULL); return -1; }
    c->fr[0] = (vkn_ext){0, size}; c->nfr = 1; c->size = size; c->cls = (uint8_t)cls; c->live = 1;
    g_arena_res[cls] += size;
    return ci;
}
static void chunk_drop(vkn_chunk *c) {
    const vkn_dev *d = vkn_device();
    g_arena_res[c->cls] -= c->size;
    if (d) { vkDestroyBuffer(d->dev, c->buf, NULL); vkFreeMemory(d->dev, c->mem, NULL); }   // freeing mapped memory unmaps it
    free(c->fr); memset(c, 0, sizeof *c);
}
static int chunk_take(vkn_chunk *c, uint64_t size, uint64_t align, uint64_t *off) {
    for (uint32_t i = 0; i < c->nfr; i++) {
        const uint64_t a = (c->fr[i].off + align - 1) & ~(align - 1), end = c->fr[i].off + c->fr[i].size;
        if (a + size > end) continue;
        const uint64_t head = a - c->fr[i].off, tail = end - (a + size);
        if (head && tail) {
            if (c->nfr == c->capfr) { if (c->capfr >= VKN_EXTENTS) continue; vkn_ext *n = (vkn_ext *)realloc(c->fr, sizeof(vkn_ext) * c->capfr * 2); if (!n) return 0; c->fr = n; c->capfr *= 2; }
            memmove(&c->fr[i + 2], &c->fr[i + 1], sizeof(vkn_ext) * (c->nfr - i - 1)); c->nfr++;
            c->fr[i].size = head; c->fr[i + 1] = (vkn_ext){a + size, tail};
        } else if (head) c->fr[i].size = head;
        else if (tail) c->fr[i] = (vkn_ext){a + size, tail};
        else { memmove(&c->fr[i], &c->fr[i + 1], sizeof(vkn_ext) * (c->nfr - i - 1)); c->nfr--; }
        *off = a; c->used += size;
        return 1;
    }
    return 0;
}
int vkn_block_alloc(int cls, uint64_t size, uint32_t align, vkn_block *out) {
    if (cls < 0 || cls >= VKN_ARENAS || !size || !out) return -1;
    uint64_t al = align ? align : 16; if (al & (al - 1)) return -1;
    size = (size + 15) & ~15ull;
    int rc = -1; uint64_t off = 0;
    pthread_mutex_lock(&g_arena_mu);
    int ci = -1;
    for (int i = 0; i < VKN_CHUNKS && ci < 0; i++) if (g_chunk[i].live && g_chunk[i].cls == cls && g_chunk[i].size - g_chunk[i].used >= size && chunk_take(&g_chunk[i], size, al, &off)) ci = i;
    if (ci < 0) {
        ci = chunk_new(cls, size > VKN_CHUNK ? (size + 0xffff) & ~0xffffull : VKN_CHUNK);
        if (ci >= 0 && !chunk_take(&g_chunk[ci], size, al, &off)) ci = -1;
    }
    if (ci >= 0) {
        vkn_chunk *c = &g_chunk[ci];
        *out = (vkn_block){c->buf, off, size, (uint8_t *)c->cpu + off, (uint32_t)ci, (uint8_t)cls};
        g_arena_used[cls] += size; rc = 0;
    }
    pthread_mutex_unlock(&g_arena_mu);
    return rc;
}
void vkn_block_free(const vkn_block *b) {
    if (!b || !b->size || b->chunk >= VKN_CHUNKS) return;
    pthread_mutex_lock(&g_arena_mu);
    vkn_chunk *c = &g_chunk[b->chunk];
    if (c->live && c->buf == b->buf) {
        uint32_t i = 0; while (i < c->nfr && c->fr[i].off < b->off) i++;
        const int ml = i > 0 && c->fr[i - 1].off + c->fr[i - 1].size == b->off, mr = i < c->nfr && b->off + b->size == c->fr[i].off;
        if (ml && mr) { c->fr[i - 1].size += b->size + c->fr[i].size; memmove(&c->fr[i], &c->fr[i + 1], sizeof(vkn_ext) * (c->nfr - i - 1)); c->nfr--; }
        else if (ml) c->fr[i - 1].size += b->size;
        else if (mr) { c->fr[i].off = b->off; c->fr[i].size += b->size; }
        else if (c->nfr < c->capfr || (c->capfr < VKN_EXTENTS && (c->fr = (vkn_ext *)realloc(c->fr, sizeof(vkn_ext) * (c->capfr *= 2))))) {
            memmove(&c->fr[i + 1], &c->fr[i], sizeof(vkn_ext) * (c->nfr - i)); c->fr[i] = (vkn_ext){b->off, b->size}; c->nfr++; }
        // (an extent list that cannot grow leaks the block inside its chunk: counted as used, never handed out twice)
        c->used -= b->size; g_arena_used[c->cls] -= b->size;
        if (!c->used && c->size != VKN_CHUNK) chunk_drop(c);   // a dedicated chunk goes with its block; 16 MB chunks stay for reuse
    }
    pthread_mutex_unlock(&g_arena_mu);
}
void vkn_arena_stats(uint64_t used[VKN_ARENAS], uint64_t reserved[VKN_ARENAS]) {
    pthread_mutex_lock(&g_arena_mu);
    for (int i = 0; i < VKN_ARENAS; i++) { used[i] = g_arena_used[i]; reserved[i] = g_arena_res[i]; }
    pthread_mutex_unlock(&g_arena_mu);
}

// ---- frames, deferred destruction ----
enum { VKN_RETIRE_DELAY = 3 };
typedef struct vkn_ret { void (*fn)(void *); void *ctx; uint64_t frame; struct vkn_ret *next; } vkn_ret;
static vkn_ret *g_ret_head, *g_ret_tail; static pthread_mutex_t g_ret_mu = PTHREAD_MUTEX_INITIALIZER; static uint64_t g_frame;
uint64_t vkn_frame(void) { return __atomic_load_n(&g_frame, __ATOMIC_RELAXED); }
void vkn_retire(void (*fn)(void *ctx), void *ctx) {
    vkn_ret *r = (vkn_ret *)malloc(sizeof *r); if (!r) return;   // no memory: the object leaks (never freed early)
    r->fn = fn; r->ctx = ctx; r->next = NULL;
    pthread_mutex_lock(&g_ret_mu);
    r->frame = g_frame;
    if (g_ret_tail) g_ret_tail->next = r; else g_ret_head = r;
    g_ret_tail = r;
    pthread_mutex_unlock(&g_ret_mu);
}
static void retire_run(int all) {
    for (;;) {
        pthread_mutex_lock(&g_ret_mu);
        vkn_ret *r = g_ret_head;
        if (r && (all || r->frame + VKN_RETIRE_DELAY <= g_frame)) { g_ret_head = r->next; if (!g_ret_head) g_ret_tail = NULL; } else r = NULL;
        pthread_mutex_unlock(&g_ret_mu);
        if (!r) return;
        r->fn(r->ctx); free(r);
    }
}
void vkn_frame_end(void) { __atomic_add_fetch(&g_frame, 1, __ATOMIC_RELAXED); retire_run(0); }
void vkn_retire_all(void) { retire_run(1); }
static void retire_block_fn(void *ctx) { vkn_block_free((vkn_block *)ctx); free(ctx); }
void vkn_retire_block(const vkn_block *b) {
    vkn_block *c = (vkn_block *)malloc(sizeof *c); if (!c) return;
    *c = *b; vkn_retire(retire_block_fn, c);
}

// ---- upload queue ----
typedef struct vkn_up { vkn_upload_fn fn; void *ctx; uint64_t bytes; struct vkn_up *next; } vkn_up;
static vkn_up *g_up_head, *g_up_tail; static uint64_t g_up_bytes; static pthread_mutex_t g_up_mu = PTHREAD_MUTEX_INITIALIZER;
void vkn_upload_push(vkn_upload_fn fn, void *ctx, uint64_t bytes) {
    vkn_up *u = (vkn_up *)malloc(sizeof *u); if (!u) return;
    u->fn = fn; u->ctx = ctx; u->bytes = bytes; u->next = NULL;
    pthread_mutex_lock(&g_up_mu);
    if (g_up_tail) g_up_tail->next = u; else g_up_head = u;
    g_up_tail = u; __atomic_store_n(&g_up_bytes, g_up_bytes + bytes, __ATOMIC_RELAXED);   // [rt2] read lock-free by vkn_upload_pending_bytes (a loader thread pushes while the recording thread reads)
    pthread_mutex_unlock(&g_up_mu);
}
uint64_t vkn_upload_pending_bytes(void) { return __atomic_load_n(&g_up_bytes, __ATOMIC_RELAXED); }
int vkn_upload_run(VkCommandBuffer cb) {
    pthread_mutex_lock(&g_up_mu);
    vkn_up *u = g_up_head; g_up_head = g_up_tail = NULL; __atomic_store_n(&g_up_bytes, 0, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&g_up_mu);
    int n = 0;
    while (u) { vkn_up *nx = u->next; u->fn(cb, u->ctx); free(u); u = nx; n++; }
    return n;
}

// ---- one-shot command buffers ----
static VkCommandPool g_once_pool; static VkCommandBuffer g_once_cb; static VkFence g_once_fence; static pthread_mutex_t g_once_mu = PTHREAD_MUTEX_INITIALIZER;
VkCommandBuffer vkn_once_begin(void) {
    const vkn_dev *d = vkn_device(); if (!d) return VK_NULL_HANDLE;
    pthread_mutex_lock(&g_once_mu);
    if (!g_once_pool) {
        const VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = d->family};
        const VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateCommandPool(d->dev, &pi, NULL, &g_once_pool) != VK_SUCCESS) { g_once_pool = VK_NULL_HANDLE; pthread_mutex_unlock(&g_once_mu); return VK_NULL_HANDLE; }
        const VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_once_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        if (vkAllocateCommandBuffers(d->dev, &ai, &g_once_cb) != VK_SUCCESS || vkCreateFence(d->dev, &fi, NULL, &g_once_fence) != VK_SUCCESS) {
            vkDestroyCommandPool(d->dev, g_once_pool, NULL); g_once_pool = VK_NULL_HANDLE; pthread_mutex_unlock(&g_once_mu); return VK_NULL_HANDLE; }
    }
    const VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (vkResetCommandBuffer(g_once_cb, 0) != VK_SUCCESS || vkBeginCommandBuffer(g_once_cb, &bi) != VK_SUCCESS) { pthread_mutex_unlock(&g_once_mu); return VK_NULL_HANDLE; }
    return g_once_cb;   // g_once_mu stays held until vkn_once_end
}
int vkn_once_end(VkCommandBuffer cb) {
    const vkn_dev *d = vkn_device(); int rc = -1;
    if (d && cb == g_once_cb && vkEndCommandBuffer(cb) == VK_SUCCESS) {
        const VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
        vkResetFences(d->dev, 1, &g_once_fence);
        vkn_queue_lock(); const VkResult r = vkQueueSubmit(d->queue, 1, &si, g_once_fence); vkn_queue_unlock();
        if (r == VK_SUCCESS && vkWaitForFences(d->dev, 1, &g_once_fence, VK_TRUE, 10000000000ull) == VK_SUCCESS) rc = 0;
    }
    pthread_mutex_unlock(&g_once_mu);
    return rc;
}
int vkn_upload_flush(void) {
    if (!vkn_upload_pending_bytes() && !g_up_head) return 0;
    VkCommandBuffer cb = vkn_once_begin(); if (!cb) return 0;
    const int n = vkn_upload_run(cb);
    if (vkn_once_end(cb)) vkn_logf("\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91upload flush: submit or wait failed (%d uploads)", n);
    return n;
}

// ---- handle tables ----
int vkn_tab_init(vkn_tab *t, uint32_t n, uint32_t rec_bytes) {
    memset(t, 0, sizeof *t);
    t->rec = calloc(n, rec_bytes); t->handle = (uint64_t *)calloc(n, sizeof(uint64_t));
    if (!t->rec || !t->handle) { free(t->rec); free(t->handle); memset(t, 0, sizeof *t); return -1; }
    t->n = n; t->rec_bytes = rec_bytes;
    return 0;
}
void *vkn_tab_at(const vkn_tab *t, uint32_t idx) { return idx < t->n ? (uint8_t *)t->rec + (size_t)idx * t->rec_bytes : NULL; }
void *vkn_tab_put(vkn_tab *t, uint64_t handle, int *was_alive) {
    const uint32_t i = vkn_h_idx(handle);
    if (i >= t->n) { t->overflow++; return NULL; }
    if (was_alive) *was_alive = t->handle[i] != 0;
    if (t->handle[i]) t->replaced++;
    t->handle[i] = handle ? handle : 1; t->puts++;
    return (uint8_t *)t->rec + (size_t)i * t->rec_bytes;
}
void *vkn_tab_get(const vkn_tab *t, uint64_t handle) {
    const uint32_t i = vkn_h_idx(handle);
    return i < t->n && handle && t->handle[i] == handle ? (uint8_t *)t->rec + (size_t)i * t->rec_bytes : NULL;
}
int vkn_tab_kill(vkn_tab *t, uint64_t handle) {
    const uint32_t i = vkn_h_idx(handle);
    if (i >= t->n || !handle || t->handle[i] != handle) return 0;
    t->handle[i] = 0; t->kills++;
    return 1;
}
#endif
