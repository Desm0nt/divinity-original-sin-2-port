// [DOS2] NR-MDI DM_NR_MDI (docs/dos2/nr/NR-MDI.md): the count that decides whether depth-only draws are worth batching into
// vkCmdDrawMultiIndexedEXT with the per-draw uniform blocks read by gl_DrawID. Included by xbg_game.cpp right after xbg_nr_sort.inc (it
// uses nrsort's order-free test pieces); the NR-09 lane calls note() for every draw it records, after its commands. Model in nr_mdi.h.
//   DM_NR_MDI  0  default: nothing (the lane pays one load and a not-taken branch).
//              2  count: every recorded draw goes through the model (recorded order: consecutive mergeable runs; sorted: distinct batch
//                 keys of the pass's order-free draws), what changes between consecutive depth draws, which dynamic offsets vary inside a
//                 would-be batch. Nothing is recorded differently. One line every 10 s: "[nr] mdi (DM_NR_MDI=2, ...)".
//              3  PROBE (breaks the picture, standing only, then off): as 2, and a depth-only draw whose set rebind would only change
//                 dynamic offsets keeps the previous binding - the draws read the previous draw's blocks (wrong shadows / depth). What the GPU
//                 saves then is the ceiling of what batching can save (the state between draws, NR-MDI.md section 3); read NVN GPASS /
//                 gpu_ms against a =2 run. DOOM Eternal had the same probe (DM_GFX_PROBE_KEEPSETS, GPUCOST 2026-11-05).
//              1  apply: not built (needs the VS variant that reads the per-draw blocks by gl_DrawID; NR-MDI.md section 5 says when it is
//                 worth building) - runs as 2, red line.
#include "../../nr/nr_mdi.h"
namespace nrmdi{
int mode=-1;nrmdi_state *st=nullptr;nrmdi_stats s{};uint64_t next_ns=0,fb_seen=0,frames0=0,kept=0;
void init(){const char *e=getenv("DM_NR_MDI");int m=e && *e?atoi(e):0;
    if(m==1){xbg_logf("【红】[nr] DM_NR_MDI=1: the apply mode is not built yet (docs/dos2/nr/NR-MDI.md section 5) - counting as DM_NR_MDI=2");m=2;}
    if(m!=2 && m!=3)m=0;
    if(m && !(st=static_cast<nrmdi_state*>(calloc(1,sizeof(nrmdi_state))))){xbg_logf("【红】[nr] DM_NR_MDI: no memory: off");m=0;}
    if(m==3)xbg_logf("【红】[nr] DM_NR_MDI=3: PROBE - offset-only set rebinds of depth-only draws are dropped, shadows and the depth pre-pass are WRONG on purpose (GPU ceiling of batching; standing, then off)");
    if(m)xbg_logf("[nr] DM_NR_MDI=%d (NR-MDI: count%s): depth-only draws of the NR-09 lane against the multi-draw batch rule (same pass, pipeline, layout, "
        "set, vertex / index bindings, viewport, scissor, instances; dynamic offsets and index ranges may differ), recorded order and sorted by key for "
        "order-free draws; %s",m,m==3?" + probe":" only",m==3?"depth-only offset-only set rebinds dropped (probe)":"nothing is recorded differently");
    mode=m;}
// =3 probe, from the lane's set bind: true = do not rebind (only when the layout and set are the bound ones and only offsets differ)
bool probe_keep(bool same_set,bool depth_only){if(!same_set || !depth_only)return false;++kept;return true;}
inline uint64_t hb(const void *p,size_t n){uint64_t h=0x9E37;const uint8_t *b=static_cast<const uint8_t*>(p);for(size_t i=0;i<n;i++)h=(h^b[i])*0x100000001B3ull;return h;}
// From lane(), after its commands (only while mode != 0).
void note(Commands &c,Descriptors &d,Pass &p,const p_draw_idx *indexed,uint32_t instances,uint32_t first_instance){
    if(mode<0)init();if(mode<=0)return;(void)c;
    const nrdraw::Hand &H=nrdraw::H;nrmdi_draw x{};
    x.pass=perf.passes;x.pipe=uint64_t(uintptr_t(H.pipe));x.layout=uint64_t(uintptr_t(d.pipeline_layout));x.set=uint64_t(uintptr_t(d.set));
    x.used=H.used;for(uint32_t m=H.used;m;m&=m-1){const uint32_t i=uint32_t(__builtin_ctz(m));x.vb[i]=uint64_t(uintptr_t(H.vb[i]));x.vbo[i]=H.vbo[i];}
    if(indexed){x.ib=uint64_t(uintptr_t(H.ib));x.ibo=H.ibo;x.it=uint32_t(H.it)+1;x.inst=uint32_t(indexed->instances);x.first_inst=uint32_t(indexed->base_instance);}
    else{x.inst=instances;x.first_inst=first_instance;}
    x.vp=hb(&H.vp,sizeof H.vp);x.sc=hb(&H.sc,sizeof H.sc);
    x.noff=uint32_t(d.dynamic_offsets.size());for(uint32_t i=0;i<x.noff && i<NRMDI_MAXOFF;i++)x.off[i]=d.dynamic_offsets[i];
    // depth-only and order-free: the NR-21 test (nrsort::take), with its own depth-feedback watermark
    nrs_in in{};in.query=open_occlusion.cmd!=VK_NULL_HANDLE || state.count_samples;in.condr=condr::cg.mode>0 && condr::cg.kind!=CONDR_K_ON;
    in.feedback=depth_feedback_reads!=fb_seen;fb_seen=depth_feedback_reads;
    in.storage=!storage_images_bound.empty() || d.needs_writeback() || nrsort::storage_stage(state.shader[0]) || nrsort::storage_stage(state.shader[4]);
    in.depth_att=state.rt.depth!=nullptr;for(const Image *im:p.images)if(im->aspects&VK_IMAGE_ASPECT_DEPTH_BIT){in.depth_fixed=nrsort::fixed_depth(im->fmt);break;}
    {const Shader *fs=state.shader[4];const bool noout=noout_mask_on && (!fs || (!fs->raster.outputs && !fs->raster.sample_mask));
     for(int i=0;i<state.rt.n && i<8;i++)if(state.rt.color[i] && !noout && ((state.mask.mask>>(4*i))&15u))in.color_written=1;}
    in.st=state.ds.st;in.bounds=state.bounds.enable!=0;in.a2c=state.ms.a2c!=0;in.dt=state.ds.dt;in.dw=state.ds.dw;in.op=state.ds.df>=1 && state.ds.df<=8?uint8_t(state.ds.df-1):uint8_t(255);
    x.depth_only=in.depth_att && !in.color_written;x.order_free=x.depth_only && nrs_why(&in)==NRS_OK;
    nrmdi_note(st,&s,&x);}
void tick(){if(mode<=0 || !st)return;const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;frames0=stats.presents.load();return;}if(now<next_ns)return;next_ns=now+10000000000ull;
    nrmdi_flush(st,&s);const uint64_t f1=stats.presents.load();const double fr=f1>frames0?double(f1-frames0):1.;frames0=f1;
    static const char *const kn[8]={"pipeline","set","dynamic offsets","vertex buffers","index buffer","viewport / scissor","instances","pass"};
    char ch[400];int k=0;ch[0]=0;for(int b=0;b<7;b++)k+=snprintf(ch+k,sizeof ch-size_t(k),"%s%s %.0f %%",b?", ":"",kn[b],s.chg_draws?100.*double(s.chg[b])/double(s.chg_draws):0.);
    char om[200];int o=0;om[0]=0;for(int i=0;i<=NRMDI_MAXOFF && o<180;i++)if(s.offmask_or[i])o+=snprintf(om+o,sizeof om-size_t(o),"%s%d:%llu",o?" ":"",i,(unsigned long long)s.offmask_or[i]);
    if(mode==3){xbg_logf("【红】[nr] mdi PROBE (DM_NR_MDI=3): %.1f offset-only set rebinds of depth-only draws not made a frame - the picture is wrong on purpose",kept/fr);kept=0;}
    xbg_logf("[nr] mdi (DM_NR_MDI=%d, NR-MDI, last 10 s, %.0f frames): lane draws %.1f a frame, depth-only %.1f; recorded order: %.1f batches a frame (%.1f draws in batches of 2+, "
        "sizes 1/2/3-4/5-8/9-32/33+ = %llu/%llu/%llu/%llu/%llu/%llu), set rebinds a batch would not emit %.1f a frame; varying dynamic offsets per batch (popcount:batches) %s; batches whose blocks step by one stride in the ring %.1f a frame (%.1f draws); "
        "sorted by key (order-free %.1f a frame in %.1f passes): %.1f batches a frame; between consecutive depth draws of a pass: %s",
        mode,fr,s.draws/fr,s.depth/fr,s.batches/fr,s.in_multi/fr,(unsigned long long)s.sizes[0],(unsigned long long)s.sizes[1],(unsigned long long)s.sizes[2],(unsigned long long)s.sizes[3],
        (unsigned long long)s.sizes[4],(unsigned long long)s.sizes[5],s.batched_rebinds/fr,om[0]?om:"-",s.arith_batches/fr,s.arith_draws/fr,s.of_draws/fr,s.of_passes/fr,s.sorted_batches/fr,ch);
    s=nrmdi_stats{};}
}
