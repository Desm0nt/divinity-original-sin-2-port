// [DOS2] NR-UBWC DM_NR_UBWC (docs/dos2/nr/NR-UBWC.md): is the driver's framebuffer compression (UBWC on Adreno, AFBC on Mali) on for the
// images the frame renders into, and when only a usage flag of ours turns it off, drop that flag. Included by xbg_game.cpp after
// namespace nrrp's forward block (before get_image_slow, which calls lean()). Hooks (each behind `nrubwc::mode`):
//   get_image_slow / texstream ci_of   lean(): the create info's usage (=1 may drop STORAGE, both paths decide the same, worker-safe)
//   create_pass                        att(): each attachment image once a frame - the driver's answer (asked once per image) + bytes
//   descriptors() STORAGE_IMAGE bind   upgrade(): an image created without STORAGE gets it back (recreated + copied, as upgrade_mutable)
//
// The answer comes from the driver itself: VK_EXT_image_compression_control (enabled by vulkan_present.cpp only while DM_NR_UBWC != 0;
// the engine never passes VkImageCompressionControlEXT, so no image is created differently by it) - vkGetImageSubresourceLayout2EXT with
// VkImageCompressionPropertiesEXT returns DISABLED / DEFAULT for an image as created (Turnip: fdl_ubwc_enabled of the layout, mip 0), and
// vkGetPhysicalDeviceImageFormatProperties2 with it what a create info WOULD get (Turnip: ubwc_possible, tu_formats.cc). The reason of an
// uncompressed attachment is what the driver says for the same create info without STORAGE / without MUTABLE_FORMAT / without
// INPUT_ATTACHMENT; none of them = a rule of the format / size / tiling (Turnip 1c3a936 tu_image.cc + fd6_layout.c: compressed formats,
// E5B9G9R9, S8, non-power-of-two bytes per pixel, width < 16 (linear fallback), 3D with mips, 1D, linear tiling; on A6xx also STORAGE
// (no supports_uav_ubwc), 8-bit formats (no has_8bpp_ubwc), SNORM before a740; MUTABLE_FORMAT without a compatible format list).
// DM_NR_UBWC: 0 (default, every family: xbg_nr_ubwc_default) - nothing: the extension is not enabled, no call, no flag changes.
//   2 count: the answers and reasons, one 10-s line + the first 24 uncompressed attachments in full; images created as before.
//   1 apply: 2 + a colour 2D attachment-capable image whose create info the driver would compress WITHOUT STORAGE but not with it is
//     created without STORAGE (DOS2 records no compute: 0 dispatches a frame); its first storage bind recreates it with STORAGE and copies
//     every level (upgrade_mutable's path), so the contents and every later use are the same. 3D / 1D / depth images keep their usage.
extern "C" int dm_vk_compr_ctl __attribute__((weak));
extern "C" PFN_vkGetPhysicalDeviceImageFormatProperties2 dm_vk_ifp2 __attribute__((weak));
namespace nrubwc{
int mode=0;PFN_vkGetImageSubresourceLayout2EXT ask_pfn=nullptr;
enum{R_STORAGE,R_MUTABLE,R_INPUT,R_RULE,R_UNKNOWN,R_N};
static const char *const rname[R_N]={"storage usage","mutable format","input-attachment usage","format / size / tiling rule","no answer"};
struct Stats{uint64_t att=0,on=0,off=0,why[R_N]{},bytes_on=0,bytes_off=0,upgraded=0,stor=0,stor_bytes=0,lean=0;} s;
std::atomic<uint64_t> deferred{0};uint64_t next_ns=0,frames0=0;unsigned told=0;
inline bool can_ask(){return &dm_vk_compr_ctl && (dm_vk_compr_ctl&1) && ask_pfn;}
// What a create info would get: 1 compressed, 2 not, 0 unknown (no extension / the query failed).
int would(VkFormat f,VkImageType t,VkImageTiling tl,VkImageUsageFlags u,VkImageCreateFlags fl){
    if(!&dm_vk_ifp2 || !dm_vk_ifp2 || !(&dm_vk_compr_ctl && (dm_vk_compr_ctl&1)))return 0;
    VkImageCompressionPropertiesEXT cp{VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_PROPERTIES_EXT};VkImageFormatProperties2 out{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};out.pNext=&cp;
    VkPhysicalDeviceImageFormatInfo2 in{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};in.format=f;in.type=t;in.tiling=tl;in.usage=u;in.flags=fl;
    if(dm_vk_ifp2(cfg.physical,&in,&out)!=VK_SUCCESS)return 0;
    return cp.imageCompressionFlags==VK_IMAGE_COMPRESSION_DISABLED_EXT?2:1;}
// The driver's answer for an image as created (mip 0, layer 0, its first aspect).
int ask(const Image &im){
    if(!can_ask() || !im.image)return 0;
    VkImageSubresource2KHR sub{VK_STRUCTURE_TYPE_IMAGE_SUBRESOURCE_2_EXT};
    sub.imageSubresource.aspectMask=(im.aspects&VK_IMAGE_ASPECT_DEPTH_BIT)?VK_IMAGE_ASPECT_DEPTH_BIT:(im.aspects&VK_IMAGE_ASPECT_COLOR_BIT)?VK_IMAGE_ASPECT_COLOR_BIT:VK_IMAGE_ASPECT_STENCIL_BIT;
    VkImageCompressionPropertiesEXT cp{VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_PROPERTIES_EXT};VkSubresourceLayout2KHR lay{VK_STRUCTURE_TYPE_SUBRESOURCE_LAYOUT_2_EXT};lay.pNext=&cp;
    ask_pfn(cfg.device,im.image,&sub,&lay);
    return cp.imageCompressionFlags==VK_IMAGE_COMPRESSION_DISABLED_EXT?2:1;}
// DM_NR_UBWC=1 at creation (get_image_slow and the texstream worker's ci_of take the same decision: their create infos must match)
VkImageUsageFlags lean(const VkImageCreateInfo &ci,bool *dropped){
    if(dropped)*dropped=false;
    if(mode!=1 || !(ci.usage&VK_IMAGE_USAGE_STORAGE_BIT) || ci.imageType!=VK_IMAGE_TYPE_2D || !(ci.usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))return ci.usage;
    const VkImageUsageFlags u2=ci.usage&~VkImageUsageFlags(VK_IMAGE_USAGE_STORAGE_BIT);
    if(would(ci.format,ci.imageType,ci.tiling,ci.usage,ci.flags)==2 && would(ci.format,ci.imageType,ci.tiling,u2,ci.flags)==1){if(dropped)*dropped=true;++deferred;return u2;}
    return ci.usage;}
// descriptors(): a storage bind of an image lean() created without STORAGE
void upgrade(Image &im){im.nrubwc_def=false;upgrade_mutable(im,VK_FORMAT_UNDEFINED,VK_IMAGE_USAGE_STORAGE_BIT);im.nrubwc=0;++s.upgraded;
    xbg_logf("[nr] ubwc: serial %llu %ux%u fmt %d bound as a storage image: recreated with STORAGE (contents copied)",(unsigned long long)im.source.serial,im.width,im.height,int(im.fmt));}
int reason(const Image &im){
    const VkImageCreateInfo &c=im.create;
    if(!would(c.format,c.imageType,c.tiling,c.usage,c.flags))return R_UNKNOWN;
    if((c.usage&VK_IMAGE_USAGE_STORAGE_BIT) && would(c.format,c.imageType,c.tiling,c.usage&~VkImageUsageFlags(VK_IMAGE_USAGE_STORAGE_BIT),c.flags)==1)return R_STORAGE;
    if((c.flags&VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) && would(c.format,c.imageType,c.tiling,c.usage,c.flags&~VkImageCreateFlags(VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT))==1)return R_MUTABLE;
    if((c.usage&VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) && would(c.format,c.imageType,c.tiling,c.usage&~VkImageUsageFlags(VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT),c.flags)==1)return R_INPUT;
    return R_RULE;}
void tick();
// create_pass: every attachment image, once a frame
void att(Image &im){
    const uint64_t fr=stats.presents.load()+1;if(im.nrubwc_frame==fr)return;im.nrubwc_frame=fr;
    if(!im.nrubwc){im.nrubwc=uint8_t(ask(im));
        if(im.nrubwc==2){const int w=reason(im);im.nrubwc_why=uint8_t(w);
            if(told<24){++told;xbg_logf("[nr] ubwc: attachment not compressed: serial %llu NVN fmt %#x vk %d %ux%u levels %u layers %u samples %d usage %#x flags %#x: %s",(unsigned long long)im.source.serial,
                im.source.d.format,int(im.fmt),im.width,im.height,im.levels,im.layers,int(im.samples),unsigned(im.create.usage),unsigned(im.create.flags),rname[w]);}}}
    const uint64_t b=uint64_t(im.width)*im.height*(im.bpp?im.bpp:4u)*std::max(1u,unsigned(im.samples));
    if(im.create.usage&VK_IMAGE_USAGE_STORAGE_BIT){++s.stor;s.stor_bytes+=b;}   // Adreno 6xx (no supports_uav_ubwc) compresses none of these
    if(im.nrubwc_def)++s.lean;
    ++s.att;if(im.nrubwc==1){++s.on;s.bytes_on+=b;}else if(im.nrubwc==2){++s.off;s.bytes_off+=b;++s.why[im.nrubwc_why<R_N?im.nrubwc_why:R_UNKNOWN];}else ++s.why[R_UNKNOWN];
    tick();}
void init(XbgGpuFamily fam){const char *e=getenv("DM_NR_UBWC");mode=e && *e?atoi(e):xbg_nr_ubwc_default(fam);if(mode<0 || mode>2){xbg_logf("【红】[nr] DM_NR_UBWC=%d: only 0 / 1 / 2: off",mode);mode=0;}
    if(!mode)return;
    ask_pfn=reinterpret_cast<PFN_vkGetImageSubresourceLayout2EXT>(vkGetDeviceProcAddr(cfg.device,"vkGetImageSubresourceLayout2EXT"));
    if(!ask_pfn)ask_pfn=reinterpret_cast<PFN_vkGetImageSubresourceLayout2EXT>(vkGetDeviceProcAddr(cfg.device,"vkGetImageSubresourceLayout2KHR"));
    xbg_logf("%s[nr] ubwc DM_NR_UBWC=%d (NR-UBWC, policy family %s): %s; the driver %s (VK_EXT_image_compression_control %s, create-info query %s); line every 10 s",
        can_ask()?"":"【红】",mode,xbg_gpu_family_name(fam),mode==1?"count + colour 2D images whose compression only STORAGE turns off are created without it (back on a storage bind)":"count only, images created as before",
        can_ask()?"is asked per attachment image":"cannot be asked: nothing is counted or changed",&dm_vk_compr_ctl && dm_vk_compr_ctl?"enabled":"missing",&dm_vk_ifp2 && dm_vk_ifp2?"ready":"missing");}
void tick(){const uint64_t now=timing_clock();if(!next_ns){next_ns=now+10000000000ull;frames0=stats.presents.load();return;}if(now<next_ns)return;next_ns=now+10000000000ull;
    const uint64_t fr=stats.presents.load();const double f=fr>frames0?double(fr-frames0):1.;frames0=fr;
    xbg_logf("[nr] ubwc DM_NR_UBWC=%d: %.0f frames; a frame: attachment images %.1f, compressed %.1f, not %.2f (%s %.2f, %s %.2f, %s %.2f, %s %.2f, %s %.2f); attachment bytes (mip 0, once per image) %.1f MB, on uncompressed images %.1f MB; "
        "with STORAGE usage %.2f (%.1f MB: uncompressed on Adreno 6xx), created without it here %.2f; created without STORAGE (=1) %llu images so far, given STORAGE back at a storage bind %llu",
        mode,f,s.att/f,s.on/f,s.off/f,rname[0],s.why[0]/f,rname[1],s.why[1]/f,rname[2],s.why[2]/f,rname[3],s.why[3]/f,rname[4],s.why[4]/f,(s.bytes_on+s.bytes_off)/f/1e6,s.bytes_off/f/1e6,
        s.stor/f,s.stor_bytes/f/1e6,s.lean/f,(unsigned long long)deferred.load(),(unsigned long long)s.upgraded);
    const uint64_t up=s.upgraded;s=Stats{};s.upgraded=up;}
}
