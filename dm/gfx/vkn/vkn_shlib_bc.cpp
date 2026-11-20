// [DOS2] VK-NATIVE stage 4 (GPU BC route of vkn_bcgpu.c, KB TEX-01 / TEX-02 / TEX-18): what the backend takes from the GPU BC decoder
// (xbg_bc_decode.comp) and from port-common bc-astc-gpu (xbg_bc_astc_encode.comp) to run them on its OWN pipelines, descriptor sets and
// scratch buffers: the SPIR-V, the encoder tables, both self-test sets in their batch layout (xbg_bc_gpu.h XbgBcGpuBatch) and both
// verdicts exactly as the translator's bcgpu::selftest / bcastc::selftest judge them, the encoder's per-format flags and shape rules.
// Moved here from xbg_game.cpp's VK-NATIVE export block (the shader library piece: mode 1 calls nothing of xbg_game.cpp); the code is
// that block's, renamed vkn_shlib_bc* (declarations: vkn_bcgpu.h). Pure functions of their arguments over the engine's header-only
// data / helpers (static inline, so this unit has its own copies) and xbg_texture.c's CPU decoder. Only BC1 / BC1A / BC2 / BC3 / BC7
// and their sRGB twins (0x41..0x48, 0x4d, 0x4e): the decoder writes RGBA8 for them whatever xbg_bc_8bit_channels / xbg_bc6_e5 say.
#ifdef DM_GAME_DOS2
#include <stdio.h>
#include <string.h>
#include <vector>
#include <vulkan/vulkan.h>
#include "../vulkan/engine/xbg_bc_gpu.h"
#include "../vulkan/engine/xbg_bc_decode_spv.h"
#include "../vulkan/engine/xbg_bc_astc.h"
#include "../vulkan/engine/xbg_bc_astc_tables.h"
#include "../vulkan/engine/xbg_bc_astc_encode_spv.h"
#include "vkn_bcgpu.h"   // the declarations (signatures checked here)
static bool bc_rgba8(uint32_t f){return (f>=0x41 && f<=0x48) || f==0x4d || f==0x4e;}
extern "C" const uint32_t *vkn_shlib_bc_blob(int which,size_t *bytes){
    if(bytes)*bytes=which==0?sizeof(xbg_bc_decode_cs):which==1?sizeof(xbg_bc_astc_encode_cs):which==2?sizeof(xbg_bc_astc_tables):0;
    return which==0?xbg_bc_decode_cs:which==1?xbg_bc_astc_encode_cs:which==2?xbg_bc_astc_tables:nullptr;}
extern "C" int vkn_shlib_bc_kat_input(uint32_t fmt,void *in,size_t in_cap,size_t *in_bytes,size_t *out_bytes,uint32_t push[4]){
    xbg_bc_format info;if(!bc_rgba8(fmt) || xbg_bc_format_info(fmt,&info))return -1;
    const uint32_t bb=info.bytes_per_block,bpp=info.bytes_per_pixel,bx=64,by=32;   // bcgpu::selftest: 2048 blocks, one region
    std::vector<uint8_t> raw;xbg_bc_kat_blocks(fmt,bb,bx*by,raw);XbgBcGpuBatch batch;batch.add(raw.data(),bx,by,1,bb,bpp);
    if(!in || batch.input_bytes()>in_cap)return -1;
    batch.write_input(in);*in_bytes=batch.input_bytes();*out_bytes=batch.out;
    push[0]=uint32_t(batch.regions());push[1]=batch.blocks;push[2]=bpp;push[3]=bb/4;return 0;}
extern "C" int vkn_shlib_bc_kat_check(uint32_t fmt,const void *out,size_t out_bytes,char *line,size_t cap){
    xbg_bc_format info;if(!bc_rgba8(fmt) || xbg_bc_format_info(fmt,&info) || !out)return -1;
    const uint32_t bb=info.bytes_per_block,bpp=info.bytes_per_pixel,bx=64,by=32,n=bx*by;
    if(out_bytes<size_t(n)*16*bpp)return -1;
    std::vector<uint8_t> raw;xbg_bc_kat_blocks(fmt,bb,n,raw);const uint8_t *o=static_cast<const uint8_t*>(out);int bad=0;uint32_t modes=0;
    if(line && cap)line[0]=0;
    for(uint32_t i=0;i<n;i++){   // bcgpu::selftest's comparison: decoder rows of 64 * 4 pixels, every pixel byte equal (no SNORM here)
        uint8_t cpu[128];const uint8_t *blk=raw.data()+size_t(i)*bb;if(xbg_decode_block(fmt,blk,bb,cpu,sizeof cpu))continue;
        int bad_row=-1;
        for(uint32_t row=0;row<4 && bad_row<0;row++){const uint8_t *g=o+((size_t(i/bx)*4+row)*bx*4+size_t(i%bx)*4)*bpp,*e=cpu+row*4*bpp;if(memcmp(g,e,4*bpp))bad_row=int(row);}
        if(bad_row<0)continue;
        int m=-1;if(fmt==0x4d || fmt==0x4e){m=8;for(int k=0;k<8;k++)if(blk[0]>>k&1){m=k;break;}}
        if(m>=0)modes|=1u<<m;
        if(!bad++ && line && cap){char rh[40],eh[40],gh[40];const uint8_t *g=o+((size_t(i/bx)*4+uint32_t(bad_row))*bx*4+size_t(i%bx)*4)*bpp,*e=cpu+bad_row*4*bpp;
            for(uint32_t k=0;k<bb;k++){snprintf(rh+2*k,3,"%02x",blk[k]);}for(uint32_t k=0;k<16 && k<4*bpp;k++){snprintf(eh+2*k,3,"%02x",e[k]);snprintf(gh+2*k,3,"%02x",g[k]);}
            snprintf(line,cap,"first: block %u mode %d row %d raw=%s cpu=%s gpu=%s",i,m,bad_row,rh,eh,gh);}
    }
    if(bad && line && cap){const size_t at=strlen(line);snprintf(line+at,cap-at," (BC7 modes %#x)",modes);}
    return bad;}
extern "C" int vkn_shlib_bca_kat_input(void *in,size_t in_cap,size_t *in_bytes,void *rgba,size_t rgba_cap,size_t *rgba_bytes,uint32_t push[][4],uint32_t max_disp,uint32_t *ndisp,uint32_t *blocks){
    XbgAstcKatSet s;xbg_bc_astc_kat(s);XbgBcGpuBatch batch;std::vector<size_t> out_at;   // bcastc::selftest: one region a category, BC blocks as 4 words
    for(const auto &c:s.cats)out_at.push_back(batch.add(s.bc.data()+size_t(c.first)*4,c.count,1,1,16,4));
    if(!in || !rgba || batch.input_bytes()>in_cap || batch.out>rgba_cap || s.cats.size()>max_disp)return -1;
    batch.write_input(in);uint8_t *o=static_cast<uint8_t*>(rgba);memset(o,0,batch.out);
    for(size_t k=0;k<s.cats.size();k++){const auto &c=s.cats[k];const uint32_t row=c.count*4;   // decoder layout: rows of count * 4 texels
        for(uint32_t b=0;b<c.count;b++)for(uint32_t t=0;t<16;t++)memcpy(o+out_at[k]+(size_t(t>>2)*row+size_t(b)*4+(t&3))*4,s.rgba.data()+(size_t(c.first+b)*16+t)*4,4);
        push[k][0]=uint32_t(batch.regions());push[k][1]=c.first;push[k][2]=c.first+c.count;push[k][3]=xbg_bc_astc_direct(c.nvn)|(4u<<8);}
    *in_bytes=batch.input_bytes();*rgba_bytes=batch.out;*ndisp=uint32_t(s.cats.size());*blocks=s.blocks();return 0;}
extern "C" int vkn_shlib_bca_kat_check(const void *enc,uint32_t cands,int *zero,char *line,size_t cap){
    if(!enc)return 0;
    XbgAstcKatSet s;xbg_bc_astc_kat(s);const float *expected=xbg_bc_astc_kat_expected(cands);
    const XbgAstcKatResult r=xbg_bc_astc_check(s,static_cast<const uint8_t*>(enc),expected);
    if(zero)*zero=r.zero!=0;
    const bool good=r.bad==0 && r.blocks==s.blocks();
    if(line && cap){
        size_t at=size_t(snprintf(line,cap,"blocks=%u candidates=%u bad=%u (error %u, zero %u, worse than host*1.25+10 %u) psnr avg/min",r.blocks,cands,r.bad,r.error,r.zero,r.worse));
        for(size_t k=0;k<s.cats.size() && k<8 && at+40<cap;k++)at+=size_t(snprintf(line+at,cap-at," %s=%.1f/%.1f",s.cats[k].name,r.cat[k].n?r.cat[k].psnr_sum/r.cat[k].n:0.0,r.cat[k].min_psnr));
        if(r.first_bad!=UINT32_MAX && at+80<cap){uint32_t w[4];memcpy(w,static_cast<const uint8_t*>(enc)+size_t(r.first_bad)*16,16);
            snprintf(line+at,cap-at," first bad block %u: %08x %08x %08x %08x",r.first_bad,w[0],w[1],w[2],w[3]);}}
    return good?1:0;}
extern "C" uint32_t vkn_shlib_bca_flags(uint32_t nvn){return xbg_bc_astc_family(nvn)<0?0u:xbg_bc_astc_flags(nvn);}
extern "C" const char *vkn_shlib_bca_shape(uint32_t nvn,int target,uint32_t w,uint32_t h,uint32_t levels,uint32_t layers,int samples,int storage,int vt){
    return xbg_bc_astc_shape(nvn,target,w,h,levels,layers,samples,storage?0x4u:0u,vt!=0);}
#endif
