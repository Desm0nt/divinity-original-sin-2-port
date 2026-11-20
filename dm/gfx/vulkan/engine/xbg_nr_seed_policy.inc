// [DOS2] NR-31: this device's policy for the seed re-target (warm::seed::launch, xbg_nr_seed.inc; dm/gfx/nr/nr_seed.h nsd_policy) - the
// same globals draw() / create_pass() / descriptors() read. Included by xbg_game.cpp at file scope right before xbg_game_initialize.
static nsd_policy nrseed_policy(){
    nsd_policy p{};
    p.tile_barrier=tile_barrier_on;                                                     // create_pass: pass_deps
    p.d24s8_as_d32s8=d24s8_as_d32s8;p.x8d24_as_d32=x8d24_as_d32;p.depth16_as_d24=depth16_as_d24;   // format() / get_image
    p.bias_x2=depth_bias_x2;                                                            // draw(): rs.depthBiasConstantFactor
    p.ubo_dynamic=ubo_dynamic_on;p.ubo_dyn_limit=properties.limits.maxDescriptorSetUniformBuffersDynamic;p.dyn_partial=dyn_partial_enabled;   // dyn_mode_for
    p.noout=noout_mask_on;p.retype=vtx_retype_mode();p.alpha_test=alpha_test_on();       // draw(): colour write masks, vertex / fragment variants
    const VkPhysicalDeviceFeatures &f=cfg.enabled_core;
    p.features=(f.fillModeNonSolid?NSD_F_NONSOLID:0u)|(f.depthClamp?NSD_F_DCLAMP:0u)|(f.depthBiasClamp?NSD_F_BCLAMP:0u)|(f.depthBounds?NSD_F_DBOUNDS:0u)|(f.logicOp?NSD_F_LOGICOP:0u);
    // [NR-31b] dyn_mode_for's other rules (nr_seed.c nsd_layout models both): NR-08b DM_NR_UBODYN read-only (set before this, at the policy
    // line) and NR-08 DM_NR_DESC_SPLIT - read at the first draw (nrdesc::init), after this: predicted from the same switches here (DM_NR_DESC=1
    // is a DOS2 default from jni_android.c). Predicting it on where nrdesc::init then turns it off (no constant ring) only refuses the few
    // split records (NSD_E_SPLIT: never a wrong build).
    p.unmodelled=0;p.ubo_ro=nrubd::ro;
    {const char *e=getenv("DM_NR_DESC"),*sp=getenv("DM_NR_DESC_SPLIT");
     p.split=e && atoi(e)==1 && ubo_dynamic_on && (sp && *sp?atoi(sp)!=0:xbg_nr_desc_split_default(ubo_dynamic_on));}
    for(uint32_t v=1;v<256;v++)if(!(format_properties(VkFormat(v)).bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))p.vtx_unsup[v>>5]|=1u<<(v&31);
    return p;
}
