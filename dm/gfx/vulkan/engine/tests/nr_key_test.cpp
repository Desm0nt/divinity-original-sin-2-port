// [dos2-nr02] NR-02: host test of the draw identity tables (dm/gfx/nr/nr_key.h) on top of the registry (nr_reg.h): render-target format
// sets, draw keys and why they are not eligible, the no-registry fast path, windows, bounded tables, texture sets (hash, classification
// through the pool id map), name <-> pack key lookups, the bundle layout word and the TSV writer.
// Optional: nr_key_test <bundle dir> [layout.tsv] - every <key>.xgs of the directory: the C layout word of its binding table must equal
// the header's reserved word when it is set (a copy stamped by tools/dos2_nr_layout.py --write) and the tool's --tsv row; the old
// reader's header and extent checks still pass on the stamped bundle.
// Build (Windows clang + MSVC libs, or WSL g++):
//   clang -std=c11 -c ../../../nr/nr_reg.c ../../../nr/nr_key.c -D_CRT_SECURE_NO_WARNINGS && clang++ -std=c++17 -Wall -D_CRT_SECURE_NO_WARNINGS nr_key_test.cpp nr_reg.o nr_key.o -o nr_key_test
#include "../../../nr/nr_key.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>

static int failures, checks;
#define CHECK(expression) do { ++checks; if(!(expression)){printf("FAIL line %d: %s\n",__LINE__,#expression);++failures;} } while(0)

static uint64_t H(uint32_t idx, uint32_t gen) { return (uint64_t)gen << 32 | idx; }
static nr_tex_desc desc(int src, unsigned efmt, unsigned msaa = 1) {
    nr_tex_desc e; memset(&e, 0, sizeof e);
    e.w = 64; e.h = 64; e.efmt = (uint8_t)efmt; e.mips = 1; e.layers = 1; e.msaa = (uint8_t)msaa; e.src = (uint8_t)src;
    e.fmt_override = src == NR_SRC_IMAGE_LINEAR || src == NR_SRC_IMAGE_PACKAGED ? -1 : 0;
    return e;
}
static void shader(nr_reg *r, uint64_t h, const char *name, uint64_t vs, uint64_t fs, uint64_t cs, const char *kvs, const char *kfs) {
    uint64_t prog[NR_STAGES] = {vs, fs, cs}; char key[NR_STAGES][17]; memset(key, 0, sizeof key);
    if (kvs) snprintf(key[0], 17, "%s", kvs);
    if (kfs) snprintf(key[1], 17, "%s", kfs);
    CHECK(nr_shader_put(r, h, name, "Shaders/x.bshd", prog, key) != NULL);
}
static int count_lines(const std::string &s) { int n = 0; for (char c : s) n += c == '\n'; return n; }
static int fields(const std::string &line) { int n = 1; for (char c : line) n += c == '\t'; return n; }
static std::string tsv(const nr_keys *k, const nr_reg *r, int which, int *rows) {
    FILE *f = tmpfile(); *rows = nr_keys_write_tsv(k, r, which, f);
    std::string s; rewind(f); char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f); return s;
}

static void bundles(const char *dir, const char *tsvpath) {
    std::map<std::string, uint32_t> tool;
    if (tsvpath) {
        std::ifstream in(tsvpath); std::string line; std::getline(in, line);
        while (std::getline(in, line)) { std::istringstream ss(line); std::string key, st, w; std::getline(ss, key, '\t'); std::getline(ss, st, '\t'); std::getline(ss, w, '\t'); tool[key] = (uint32_t)strtoul(w.c_str(), nullptr, 16); }
    }
    int n = 0, stamped = 0, vs_tool = 0;
    for (auto &e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() != ".xgs") continue;
        std::ifstream in(e.path(), std::ios::binary); std::vector<char> b((std::istreambuf_iterator<char>(in)), {});
        uint32_t h[8]; if (b.size() < 32) { CHECK(!"bundle too short"); continue; } memcpy(h, b.data(), 32);
        // the old reader (xbg_game.cpp load_shader_key): header words and the exact extent; reserved is not looked at
        CHECK(h[0] == 0x31534758u && h[1] == 1 && h[2] < 6 && h[3] <= 256 && h[4] <= 16 && h[5] <= 65536 && h[6] % 4 == 0);
        CHECK(32 + (size_t)h[3] * 32 + (size_t)h[4] * 4 + h[5] + h[6] == b.size());
        const uint32_t w = nr_layout_word((const uint32_t *)(b.data() + 32), h[3]);
        CHECK(nr_layout_valid(w));
        if (h[7]) { stamped++; CHECK(h[7] == w); }
        const std::string key = e.path().stem().string();
        if (tool.count(key)) { vs_tool++; CHECK(tool[key] == w); }
        n++;
    }
    printf("bundles: %d read from %s, %d stamped words equal the C word, %d tool rows equal the C word\n", n, dir, stamped, vs_tool);
    CHECK(n > 0);
}

int main(int argc, char **argv) {
    nr_reg *r = nr_reg_new(); nr_keys *k = nr_keys_new();
    CHECK(r && k);
    nr_keys_free(nullptr);
    // ---- names / kinds ----
    CHECK(!strcmp(nr_dsite_name(NR_DS_LIST_IDX), "list-indexed") && !strcmp(nr_dsite_name(99), "?"));
    CHECK(nr_dsite_is_draw(NR_DS_LIST_IDX) && nr_dsite_is_draw(NR_DS_FULL) && nr_dsite_is_draw(NR_DS_UNKNOWN) && !nr_dsite_is_draw(NR_DS_FLUSH) && !nr_dsite_is_draw(NR_DS_COMPUTE) && !nr_dsite_is_draw(-1));
    char t[128];
    nr_ne_text(0, t, sizeof t); CHECK(!strcmp(t, "eligible"));
    nr_ne_text(NR_NE_SHADER | NR_NE_RT, t, sizeof t); CHECK(!strcmp(t, "shader+rt"));
    nr_ne_text(NR_NE_FULL | NR_NE_LOST, t, sizeof t); CHECK(!strcmp(t, "full-state+lost"));

    // ---- registry content ----
    const uint64_t rt0 = H(10, 1), rt1 = H(11, 1), dep = H(12, 1), mat = H(13, 1), ms = H(14, 1);
    nr_nvn_tex nt; memset(&nt, 0, sizeof nt); nt.obj = 0x1000;
    { const nr_tex_desc d_ = desc(NR_SRC_RT, 1); CHECK(nr_tex_put(r, rt0, &d_, &nt, 1)); }
    { const nr_tex_desc d_ = desc(NR_SRC_RT, 20); CHECK(nr_tex_put(r, rt1, &d_, &nt, 1)); }
    { const nr_tex_desc d_ = desc(NR_SRC_RT, 45); CHECK(nr_tex_put(r, dep, &d_, &nt, 1)); }
    { const nr_tex_desc d_ = desc(NR_SRC_IMAGE_PACKAGED, 30); CHECK(nr_tex_put(r, mat, &d_, &nt, 1)); }
    { const nr_tex_desc d_ = desc(NR_SRC_RT, 1, 4); CHECK(nr_tex_put(r, ms, &d_, &nt, 1)); }
    const uint8_t ba[6] = {6, 7, 1, 7, 0, 0}, da[8] = {1, 1, 3, 0, 0xff, 7, 0, 0}, ra[5] = {1, 0, 1, 0, 0};
    const uint64_t blend = H(3, 0), depth = H(5, 0), raster = H(7, 0), vf = H(9, 2);
    CHECK(nr_state_put(r, NR_ST_BLEND, blend, ba, 6) && nr_state_put(r, NR_ST_DEPTH, depth, da, 8) && nr_state_put(r, NR_ST_RASTER, raster, ra, 5));
    const uint8_t el[12] = {0, 0, 0, 0, 3, 3, 0, 1, 0, 0, 3, 2};
    CHECK(nr_vformat_put(r, vf, el, 2));
    const uint64_t sh = H(1, 5), shc = H(2, 5), shk = H(3, 5);
    shader(r, sh, "Shaders/NX/DeferredLight", 0xa0, 0xb0, 0, "1111111111111111", "2222222222222222");
    shader(r, shc, "Shaders/NX/BlurH", 0, 0, 0xc0, nullptr, nullptr);
    shader(r, shk, "", 0xd0, 0xe0, 0, "3333333333333333", nullptr);
    CHECK(nr_state_find(r, NR_ST_BLEND, blend) && !nr_state_find(r, NR_ST_BLEND, H(3, 1)) && !nr_state_find(r, NR_ST_BLEND, 0) && !nr_state_find(r, 9, blend));
    CHECK(nr_vformat_find(r, vf) && !nr_vformat_find(r, H(9, 3)) && !nr_vformat_find(r, 0));
    int st = -1;
    CHECK(nr_shader_find_name(r, "Shaders/NX/DeferredLight") == nr_shader_get(r, sh) && !nr_shader_find_name(r, "nope") && !nr_shader_find_name(r, "") && !nr_shader_find_name(r, nullptr));
    CHECK(nr_shader_find_key(r, "2222222222222222", &st) == nr_shader_get(r, sh) && st == NR_STAGE_FS);
    CHECK(nr_shader_find_key(r, "3333333333333333", &st) == nr_shader_get(r, shk) && st == NR_STAGE_VS && !nr_shader_find_key(r, "4444444444444444", &st));

    // ---- RT format sets ----
    uint64_t col[NR_RT_COLORS] = {rt0, rt1, 0, 0, 0, 0, 0, 0};
    const uint16_t f1 = nr_rtfmt_note(k, r, col, dep);
    CHECK(f1 == 1 && nr_rtfmt_note(k, r, col, dep) == 1 && nr_rtfmt_get(k, 1)->uses == 2);
    const nr_rtfmt *g = nr_rtfmt_get(k, f1);
    CHECK(g->n == 2 && g->fmt[0] == 1 && g->fmt[1] == 20 && g->fmt[2] == NR_FMT_NONE && g->depth == 45 && g->msaa == 1 && !g->unknown);
    uint64_t col2[NR_RT_COLORS] = {0, 0, rt0, 0, 0, 0, 0, 0};   // a gap: slot 2 only
    const uint16_t f2 = nr_rtfmt_note(k, r, col2, 0);
    CHECK(f2 == 2 && nr_rtfmt_get(k, f2)->n == 3 && nr_rtfmt_get(k, f2)->fmt[0] == NR_FMT_NONE && nr_rtfmt_get(k, f2)->depth == NR_FMT_NONE);
    uint64_t col3[NR_RT_COLORS] = {H(99, 1), 0, 0, 0, 0, 0, 0, 0};
    const uint16_t f3 = nr_rtfmt_note(k, r, col3, 0);
    CHECK(f3 == 3 && nr_rtfmt_get(k, f3)->unknown && nr_rtfmt_get(k, f3)->fmt[0] == NR_FMT_UNKNOWN);
    uint64_t col4[NR_RT_COLORS] = {ms, 0, 0, 0, 0, 0, 0, 0};
    const uint16_t f4 = nr_rtfmt_note(k, r, col4, 0);
    CHECK(f4 == 4 && nr_rtfmt_get(k, f4)->msaa == 4);
    uint64_t none[NR_RT_COLORS] = {0};
    const uint16_t f5 = nr_rtfmt_note(k, r, none, 0);
    CHECK(f5 == 5 && !nr_rtfmt_get(k, 0) && !nr_rtfmt_get(k, 99));

    // ---- keys ----
    nr_dkey key; memset(&key, 0, sizeof key);
    key.shader = sh; key.blend = blend; key.depth = depth; key.raster = raster; key.vformat = vf; key.blend_arg = 0xffffffffu; key.rtfmt = f1; key.prim = 4;
    CHECK(nr_dkey_why(r, k, &key) == 0);
    int fresh = 0;
    const nr_kent *e = nr_key_note(k, nullptr, &key, &fresh);   // new key, no registry: retry asked, nothing counted
    CHECK(!e && fresh == -1 && k->nkey == 0 && k->key_lost == 0 && k->win_keys == 0);
    e = nr_key_note(k, r, &key, &fresh);
    CHECK(e && fresh == 1 && e->id == 1 && e->why == 0 && e->draws == 1 && k->win_keys == 1);
    e = nr_key_note(k, nullptr, &key, &fresh);                  // known key: found without the registry
    CHECK(e && fresh == 0 && e->id == 1 && e->draws == 2 && k->win_keys == 1);
    CHECK(nr_key_get(k, 1) == e && !nr_key_get(k, 0) && !nr_key_get(k, 2));
    nr_dkey k2 = key; k2.alpha_test = 1;
    CHECK(nr_dkey_hash(&k2) != nr_dkey_hash(&key));
    e = nr_key_note(k, r, &k2, &fresh); CHECK(e && fresh == 1 && e->id == 2 && k->win_keys == 2);
    struct { void (*edit)(nr_dkey &); unsigned why; } cases[] = {
        {[](nr_dkey &x) { x.shader = H(1, 6); }, NR_NE_SHADER},               // another generation: not that shader
        {[](nr_dkey &x) { x.shader = H(2, 5); }, NR_NE_COMPUTE},
        {[](nr_dkey &x) { x.shader = H(3, 5); }, NR_NE_PACKKEY},              // fragment program without a pack key
        {[](nr_dkey &x) { x.blend = 0; }, NR_NE_STATE},
        {[](nr_dkey &x) { x.depth = H(6, 0); }, NR_NE_STATE},
        {[](nr_dkey &x) { x.raster = H(7, 1); }, NR_NE_STATE},
        {[](nr_dkey &x) { x.vformat = 0; }, NR_NE_VFORMAT},
        {[](nr_dkey &x) { x.rtfmt = 3; }, NR_NE_RT},                          // a target the registry does not know
        {[](nr_dkey &x) { x.rtfmt = 0; }, NR_NE_RT},                          // table was full
        {[](nr_dkey &x) { x.rtfmt = 5; }, NR_NE_RT},                          // nothing bound
        {[](nr_dkey &x) { x.rtfmt = 2; }, 0},                                 // colour in slot 2 only: fine
        {[](nr_dkey &x) { x.shader = 0; x.vformat = 0; }, NR_NE_SHADER | NR_NE_VFORMAT},
    };
    for (auto &c : cases) { nr_dkey x = key; c.edit(x); CHECK(nr_dkey_why(r, k, &x) == c.why); }
    // windows
    nr_keys_window(k);
    CHECK(k->win == 2 && k->win_keys == 0);
    e = nr_key_note(k, nullptr, &key, &fresh); CHECK(e && e->last_win == 2 && e->first_win == 1 && k->win_keys == 1);
    e = nr_key_note(k, nullptr, &key, &fresh); CHECK(k->win_keys == 1 && e->draws == 4);
    // the table is bounded
    nr_keys *kb = nr_keys_new(); unsigned made = 0;
    for (unsigned i = 0; i < NR_KEY_MAX + 10; i++) { nr_dkey x = key; x.blend_arg = i; if (nr_key_note(kb, r, &x, &fresh)) made++; }
    CHECK(made == NR_KEY_MAX && kb->key_lost == 10 && kb->nkey == NR_KEY_MAX);
    { nr_dkey x = key; x.blend_arg = 5; e = nr_key_note(kb, nullptr, &x, &fresh); CHECK(e && e->k.blend_arg == 5 && fresh == 0); }   // old keys still found
    // rt format table bounded
    for (unsigned f = 0; f < 63; f++) { const nr_tex_desc d_ = desc(NR_SRC_RT, f); CHECK(nr_tex_put(r, H(1000 + f, 1), &d_, &nt, 1)); }
    for (unsigned i = 0; i < NR_RTFMT_MAX + 3; i++) { uint64_t c[NR_RT_COLORS] = {0}; c[i % NR_RT_COLORS] = H(1000 + i / NR_RT_COLORS, 1); nr_rtfmt_note(kb, r, c, 0); }
    CHECK(kb->nrtf == NR_RTFMT_MAX && kb->rtf_lost > 0);
    nr_keys_free(kb);

    // ---- pool ids, texture sets ----
    CHECK(nr_texid_put(k, 0x100, rt0) && nr_texid_put(k, 0x101, mat) && nr_texid_put(k, 0x102, H(77, 1)));
    CHECK(nr_texid_get(k, 0x100) == rt0 && nr_texid_get(k, 0x999) == 0 && nr_texid_tex(k, r, 0x101) == nr_tex_get(r, mat) && !nr_texid_tex(k, r, 0x102));
    CHECK(nr_texid_put(k, 0x100, rt1) && nr_texid_get(k, 0x100) == rt1 && k->ntid == 3);   // re-registered id: the newer texture
    int32_t tex[NR_TS_STAGES][NR_TS_SLOTS], smp[NR_TS_STAGES][NR_TS_SLOTS];
    memset(tex, 0xff, sizeof tex); memset(smp, 0xff, sizeof smp);
    tex[1][0] = 0x101; smp[1][0] = 3; tex[1][1] = 0x100; smp[1][1] = 4; tex[0][0] = 0x102; tex[1][5] = 0x555;
    const uint64_t h1 = nr_texset_hash(tex, smp);
    smp[1][1] = 5; const uint64_t h2 = nr_texset_hash(tex, smp); smp[1][1] = 4;
    tex[1][1] = 0x101; const uint64_t h3 = nr_texset_hash(tex, smp); tex[1][1] = 0x100;
    CHECK(h1 != h2 && h1 != h3 && h2 != h3 && h1 == nr_texset_hash(tex, smp));
    { int32_t a[NR_TS_STAGES][NR_TS_SLOTS], b[NR_TS_STAGES][NR_TS_SLOTS]; memcpy(a, tex, sizeof a); memcpy(b, smp, sizeof b); a[0][0] = tex[1][0]; a[1][0] = tex[0][0];
      CHECK(nr_texset_hash(a, b) != h1); }   // the same ids in other stages / slots are another set
    const nr_texset *ts = nr_texset_note(k, nullptr, h1, tex, &fresh);
    CHECK(!ts && fresh == -1 && k->nts == 0);
    ts = nr_texset_note(k, r, h1, tex, &fresh);
    CHECK(ts && fresh == 1 && ts->id == 1 && ts->nslots == 4 && ts->nrt == 1 && ts->nmat == 1 && ts->nunknown == 2 && ts->draws == 1 && k->win_ts == 1);
    ts = nr_texset_note(k, nullptr, h1, tex, &fresh); CHECK(ts && fresh == 0 && ts->draws == 2);
    nr_texset_count(k, 1); nr_texset_count(k, 0); nr_texset_count(k, 77);
    CHECK(nr_texset_get(k, 1)->draws == 3 && !nr_texset_get(k, 0) && !nr_texset_get(k, 2));
    nr_keys_window(k);
    nr_texset_count(k, 1); CHECK(k->win_ts == 1 && nr_texset_get(k, 1)->last_win == 3);
    ts = nr_texset_note(k, r, h2, tex, &fresh); CHECK(ts && ts->id == 2 && k->win_ts == 2);
    CHECK(nr_tex_kill(r, mat) && !nr_texid_tex(k, r, 0x101));   // a destroyed texture: its id is unknown from now on
    { int32_t e2[NR_TS_STAGES][NR_TS_SLOTS]; memset(e2, 0xff, sizeof e2); ts = nr_texset_note(k, r, nr_texset_hash(e2, e2), e2, &fresh); CHECK(ts && ts->nslots == 0); }

    // ---- TSV ----
    int rows = 0;
    for (int w = 0; w < NR_KTSV_N; w++) {
        const std::string s = tsv(k, r, w, &rows);
        CHECK(rows >= 1 && count_lines(s) == rows + 1);
        const std::string head = s.substr(0, s.find('\n'));
        size_t p = s.find('\n') + 1; bool same = true;
        while (p < s.size()) { const size_t q = s.find('\n', p); if (fields(s.substr(p, q - p)) != fields(head)) same = false; p = q + 1; }
        CHECK(same);
    }
    CHECK(!strcmp(nr_ktsv_name(NR_KTSV_KEYS), "nr_keys.tsv") && !strcmp(nr_ktsv_name(NR_KTSV_NAMES), "nr_names.tsv") && !strcmp(nr_ktsv_name(9), "nr_unknown.tsv"));
    { const std::string s = tsv(k, r, NR_KTSV_KEYS, &rows); CHECK(rows == 2 && s.find("Shaders/NX/DeferredLight\t1111111111111111\t2222222222222222") != std::string::npos && s.find("\t6,7,1,7,0,0\t") != std::string::npos); }
    { const std::string s = tsv(k, r, NR_KTSV_NAMES, &rows); CHECK(rows == 3 && s.find("Shaders/NX/BlurH\t") != std::string::npos && s.find("-\tShaders/x.bshd\t0x500000003\t1\t3333333333333333\t-\t-") != std::string::npos); }
    { const std::string s = tsv(k, r, NR_KTSV_RTFMTS, &rows); CHECK(rows == 5 && s.find("\t?\t") != std::string::npos); }
    { FILE *f = tmpfile(); CHECK(nr_keys_write_tsv(k, r, 42, f) == -1); fclose(f); }

    // ---- bundle layout word ----
    auto B = [](uint32_t src, uint32_t count = 1) { std::vector<uint32_t> v(8, 0); v[2] = count; v[4] = src; return v; };
    auto word = [](std::vector<std::vector<uint32_t>> rows) { std::vector<uint32_t> f; for (auto &x : rows) f.insert(f.end(), x.begin(), x.end()); return nr_layout_word(f.data(), (unsigned)rows.size()); };
    uint32_t w = word({B(1), B(4), B(2), B(2), B(7), B(7), B(7)});
    CHECK(nr_layout_valid(w) && nr_layout_ubos(w) == 2 && nr_layout_textures(w) == 3 && nr_layout_flags(w) == (NR_LW_SUPPORT | NR_LW_C1 | NR_LW_FITS) && w >> 28 == 0xA && (w >> 24 & 15) == 1);
    nr_layout_text(w, t, sizeof t); CHECK(!strcmp(t, "NR v1: 2 ubo, 3 tex c1 support fits"));
    CHECK(word({}) == (0xA1000000u | NR_LW_FITS));
    w = word({B(2), B(3)}); CHECK((nr_layout_flags(w) & (NR_LW_STORAGE | NR_LW_FITS)) == NR_LW_STORAGE);
    w = word({B(8)}); CHECK(nr_layout_flags(w) == NR_LW_STORAGE);
    w = word({B(7, 4)}); CHECK(nr_layout_flags(w) == NR_LW_ARRAYS && nr_layout_textures(w) == 1);
    w = word({B(12)}); CHECK(nr_layout_flags(w) == NR_LW_OTHER);
    w = word({B(5), B(6)}); CHECK(nr_layout_flags(w) == (NR_LW_DRIVER | NR_LW_FITS));
    { std::vector<std::vector<uint32_t>> v(8, B(2)); CHECK(nr_layout_flags(word(v)) & NR_LW_FITS); v.push_back(B(2)); CHECK(!(nr_layout_flags(word(v)) & NR_LW_FITS) && nr_layout_ubos(word(v)) == 9); }
    { std::vector<std::vector<uint32_t>> v(20, B(2)); for (int i = 0; i < 70; i++) v.push_back(B(7)); w = word(v); CHECK(nr_layout_ubos(w) == 15 && nr_layout_textures(w) == 63 && (nr_layout_flags(w) & NR_LW_CLAMPED)); }
    CHECK(!nr_layout_valid(0) && !nr_layout_valid(0xA2000000u) && !nr_layout_valid(0xB1000000u) && !nr_layout_valid(0xA1000100u));
    nr_layout_text(0, t, sizeof t); CHECK(!strncmp(t, "no NR layout", 12));
    if (argc > 1) bundles(argv[1], argc > 2 ? argv[2] : nullptr);

    nr_keys_free(k); nr_reg_free(r);
    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
