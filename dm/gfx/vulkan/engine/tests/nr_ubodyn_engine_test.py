"""[DOS2] NR-08b: the production DM_NR_UBODYN code under the SIM powervr family, on the PC.

Takes from the sources as they are: xbg_nr_ubodyn.inc (init / mark / opt_ok / summary), the UBO_DYN macro and dyn_mode_for of
xbg_game.cpp, xbg_vendor_policy.h and dm/gfx/nr/nr_ubodyn.c; stubs only the engine around them (log, clock, Shader, state, device
limits, NR-08's split hooks). Every DOS2 bundle (work/shaders-vk) is loaded through mark() like load_shader_key does, then every
vertex + fragment pair of work/shaders-jobs/programs.tsv is bound and dyn_mode_for / UBO_DYN decide each uniform binding, per run:

  A  DM_GFX_SIM_GPU=powervr (family powervr, policy: dynamic off), DM_NR_UBODYN=0      -> every binding static (PowerVR before 17.11, safe-mode step 1)
  A1 same, DM_NR_UBODYN unset                                                          -> = B (default 1 on PowerVR since 17.11, xbg_nr_ubodyn_default)
  B  same + DM_NR_UBODYN=1                                                             -> the rule: no dynamic binding its stage does
                                                                                          not read; every game block dynamic; support /
                                                                                          c1 never dynamic
  C  same + DM_NR_UBODYN=2 (dry run)                                                   -> bit-identical to A, modules scanned
  D  Adreno (dynamic on) + DM_NR_UBODYN=1                                              -> bit-identical to Adreno without it
  E  Adreno + DM_GFX_UBO_DYNAMIC=2                                                     -> the rule forced (= B on 16 dynamic)
  F  powervr + DM_NR_UBODYN=1 + DM_GFX_UBO_DYNAMIC=1                                   -> = Adreno layouts (user said "all dynamic")
  G  powervr + DM_NR_UBODYN=1, a module the scan does not understand / a binding never scanned -> static
  H  opt_ok: an optimized copy that drops a read is refused under B, used under C

  python nr_ubodyn_engine_test.py --compiler "<LLVM clang++>" [--bundles <work/shaders-vk> --programs <programs.tsv>]
"""
import argparse
import os
import pathlib
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from texture_cache_regression import balanced

HERE = pathlib.Path(__file__).resolve().parent
ENGINE = HERE.parent
NR = ENGINE.parent.parent / 'nr'

PREFIX = r'''
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <vulkan/vulkan.h>
#include "@@ENGINE@@/xbg_vendor_policy.h"
extern "C" {
#include "@@NR@@/nr_ubodyn.h"
}
enum { XBG_SOURCE_SUPPORT=1,XBG_SOURCE_UNIFORM=2,XBG_SOURCE_STORAGE=3,XBG_SOURCE_C1=4,XBG_SOURCE_DRIVER0=5,XBG_SOURCE_DRIVER2=6,XBG_SOURCE_TEXTURE=7 };
struct xbg_shader_binding{uint32_t binding,type,count,bytes,source,slot,sampler_slot,flags;};
struct Shader{std::string key;uint32_t stage;std::vector<xbg_shader_binding> bindings;};
static int log_lines=0;static std::string last_log;
void xbg_logf(const char *fmt,...){char b[2048];va_list a;va_start(a,fmt);vsnprintf(b,sizeof b,fmt,a);va_end(a);last_log=b;if(++log_lines<=40)printf("    log: %s\n",b);}
uint64_t fake_clock=0;uint64_t timing_clock(){return fake_clock+=1000;}
bool ubo_dynamic_on=true;bool dyn_partial_enabled=true;
struct {Shader *shader[6];} state;
struct {struct {uint32_t maxDescriptorSetUniformBuffersDynamic;} limits;} properties;
namespace nrdesc{bool split_on=false;bool split_pick(unsigned){return false;}bool split_has(const xbg_shader_binding*){return false;}}
'''

SUFFIX = r'''
struct Run{const char *name;XbgGpuFamily fam;const char *nr,*ubo;unsigned lim;};
static void setenv_(const char *k,const char *v){
#ifdef _WIN32
    _putenv_s(k,v?v:"");
#else
    if(v)setenv(k,v,1);else unsetenv(k);
#endif
}
// One run: init as the policy line does, mark every module, then the layout word of every pair (one byte per uniform binding:
// 0 static, 1 dynamic) into `out`. Counts what the PowerVR rule forbids.
struct Out{std::vector<uint8_t> words;unsigned unsafe=0,game=0,game_dyn=0,fixed_dyn=0,modes[4]={0,0,0,0};};
static std::vector<Shader> mods;static std::vector<std::vector<uint32_t>> spv;static std::vector<std::pair<int,int>> pairs;
static Out run(const Run &r){
    setenv_("DM_NR_UBODYN",r.nr);setenv_("DM_GFX_UBO_DYNAMIC",r.ubo);
    properties.limits.maxDescriptorSetUniformBuffersDynamic=r.lim;
    ubo_dynamic_on=xbg_env_or(getenv("DM_GFX_UBO_DYNAMIC"),xbg_ubo_dynamic_default(r.fam));
    nrubd::init(r.fam,xbg_ubo_dynamic_default(r.fam));
    std::vector<Shader> ms=mods;for(size_t i=0;i<ms.size();i++)nrubd::mark(ms[i],spv[i]);
    Out o;
    for(auto &pr:pairs){
        state.shader[0]=&ms[pr.first];state.shader[4]=&ms[pr.second];
        const unsigned dmode=dyn_mode_for(false);o.modes[dmode&3]++;
        for(int st:{0,4}){const Shader &s=*state.shader[st];std::vector<uint8_t> rd(s.bindings.size());
            std::vector<uint32_t> bn;for(auto &b:s.bindings)bn.push_back(b.binding);
            nr_spv_binding_reads(spv[st==0?pr.first:pr.second].data(),spv[st==0?pr.first:pr.second].size(),bn.data(),unsigned(bn.size()),rd.data());
            for(size_t i=0;i<s.bindings.size();i++){const auto &b=s.bindings[i];if(b.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)continue;
                const bool d=UBO_DYN(dmode,b);o.words.push_back(d);
                if(d && !rd[i])o.unsafe++;
                if(b.source==XBG_SOURCE_UNIFORM){o.game++;o.game_dyn+=d;}
                if(d && (b.source==XBG_SOURCE_SUPPORT || b.source==XBG_SOURCE_C1))o.fixed_dyn++;}}
    }
    state.shader[0]=state.shader[4]=nullptr;
    return o;}
static int fails=0;
#define CHECK(c) do{if(!(c)){printf("FAIL line %d: %s\n",__LINE__,#c);++fails;}}while(0)
static std::vector<uint32_t> rd_file(const char *path,uint32_t h[8],std::vector<xbg_shader_binding> &b){
    std::vector<uint32_t> w;FILE *f=fopen(path,"rb");if(!f)return w;
    if(fread(h,4,8,f)==8 && h[0]==0x31534758 && h[3]<=256 && h[6] && !(h[6]&3)){b.resize(h[3]);
        if(fread(b.data(),sizeof(xbg_shader_binding),h[3],f)==h[3] && !fseek(f,long(h[4]*4+h[5]),SEEK_CUR)){w.resize(h[6]/4);if(fread(w.data(),1,h[6],f)!=h[6])w.clear();}}
    fclose(f);return w;}
int main(int argc,char **argv){
    // a module of our own first: one game block read, one game block never read, a support block not read (prepended to the list)
    {   std::vector<uint32_t> w={0x07230203u,0x10300,0,100,0};auto I=[&](uint32_t op,std::initializer_list<uint32_t> a){w.push_back(uint32_t((a.size()+1)<<16)|op);for(auto x:a)w.push_back(x);};
        I(15,{0,5,0x6e69616d,0});I(71,{10,33,4});I(71,{11,33,10});I(71,{12,33,1});I(59,{4,10,2});I(59,{4,11,2});I(59,{4,12,2});I(59,{4,15,3});
        I(54,{1,5,0,2});I(248,{40});I(61,{3,30,10});I(62,{15,30});I(253,{});I(56,{});
        Shader s{"own-fs",4,{{4,6,1,256,XBG_SOURCE_UNIFORM,0,0xffffffffu,0},{10,6,1,256,XBG_SOURCE_UNIFORM,1,0xffffffffu,0},{1,6,1,1392,XBG_SOURCE_SUPPORT,0,0xffffffffu,0}}};
        mods.push_back(s);spv.push_back(w);
        Shader v{"own-vs",0,{{0,6,1,256,XBG_SOURCE_UNIFORM,0,0xffffffffu,0}}};   // not understood: no entry point
        std::vector<uint32_t> bad={0x07230203u,0x10300,0,100,0,(5u<<16)|54,1,5,0,2,(1u<<16)|56};mods.push_back(v);spv.push_back(bad);
        pairs.push_back({1,0});}
    if(argc>=3){
        FILE *pf=fopen(argv[2],"rb");std::map<std::string,int> idx;char line[2048];int first=1;
        std::vector<std::pair<std::string,std::string>> want;
        while(pf && fgets(line,sizeof line,pf)){if(first){first=0;continue;}char *c[6]={0};unsigned n=0;char *q=line;
            while(n<6){c[n++]=q;char *t=strchr(q,'\t');if(!t)break;*t=0;q=t+1;}
            if(n<5 || !strcmp(c[1],"-") || !strcmp(c[3],"-"))continue;want.push_back({c[1],c[3]});}
        if(pf)fclose(pf);
        for(auto &p:want)for(const std::string &k:{p.first,p.second})if(!idx.count(k)){
            uint32_t h[8];std::vector<xbg_shader_binding> b;std::string path=std::string(argv[1])+"/"+k+".xgs";auto w=rd_file(path.c_str(),h,b);
            if(w.empty()){idx[k]=-1;continue;}idx[k]=int(mods.size());mods.push_back(Shader{k,h[2],b});spv.push_back(w);}
        for(auto &p:want)if(idx[p.first]>=0 && idx[p.second]>=0)pairs.push_back({idx[p.first],idx[p.second]});
        printf("  bundles: %zu modules, %zu pairs\n",mods.size()-2,pairs.size()-1);}
    const XbgGpuFamily pvr=xbg_effective_family(XBG_GPU_ADRENO,XBG_SIM_PROFILE_POWERVR);CHECK(pvr==XBG_GPU_POWERVR);
    const Run A{"A powervr NR_UBODYN=0",pvr,"0",nullptr,16},A1{"A1 powervr unset",pvr,nullptr,nullptr,16},B{"B powervr NR_UBODYN=1",pvr,"1",nullptr,16},C{"C powervr NR_UBODYN=2",pvr,"2",nullptr,16},
        D0{"D0 adreno",XBG_GPU_ADRENO,nullptr,nullptr,16},D{"D adreno NR_UBODYN=1",XBG_GPU_ADRENO,"1",nullptr,16},
        E{"E adreno UBO_DYNAMIC=2",XBG_GPU_ADRENO,nullptr,"2",16},F{"F powervr NR_UBODYN=1 UBO_DYNAMIC=1",pvr,"1","1",16},
        B8{"B8 powervr NR_UBODYN=1 limit 8",pvr,"1",nullptr,8},M0{"M0 mali 8",XBG_GPU_MALI,nullptr,nullptr,8},M1{"M1 mali 8 NR_UBODYN=1",XBG_GPU_MALI,"1",nullptr,8},
        X{"X powervr NR_UBODYN=3",pvr,"3",nullptr,16};
    std::map<std::string,Out> o;
    for(const Run *r:{&A,&A1,&B,&C,&D0,&D,&E,&F,&B8,&M0,&M1,&X}){o[r->name]=run(*r);const Out &x=o[r->name];
        printf("  %-38s ro=%d scan=%d modes 0/1/2/3 %u/%u/%u/%u; uniform bindings %zu: game blocks dynamic %u of %u, support / c1 dynamic %u, dynamic but unread %u\n",
               r->name,int(nrubd::ro),int(nrubd::scan),x.modes[0],x.modes[1],x.modes[2],x.modes[3],x.words.size(),x.game_dyn,x.game,x.fixed_dyn,x.unsafe);}
    const Out &a=o[A.name],&a1=o[A1.name],&b=o[B.name],&c=o[C.name],&d0=o[D0.name],&d=o[D.name],&e=o[E.name],&f=o[F.name],&b8=o[B8.name],&m0=o[M0.name],&m1=o[M1.name],&x=o[X.name];
    CHECK(a.game_dyn==0 && a.fixed_dyn==0 && a.modes[0]==pairs.size());              // today: nothing dynamic on PowerVR
    CHECK(b.unsafe==0);                                                              // the rule
    CHECK(a1.words==b.words);                                                        // unset = the PowerVR default 1 (17.11)
    CHECK(b.fixed_dyn==0);                                                           // support / c1 never
    CHECK(b.game_dyn+1+1==b.game);   // every game block dynamic except our own unread one and the one of the module not understood
    CHECK(c.words==a.words);                                                         // dry run = today
    CHECK(d.words==d0.words && m1.words==m0.words);                                  // where dynamic buffers are on: nothing changes
    CHECK(e.words==b.words);                                                         // DM_GFX_UBO_DYNAMIC=2 = the rule
    CHECK(f.words==d0.words);                                                        // the user's "all dynamic" wins
    CHECK(b8.unsafe==0 && b8.fixed_dyn==0);
    CHECK(x.words==a.words);                                                         // 3 does not exist: off
    CHECK(d0.unsafe>0 || pairs.size()<=1);   // Adreno's mode 1 does bind unread support blocks dynamically (harmless there): the test sees the case
    // a binding that was never scanned stays static under the rule
    {   setenv_("DM_NR_UBODYN","1");setenv_("DM_GFX_UBO_DYNAMIC",nullptr);ubo_dynamic_on=false;nrubd::init(pvr,false);
        xbg_shader_binding nb{4,6,1,256,XBG_SOURCE_UNIFORM,0,0xffffffffu,0};CHECK(!UBO_DYN(2,nb));nb.flags=XBG_BIND_SCANNED;CHECK(UBO_DYN(2,nb));
        nb.flags=XBG_BIND_SCANNED|XBG_BIND_UNREAD;CHECK(!UBO_DYN(2,nb));CHECK(!UBO_DYN(1,nb));
        // opt_ok: a copy that drops the read of binding 4 is refused under the rule, used in the dry run
        std::vector<uint32_t> keep=spv[0],drop=spv[0];for(size_t i=5;i<drop.size();){uint32_t wc=drop[i]>>16;if((drop[i]&0xffff)==62){drop[i]=(1u<<16)|253;drop[i+1]=drop[i+2]=(1u<<16)|253;}i+=wc?wc:1;}
        CHECK(nrubd::opt_ok(keep,keep,"k"));CHECK(!nrubd::opt_ok(keep,drop,"k"));
        setenv_("DM_NR_UBODYN","2");nrubd::init(pvr,false);CHECK(nrubd::opt_ok(keep,drop,"k"));
        setenv_("DM_NR_UBODYN","0");nrubd::init(pvr,false);CHECK(nrubd::opt_ok(keep,drop,"k") && !nrubd::scan && !nrubd::ro);
        nrubd::summary(true);}
    printf("%s: %d failures\n",fails?"FAIL":"PASS",fails);return fails!=0;}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--bundles')
    ap.add_argument('--programs')
    ap.add_argument('--vulkan-include', default=os.environ.get('VULKAN_INCLUDE', os.environ.get('ANDROID_NDK_HOME', '<ndk>') + '/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/include'))
    a = ap.parse_args()
    game = (ENGINE / 'xbg_game.cpp').read_text(encoding='utf-8')
    inc = (ENGINE / 'xbg_nr_ubodyn.inc').read_text(encoding='utf-8').replace('#include "../../nr/nr_ubodyn.h"', '')
    macro = re.search(r'^#define UBO_DYN\(mode,b\).*$', game, re.M)
    assert macro, 'UBO_DYN macro not found'
    start = game.index('unsigned dyn_mode_for(bool compute,bool *no_ubo=nullptr){')
    dyn = balanced(game, start)
    src = (PREFIX.replace('@@ENGINE@@', ENGINE.as_posix()).replace('@@NR@@', NR.as_posix()) + inc + '\n' + macro.group(0) + '\n' + dyn + '\n' + SUFFIX)
    with tempfile.TemporaryDirectory() as td:
        cpp = pathlib.Path(td) / 'nr_ubodyn_engine.cpp'
        cpp.write_text(src, encoding='utf-8')
        exe = pathlib.Path(td) / 'nr_ubodyn_engine.exe'
        obj = pathlib.Path(td) / 'nr_ubodyn.o'
        r = subprocess.run([a.compiler, '-x', 'c', '-std=c11', '-O1', '-c', str(NR / 'nr_ubodyn.c'), '-o', str(obj)], capture_output=True, text=True)
        if not r.returncode:
            cc = [a.compiler, '-std=c++17', '-O1', '-w', '-D_CRT_SECURE_NO_WARNINGS', '-idirafter', a.vulkan_include, str(cpp), str(obj), '-o', str(exe)]
            r = subprocess.run(cc, capture_output=True, text=True)
        if r.returncode:
            print(r.stdout + r.stderr)
            return 1
        args = [str(exe)] + ([a.bundles, a.programs] if a.bundles and a.programs else [])
        return subprocess.run(args).returncode


if __name__ == '__main__':
    sys.exit(main())
