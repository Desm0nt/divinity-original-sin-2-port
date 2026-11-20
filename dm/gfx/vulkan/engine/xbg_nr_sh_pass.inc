// [DOS2] NR-04 DM_NR_SH, the pass half (read half: xbg_nr_sh.inc; table: dm/gfx/nr/nr_sh.c). draw() calls nrpass::choose when it
// builds a pipeline whose fragment shader is a named one (Shader::nr_sh >= 0; a pipeline is built once per shader + pass, later draws hit
// the memo / fast keys, so this costs nothing per draw). The pass hash = active colour attachments in slot order (NVN formats, a view's
// format when it sets one), depth format (0 none), samples (nr_sh_pass_hash): every distinct (shader, pass hash) is logged once, with
// whether it is the pass the shader was verified in (the phone's GPASS lines: colour count, format of colour 0, one sample).
// Mode 1: outside that pass the pipeline takes the translated module (bundle read again past nrsh, the same load-time rewrites);
// mode 2: only counted / logged (red line for a pass outside). Alpha-tested draws keep alpha_module's variant of the named bundle
// (no engine shader of the table is drawn with the NVN alpha test).
namespace nrpass {
struct Seen{int entry;uint32_t hash;unsigned why;};std::vector<Seen> seen;   // graphics thread only
uint64_t verified=0,outside=0,alts=0;
VkShaderModule alt(Shader &s){
    if(s.nr_alt)return s.nr_alt;
    std::vector<uint32_t> w;uint32_t stage=0;bool ok;{++xpack::nrsh::bypass;ok=warm::bundle_words(s.key,w,stage,"nr-alt");--xpack::nrsh::bypass;}
    if(ok && stage==4 && module_gap(w,stage).empty() && bundle_module(w,&s.nr_alt,s.key+"#nrt")==VK_SUCCESS){++alts;return s.nr_alt;}
    xpack::nrsh::red("%s: the translated module for a pass outside the verified one could not be made: the named one is used there",s.key.c_str());
    s.nr_alt=s.module;return s.module;}
void choose(Shader &fs,std::vector<VkPipelineShaderStageCreateInfo> &stages,VkSampleCountFlagBits samples){
    const int i=fs.nr_sh;if(i<0 || i>=nr_sh_count)return;
    uint32_t fmt[8];int n=0;
    for(int c=0;c<state.rt.n && c<8;c++)if(state.rt.color[c])fmt[n++]=uint32_t(state.rt.has_cview[c] && state.rt.cview[c].set_format?state.rt.cview[c].format:state.rt.color[c]->d.format);
    const uint32_t depth=state.rt.depth?uint32_t(state.rt.has_dview && state.rt.dview.set_format?state.rt.dview.format:state.rt.depth->d.format):0u;
    const uint32_t h=nr_sh_pass_hash(n,fmt,depth,uint32_t(samples));const unsigned why=nr_sh_pass_check(i,n,fmt,uint32_t(samples));
    bool fresh=true;for(const Seen &x:seen)if(x.entry==i && x.hash==h){fresh=false;break;}
    if(fresh && seen.size()<256){seen.push_back({i,h,why});++xpack::nrsh::changed;
        char cs[96];int k=0;for(int c=0;c<n && k<(int)sizeof cs-8;c++)k+=snprintf(cs+k,sizeof cs-size_t(k),"%s%#x",c?",":"",fmt[c]);if(!n)snprintf(cs,sizeof cs,"none");
        const nr_sh_entry &e=nr_sh_table[i];
        if(!why)xbg_logf("[nr] sh %s pass %08x: colours [%s] depth %#x samples %u, %dx%d - the verified pass",e.name,h,cs,depth,unsigned(samples),
            state.rt.color[0]?int(state.rt.color[0]->d.width):-1,state.rt.color[0]?int(state.rt.color[0]->d.height):-1);
        else xpack::nrsh::red("%s pass %08x: colours [%s] depth %#x samples %u - not the verified pass (%d colour%s, colour 0 %#x, 1 sample; differs:%s%s%s): %s",
            e.name,h,cs,depth,unsigned(samples),e.colors,e.colors==1?"":"s",unsigned(e.color0),why&NR_SH_P_COLORS?" count":"",why&NR_SH_P_FORMAT?" format":"",
            why&NR_SH_P_SAMPLES?" samples":"",xpack::nrsh::mode==1?"the translated module is used in this pass":"counted (DM_NR_SH=2)");}
    if(!why){++verified;return;}
    ++outside;if(xpack::nrsh::mode!=1)return;
    // T11: a Dref bundle's depth binding gets a compare sampler for every pipeline of this shader (descriptors are per shader); the translated
    // module samples it without Dref, which a compare sampler makes undefined - the named module stays (it writes the same vec4 to location 0)
    if(fs.dref_bindings){static unsigned told=0;if(told++<4)xpack::nrsh::red("%s: pass outside the verified one, but a Dref bundle keeps its module (compare sampler)",nr_sh_table[i].name);return;}
    const VkShaderModule m=alt(fs);for(auto &s:stages)if(s.stage==VK_SHADER_STAGE_FRAGMENT_BIT)s.module=m;}
}
// The '[nr] sh' summary: every 10 s from the submission, when a count changed since the last one.
void nrsh_tick(){static uint64_t next=0,last=~0ull;const uint64_t now=timing_clock();if(now<next)return;next=now+10000000000ull;
    namespace S=xpack::nrsh;const uint64_t c=S::changed.load()+nrpass::verified+nrpass::outside;if(c==last)return;last=c;
    int on=0;for(int i=0;i<nr_sh_count && i<16;i++)on+=S::st[i].load()==1;
    xbg_logf("[nr] sh DM_NR_SH=%d (%s, DM_NR_SH_PCF=%d): pack %s; entries %s %d of %d (bundle reads %s %llu), refused %llu (head / interface / descriptors), "
        "missing in the pack %llu, kept translated %llu, off by a name contradiction %llu; engine names agreeing %llu; pipelines in the verified pass %llu, "
        "outside it %llu (%s; translated modules made %llu), distinct shader passes %zu; red lines %llu",S::mode,S::mode==1?"replace":"check",S::pcf,
        S::fd>=0?"open":"none",S::mode==1?"replaced":"passing every check",on,nr_sh_count,S::mode==1?"swapped":"checked",
        (unsigned long long)(S::mode==1?S::used.load():S::would.load()),(unsigned long long)S::refused.load(),(unsigned long long)S::missing.load(),
        (unsigned long long)S::kept.load(),(unsigned long long)S::offs.load(),(unsigned long long)nr_sh_names_agreed(),(unsigned long long)nrpass::verified,
        (unsigned long long)nrpass::outside,S::mode==1?"translated there":"counted",(unsigned long long)nrpass::alts,nrpass::seen.size(),(unsigned long long)S::reds.load());}
