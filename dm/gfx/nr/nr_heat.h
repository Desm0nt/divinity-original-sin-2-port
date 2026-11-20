// [DOS2] NR-HEAT DM_NR_HEAT (docs/dos2/nr/NR-HEAT.md): the always-on shader / pass heat logger. Pure C11: no Vulkan, no getenv, no
// threads, no host. The engine side is dm/gfx/vulkan/engine/xbg_nr_heat.inc (frame sampling on the pass profiler's records, the writer
// thread), the location side dm/host/hle_dos2_heat.c (which level the game reads: LSPK file tables of the level packages), the PC side
// tools/dos2_shader_heat.py (ranking). Host test: dm/gfx/vulkan/engine/tests/nr_heat_test.c.
//
// What lives here:
//   - LSPK v13 package file tables (LZ4 block) -> sorted spans {offset, end, level} of every file under .../Levels/<name>/ or
//     .../Globals/<name>/, level names interned (nrh_names), offset -> level lookup;
//   - the location vote: bytes the game read per level since the last step, decayed scores, a switch only for a clear winner;
//   - shader key ids (16-hex pack keys as their value, any other key string hashed);
//   - the aggregation of sampled frames: rows {level, pass signature (kind, WxH, format, attachments), VS, FS / CS} with frames,
//     passes, draws, vertices and three GPU time attributions (draw share, vertex share, whole pass);
//   - the file format (one line per record, tab separated) and the capped, rotated output file.
//
// File shader-heat.tsv (append only; tools/dos2_shader_heat.py reads it; fields after the record type, times in microseconds):
//   H  v=1 key=value ...                                   a writer start / a new file after rotation (mode, period, GPU, driver, app)
//   L  unix_s  t_s  level  score_kb  previous_level        the location changed (t_s = seconds since the process started)
//   F  unix_s  t_s  level  frame  slots  passes  cpasses  draws  span_us  pass_us  compute_us  fps
//                                                          one sampled frame (fps = presents / s since the previous sample)
//   P  frame  i  kind  us  WxH  fmt  att  draws  verts  vs/fs/draws/verts,...      one pass of a sampled frame (DM_NR_HEAT=2 only)
//   S  key  stage  words  src  inst  alu  trans  tex  branch  loops  ubo  ubo_dyn  bitcast  cost
//                                                          a shader key seen in the rows below (once per file): src x = translated,
//                                                          nr = NR-04 engine shader table (DM_NR_SH), f = fused post bundle (NB-18);
//                                                          then the static counts of the SPIR-V the device got (nrh_spirv_cost; '-' unknown)
//   B  unix_s  t_s  level  frames  passes  draws  span_us  pass_us  compute_us     a block: the level's totals since the last block
//   A  kind  WxH  fmt  att  vs  fs  frames  passes  draws  verts  us_draw  us_vert  us_pass  us_cost
//                                                          a row of that block (sums since the last block; kind p = pass, c = compute,
//                                                          vs / fs = '-' when none, cs in the fs column): us_draw = the passes' time
//                                                          split by draw count, us_vert by vertices, us_pass = the whole passes' time,
//                                                          us_cost by draws x (FS + VS static cost) inside each pass
#ifndef DM_NR_HEAT_H
#define DM_NR_HEAT_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif

#define NRH_LEVELS 512        // interned level names (DOS2 packages hold ~330 distinct ones)
#define NRH_NAME 48           // bytes of a name, NUL included (longer names are cut)
#define NRH_FORMAT_VERSION 1

// ---- level names ----
typedef struct nrh_names { char name[NRH_LEVELS][NRH_NAME]; uint32_t n; } nrh_names;
int nrh_intern(nrh_names *t, const char *s, size_t n);   // index of the name (added when new), -1 = table full / empty name
// The level a package path belongs to: the component after "/Levels/" or "/Globals/" (both DOS2 layouts: Mods/<mod>/Levels/<name>/...,
// Mods/<mod>/Globals/<name>/...). 1 = found (*lv, *ln = the name inside `path`), 0 = not a level file.
int nrh_level_of_path(const char *path, size_t n, const char **lv, size_t *ln);

// ---- LSPK v13 ----
int nrh_lz4(const uint8_t *s, size_t sn, uint8_t *d, size_t dn);   // LZ4 block; bytes written, -1 = corrupt / does not fit
// The last 8 bytes of a package: u32 header size + "LSPK". 0 = ok.
int nrh_lspk_tail(const uint8_t t[8], uint32_t *hsize);
// The header (at file size - hsize): u32 version 13, u32 list offset, u32 list size, ... 0 = ok.
int nrh_lspk_header(const uint8_t *h, size_t n, uint32_t *list_off, uint32_t *list_size);
typedef struct nrh_span { uint64_t off, end; uint32_t level; } nrh_span;
// The file list (u32 count + LZ4 block of count x 280-byte entries {name[256], u32 offset, u32 size on disk, u32 size, u32 part,
// u32 flags, u32 crc}) -> malloc'd spans of the level files of archive part 0 (sorted by offset, empty ones dropped), names interned.
// Returns the number of spans (0 = none, *out NULL) or -1 = corrupt list / no memory / names full.
int nrh_lspk_levels(const uint8_t *list, size_t n, nrh_names *names, nrh_span **out);
int nrh_span_find(const nrh_span *v, uint32_t n, uint64_t off);   // level of the span holding `off`, -1 = none
// [NR-BN] A read [off, off + len) split over the spans it overlaps: add(level, bytes) per overlapping span; returns the bytes inside
// level files (a buffered read that starts in another file and runs into a level's files counts for that level, by bytes).
uint64_t nrh_span_range(const nrh_span *v, uint32_t n, uint64_t off, uint64_t len, void (*add)(void *, uint32_t, uint64_t), void *ctx);

// ---- save games (NR-BN: the location of a save load) ----
// A save (save:/SaveNNNN/SaveNNNN.lsv) is an LSPK v13 package too: globals.lsf (the current level's whole state, MBs, read only when
// the save is loaded), meta.lsf (KBs, read by the load menu for every save; its values name the save's current level),
// levelcache/<level>.lsf (levels visited before). A save load reads the level from globals.lsf, not from Origins.pak, so the pak vote
// alone keeps the level the player left (the 18.11 play log: 20 min of Fort Joy logged as TUT_Tutorial_A).
typedef struct nrh_lspk_ent { uint64_t off; uint32_t disk, size, flags; } nrh_lspk_ent;   // flags & 0x0f: 0 stored, 1 zlib, 2 LZ4 block
// The entry called `name` (case-insensitive, archive part 0) of a file list as nrh_lspk_levels reads it. 0 = found, 1 = absent, -1 = corrupt.
int nrh_lspk_entry(const uint8_t *list, size_t n, const char *name, nrh_lspk_ent *e);
int nrh_lz4f(const uint8_t *s, size_t sn, uint8_t *d, size_t dn);   // LZ4 frame (linked or independent blocks); bytes written, -1 = corrupt
// The level an LSF v3..v5 resource (decompressed meta.lsf) names in its values section: the known name (whole word, case-insensitive)
// found most often, the first on a tie. -1 = none / not LSF / corrupt.
int nrh_lsf_level(const uint8_t *lsf, size_t n, const nrh_names *names);

// ---- the location vote ----
// Each step gets the bytes read per level since the previous step. score = score * keep + bytes. The current level changes to the
// best scored level when its score >= min_bytes and >= ratio x the current level's score (a level load reads MBs of one level; a
// stray read of another level's file does not move it).
typedef struct nrh_vote { double score[NRH_LEVELS]; int cur; double min_bytes, ratio, keep; } nrh_vote;
void nrh_vote_init(nrh_vote *v);   // cur -1, min_bytes 256 KB, ratio 3, keep 0.5 (one step a second: half-life 1 s)
int nrh_vote_step(nrh_vote *v, const uint64_t *delta, uint32_t n);   // 1 = cur changed

// ---- shader keys ----
uint64_t nrh_key_id(const char *key);   // exactly 16 hex digits: their value; anything else (F1.<key>, x.<name>): FNV-1a 64 | 1 << 63
void nrh_key_hex(uint64_t id, char out[17]);

// ---- static cost of a SPIR-V module (function bodies; the writer thread, once per key) ----
typedef struct nrh_cost { uint32_t inst, alu, trans, tex, branch, loops, ubo, ubo_dyn, bitcast, kill, cost; } nrh_cost;
// alu: arithmetic / conversion / compare / logic / derivative instructions and GLSL.std.450 calls that are not transcendental; trans: exp /
// log / pow / sqrt / inversesqrt / trigonometry / length / distance / normalize; tex: image sample / fetch / gather / read; branch:
// conditional branches and switches; ubo / ubo_dyn: loads from uniform / push-constant / storage blocks (with a non-constant index);
// cost = alu + 4 trans + 4 tex + 2 ubo_dyn + 2 branch (a ranking proxy, not cycles). 0 = ok, -1 = not SPIR-V / no memory.
int nrh_spirv_cost(const uint32_t *w, size_t n, nrh_cost *c);

// ---- aggregation ----
typedef struct nrh_prog { uint64_t vs, fs; uint32_t draws; uint64_t verts; float cvs, cfs; } nrh_prog;   // fs = CS for compute; 0 = none; cost < 0 = unknown
typedef struct nrh_pass { uint8_t compute; uint32_t w, h, fmt, att; double us; uint32_t draws; uint64_t verts; uint32_t nprog; const nrh_prog *prog; } nrh_pass;
typedef struct nrh_rkey { uint32_t level, w, h, fmt; uint8_t kind, att; uint64_t vs, fs; } nrh_rkey;   // kind 0 pass, 1 compute
typedef struct nrh_row { nrh_rkey k; uint8_t used; uint64_t last_frame; uint32_t frames, passes; uint64_t draws, verts; double us_d, us_v, us_p, us_c; } nrh_row;
typedef struct nrh_ltot { uint32_t frames, passes, cpasses; uint64_t draws, last_frame; double span_us, pass_us, compute_us; } nrh_ltot;
typedef struct nrh_agg { nrh_row *rows; uint32_t cap, n; uint64_t dropped; nrh_ltot lt[NRH_LEVELS]; } nrh_agg;
int nrh_agg_init(nrh_agg *a, uint32_t cap);   // cap rounded up to a power of two (>= 64); 0 = ok
void nrh_agg_free(nrh_agg *a);
void nrh_agg_reset(nrh_agg *a);               // after a flush: every row and total back to zero
// One submission's passes of a sampled frame (a frame may come in several calls with the same `frame`). A pass without draws / programs
// gets a row with vs = fs = 0. Inside a pass: us_d by draw share, us_v by vertex share (draw share when no vertices), us_p = all of it,
// us_c by draws x (cfs + cvs) (a program of unknown cost counts at the pass's known draw-weighted mean; none known: the draw share).
void nrh_agg_add(nrh_agg *a, uint32_t level, uint64_t frame, const nrh_pass *p, uint32_t n);
void nrh_agg_span(nrh_agg *a, uint32_t level, uint64_t frame, double span_us);   // the submission's first-to-last stamp span
int nrh_agg_levels(const nrh_agg *a, uint32_t *out, int cap);   // levels with frames since the last reset
// Rows of `level` with the biggest us_d (up to k, indices into a->rows, best first); returns how many.
int nrh_agg_top(const nrh_agg *a, uint32_t level, uint32_t *idx, int k);

// ---- lines ----
// Key text: the 16-hex form, or for hashed ids what name(id, ctx) gives (NULL / no callback: the hex form); id 0 = "-".
typedef const char *(*nrh_key_name_fn)(uint64_t id, void *ctx);
int nrh_fmt_row(char *out, size_t n, const nrh_row *r, nrh_key_name_fn name, void *ctx);   // "A\t..." with '\n'; length or -1
int nrh_fmt_block(char *out, size_t n, const char *level, const nrh_ltot *t, uint64_t unix_s, double t_s);   // "B\t...\n"
int nrh_fmt_pass(char *out, size_t n, uint64_t frame, uint32_t i, const nrh_pass *p, nrh_key_name_fn name, void *ctx);   // "P\t...\n"
void nrh_clean(char *s);   // tabs / newlines / CR in a name -> ' '

// ---- the output file ----
typedef struct nrh_out { FILE *f; char path[600], old[608]; uint64_t bytes, cap; uint32_t rotations; } nrh_out;
// <dir>/<name> opened for appending (bytes = its size); the rotated copy is <name minus .tsv>.1.tsv. cap = bytes per file. 0 = ok.
int nrh_out_open(nrh_out *o, const char *dir, const char *name, uint64_t cap);
int nrh_out_rotate(nrh_out *o);   // close, the old copy replaced by this file, a new empty file; 0 = ok
int nrh_out_full(const nrh_out *o, size_t more);   // 1 = writing `more` bytes would pass the cap (the caller rotates, then writes H / S again)
int nrh_out_write(nrh_out *o, const char *s, size_t n);   // 1 = written, 0 = no file / write error
void nrh_out_close(nrh_out *o);

#ifdef __cplusplus
}
#endif
#endif
