// [DOS2] N6 "Хаки" (docs/dos2/nr/NR-HACKS.md; values and tiers: dm/gfx/nr/nr_hacks.h): the backend half of the lossy speed settings.
// Included by xbg_game.cpp right after xbg_nr_shc.inc (before draw()). The HLE half (render scale, bloom, FXAA, shadow-map size) is
// dm/host/hle_dos2_quality.c; FSR 1 EASU is a variant of the PPFXAA bundle in xbg_nr_sh.inc (nrsh::fsr_apply). Hooks, one line each:
//   draw() first line   far_draw(): DM_HACK_SHFAR - a draw into the far cascade's tile of the sun-shadow atlas is not drawn (1) / counted (2)
//   sampler()           par(): DM_HACK_TEXBIAS / DM_HACK_ANISO - the game's sampler of an uploaded mipmapped texture gets the bias / cap
//   present             tick(): one "[hack]" line every 10 s while any hack of this file is on
// Every value 0 (the default, no DM_HACK_* / tier) = today: far_draw() is one load and a branch, par() one load.
#include "../../nr/nr_hacks.h"
namespace nrhack {
int shfar=-1,texbias=0,aniso=0;   // shfar: -1 not read yet
uint64_t frame_key=~0ull;nrh_tiles tt{};uint64_t atlas_serial=0;
struct Stats{uint64_t frames=0,far_draws=0,atlas_draws=0,biased=0,capped=0,seen=0;} s;
uint64_t next_line=0;
void init(){
    char f1=0,f2=0,f3=0;shfar=nrh_value(NRH_SHFAR,&f1);texbias=nrh_value(NRH_TEXBIAS,&f2);aniso=nrh_value(NRH_ANISO,&f3);
    if(shfar || texbias || aniso)xbg_logf("[hack] backend: DM_HACK_SHFAR=%d%s (%s), DM_HACK_TEXBIAS=%.2f%s (mip LOD bias on uploaded mipmapped textures), DM_HACK_ANISO=%d%s (%s)",
        shfar,f1=='t'?" (tier)":"",shfar==1?"the far sun-shadow cascade is not drawn":shfar==2?"count only":"off",texbias/100.0,f2=='t'?" (tier)":"",aniso,f3=='t'?" (tier)":"",
        aniso?"anisotropy capped":"the game's anisotropy");}
// The sun-shadow atlas: depth only (no colour bound), square, >= 1024 (ShadowQuality 0/1/2 = 2048 / 1536 / 1024: map x 2, nr_shc.h). Its
// cascades are its tiles; the frame's first draw into each tile gives its ordinal (near first: the engine draws them in that order, NB IR
// dumps ra1 / rb1). The far cascade = the last: ordinal 1 of 2 (2048 atlas, 1024 x 2) or 2 of 3. A tile seen again in the frame keeps its
// ordinal; a fifth rect or another atlas in the same frame = not touched (counted as atlas draws only).
bool far_draw(){
    const tex_t *d=state.rt.depth;if(!d || d->d.width!=d->d.height || d->d.width<1024)return false;
    for(int i=0;i<state.rt.n && i<8;i++)if(state.rt.color[i])return false;
    const uint64_t fk=stats.presents.load(std::memory_order_relaxed);
    if(fk!=frame_key){frame_key=fk;tt.n=0;atlas_serial=d->serial;++s.frames;}
    if(d->serial!=atlas_serial)return false;
    const VkViewport &v=state.viewport;const int32_t r[4]={int32_t(v.x),int32_t(v.y),int32_t(v.width),int32_t(v.height)};
    const int f=nrh_far(&tt,r,d->d.width);if(f<0)return false;
    ++s.atlas_draws;if(!f)return false;
    ++s.far_draws;return shfar==1;}
// The game's sampler, before the sampler caches (their keys hold the changed values): an image the CPU uploaded, never rendered to, with
// mip levels (the materials' textures; render targets, the post chain and the UI atlas keep the game's values - a sampler bias also moves
// explicit textureLod reads, Vulkan's lambda = lod + mipLodBias).
samp_par par(const samp_par &in,const Image &im){
    samp_par p=in;if(!im.uploaded || im.gpu_written || im.levels<2)return p;
    if(texbias){p.lod_bias+=texbias/100.0f;++s.biased;}
    if(aniso && p.aniso>float(aniso)){p.aniso=float(aniso);++s.capped;}
    return p;}
void tick(){
    if(shfar<=0 && !texbias && !aniso)return;
    const uint64_t t=timing_clock();if(!next_line){next_line=t+10000000000ull;return;}if(t<next_line)return;next_line=t+10000000000ull;
    const double fr=s.frames?double(s.frames):1.0;
    xbg_logf("[hack] last 10 s: %llu frames with the shadow atlas, atlas draws %.0f a frame, far cascade %.0f a frame (%s), sampler lookups biased %llu, anisotropy capped %llu",
        (unsigned long long)s.frames,s.atlas_draws/fr,s.far_draws/fr,shfar==1?"not drawn":shfar==2?"counted, drawn":"off",(unsigned long long)s.biased,(unsigned long long)s.capped);
    s=Stats{};}
}
