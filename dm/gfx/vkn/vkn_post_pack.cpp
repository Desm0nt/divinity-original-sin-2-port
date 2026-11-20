// [DOS2] VK-NATIVE stage 5.2 piece B3: the fused post bundles for the backend's shader library (vkn_post.h: the design). Ported from
// xbg_nb_post.inc pack_get / compose: a fused key "F1." / "F1A." / "F1AS." / "F1AR." / "F2." + the host's translated key is read from
// <shaders>/native-post.xgspack (xgs_pack v1, CRC-32; setup builds it with tools/dos2_post_fuse.py), parsed (nb_fx_parse), exactly one
// elided pass, the host part of its bundle checked byte for byte against the translated host bundle (nb_fx_host_head_matches, the
// bundle read through the library's own reader), and the extra bindings (source 0xF0) rewritten to spare slots of the fragment stage -
// textures 120.., uniform blocks 15, 14, the support buffer as a SUPPORT binding - so the library's load-time chain treats them like
// the host's own (the old path did the same before load_shader_key). vkn_post.c finds the extras again by their Vulkan binding and
// feeds them from the stash of the skipped draw. The parsed table stays here for vkn_post_meta. Any thread (the library's lock is held
// by the caller); the module's install runs at the first call.
#ifdef DM_GAME_DOS2
#include "vkn_post.h"
#include "vkn_shlib.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <algorithm>
#include <map>
#include <mutex>
#include "../nb/nb_fused.h"
#include "../vulkan/engine/xbg_game.h"   // XBG_SOURCE_*

#define PP_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"
extern "C" int vkn_post_meta(const char *key, nb_fx *out, char *why, size_t why_bytes);

namespace {
struct Ent { char key[32]; uint64_t off; uint32_t size, pad; uint64_t hash; };   // xgs_pack v1 index entry
static_assert(sizeof(Ent) == 56, "xgspack index entry");
enum { SPARE_TEX = 120, SPARE_UBO = 15 };
std::once_flag g_pack_once; int g_fd = -1; std::vector<Ent> g_idx; std::string g_dir;
std::mutex g_mu;
struct Rec { bool ok; std::string why; std::vector<uint8_t> blob; nb_fx fx; };
std::map<std::string, Rec> g_rec;   // under g_mu

void open_pack() {
    std::call_once(g_pack_once, [] {
        if (g_dir.empty()) return;
        const std::string path = g_dir + "/native-post.xgspack"; const int f = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (f < 0) { vkn_logf(PP_RED "post: no %s - run setup again (it builds the fused post shaders); the post chain is drawn pass by pass", path.c_str()); return; }
        struct stat s{}; uint32_t h[4] = {};
        bool ok = !fstat(f, &s) && pread(f, h, 16, 0) == 16 && h[0] == 0x50534758u && h[1] == 1u && h[2] < 4096;
        std::vector<Ent> v; if (ok) { v.resize(h[2]); ok = pread(f, v.data(), v.size() * sizeof(Ent), 16) == ssize_t(v.size() * sizeof(Ent)); }
        for (size_t i = 0; ok && i < v.size(); i++) ok = v[i].key[31] == 0 && v[i].off + v[i].size <= uint64_t(s.st_size);
        if (!ok) { ::close(f); vkn_logf(PP_RED "post: %s is damaged or of another version: ignored (run setup again)", path.c_str()); return; }
        g_fd = f; g_idx.swap(v); vkn_logf("post: %s, %zu fused bundles", path.c_str(), g_idx.size());
    });
}
bool pack_get(const std::string &key, std::vector<uint8_t> &out) {
    open_pack(); if (g_fd < 0) return false;
    for (const Ent &e : g_idx) {
        if (key != e.key) continue;
        out.resize(e.size); size_t got = 0;
        while (got < out.size()) { const ssize_t r = pread(g_fd, out.data() + got, out.size() - got, off_t(e.off + got)); if (r < 0 && errno == EINTR) continue; if (r <= 0) break; got += size_t(r); }
        if (got != out.size()) return false;
        uLong c = crc32(0L, Z_NULL, 0); c = crc32(c, out.data(), uInt(out.size()));
        return uint64_t(uint32_t(c)) == e.hash;
    }
    return false;
}
bool fused_name(const std::string &k, size_t &dot) {   // "F1." "F2." "F1A." "F1AS." "F1AR." + 16 hex digits
    dot = k.find('.');
    if (dot == std::string::npos || dot < 2 || dot > 4 || k[0] != 'F' || (k[1] != '1' && k[1] != '2') || k.size() != dot + 17) return false;
    return true;
}
// compose (xbg_nb_post.inc compose): false = refused (why)
bool compose(const std::string &key, const std::string &host, vkn_shlib_reader read, Rec &x) {
    const char *w = nullptr;
    auto fail = [&](const std::string &s) { x.why = s; return false; };
    if (!pack_get(key, x.blob)) return fail(g_fd < 0 ? "no native-post.xgspack" : "no such bundle in native-post.xgspack (or its CRC differs)");
    if (nb_fx_parse(x.blob.data(), x.blob.size(), &x.fx, &w)) return fail(std::string("XGF1 ") + (w ? w : "?"));
    if (x.fx.n_elided != 1) return fail("not one elided pass");
    if (strncmp(x.fx.host_key, host.c_str(), 16)) return fail("the bundle's host key differs from its name");
    {
        std::vector<uint8_t> tb; bool fp = false;
        if (!read(host, tb, fp) || !nb_fx_host_head_matches(&x.fx, tb.data(), tb.size()))
            return fail("host head differs from the translated bundle (rebuild with setup / tools/dos2_post_fuse.py)");
    }
    std::vector<uint8_t> b(x.fx.bundle, x.fx.bundle + x.fx.bundle_size);
    for (uint32_t i = 0; i < x.fx.host_bindings; i++) {
        xbg_shader_binding hb; memcpy(&hb, b.data() + 32 + 32 * i, 32);
        if ((hb.source == XBG_SOURCE_TEXTURE && hb.slot + (hb.count ? hb.count : 1) > SPARE_TEX) || (hb.source == XBG_SOURCE_UNIFORM && hb.slot >= SPARE_UBO - 1))
            return fail("host binding on a spare slot");
    }
    int nt = 0, nu = 0;
    for (uint32_t i = 0; i < x.fx.n_extra; i++) {
        const nb_fx_extra &e = x.fx.extra[i]; uint8_t *ent = b.data() + 32 + 32 * (x.fx.host_bindings + i); uint32_t src = 0, slot = 0;
        if (e.src_kind == NB_FX_K_TEX) { if (nt >= 8) return fail("too many extra textures"); src = XBG_SOURCE_TEXTURE; slot = SPARE_TEX + uint32_t(nt++); }
        else if (e.src_kind == NB_FX_K_CBUF) { if (nu >= 2) return fail("too many extra c-buffers"); src = XBG_SOURCE_UNIFORM; slot = SPARE_UBO - uint32_t(nu++); }
        else src = XBG_SOURCE_SUPPORT;
        if (e.src_slot >= (e.src_kind == NB_FX_K_TEX ? 32u : 16u)) return fail("extra source slot out of the backend's slot range");
        if ((e.flags & NB_FX_E_LINEAR) && e.src_kind != NB_FX_K_TEX) return fail("NB_FX_E_LINEAR on a buffer");
        memcpy(ent + 16, &src, 4); memcpy(ent + 20, &slot, 4);
    }
    x.blob.swap(b);   // the composed XGS1 bundle (x.fx keeps the parsed tables; its bundle pointer is not used again)
    x.fx.bundle = nullptr;
    return true;
}
}  // namespace

extern "C" void vkn_post_install(void);

int vkn_post_read(const std::string &key, std::vector<uint8_t> &out, bool &from_pack, vkn_shlib_reader read, const std::string &dir) {
    static std::once_flag inst;
    std::call_once(inst, [&] { g_dir = dir; if (vkn_mode() == VKN_NATIVE) vkn_post_install(); });
    size_t dot = 0;
    if (key.size() < 19 || key[0] != 'F' || !fused_name(key, dot) || !read) return -1;
    from_pack = true;
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_rec.find(key);
    if (it == g_rec.end()) {
        Rec x{}; x.ok = compose(key, key.substr(dot + 1), read, x);
        if (x.ok) vkn_logf("post: %s composed (%u extra bindings, %u requirements, flags %#x, %zu bytes)", key.c_str(), x.fx.n_extra, x.fx.n_req, x.fx.flags, x.blob.size());
        else vkn_logf(PP_RED "post: %s unusable: %s (that fusion stays off; the chain is drawn as recorded)", key.c_str(), x.why.c_str());
        it = g_rec.emplace(key, std::move(x)).first;
    }
    if (!it->second.ok) return 0;
    out = it->second.blob;
    return 1;
}

extern "C" int vkn_post_meta(const char *key, nb_fx *out, char *why, size_t why_bytes) {
    if (why && why_bytes) why[0] = 0;
    if (!key || !out) return -1;
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_rec.find(key);
    if (it == g_rec.end()) { if (why && why_bytes) snprintf(why, why_bytes, "not read"); return -1; }
    if (!it->second.ok) { if (why && why_bytes) snprintf(why, why_bytes, "%s", it->second.why.c_str()); return -1; }
    *out = it->second.fx;
    return 0;
}
#endif
