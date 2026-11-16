// [DOS2] NB-19 DM_NB_TRANSIENT (docs/dos2/NATIVE_BACKEND_PLAN.md "NB-19 findings"): attachments whose contents never leave a render pass
// live in tile memory only - re-created with VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT and VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT memory
// (no DRAM backing on Mali / PowerVR / Xclipse; on Adreno / Turnip sysmem nothing is gained, KB FN-23). Included by xbg_game.cpp before
// begin_pass. The decision is dm/gfx/nb/nb_transient.h (host test tests/nb_transient_test.c); this file gathers its inputs and does the
// Vulkan side.
//   inputs  begin(): every host pass begin (lrz_touch_before, all begin paths) flags its attachments; a pass begun from the applied NB
//           header (nbrp_begin / nbrp_lazy_begin call decide() right before) is "planned" with its real load ops (DONT_CARE bits of the
//           twin, or the host's clear twin bits on every aspect) and store DONT_CARE bits; any other begin = UNPLANNED.
//           touch(): Image::transition to TRANSFER_SRC (any) / TRANSFER_DST (of a GPU-written image: an upload before it was ever
//           rendered does not count), Image::view for a sampled / storage view, prepare_read - every use a transient image cannot have.
//   judge   frame() after each present: every image flagged this frame gets one reason (nb_tr_judge) and an action.
//   1       CREATE: a new VkImage of the original create info with only the attachment usage bits + TRANSIENT, memory of a LAZILY_ALLOCATED
//           type when the driver lists one for it, else device-local (valid, nothing saved: lets the path run on Adreno /
//           DM_GFX_SIM_GPU=mali); the old image, views and framebuffers are retired as upgrade_mutable does (descriptor memo epoch, image
//           cache, last pass). Skipped (counted) while a pass is open or a clear of the image is pending. The contents are dead: the
//           last pass of the frame stored DONT_CARE and the next frames begin it with CLEAR / DONT_CARE (>= DM_NB_TRANSIENT_FRAMES frames).
//           Use that needs other usage while transient (touch): re-created as before at once (contents lost: one frame of garbage), banned.
//           A transient image loaded / stored / begun by heuristics: valid Vulkan (lazy memory is committed), reverted at the frame end.
//   2       count only: the same reasons and candidates, nothing re-created.
// Line "[nb] transient ..." every 10 s while DM_NB_TRANSIENT != 0. With 0 every hook is one test of nbtr::mode: output unchanged.
namespace nbtr{
int mode=0,min_frames=8;unsigned live=0;bool logged_mem=false;
struct Pending{bool valid=false;const Pass *p=nullptr;uint32_t ok=0,store=0;} dec;   // the decision of the host pass about to begin
std::vector<Image*> listed;   // images flagged this frame (Image::nbtr_listed = frame + 1)
struct Stats{uint64_t frames=0,judged=0,why[NB_TRR_KINDS]{},shape[NB_TRS_KINDS]{},cand_px=0,created=0,create_fail=0,skip_open=0,skip_clear=0,
    rev_use[2]{},rev_pat[NB_TRR_KINDS]{},unplanned_passes=0,planned_passes=0;} s;
uint64_t next_ns=0;unsigned told=0;
std::map<std::tuple<int,int,uint32_t,uint32_t>,bool> usage_ok;   // (format, samples, usage, flags) -> the driver takes it with TRANSIENT
inline uint32_t frame_no(){return uint32_t(stats.presents.load());}
int lazy_type(uint32_t bits){for(uint32_t i=0;i<memories.memoryTypeCount;i++)if((bits>>i&1u) && (memories.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT))return int(i);return -1;}
void init(){const char *e=getenv("DM_NB_TRANSIENT");mode=e && *e?std::clamp(atoi(e),0,2):0;
    e=getenv("DM_NB_TRANSIENT_FRAMES");if(e && *e)min_frames=std::max(1,atoi(e));
    if(!mode)return;
    int lazy=-1;for(uint32_t i=0;i<memories.memoryTypeCount && lazy<0;i++)if(memories.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)lazy=int(i);
    xbg_logf("[nb] DM_NB_TRANSIENT=%d (NB-19: %s after %d clean frames; needs DM_NB=1 + DM_NB_RP=1/3%s); lazily allocated memory type: %s",mode,
        mode==1?"attachments never read / copied, begun CLEAR / DONT_CARE and stored DONT_CARE by the plan, re-created transient":"transient candidates counted, nothing re-created",
        min_frames,(nb_rp_mode&1)?"":" - DM_NB_RP is not 1/3 here: every pass is unplanned, no candidates",
        lazy>=0?"yes":"none on this driver (re-created images get device-local memory: valid, nothing saved)");
    if(lazy>=0)xbg_logf("[nb] transient: lazily allocated memory type %d flags %#x heap %u",lazy,unsigned(memories.memoryTypes[lazy].propertyFlags),memories.memoryTypes[lazy].heapIndex);}
void list(Image &im,uint32_t fr){if(im.nbtr_listed!=fr+1){im.nbtr_listed=fr+1;listed.push_back(&im);}}
void gone(Image *im){if(im->nbtr.transient && live)--live;im->nbtr.transient=0;
    if(im->nbtr_listed)for(size_t i=0;i<listed.size();i++)if(listed[i]==im){listed[i]=listed.back();listed.pop_back();break;}}
// nbrp_begin / nbrp_lazy_begin, right before the pass begins: ok = bits of attachments begun without their old texels, store = DONT_CARE bits.
void decide(const Pass &p,uint32_t ok,uint32_t store){if(!mode)return;dec.valid=true;dec.p=&p;dec.ok=ok;dec.store=store;}
// ok bits of the lazy (clear twin) begin: the host's clear mask covers every aspect of the attachment, or the plan's DONT_CARE.
uint32_t ok_bits(const Pass &p,uint32_t clear_mask,uint32_t dc){uint32_t ok=dc;
    for(uint32_t k=0;k<p.images.size() && k<9;k++){const VkImageAspectFlags a=p.images[k]->aspects;const unsigned c=(clear_mask>>(2*k))&3u;
        const bool all=(a&VK_IMAGE_ASPECT_COLOR_BIT)?(c&1u)!=0:(!(a&VK_IMAGE_ASPECT_DEPTH_BIT) || (c&1u)) && (!(a&VK_IMAGE_ASPECT_STENCIL_BIT) || (c&2u));
        if(all)ok|=1u<<k;}
    return ok;}
void begin(const Pass &p){if(!mode)return;const uint32_t fr=frame_no();const bool planned=dec.valid && dec.p==&p;
    if(planned)++s.planned_passes;else ++s.unplanned_passes;
    for(uint32_t k=0;k<p.images.size() && k<9;k++){Image &im=*p.images[k];nb_tr_pass(&im.nbtr,fr,planned,(dec.ok>>k)&1u,(dec.store>>k)&1u);list(im,fr);}
    dec=Pending{};}
// Vulkan side: swap im's image + memory for (img, mem); the old ones and their views / framebuffers retire with the recording slot.
void swap_in(Image &im,VkImage img,VkDeviceMemory mem,VkImageUsageFlags usage,int64_t bytes){
    VkImage old_image=im.image;VkDeviceMemory old_memory=im.memory;std::vector<VkImageView> old_views;for(auto &v:im.views)old_views.push_back(v.second);im.views.clear();
    std::vector<VkFramebuffer> old_fbs;forget_framebuffers(im,&old_fbs);
    retire_handle([old_image,old_memory,old_views,old_fbs]{for(auto f:old_fbs)vkDestroyFramebuffer(cfg.device,f,nullptr);for(auto v:old_views)vkDestroyImageView(cfg.device,v,nullptr);vkDestroyImage(cfg.device,old_image,nullptr);vkFreeMemory(cfg.device,old_memory,nullptr);});
    im.image=img;im.memory=mem;im.usage=usage;im.layout=VK_IMAGE_LAYOUT_UNDEFINED;image_bytes_live+=bytes-im.mem_bytes;im.mem_bytes=bytes;
    ++memo_epoch;img_cache.clear();last_pass_reset();}
bool make(Image &im,bool transient){
    VkImageCreateInfo ci=im.create;ci.pNext=nullptr;
    if(transient)ci.usage=(im.create.usage&(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))|VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    VkImageFormatListCreateInfo fl{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    if(im.mutable_format && !im.view_formats.empty() && &dm_vk_format_list && dm_vk_format_list){fl.viewFormatCount=uint32_t(im.view_formats.size());fl.pViewFormats=im.view_formats.data();ci.pNext=&fl;}
    VkImage img{};if(vkCreateImage(cfg.device,&ci,nullptr,&img)!=VK_SUCCESS){if(!transient)xbg_fatalf("NB-19: re-creating serial=%llu as before failed",(unsigned long long)im.source.serial);return false;}
    VkMemoryRequirements r;vkGetImageMemoryRequirements(cfg.device,img,&r);
    const int lt=transient?lazy_type(r.memoryTypeBits):-1;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=r.size;ai.memoryTypeIndex=lt>=0?uint32_t(lt):memory_type(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory mem{};VkResult ar=vkAllocateMemory(cfg.device,&ai,nullptr,&mem);
    if(ar!=VK_SUCCESS && lt>=0){ai.memoryTypeIndex=memory_type(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);ar=vkAllocateMemory(cfg.device,&ai,nullptr,&mem);}
    if(ar==VK_SUCCESS && vkBindImageMemory(cfg.device,img,mem,0)!=VK_SUCCESS){vkFreeMemory(cfg.device,mem,nullptr);ar=VK_ERROR_UNKNOWN;}
    if(ar!=VK_SUCCESS){vkDestroyImage(cfg.device,img,nullptr);if(!transient)xbg_fatalf("NB-19: memory for re-created serial=%llu failed",(unsigned long long)im.source.serial);return false;}
    swap_in(im,img,mem,ci.usage,int64_t(r.size));im.nbtr_lazy=transient && ai.memoryTypeIndex==uint32_t(lt) && lt>=0;
    if(told<16){++told;xbg_logf("[nb] transient: serial=%llu fmt=%d %ux%u samples=%d %s (%s memory, %.2f MB; %u other transient images)",(unsigned long long)im.source.serial,int(im.fmt),im.width,im.height,int(im.samples),
        transient?"re-created transient":"re-created as before (banned)",im.nbtr_lazy?"lazily allocated":"device-local",double(r.size)/1048576.,live);}
    return true;}
void revert(Image &im){if(live)--live;make(im,false);}
// Any use of im that is not an attachment of a pass (kind NB_TRF_READ / NB_TRF_XFER).
void touch(Image &im,unsigned kind){if(kind==NB_TRF_XFER && !im.gpu_written && !im.nbtr.transient)return;   // an upload before it was ever rendered
    if(nb_tr_touch(&im.nbtr,frame_no(),kind)){++s.rev_use[kind==NB_TRF_XFER];revert(im);}}
int shape(Image &im){
    int aliased=0;if(im.source.mem)for_images_overlapping(uint64_t(uintptr_t(im.source.mem)),std::max<uint64_t>(1,im.source.L.size),[&](Image &o){if(&o!=&im)aliased=1;});
    const bool t2d=im.create.imageType==VK_IMAGE_TYPE_2D && im.depth==1 && !(im.create.flags&(VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT|VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT));
    int sh=nb_tr_shape(t2d,im.levels,im.layers,aliased,1);if(sh!=NB_TRS_OK)return sh;
    const uint32_t usage=(im.create.usage&(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))|VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    auto key=std::make_tuple(int(im.create.format),int(im.create.samples),usage,uint32_t(im.create.flags));auto it=usage_ok.find(key);
    if(it==usage_ok.end()){VkImageFormatProperties fp;bool ok=(usage&~VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)!=0 &&
        vkGetPhysicalDeviceImageFormatProperties(cfg.physical,im.create.format,im.create.imageType,im.create.tiling,usage,im.create.flags,&fp)==VK_SUCCESS && (fp.sampleCounts&im.create.samples);
        it=usage_ok.emplace(key,ok).first;}
    return it->second?NB_TRS_OK:NB_TRS_FORMAT;}
bool pending_clear(const Image &im){for(auto &e:pclears)if(e.im==&im)return true;return false;}
void tick(){const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;return;}if(now<next_ns)return;next_ns=now+10000000000ull;
    const double f=s.frames?double(s.frames):1.;double lazy_mb=0,plain_mb=0;
    for(auto &e:images){const Image &im=*e.second;if(im.nbtr.transient && e.first==im.source.serial)(im.nbtr_lazy?lazy_mb:plain_mb)+=double(im.mem_bytes)/1048576.;}
    xbg_logf("[nb] transient DM_NB_TRANSIENT=%d (last 10 s): %llu frames; host passes a frame planned %.1f unplanned %.1f; attachment images judged %.2f a frame: "
        "candidates %.2f (%.2f Mpx), rejected: banned %.2f shape %.2f (type %.2f levels %.2f layers %.2f alias %.2f format %.2f) read %.2f transfer %.2f unplanned %.2f load %.2f store %.2f streak %.2f; "
        "transient now %u (%.2f MB lazily allocated, %.2f MB device-local); created %llu (failed %llu; deferred: pass open %llu, pending clear %llu), "
        "reverted by a use %llu (read %llu, transfer %llu), by the pattern at the frame end %llu (load %llu, store %llu, unplanned %llu)",
        mode,(unsigned long long)s.frames,double(s.planned_passes)/f,double(s.unplanned_passes)/f,double(s.judged)/f,double(s.why[NB_TRR_CAND])/f,double(s.cand_px)/1e6/f,
        double(s.why[NB_TRR_BANNED])/f,double(s.why[NB_TRR_SHAPE])/f,double(s.shape[NB_TRS_TYPE])/f,double(s.shape[NB_TRS_LEVELS])/f,double(s.shape[NB_TRS_LAYERS])/f,
        double(s.shape[NB_TRS_ALIAS])/f,double(s.shape[NB_TRS_FORMAT])/f,double(s.why[NB_TRR_READ])/f,double(s.why[NB_TRR_XFER])/f,double(s.why[NB_TRR_UNPLANNED])/f,
        double(s.why[NB_TRR_LOAD])/f,double(s.why[NB_TRR_STORE])/f,double(s.why[NB_TRR_STREAK])/f,live,lazy_mb,plain_mb,
        (unsigned long long)s.created,(unsigned long long)s.create_fail,(unsigned long long)s.skip_open,(unsigned long long)s.skip_clear,
        (unsigned long long)(s.rev_use[0]+s.rev_use[1]),(unsigned long long)s.rev_use[0],(unsigned long long)s.rev_use[1],
        (unsigned long long)(s.rev_pat[NB_TRR_LOAD]+s.rev_pat[NB_TRR_STORE]+s.rev_pat[NB_TRR_UNPLANNED]),(unsigned long long)s.rev_pat[NB_TRR_LOAD],
        (unsigned long long)s.rev_pat[NB_TRR_STORE],(unsigned long long)s.rev_pat[NB_TRR_UNPLANNED]);
    s=Stats{};}
// After each present (xbg_present_texture_now): judge the images flagged this frame.
void frame(){if(!mode)return;++s.frames;
    for(Image *p:listed){Image &im=*p;im.nbtr_listed=0;if(!(im.nbtr.frame_flags&NB_TRF_ATT))continue;
        const int sh=(im.nbtr.banned || (im.nbtr.ever&(NB_TRF_READ|NB_TRF_XFER)))?NB_TRS_OK:shape(im);   // shape only where it can decide
        int act=NB_TRA_NONE;const int why=nb_tr_judge(&im.nbtr,sh,min_frames,mode==NB_TR_ON,&act);
        ++s.judged;++s.why[why<NB_TRR_KINDS?why:0];if(why==NB_TRR_SHAPE)++s.shape[sh<NB_TRS_KINDS?sh:0];
        if(why==NB_TRR_CAND)s.cand_px+=uint64_t(im.width)*im.height;
        if(act==NB_TRA_CREATE){
            if(open_pass.cmd || lazy_pending){im.nbtr.transient=0;++s.skip_open;continue;}   // next frame (the streak keeps going)
            if(pclear_n && pending_clear(im)){im.nbtr.transient=0;++s.skip_clear;continue;}
            if(make(im,true)){++live;++s.created;}else{im.nbtr.transient=0;im.nbtr.banned=1;++s.create_fail;}}
        else if(act==NB_TRA_REVERT){++s.rev_pat[why<NB_TRR_KINDS?why:0];revert(im);}}
    listed.clear();tick();}
}
