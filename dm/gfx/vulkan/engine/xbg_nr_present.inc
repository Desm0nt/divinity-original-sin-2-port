// [DOS2] NR-11 DM_NR_PRESENT (docs/dos2/nr/NR-11.md; rules and counters: gfx/nr/nr_present.h; the presenter's side: android/vulkan_present.cpp
// namespace nrp). The game's last passes into its window texture X render straight into an acquired swapchain image; the present of such a
// frame is a layout transition and vkQueuePresentKHR, no blit.
//   0  (default) nothing: every hook below is one compare of nrpres::mode
//   1  on: at the SetRenderTargets that attaches the predicted X (with X's whole clear still pending) the presenter hands out an acquired
//      image; the slot's command buffer ends there and the rest of the frame goes into a second one (its batch, in the same vkQueueSubmit,
//      waits for the acquire: only the tail waits, the 3D does not); every pass on X attaches the image, X's pending clear lands on it; a read
//      / write of X outside a pass copies the image back into X and the frame ends with the old blit into the same image
//   2  count only: every decision made and checked, the old path executed (no acquire, no split); broken assumptions as red lines
//   3  as 1, and the image is cleared to magenta before the first pass (what the passes do not cover shows)
// Included by xbg_game.cpp after namespace taafix (inside the file's unnamed namespace). Forward declarations: next to Slot.
namespace nrpres {
int mode=0;nrp_frame fr{};nrp_pres_stats st{},prev{};
dm_nrp_img img{};bool have=false;   // the taken swapchain image of the active frame
VkSemaphore sig_next=VK_NULL_HANDLE;   // the present semaphore the next submission's last batch signals (direct present)
bool last_crop_full=true,last_flip=false;uint64_t frame_seq=0;   // the last present's crop / flip; write_seq at the last present
std::map<std::array<uint64_t,4>,VkFramebuffer> fbs;uint64_t fb_gen=0;   // one-attachment framebuffers on swapchain views, per swapchain generation
uint64_t copy_backs=0,splits=0,reported_ns=0;unsigned red_lines=0;uint64_t pstat_prev[6]{};
bool subst=false;   // create_pass: this attachment list holds the swapchain view
bool subst_pending(){return subst;}
void red(const char *fmt,...){if(red_lines>=16)return;++red_lines;char t[512];va_list a;va_start(a,fmt);vsnprintf(t,sizeof t,fmt,a);va_end(a);xbg_logf("【红】[nr] present: %s",t);}
void init(){
    const char *e=getenv("DM_NR_PRESENT");mode=e && *e?nrp_clamp_mode(atol(e)):0;if(!mode)return;
    const bool presenter=dm_present_nrp_query && dm_present_nrp_take && dm_present_async_nrp;
    if(!presenter){xbg_logf("[nr] DM_NR_PRESENT=%d ignored: this presenter has no direct present",mode);mode=0;return;}
    xbg_logf("[nr] DM_NR_PRESENT=%d: %s",mode,mode==2?"count only (decisions and checks, the old blit executed)":mode==3?"the last passes into the swapchain image, poisoned magenta first":"the last passes into the swapchain image, no present blit");
}
// The swapchain image went (a generation change): its framebuffers go with the current slot.
void drop_fbs(){for(auto &f:fbs){VkFramebuffer fb=f.second;retire_handle([fb]{vkDestroyFramebuffer(cfg.device,fb,nullptr);});}fbs.clear();}
// The whole image of X: one level, one layer, one sample, viewed in its own format.
bool whole_view(const Image &im,const view_t *v){
    if(im.samples!=VK_SAMPLE_COUNT_1_BIT || im.levels!=1 || im.layers!=1 || im.depth!=1 || im.aspects!=VK_IMAGE_ASPECT_COLOR_BIT || im.decoded || im.subst)return false;
    if(!v)return true;
    if(v->set_levels && (v->base_level!=0 || v->nlevels!=1))return false;if(v->set_layers && (v->min_layer!=0 || v->nlayers!=1))return false;
    return im.view_format(v)==im.fmt;
}
// Split the recording slot here: the command buffer so far ends, the tail of the frame goes into the slot's second one (submitted as the
// second batch of the same vkQueueSubmit, waiting for the acquire). Then the image goes UNDEFINED -> GENERAL behind that wait.
void split(){
    end_open_pass();Slot &s=recording_slot();
    if(samplets::armed && samplets::cmd==s.cmd)samplets::flush(s.cmd);
    nbmt::end(s.cmd);VK(vkEndCommandBuffer(s.cmd));s.nrp_main=s.cmd;
    if(!s.nrp_tail){VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};a.commandPool=s.pool?s.pool:command_pool;a.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;a.commandBufferCount=1;VK(vkAllocateCommandBuffers(cfg.device,&a,&s.nrp_tail));}
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;VK(vkBeginCommandBuffer(s.nrp_tail,&bi));
    s.cmd=s.nrp_tail;s.nrp_split=true;s.nrp_wait=img.acquired;
    nbmt::begin(s.cmd,s.pool!=VK_NULL_HANDLE,ab_off("nbmt"));   // [DOS2] NB-20b DM_NB_MT: the tail is recorded like the slot's own buffer
    bind_cache_reset();last_pass_reset();   // a new command buffer: nothing bound in it
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=img.image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;b.srcAccessMask=0;b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    // the source stages = the wait stages of the batch's acquire semaphore: the transition happens after the presentation engine let go
    vkCmdPipelineBarrier(s.cmd,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
    if(mode==3){VkClearColorValue m{};m.float32[0]=1;m.float32[2]=1;m.float32[3]=1;vkCmdClearColorImage(s.cmd,img.image,VK_IMAGE_LAYOUT_GENERAL,&m,1,&b.subresourceRange);full_barrier(s.cmd);}
    barrier_seq=write_seq;
}
// OP_SET_RT: the frame's one start decision, at the first SetRenderTargets that attaches the predicted window texture.
void set_rt(){
    if(fr.tried || !fr.p2)return;
    int slot=-1;for(int i=0;i<state.rt.n && i<8;i++)if(state.rt.color[i] && nrp_wants(&fr,state.rt.color[i]->serial)){slot=i;break;}
    if(slot<0)return;
    Image &im=get_image(state.rt.color[slot],false);
    nrp_facts x{};x.pending_clear=pclear_whole(&im);x.written=im.last_write>frame_seq;x.whole_view=whole_view(im,state.rt.has_cview[slot]?&state.rt.cview[slot]:nullptr);
    x.crop_full=last_crop_full;x.flip=last_flip || taafix::viewport_ll();x.cond_gpu=condr::cg.mode==CONDR_GPU || open_occlusion.cmd!=VK_NULL_HANDLE;
    x.presenter=NRP_P_OFF;
    const bool rest_ok=x.pending_clear && !x.written && x.whole_view && x.crop_full && !x.flip && !x.cond_gpu;
    if(rest_ok){if(mode==2)x.presenter=dm_present_nrp_query(im.fmt,im.width,im.height);
        else{dm_nrp_img got{};x.presenter=dm_present_nrp_take(im.fmt,im.width,im.height,&got);if(x.presenter==NRP_P_OK){img=got;have=true;}}}
    if(nrp_decide(&fr,&st,im.source.serial,&x,mode==2)!=NRP_R_OK){if(have){have=false;xbg_logf("【红】[nr] present: an image was taken for a frame that did not start");}return;}
    if(mode==2)return;
    if(img.gen!=fb_gen){drop_fbs();fb_gen=img.gen;}
    split();
}
// create_pass, each attachment: the swapchain view for X while the frame is active.
VkImageView att(Image &im,VkImageView v){
    if(!fr.target || im.source.serial!=fr.target)return v;
    const int a=nrp_touch(&fr,im.source.serial,NRP_T_PASS);
    if(a==NRP_A_SUBST && have){subst=true;return img.view;}
    return v;
}
// create_pass, the framebuffer of an attachment list with the swapchain view (subst): cached when the view is the only attachment.
VkFramebuffer framebuffer(VkRenderPass pass,const std::vector<VkImageView> &views,uint32_t w,uint32_t h){
    subst=false;
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=pass;fb.attachmentCount=uint32_t(views.size());fb.pAttachments=views.data();fb.width=w;fb.height=h;fb.layers=1;
    if(views.size()==1){const std::array<uint64_t,4> key{uint64_t(pass),uint64_t(views[0]),w,h};auto it=fbs.find(key);if(it!=fbs.end())return it->second;
        VkFramebuffer f;VK(vkCreateFramebuffer(cfg.device,&fb,nullptr,&f));fbs.emplace(key,f);return f;}
    VkFramebuffer f;VK(vkCreateFramebuffer(cfg.device,&fb,nullptr,&f));retire_handle([f]{vkDestroyFramebuffer(cfg.device,f,nullptr);});return f;   // other attachments' views may die: per slot
}
// The swapchain image's contents into X (an abort or a mispredict), on the recording slot after its acquire wait.
void copy_back(Image &x){
    end_open_pass();VkCommandBuffer cmd=recording_slot().cmd;full_barrier(cmd);x.transition(cmd,VK_IMAGE_LAYOUT_GENERAL);
    VkImageCopy c{};c.srcSubresource=c.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.extent={x.width,x.height,1};
    vkCmdCopyImage(cmd,img.image,VK_IMAGE_LAYOUT_GENERAL,x.image,VK_IMAGE_LAYOUT_GENERAL,1,&c);full_barrier(cmd);
    x.prior_write=x.last_write;x.last_write=++write_seq;x.gpu_written=true;++copy_backs;last_pass_reset();
}
// A read (sampled, copy / blit source) or write (copy / resolve destination) of an image outside a pass.
void touch(Image &im,int kind){
    if(!fr.target || im.source.serial!=fr.target)return;
    const int a=nrp_touch(&fr,im.source.serial,kind);
    if(mode==2 && fr.fail==(kind==NRP_T_READ?NRP_F_READ:NRP_F_WRITE) && fr.phase==NRP_S_WOULD){static unsigned n=0;if(n++<4)red("X %s outside a pass after the start (frame %llu, serial %llu)",kind==NRP_T_READ?"read":"written",(unsigned long long)stats.presents.load(),(unsigned long long)im.source.serial);}
    if(a==NRP_A_ABORT && have)copy_back(im);
}
// Image::transition: a copy / blit / resolve source (TRANSFER_SRC) or destination (TRANSFER_DST) outside a pass.
void xfer(Image &im,VkImageLayout next){if(next==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)touch(im,NRP_T_READ);else if(next==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)touch(im,NRP_T_WRITE);}
// pclear_land: X's pending clear lands outside a pass while the frame is active -> on the swapchain image. True = done here.
bool land_clear(Image &im,const VkClearValue &v,VkImageAspectFlags aspects){
    if(!fr.target || im.source.serial!=fr.target || !(aspects&VK_IMAGE_ASPECT_COLOR_BIT))return false;
    if(nrp_touch(&fr,im.source.serial,NRP_T_CLEAR)!=NRP_A_SUBST || !have)return false;
    end_open_pass();VkCommandBuffer cmd=recording_slot().cmd;full_barrier(cmd);VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdClearColorImage(cmd,img.image,VK_IMAGE_LAYOUT_GENERAL,&v.color,1,&r);full_barrier(cmd);im.prior_write=im.last_write;im.last_write=++write_seq;return true;
}
// xbg_present_texture_now, before anything is recorded for the present. Returns NRP_V_* (OLD: the old path, nothing taken).
int present(Image &im,VkCommandBuffer cmd,const win_t *w){
    const bool crop_full=!(w->crop[2]>0 && w->crop[3]>0) || (w->crop[0]==0 && w->crop[1]==0 && uint32_t(w->crop[2])==im.width && uint32_t(w->crop[3])==im.height);
    const bool flip=taafix::viewport_ll();const uint64_t target=fr.target;const int phase=fr.phase;
    int back=0;const int v=nrp_present(&fr,&st,im.source.serial,crop_full,flip,&back);
    if(mode==2 && phase==NRP_S_WOULD && target!=im.source.serial)red("predicted serial %llu, presented %llu (frame %llu)",(unsigned long long)target,(unsigned long long)im.source.serial,(unsigned long long)stats.presents.load());
    last_crop_full=crop_full;last_flip=flip;frame_seq=write_seq;last_pass_reset();
    if(v==NRP_V_OLD)return v;
    if(!have){xbg_logf("【红】[nr] present: an active frame without its image");return NRP_V_OLD;}
    if(back){auto it=images.find(target);if(it!=images.end())copy_back(*it->second);}
    if(v==NRP_V_DIRECT){
        if(hist_on)hist_note_read(im,'t');   // the present reads X (DM_GFX_STORE_HIST must keep the store of its last pass)
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=img.image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        b.oldLayout=VK_IMAGE_LAYOUT_GENERAL;b.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=0;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&b);
        sig_next=img.finished;}
    return v;
}
// After the present's submit_slots: hand the frame's image back with its present (direct, or the blit of `shown` into it).
int give(int v,VkImage shown,VkFormat fmt,uint32_t w,uint32_t h,const int32_t crop[4],int32_t interval){
    have=false;sig_next=VK_NULL_HANDLE;++ptq::handed;
    return dm_present_async_nrp(shown,fmt,w,h,crop,interval,v==NRP_V_DIRECT?1:2,&img);
}
// submit_slots: the batches of the slot. A split slot: {prologue, the first buffer} then {the tail, waiting for the acquire}; the last batch
// signals the present semaphore when the frame presents directly.
uint32_t batches(Slot &s,VkSubmitInfo si[2],VkCommandBuffer *cbs,uint32_t ncb,VkCommandBuffer *tail,VkPipelineStageFlags *stage){
    uint32_t n=1;si[0]=VkSubmitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};si[0].commandBufferCount=ncb;si[0].pCommandBuffers=cbs;
    if(s.nrp_split){*tail=s.cmd;si[1]=VkSubmitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};si[1].commandBufferCount=1;si[1].pCommandBuffers=tail;
        if(s.nrp_wait){*stage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT;si[1].waitSemaphoreCount=1;si[1].pWaitSemaphores=&s.nrp_wait;si[1].pWaitDstStageMask=stage;}
        n=2;}
    if(sig_next){si[n-1].signalSemaphoreCount=1;si[n-1].pSignalSemaphores=&sig_next;}
    return n;
}
void submitted(Slot &s){if(s.nrp_split){s.cmd=s.nrp_main;s.nrp_split=false;s.nrp_wait=VK_NULL_HANDLE;++splits;}sig_next=VK_NULL_HANDLE;}
// Every NVN PERF window (about 1 s); the [nr] line every 10 s.
void perf(){
    if(!mode)return;const uint64_t now=timing_clock();if(!reported_ns){reported_ns=now;prev=st;return;}if(now-reported_ns<10000000000ull)return;reported_ns=now;
    nrp_pres_stats d=st;const uint64_t *a=reinterpret_cast<const uint64_t*>(&st),*b=reinterpret_cast<const uint64_t*>(&prev);uint64_t *o=reinterpret_cast<uint64_t*>(&d);
    for(size_t i=0;i<sizeof d/8;i++)o[i]=a[i]-b[i];prev=st;
    char why[512];int n=0;for(int r=1;r<NRP_R_N;r++)if(d.why[r])n+=snprintf(why+n,sizeof why-n,"%s%s %llu",n?", ":"",nrp_reason_name(r),(unsigned long long)d.why[r]);if(!n)snprintf(why,sizeof why,"-");
    char pres[384];n=0;for(int r=1;r<NRP_P_N;r++)if(d.presenter[r])n+=snprintf(pres+n,sizeof pres-n,"%s%s %llu",n?", ":"",nrp_presenter_name(r),(unsigned long long)d.presenter[r]);if(!n)snprintf(pres,sizeof pres,"-");
    char fail[384];n=0;const uint64_t *fs=mode==2?d.would_fail:d.fail;for(int r=1;r<NRP_F_N;r++)if(fs[r])n+=snprintf(fail+n,sizeof fail-n,"%s%s %llu",n?", ":"",nrp_fallback_name(r),(unsigned long long)fs[r]);if(!n)snprintf(fail,sizeof fail,"-");
    uint64_t p[6]{};if(dm_present_nrp_stats)dm_present_nrp_stats(p);
    xbg_logf("[nr] present (DM_NR_PRESENT=%d, last 10 s): %llu frames; %s %llu (%s direct %llu, blit %llu: %s); not started: %s (presenter: %s); passes on the image %.2f a frame, clears landed on it %llu, copy-backs %llu; presenter: parked %llu (failed %llu), taken %llu, presented %llu, blits into a taken image %llu, lost %llu; red %llu",
        mode,(unsigned long long)d.frames,mode==2?"would start":"started",(unsigned long long)(mode==2?d.would:d.started),mode==2?"would present":"presented",(unsigned long long)(mode==2?d.would_direct:d.direct),
        (unsigned long long)(mode==2?d.would_fallback:d.fallback),fail,why,pres,d.frames?double(d.passes)/double(d.frames):0.0,(unsigned long long)d.clears,(unsigned long long)copy_backs,
        (unsigned long long)(p[0]-pstat_prev[0]),(unsigned long long)(p[1]-pstat_prev[1]),(unsigned long long)(p[2]-pstat_prev[2]),(unsigned long long)(p[3]-pstat_prev[3]),(unsigned long long)(p[4]-pstat_prev[4]),(unsigned long long)(p[5]-pstat_prev[5]),(unsigned long long)d.red);
    for(int i=0;i<6;i++)pstat_prev[i]=p[i];copy_backs=0;
}
}   // namespace nrpres
