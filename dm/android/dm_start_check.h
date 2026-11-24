// [port-common start-files-check] Start check of the files setup made and the player copied to the phone (KB APP-12 + HLE-36).
//
// Players copy ~10-20 GB to the phone by hand; a copy that was cut off, a folder copied into the wrong place or a setup step that did
// not finish used to give a black screen at 60 fps (the shaders were missing: D3 issue #15, DOS2 issues #8 / #10) or a crash much later
// (an archive cut short: D3 issue #26). The start now checks the files first and stops on a screen that names them.
//
// Reference: D3 (d3-port-repo dm/android/jni_android.c d3_pick_shader_dir 1154062, d3_check_archives + d3_cpk_check.h e7742fd / 1a5a99c):
// the folder search for a misplaced shader copy, the "set of files that passed" cache (name / size / mtime -> files/<x>.ok), the
// start code -11 with the names (nativeStartDetail). This header holds the parts that do not depend on the game:
//   sc_list          the start screen's names, one per line
//   sc_sig_*         the signature of a set of files (sum of FNV-1a 64 of "path size mtime" lines + count), sc_read_line / sc_write_line
//   sc_xgspack_check the dm/ shader pack (tools xgs_pack.py v1: "XGSP", 1, count, 0; count x {key[32], off u64, size u32, pad u32,
//                    hash u64}): the index must fit and the last bundle must end at the file's real length
//   sc_lspk_check    a Larian package (.pak, LSPK v10 / v13 with the header at the end; DOS2 DE, BG3-era v15+ = header at the start):
//                    the footer must be there, the file list must decompress (LZ4 block), every file must end inside its part
//                    (part 0 before the file list, part k = <name>_<k>.pak at least as long as its last file)
// A game format of its own (D3's .cpk: d3_cpk_check.h) plugs in next to these the same way: 0 = complete (or a layout the check does
// not know: never blocks on a doubt), 1 = damaged (why says how), -1 = cannot be read, -2 = missing.
// Self-contained (no dm/ headers) so test/t_start_check.c runs it on a PC.
#ifndef DM_START_CHECK_H
#define DM_START_CHECK_H
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { char *buf; size_t cap, used; int count; } sc_list;
static void sc_list_add(sc_list *l, const char *s) {
    l->count++;
    if (!l->buf || l->used + 1 >= l->cap) return;
    int w = snprintf(l->buf + l->used, l->cap - l->used, "%s%s", l->used ? "\n" : "", s);
    if (w > 0) l->used += (size_t)w;
    if (l->used >= l->cap) l->used = l->cap - 1;
}

static void sc_sig_add(uint64_t *sig, const char *path, const struct stat *st) {
    char line[1500]; snprintf(line, sizeof line, "%s %lld %lld\n", path, (long long)st->st_size, (long long)st->st_mtime);
    uint64_t h = 0xCBF29CE484222325ull; for (const char *c = line; *c; c++) h = (h ^ (uint8_t)*c) * 0x100000001B3ull;
    *sig += h;   // summed: the same set gives the same signature in any directory order
}
static void sc_sig_text(uint64_t sig, int count, char out[40]) { snprintf(out, 40, "%016llx %d", (unsigned long long)sig, count); }
static void sc_read_line(const char *path, char *out, size_t n) {
    out[0] = 0; FILE *f = fopen(path, "r"); if (!f) return;
    if (!fgets(out, (int)n, f)) out[0] = 0;
    fclose(f); out[strcspn(out, "\r\n")] = 0;
}
static void sc_write_line(const char *path, const char *s) { FILE *f = fopen(path, "w"); if (f) { fprintf(f, "%s\n", s); fclose(f); } }

static int sc_pread(int fd, void *buf, size_t n, uint64_t off) {
    size_t got = 0;
    while (got < n) { ssize_t r = pread(fd, (uint8_t *)buf + got, n - got, (off_t)(off + got)); if (r < 0 && errno == EINTR) continue; if (r <= 0) return -1; got += (size_t)r; }
    return 0;
}
static int sc_open(const char *path, uint64_t *size, char *why, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { const int e = errno; snprintf(why, n, e == ENOENT ? "missing" : "cannot be opened (errno %d)", e); return e == ENOENT ? -2 : -1; }
    struct stat st; if (fstat(fd, &st)) { snprintf(why, n, "cannot be read (fstat errno %d)", errno); close(fd); return -1; }
    *size = (uint64_t)st.st_size; return fd;
}

// ---- dm/ shader pack (xgspack v1) ---------------------------------------------------------------------------------------------
// count (optional) = the bundles the index lists; min_count > 0: fewer is damaged (a pack whose size is known, e.g. 12329 for DOS2).
static int sc_xgspack_check(const char *path, unsigned min_count, unsigned *count, char *why, size_t n) {
    uint64_t size = 0; const int fd = sc_open(path, &size, why, n); if (fd < 0) return fd;
    uint32_t h[4]; int r = 0;
    if (size < 16 || sc_pread(fd, h, 16, 0)) { snprintf(why, n, "shorter than its header (%llu bytes)", (unsigned long long)size); r = 1; goto out; }
    if (h[0] != 0x50534758u || h[1] != 1) { snprintf(why, n, "not a shader pack (magic %08x version %u)", h[0], h[1]); r = 1; goto out; }
    if (count) *count = h[2];
    const uint64_t index = 16 + 56ull * h[2];
    if (index > size) { snprintf(why, n, "cut short inside its index (%u bundles need %llu bytes, the file has %llu)", h[2], (unsigned long long)index, (unsigned long long)size); r = 1; goto out; }
    if (min_count && h[2] < min_count) { snprintf(why, n, "holds %u of %u bundles (setup's shader step did not finish)", h[2], min_count); r = 1; goto out; }
    uint8_t *ix = malloc(index - 16 ? (size_t)(index - 16) : 1); if (!ix) { snprintf(why, n, "out of memory"); r = -1; goto out; }
    if (sc_pread(fd, ix, (size_t)(index - 16), 16)) { free(ix); snprintf(why, n, "index cannot be read"); r = -1; goto out; }
    uint64_t end = index;
    for (uint32_t i = 0; i < h[2]; i++) {
        uint64_t off; uint32_t sz; memcpy(&off, ix + 56ull * i + 32, 8); memcpy(&sz, ix + 56ull * i + 40, 4);
        if (off + sz > end) end = off + sz;
    }
    free(ix);
    if (end > size) { snprintf(why, n, "cut short: %llu of %llu bytes (the copy to the phone stopped early)", (unsigned long long)size, (unsigned long long)end); r = 1; goto out; }
    snprintf(why, n, "%u bundles, complete", h[2]);
out:
    close(fd); return r;
}

// ---- Larian package (.pak, LSPK) ----------------------------------------------------------------------------------------------
// LZ4 block decoder (bounds-checked; the file list is one block). Returns the bytes written, -1 on malformed input.
static long sc_lz4_block(const uint8_t *s, size_t sn, uint8_t *d, size_t dn) {
    size_t i = 0, o = 0;
    while (i < sn) {
        const unsigned t = s[i++]; size_t l = t >> 4;
        if (l == 15) { unsigned b; do { if (i >= sn) return -1; b = s[i++]; l += b; } while (b == 255); }
        if (l > sn - i || l > dn - o) return -1;
        memcpy(d + o, s + i, l); i += l; o += l;
        if (i >= sn) break;   // the last sequence has literals only
        if (sn - i < 2) return -1;
        const size_t off = (size_t)s[i] | (size_t)s[i + 1] << 8; i += 2;
        size_t m = t & 15;
        if (m == 15) { unsigned b; do { if (i >= sn) return -1; b = s[i++]; m += b; } while (b == 255); }
        m += 4;
        if (!off || off > o || m > dn - o) return -1;
        for (size_t k = 0; k < m; k++, o++) d[o] = d[o - off];   // overlapping copies are part of the format
    }
    return (long)o;
}
// The part file of base "<x>.pak" number k: "<x>_<k>.pak" (LSLib Package.MakePartFilename).
static void sc_lspk_part(const char *base, unsigned k, char *out, size_t n) {
    size_t l = strlen(base); if (l > 4 && !strcasecmp(base + l - 4, ".pak")) l -= 4;
    snprintf(out, n, "%.*s_%u.pak", (int)l, base, k);
}
// Whether path is a part file (<x>_<k>.pak with <x>.pak next to it): checked through its base, not on its own.
static int sc_lspk_is_part(const char *path) {
    const size_t l = strlen(path); if (l < 7 || strcasecmp(path + l - 4, ".pak")) return 0;
    size_t i = l - 4; while (i > 0 && path[i - 1] >= '0' && path[i - 1] <= '9') i--;
    if (i == l - 4 || i == 0 || path[i - 1] != '_') return 0;
    char base[1400]; snprintf(base, sizeof base, "%.*s.pak", (int)(i - 1), path);
    return !access(base, F_OK);
}
// bad (optional, n bytes): the file to copy again when damaged (the base or one of its parts).
static int sc_lspk_check(const char *path, char *why, size_t n, char *bad, size_t bn) {
    if (bad && bn) snprintf(bad, bn, "%s", path);
    uint64_t size = 0; const int fd = sc_open(path, &size, why, n); if (fd < 0) return fd;
    int r = 0; uint8_t *comp = NULL, *list = NULL;
    uint8_t foot[8], head[4];
    if (size < 8 || sc_pread(fd, foot, 8, size - 8) || sc_pread(fd, head, 4, 0)) { snprintf(why, n, "too short to be a package (%llu bytes)", (unsigned long long)size); r = 1; goto out; }
    if (!memcmp(head, "LSPK", 4)) {   // v15+ (header at the start): u32 version, u64 file list offset, u32 file list size
        uint8_t hv[16]; if (sc_pread(fd, hv, 16, 4)) { snprintf(why, n, "header cannot be read"); r = 1; goto out; }
        uint64_t flo; uint32_t fls; memcpy(&flo, hv + 4, 8); memcpy(&fls, hv + 12, 4);
        if (flo + fls > size) { snprintf(why, n, "cut short: the file list needs %llu bytes, the file has %llu", (unsigned long long)(flo + fls), (unsigned long long)size); r = 1; }
        else snprintf(why, n, "header-first package, file list in place");
        goto out;
    }
    uint32_t hs; memcpy(&hs, foot, 4);
    if (memcmp(foot + 4, "LSPK", 4) || hs < 24 || hs > size) { snprintf(why, n, "its end is missing (no package footer: the copy to the phone stopped early)"); r = 1; goto out; }
    uint8_t h[16]; if (sc_pread(fd, h, 16, size - hs)) { snprintf(why, n, "header cannot be read"); r = 1; goto out; }
    uint32_t ver, flo, fls; uint16_t parts; memcpy(&ver, h, 4); memcpy(&flo, h + 4, 4); memcpy(&fls, h + 8, 4); memcpy(&parts, h + 12, 2);
    if (ver != 10 && ver != 13) { snprintf(why, n, "package version %u (not checked)", ver); goto out; }
    if (fls < 4 || (uint64_t)flo + fls > size - hs) { snprintf(why, n, "file list outside the file (offset %u size %u, file %llu)", flo, fls, (unsigned long long)size); r = 1; goto out; }
    uint32_t nf; if (sc_pread(fd, &nf, 4, flo)) { snprintf(why, n, "file list cannot be read"); r = 1; goto out; }
    const size_t esz = 280;   // v10 / v13 FileEntry13: name[256], offset, size on disk, size, part, flags, crc (u32 each)
    if (nf > 4000000u) { snprintf(why, n, "file list claims %u files", nf); r = 1; goto out; }
    comp = malloc(fls - 4 ? fls - 4 : 1); list = malloc((size_t)nf * esz + 1);
    if (!comp || !list) { snprintf(why, n, "out of memory (%u files)", nf); r = -1; goto out; }
    if (sc_pread(fd, comp, fls - 4, (uint64_t)flo + 4)) { snprintf(why, n, "file list cannot be read"); r = 1; goto out; }
    const long got = sc_lz4_block(comp, fls - 4, list, (size_t)nf * esz);
    if (got != (long)((size_t)nf * esz)) { snprintf(why, n, "file list does not unpack (%ld of %zu bytes)", got, (size_t)nf * esz); r = 1; goto out; }
    uint64_t ends[64] = {0}; unsigned maxpart = 0;
    for (uint32_t i = 0; i < nf; i++) {
        uint32_t f[4]; memcpy(f, list + (size_t)i * esz + 256, 16);   // offset, size on disk, size, part
        if (f[3] >= 64) { snprintf(why, n, "file %u names part %u", i, f[3]); r = 1; goto out; }
        const uint64_t e = (uint64_t)f[0] + f[1]; if (e > ends[f[3]]) ends[f[3]] = e;
        if (f[3] > maxpart) maxpart = f[3];
    }
    if (ends[0] > flo) { snprintf(why, n, "its files run past the file list (%llu > %u)", (unsigned long long)ends[0], flo); r = 1; goto out; }
    const unsigned np = parts > maxpart + 1 ? parts : maxpart + 1;
    for (unsigned k = 1; k < np && k < 64; k++) {
        char part[1400]; sc_lspk_part(path, k, part, sizeof part);
        struct stat st;
        if (stat(part, &st)) { snprintf(why, n, "part %u (%s) is missing", k, strrchr(part, '/') ? strrchr(part, '/') + 1 : part); if (bad && bn) snprintf(bad, bn, "%s", part); r = 1; goto out; }
        if ((uint64_t)st.st_size < ends[k]) {
            snprintf(why, n, "part %u is cut short: %llu of %llu bytes", k, (unsigned long long)st.st_size, (unsigned long long)ends[k]);
            if (bad && bn) snprintf(bad, bn, "%s", part); r = 1; goto out;
        }
    }
    snprintf(why, n, "%u files in %u part(s), complete", nf, np);
out:
    free(comp); free(list); close(fd); return r;
}

// Every file under dir whose name ends in ext (case-insensitive), depth <= 4, into cb(path, st, user). Returns the count.
static int sc_walk(const char *dir, const char *ext, int depth, void (*cb)(const char *, const struct stat *, void *), void *user) {
    DIR *d = opendir(dir); if (!d) return 0;
    int count = 0; const size_t el = strlen(ext); struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1400]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st; if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) { if (depth > 0) count += sc_walk(p, ext, depth - 1, cb, user); continue; }
        const size_t l = strlen(e->d_name);
        if (l > el && !strcasecmp(e->d_name + l - el, ext)) { cb(p, &st, user); count++; }
    }
    closedir(d);
    return count;
}
#endif
