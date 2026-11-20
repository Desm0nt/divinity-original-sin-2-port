// [DOS2] NR-TRAFFIC DM_NR_LINTEX (docs/dos2/nr/NR-TRAFFIC.md, core dm/gfx/nr/nr_lintex.h): pitch-linear textures written by the CPU
// re-upload only the rows written since the last upload, through the dirty tracker, without hashing.
// Included by xbg_game.cpp right before partial_eligible() (namespace nrlin):
//   mode 0 (default): nothing here runs (every hook is behind nrlin::mode, the old path as before);
//   mode 1: a pitch-linear 2D colour texture of one level / layer (not BC, not decoded, not an NR-05 material, >= DM_NR_LINTEX_MIN_KB)
//     is write-protected BEFORE its upload reads it (stamp taken after the protect: a write during the read faults and is seen by
//     the next check), its rows go straight from the guest into the staging arena (no zero-filled vector, no second copy) and no hash
//     is taken (cpu_hash 1 = "uploaded, the tracker answers"); a later check that sees written pages copies only the rows that touch
//     them (nrlt_runs) into the image, ahead of the submission when no command of it used the image yet (gfx-upload-prologue rule),
//     else in place. Dense updates (> DM_NR_LINTEX_DENSE % of rows, DM_NR_LINTEX_STREAK in a row) give the texture back to the old
//     path for good (hash + whole upload, no protection: a texture rewritten whole would pay a write fault per page).
//   mode 2 (verify): drawing as mode 0; for every upload of such a texture the bytes are kept (up to DM_NR_LINTEX_SHADOW_MB), the
//     pages protected and the stamp taken; at the next upload the rows that really changed are compared with the rows the tracker
//     reports (missed rows = red line, must stay 0) and what mode 1 would have copied is counted.
// One start line, one line every 10th NVN PERF window ("[nr] lintex ...").
namespace nrlin {
int mode=-1;uint64_t min_bytes=256*1024;uint32_t dense_pct=50,streak=3,gap=4,group=16;uint64_t shadow_cap=64ull<<20;
uint64_t full_n=0,full_bytes=0,full_ns=0,part_whole=0,part_n=0,part_rows=0,part_dirty=0,part_bytes=0,part_ns=0,part_empty=0,part_pro=0,part_inpass=0,part_broke=0;
uint64_t rows_checked=0,pre_protect=0,reprotects=0,not_whole=0,hash_skip=0,hash_skip_bytes=0,gave_up=0,tracked=0,reds=0,reports=0,windows=0;
uint64_t v_checks=0,v_changed=0,v_dirty=0,v_missed=0,v_would=0,v_full=0,v_rows=0,v_noshadow=0;
uint64_t faults0=0,last_report=0;
struct Shadow{uint64_t serial=0,stamp=0,pitch=0,row=0;uint32_t h=0;std::vector<uint8_t> bytes;};
std::unordered_map<const Image*,Shadow> shadows;uint64_t shadow_bytes=0;
#define NRLIN_RED(...) do{if(nrlin::reds++<16)xbg_logf("【红】[nr] lintex: " __VA_ARGS__);}while(0)
static uint64_t env_u(const char *n,uint64_t d){const char *e=getenv(n);return e && *e?strtoull(e,nullptr,10):d;}
void init(){
    if(mode>=0)return;
    const char *e=getenv("DM_NR_LINTEX");mode=e && *e?atoi(e):0;if(mode<0 || mode>=NRLT_MODES)mode=0;
    min_bytes=env_u("DM_NR_LINTEX_MIN_KB",256)*1024;dense_pct=uint32_t(env_u("DM_NR_LINTEX_DENSE",50));streak=uint32_t(env_u("DM_NR_LINTEX_STREAK",3));
    gap=uint32_t(env_u("DM_NR_LINTEX_GAP",4));shadow_cap=env_u("DM_NR_LINTEX_SHADOW_MB",64)<<20;if(!streak)streak=1;
    const bool trk=nvn_dirty_tracked && nvn_dirty_protect && nvn_dirty_now && nvn_dirty_since;
    if(nvn_dirty_stats){uint64_t p;nvn_dirty_stats(&faults0,&p);}last_report=timing_clock();
    xbg_logf("[nr] DM_NR_LINTEX=%d (NR-TRAFFIC: %s); pitch-linear colour textures >= %llu KB, dense %u %% x %u, gap %u rows; dirty tracker %s, texture checks through it %d",
        mode,mode==NRLT_APPLY?"rows written since the last upload re-uploaded through the dirty tracker, no hashes, rows straight into the staging":mode==NRLT_VERIFY?"verify: drawn as 0, tracker rows vs changed rows at every upload":"off, the old path",
        (unsigned long long)(min_bytes>>10),dense_pct,streak,gap,trk?"present":"MISSING (mode 1 does nothing)",int(tex_dirty));
}
// The texture's shape is one this path handles: pitch-linear 2D colour, one level / layer / slice, 1x1 texel
// blocks of 1 / 2 / 4 / 8 / 16 bytes, not decoded / packed / ASTC-substituted, 1x sampled, rows fit the storage.
bool shape(const Image &im,const tex_t *t){
    if(!(t->d.flags&0x10) || !t->mem || t->L.size<min_bytes || im.levels!=1 || im.layers!=1 || im.depth!=1 || im.decoded || im.pack>=0 || im.subst
       || im.aspects!=VK_IMAGE_ASPECT_COLOR_BIT || im.samples!=VK_SAMPLE_COUNT_1_BIT || !im.bpp || (im.bpp&(im.bpp-1)) || t->d.target!=1)return false;   // NVN target 1 = 2D; bpp a power of two (16-aligned copy offsets are texel multiples)
    int bw=1,bh=1;nvn_fmt_bpb(t->d.format,&bw,&bh);if(bw!=1 || bh!=1)return false;
    const uint64_t row=uint64_t(im.width)*im.bpp;return nrlt_fits(t->L.size,im.height,nrlt_pitch(t->d.stride,im.width,im.bpp),row)!=0;
}
bool trackable(const tex_t *t){return tex_dirty && nvn_dirty_tracked && nvn_dirty_protect && nvn_dirty_now && nvn_dirty_since && !ab_off("texdirty") && nvn_dirty_tracked(t->mem,t->L.size);}
// mode 1: this texture goes the tracked row path (partial_eligible asks this for pitch-linear textures)
bool eligible(const Image &im,const tex_t *t){
    if(mode!=NRLT_APPLY || im.lin_off || (nrtex::mode>0 && im.nr==1))return false;
    return shape(im,t) && trackable(t);
}
// mode 1 / 2: the stamp, then the protection, then (the caller) the read. Stamp FIRST: a write that faults at any time after the stamp gets
// a newer generation, so whatever lands after the read is seen by the next check (protect-then-stamp leaves a write that faults between
// the two with a generation <= the stamp whose store may land after the read). A page another thread holds BUSY is skipped by
// nvn_dirty_protect and may end up writable with an old generation: protect again until every page is read-only (nvn_dirty_version),
// at most 8 rounds; *whole = 0 when that never happened (VMA pressure: such pages are GEN_STICKY, i.e. always dirty) - the caller then
// forces the next check (dirty_seen = 0) instead of trusting "no fault since".
uint64_t protect_stamp(const tex_t *t,bool &whole){
    if(nvn_dirty_stamp_protect){int w=1;const uint64_t st=nvn_dirty_stamp_protect(t->mem,t->L.size,&w);whole=w!=0;if(!whole)++not_whole;return st;}   // the tracker's own (round 2)
    const uint64_t stamp=nvn_dirty_now();whole=true;
    for(int k=0;;k++){nvn_dirty_protect(t->mem,t->L.size);uint64_t g,inc;if(!nvn_dirty_version || nvn_dirty_version(t->mem,t->L.size,&g,&inc))break;
        if(k==7){whole=false;++not_whole;break;}std::this_thread::yield();++reprotects;}
    return stamp;
}
struct Ctx{const uint8_t *base;uint64_t since;};
static int dirty_cb(void *c,uint64_t off,uint64_t len){const Ctx *x=static_cast<const Ctx*>(c);return nvn_dirty_since(x->base+off,size_t(len),x->since);}
// mode 1, full upload (first upload, or after a fallback): protect + stamp before the read, rows straight into the staging arena.
// Returns the stamp (non-zero) or 0 when the image does not go this way (the old path uploads it).
uint64_t full(Image &im,const tex_t *t,Buffer &staging,std::vector<VkBufferImageCopy> &copies,size_t &up_bytes){
    if(!eligible(im,t))return 0;
    const uint64_t t0=timing_clock();bool whole;const uint64_t stamp=protect_stamp(t,whole);if(!whole)im.lin_hold=true;   // stamp, protect, then read
    const uint64_t pitch=nrlt_pitch(t->d.stride,im.width,im.bpp),row=uint64_t(im.width)*im.bpp,total=row*im.height;
    staging=stream_buffer(size_t(total),nullptr,0,false);uint8_t *dst=static_cast<uint8_t*>(staging.mapped);
    if(pitch==row)memcpy(dst,t->mem,size_t(total));else for(uint32_t y=0;y<im.height;y++)memcpy(dst+uint64_t(y)*row,t->mem+uint64_t(y)*pitch,size_t(row));
    staging.flush();
    VkBufferImageCopy c{};c.bufferOffset=staging.offset;c.imageSubresource={im.aspects,0,0,1};c.imageExtent={im.width,im.height,1};copies.push_back(c);
    up_bytes=size_t(total);++full_n;full_bytes+=total;full_ns+=timing_clock()-t0;
    if(!im.lin_seen){im.lin_seen=true;++tracked;}
    return stamp;
}
// mode 1, a check saw written pages (protect + stamp already taken by the caller, xbg_game.cpp tracked branch): copy the rows written
// since `since`. false = give the texture back (dense updates): the caller then compares the hash (1 here) and uploads it whole.
bool partial(Image &im,const tex_t *t,uint64_t since){
    const uint64_t t0=timing_clock();
    const uint64_t pitch=nrlt_pitch(t->d.stride,im.width,im.bpp),row=uint64_t(im.width)*im.bpp;
    static std::vector<nrlt_run> runs;runs.resize(64);uint32_t drows=0;Ctx cx{t->mem,since};
    const uint32_t n=nrlt_runs(im.height,pitch,row,dirty_cb,&cx,group,gap,runs.data(),uint32_t(runs.size()),&drows);
    rows_checked+=im.height;
    if(nrlt_note(&im.lin_pol,drows,im.height,dense_pct,streak)){
        im.lin_off=true;++gave_up;
        {static unsigned told=0;if(told++<8)xbg_logf("[nr] lintex: serial=%llu %ux%u fmt=%#x rewritten densely (%u of %u rows, %u updates in a row): back to the old path (hash + whole upload)",(unsigned long long)t->serial,im.width,im.height,t->d.format,drows,im.height,streak);}
        return false;}
    if(!n){++part_empty;return true;}   // the written pages hold no texel row (padding between rows / around the storage)
    const uint64_t rows=nrlt_rows(runs.data(),n);
    Buffer staging=stream_buffer(size_t(rows*row+16*n),nullptr,0,false);uint8_t *dst=static_cast<uint8_t*>(staging.mapped);
    std::vector<VkBufferImageCopy> copies;copies.reserve(n);uint64_t at=0;
    for(uint32_t i=0;i<n;i++){at=(at+15)&~uint64_t(15);const nrlt_run &r=runs[i];
        if(pitch==row)memcpy(dst+at,t->mem+uint64_t(r.y0)*pitch,size_t(uint64_t(r.n)*row));
        else for(uint32_t y=0;y<r.n;y++)memcpy(dst+at+uint64_t(y)*row,t->mem+uint64_t(r.y0+y)*pitch,size_t(row));
        VkBufferImageCopy c{};c.bufferOffset=staging.offset+at;c.imageSubresource={im.aspects,0,0,1};c.imageOffset={0,int32_t(r.y0),0};c.imageExtent={im.width,r.n,1};copies.push_back(c);
        at+=uint64_t(r.n)*row;}
    staging.flush();
    if(upload_prologue && im.slot_use!=slot_serial && !ab_off("uppro")){Slot &s=recording_slot();s.pro_images.push_back({im.image,im.layout,im.aspects,im.levels,im.layers,staging.buffer,copies});im.layout=VK_IMAGE_LAYOUT_GENERAL;++part_pro;}
    else{++part_inpass;if(open_pass.cmd)++part_broke;
        Commands c;im.transition(c.command,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);vkCmdCopyBufferToImage(c.command,staging.buffer,im.image,im.layout,uint32_t(copies.size()),copies.data());im.transition(c.command,VK_IMAGE_LAYOUT_GENERAL);c.finish();}
    ++part_n;part_whole+=uint64_t(im.height)*row;part_rows+=rows;part_dirty+=drows;part_bytes+=rows*row;const uint64_t dt=timing_clock()-t0;part_ns+=dt;
    ++hud::reuploads;hud::upload_bytes+=rows*row;hud::upload_ns+=dt;   // the long-frame line counts it as a (small) re-upload
    return true;
}
// mode 2: after every upload of a texture of this shape (the old path drew it): compare the rows that changed since the previous upload
// with the rows the tracker reports, then keep the bytes and protect + stamp for the next one.
void verify(Image &im,const tex_t *t){
    if(mode!=NRLT_VERIFY || !shape(im,t) || !trackable(t) || (nrtex::mode>0 && im.nr==1))return;
    const uint64_t pitch=nrlt_pitch(t->d.stride,im.width,im.bpp),row=uint64_t(im.width)*im.bpp,total=row*im.height;
    auto it=shadows.find(&im);
    if(it!=shadows.end() && it->second.serial==t->serial && it->second.pitch==pitch && it->second.row==row && it->second.h==im.height && it->second.stamp){
        Shadow &s=it->second;static std::vector<nrlt_run> runs;runs.resize(64);uint32_t drows=0,changed=0;Ctx cx{t->mem,s.stamp};
        const uint32_t n=nrlt_runs(im.height,pitch,row,dirty_cb,&cx,group,gap,runs.data(),uint32_t(runs.size()),&drows);
        const uint32_t miss=nrlt_missed(s.bytes.data(),t->mem,pitch,im.height,row,runs.data(),n,&changed);
        ++v_checks;v_changed+=changed;v_dirty+=drows;v_missed+=miss;v_would+=nrlt_rows(runs.data(),n)*row;v_full+=total;v_rows+=im.height;
        if(miss)NRLIN_RED("serial=%llu %ux%u fmt=%#x: %u rows changed that the tracker did not report (%u changed, %u reported) - mode 1 would show stale rows",(unsigned long long)t->serial,im.width,im.height,t->d.format,miss,changed,drows);
    }else ++v_noshadow;
    Shadow *s=nullptr;
    if(it!=shadows.end() && it->second.bytes.size()==total)s=&it->second;
    else{if(it!=shadows.end()){shadow_bytes-=it->second.bytes.size();shadows.erase(it);}
        if(shadow_bytes+total>shadow_cap)return;
        s=&shadows[&im];s->bytes.resize(size_t(total));shadow_bytes+=total;}
    bool whole;s->stamp=protect_stamp(t,whole);s->serial=t->serial;   // the order mode 1 uses
   s->pitch=pitch;s->row=row;s->h=im.height;
    for(uint32_t y=0;y<im.height;y++)memcpy(s->bytes.data()+uint64_t(y)*row,t->mem+uint64_t(y)*pitch,size_t(row));
    if(!im.lin_seen){im.lin_seen=true;++tracked;}
}
void gone(const Image *im){if(mode!=NRLT_VERIFY)return;auto it=shadows.find(im);if(it!=shadows.end()){shadow_bytes-=it->second.bytes.size();shadows.erase(it);}}
void report(){
    if(mode<=0)return;if(++windows%10)return;const uint64_t now=timing_clock();const double secs=last_report?(now-last_report)/1e9:0;last_report=now;
    uint64_t f=0,p=0;if(nvn_dirty_stats)nvn_dirty_stats(&f,&p);const uint64_t faults=f-faults0;faults0=f;
    const double mb=1048576.0;
    xbg_logf("[nr] lintex (DM_NR_LINTEX=%d, NR-TRAFFIC, last %.0f s): pitch-linear textures on this path %llu (given back %llu); full uploads %llu (%.1f MB, %.2f ms, rows straight into the staging, protected before the read); "
             "partial %llu: rows %llu copied of %llu checked (%llu written, %.2f MB vs %.1f MB whole), %.2f ms, %llu with no texel row, ahead of the submission %llu / in place %llu (ended a pass %llu); hashes skipped %llu (%.1f MB); block-linear partial textures protected before their upload's read %llu; re-protects (a page was busy) %llu, never all read-only %llu; "
             "verify: uploads compared %llu (no copy yet %llu), rows changed %llu / reported %llu of %llu, MISSED %llu, would copy %.2f of %.1f MB, shadows %.1f MB; tracker write faults %llu (all textures and buffers)",
        mode,secs,(unsigned long long)tracked,(unsigned long long)gave_up,(unsigned long long)full_n,full_bytes/mb,full_ns/1e6,
        (unsigned long long)part_n,(unsigned long long)part_rows,(unsigned long long)rows_checked,(unsigned long long)part_dirty,part_bytes/mb,part_whole/mb,part_ns/1e6,
        (unsigned long long)part_empty,(unsigned long long)part_pro,(unsigned long long)part_inpass,(unsigned long long)part_broke,(unsigned long long)hash_skip,hash_skip_bytes/mb,(unsigned long long)pre_protect,(unsigned long long)reprotects,(unsigned long long)not_whole,
        (unsigned long long)v_checks,(unsigned long long)v_noshadow,(unsigned long long)v_changed,(unsigned long long)v_dirty,(unsigned long long)v_rows,(unsigned long long)v_missed,v_would/mb,v_full/mb,shadow_bytes/mb,(unsigned long long)faults);
    full_n=full_bytes=full_ns=part_whole=part_n=part_rows=part_dirty=part_bytes=part_ns=part_empty=part_pro=part_inpass=part_broke=rows_checked=pre_protect=reprotects=not_whole=hash_skip=hash_skip_bytes=0;
    v_checks=v_changed=v_dirty=v_missed=v_would=v_full=v_rows=v_noshadow=0;++reports;
}
}   // namespace nrlin
