// [DOS2] VK-NATIVE stage 4: the setup ASTC texture pack for GPUs without BC (vkn_astc.c). The old path's data path
// (xbg_game.cpp namespace astccache, NR-18) re-done for vkn_tex: a BC1..BC3 2D texture the engine creates from packaged (Switch
// block-linear) storage is looked up in the pack by the key of its storage bytes; a hit gives the ASTC blocks of every level, which
// vkn_tex uploads into an image of the entry's ASTC format instead of decoding the BC on the CPU (4x / 8x less image memory).
//
// The pack: <data>/astc/dos2-astc.pack, built on the PC by tools/dos2_astc_cache.py (setup step "astc") from the game's RomFS.
// Path: $DM_ASTC_CACHE_PATH, else $DM_ASTC_DATA_DIR/astc/dos2-astc.pack, else <parent of $DM_RUNTIME_DIR>/astc/dos2-astc.pack (the
// translator's data_root(): two folders above <runtime>/shaders, i.e. Download/DivinityOriginalSin2Mobile). Read only, never written.
// File 'D3AC' v1, little-endian (the Diablo III pack format; xbg_astc_cache.h is the C++ reader of the translator, this is the same
// format re-implemented in C because that header needs C++):
//   header, 64 bytes:  char magic[4] "D3AC", u32 version (1), u32 entries, u32 flags, u64 index_off, u64 data_off (64), u64 data_bytes,
//                      24 bytes zero. data_off + data_bytes == index_off; index_off + 48 * entries == file size.
//   data:              the ASTC blocks of every texture, level 0 first, each level tight rows of 16-byte blocks
//                      (ceil(w_l / bw) x ceil(h_l / bh), w_l = max(1, w >> l)). No all-zero block (the tool writes the void extent of
//                      transparent black instead, KB TEX-04; the reader replaces any it still finds).
//   index, 48 bytes an entry, sorted by (crc, adler, bytes, format, width, height, levels):
//                      u32 crc32, u32 adler32, u32 bytes (storage bytes the key was taken over), u16 NVN format, u16 levels, u16 width,
//                      u16 height, u8 block w, u8 block h (4x4 / 5x5 / 6x6), u16 flags (preset), u64 data_off (from the file start),
//                      u32 data_bytes (= the sum of the levels' bytes), u32 PSNR x100 of mip 0, 8 bytes pad.
// The key (dos2_astc_cache.py key_of, xbg_game.cpp astccache::key / cacheable): CRC-32 and Adler-32 (zlib) of the Switch storage
// as the NVN texture-packager loader put it in memory (the XTX storage block: nvn_tex_layout(...).size bytes from the image's storage
// pointer), that byte count, the NVN format the engine gave the texture builder (BC1..BC3, UNORM 0x42..0x44 and their sRGB twins
// 0x46..0x48: the tool writes both entries over the same blocks), width, height, levels. Only 2D single-layer single-sample textures.
// The key is computed by xbg_astc_storage_key (xbg_game.cpp, extern "C", pure: zlib over the bytes; the same function the NR-05
// worker uses) on the calling thread - an engine loader thread for CreateTextureFromImage. Lookups go through a sorted shape filter
// first (bytes, format, levels, w, h), so storage whose shape no entry has is never hashed.
// Thread safety: the pack opens once (first lookup, under a mutex); the index is read-only after that; reads are pread on one fd
// through a 256 KB per-thread bounce buffer (NR-18: the staging map may be write-combined, never read it back).
#ifndef DM_VKN_ASTC_H
#define DM_VKN_ASTC_H
#include <stdint.h>
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct vkn_astc_hdr { char magic[4]; uint32_t version, entries, flags; uint64_t index_off, data_off, data_bytes; uint8_t pad[24]; } vkn_astc_hdr;
typedef struct vkn_astc_entry {
    uint32_t crc, adler, bytes; uint16_t format, levels, width, height; uint8_t bw, bh; uint16_t flags;
    uint64_t data_off; uint32_t data_bytes, psnr_x100; uint8_t pad[8];
} vkn_astc_entry;
_Static_assert(sizeof(vkn_astc_hdr) == 64 && sizeof(vkn_astc_entry) == 48, "D3AC layout");

// ---- pure pieces of the format (no file, no device) ----
int vkn_astc_header_check(const vkn_astc_hdr *h, uint64_t file_bytes, const char **why);   // 0 = usable header
uint64_t vkn_astc_level_bytes(const vkn_astc_entry *e, unsigned level);                    // tight 16-byte blocks of one level
int vkn_astc_entry_ok(const vkn_astc_entry *e, const vkn_astc_hdr *h);                     // 1 = levels add up, inside the data, 4x4 / 5x5 / 6x6
int64_t vkn_astc_find(const vkn_astc_entry *idx, uint32_t n, uint32_t crc, uint32_t adler, uint32_t bytes, uint16_t format, uint16_t levels,
                      uint16_t w, uint16_t h);                                             // index of the entry with this key, -1 = none
VkFormat vkn_astc_vkformat(uint8_t bw, uint8_t bh, int srgb);                              // UNDEFINED for another block size
int vkn_astc_srgb(uint32_t nvn);                                                           // 1 = an sRGB BC format (0x45..0x48, 0x4e)
// The textures the pack can hold (astccache::cacheable): NVN format 0x41..0x48, 2D target (1), one layer, storage bytes 1..4 GB - 1,
// width and height <= 65535. samples: 1 only.
int vkn_astc_cacheable(uint32_t nvn, uint32_t target, uint32_t layers, uint32_t samples, uint64_t bytes, uint32_t w, uint32_t h);
unsigned vkn_astc_sanitize(uint8_t *blocks, uint64_t bytes);   // every all-zero 16-byte block -> the void extent of transparent black

// ---- the pack ----
enum { VKN_ASTC_UNOPENED = 0, VKN_ASTC_READY = 1, VKN_ASTC_NONE = 2, VKN_ASTC_BAD = 3 };
// Opens the pack on the first call (later calls return the state): READY, NONE (no file, DM_VK_NATIVE_ASTC=0, the device has no
// usable ASTC block size, no key function in this build: one plain log line), BAD (there but unusable: one red line).
int vkn_astc_open(void);
// A cacheable texture's storage -> its entry (copied to *out): 1 = hit. 0 = miss (*why: "shape" no entry of this shape, "key" no
// entry with these bytes, "block" the entry's block size is not usable on this device, "off" the pack is not READY).
int vkn_astc_lookup(const void *storage, uint64_t bytes, uint32_t nvn, uint32_t levels, uint32_t w, uint32_t h, vkn_astc_entry *out, const char **why);
// The entry's data_bytes into dst (written sequentially, never read back): 0 = complete; -1 = a short read (error or end of file),
// the rest zero-filled and turned into transparent black like the translator (counted, the first 8 logged red).
int vkn_astc_read(const vkn_astc_entry *e, uint8_t *dst);

typedef struct vkn_astc_stats {
    int state; uint32_t entries; uint64_t pack_bytes; uint8_t block_ok[3];        // block_ok: 4x4, 5x5, 6x6 usable on the device
    uint64_t lookups, hits, miss_shape, miss_key, miss_block;
    uint64_t hash_bytes, hash_ns, reads, read_bytes, read_ns, read_fails, zero_fixed;
} vkn_astc_stats;
void vkn_astc_get_stats(vkn_astc_stats *s);
// Tests only: closes the pack and forgets its state (the next lookup opens it again, reading the environment anew).
void vkn_astc_reset(void);

#ifdef __cplusplus
}
#endif
#endif
