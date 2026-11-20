// [DOS2] NR-29 DM_NR_MEM, engine side (docs/dos2/nr/NR-29.md; front end dm/host/hle_dos2_nr_mem.c, rules dm/gfx/nr/nr_mem.h).
// Included by xbg_game.cpp at file scope after xbg_nr_pipe_source (it needs nrts, nrbuf, warm and astclazy). The caps themselves are
// read where each cache reads its default (dm_nr_mem_cap: NR-06 nrbuf::init, the ASTC encoder queue in astccache, the warm-up SPIR-V at
// the shader-reuse switch, NR-07 lazy in warm::nr::read_mode; NR-05 / NR-22 in the front end). Here only:
//   xbg_nr_mem_usage  what the engine's caches hold now (any thread; atomics and the caches' own locks, never the engine mutex)
//   xbg_nr_mem_trim   free by tier (the front end's trim thread): 1 the free NR-22 staging chunks + the ASTC queue; 2 + the warm-up
//                     SPIR-V and the NR-06 static copies (asked: the next submission frees them, nrbuf::trim); 3 + the warm pipelines
//                     not taken. The NR-05 / NR-22 objects not taken are the front end's (its table): freed there.
// Lock order as the translation thread's: the engine mutex, then warm::mu / nrts::mu / the encoder's.
extern "C" void xbg_nr_mem_usage(uint64_t *u){
    for(int i=NRM_U_TS_IMG;i<NRM_U_N;i++)u[i]=0;
    u[NRM_U_TS_IMG]=nrts::out_img.load(std::memory_order_relaxed);
    {std::lock_guard<std::mutex> l(nrts::mu);u[NRM_U_TS_FREE]=nrts::free_bytes;}
    u[NRM_U_BUFSTATIC]=nrbuf::pub_arena.load(std::memory_order_relaxed);u[NRM_U_RING]=nrbuf::pub_mirror.load(std::memory_order_relaxed);
#ifdef DM_ASTC_LAZY
    if(astclazy::running())u[NRM_U_ASTCQ]=astclazy::stats().queue_bytes;
#endif
    {std::lock_guard<std::mutex> l(warm::mu);u[NRM_U_SPIRV]=warm::rewritten_bytes;u[NRM_U_PIPES]=warm::ready.size();}
}
extern "C" void xbg_nr_mem_trim(int tier,uint64_t *f){
    for(int i=NRM_U_TS_IMG;i<NRM_U_N;i++)f[i]=0;
    std::lock_guard<std::recursive_mutex> lock(mutex);if(!initialized)return;   // the device lives while this is held
    if(tier>=NRM_TRIM_1){
        if(narena::mode>0)narena::trim_gen.fetch_add(1);   // [DOS2] NR-TRAFFIC DM_NR_ARENA: each slot shrinks its stream arena to its last need at its next retire
        f[NRM_U_TS_FREE]=nrts::trim_free();
#ifdef DM_ASTC_LAZY
        if(astclazy::running())f[NRM_U_ASTCQ]=astclazy::drop_queue(nullptr);
#endif
    }
    if(tier>=NRM_TRIM_2){
        {std::lock_guard<std::mutex> l(warm::mu);f[NRM_U_SPIRV]=warm::rewritten_bytes;warm::rewritten.clear();warm::rewritten_bytes=0;}
        if(nrbuf::st_mode>0){f[NRM_U_BUFSTATIC]=nrbuf::pub_arena.load(std::memory_order_relaxed);int t=nrbuf::trim_req.load();while(t<tier && !nrbuf::trim_req.compare_exchange_weak(t,tier)){}}
    }
    if(tier>=NRM_TRIM_3){   // never handed out: no command buffer, memo or descriptor names them (NR-07: a draw that needed one finds it gone and builds; NR-21 forgets its create info: the handle may come back)
        std::lock_guard<std::mutex> l(warm::mu);f[NRM_U_PIPES]=warm::ready.size();for(auto &p:warm::ready){nrsort::forget_pipeline(p.second);vkDestroyPipeline(cfg.device,p.second,nullptr);}warm::ready.clear();
    }
}
