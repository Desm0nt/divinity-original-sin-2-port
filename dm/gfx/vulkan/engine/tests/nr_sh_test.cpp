// [dos2-nr04] NR-04: host test of the engine-shaders-by-name table and checks (dm/gfx/nr/nr_sh.h): lookups by key and by name, variants
// per DM_NR_SH_PCF (T11: 3 = dref 'd', nr_sh_depth_bindings), pack keys, the bundle head / interface / descriptor checks on synthetic bundles, the engine-name cross-check (only a
// swap turns an entry off), the pass hash and the verified-pass check.
// Optional: nr_sh_test <native-nr.xgspack> <translated bundle dir> [nr_sh.tsv] - every bundle of a pack built by tools/dos2_nr_sh_pack.py:
// its head equals the translated bundle's byte for byte (word 6 aside), its interface equals the translated one, its descriptors are in
// the binding table, a flipped byte of the head / an unknown binding are refused, and the C head hash equals the tool's column.
// Build (Windows clang + MSVC libs, or WSL g++):
//   clang -std=c11 -c ../../../nr/nr_sh.c -D_CRT_SECURE_NO_WARNINGS && clang++ -std=c++17 -Wall -D_CRT_SECURE_NO_WARNINGS nr_sh_test.cpp nr_sh.o -o nr_sh_test
#include "../../../nr/nr_sh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

static int failures, checks;
#define CHECK(expression) do { ++checks; if(!(expression)){printf("FAIL line %d: %s\n",__LINE__,#expression);++failures;} } while(0)

static void put32(std::vector<uint8_t> &b,uint32_t v){for(int i=0;i<4;i++)b.push_back(uint8_t(v>>(8*i)));}
static void set32(std::vector<uint8_t> &b,size_t at,uint32_t v){for(int i=0;i<4;i++)b[at+i]=uint8_t(v>>(8*i));}
// A minimal fragment module: entry point main with an input at location in_loc and outputs at out_locs, one sampler at (set, binding).
static std::vector<uint32_t> module(int in_loc,std::vector<int> out_locs,uint32_t set,uint32_t binding){
    std::vector<uint32_t> w={0x07230203u,0x00010000u,0,100,0};
    std::vector<uint32_t> ids;uint32_t id=10;
    std::vector<uint32_t> vars;
    const uint32_t vin=id++;vars.push_back(vin);std::vector<uint32_t> vout;for(size_t i=0;i<out_locs.size();i++){vout.push_back(id++);vars.push_back(vout.back());}
    const uint32_t vsam=id++;
    // OpEntryPoint Fragment %1 "main" vars...
    std::vector<uint32_t> ep={4u,1u,0x6e69616du,0u};for(uint32_t v:vars)ep.push_back(v);
    w.push_back(uint32_t((ep.size()+1)<<16|15));w.insert(w.end(),ep.begin(),ep.end());
    w.push_back(4u<<16|71);w.push_back(vin);w.push_back(30);w.push_back(uint32_t(in_loc));
    for(size_t i=0;i<vout.size();i++){w.push_back(4u<<16|71);w.push_back(vout[i]);w.push_back(30);w.push_back(uint32_t(out_locs[i]));}
    w.push_back(4u<<16|71);w.push_back(vsam);w.push_back(34);w.push_back(set);
    w.push_back(4u<<16|71);w.push_back(vsam);w.push_back(33);w.push_back(binding);
    w.push_back(4u<<16|59);w.push_back(50);w.push_back(vin);w.push_back(1);
    for(uint32_t v:vout){w.push_back(4u<<16|59);w.push_back(51);w.push_back(v);w.push_back(3);}
    w.push_back(4u<<16|59);w.push_back(52);w.push_back(vsam);w.push_back(0);
    return w;}
static std::vector<uint8_t> bundle(const std::vector<uint32_t> &spv,std::vector<uint32_t> bindings,uint32_t c1=8,uint32_t reserved=0){
    std::vector<uint8_t> b;put32(b,0x31534758u);put32(b,1);put32(b,4);put32(b,uint32_t(bindings.size()));put32(b,1);put32(b,c1);put32(b,uint32_t(spv.size()*4));put32(b,reserved);
    for(uint32_t x:bindings){put32(b,x);for(int k=0;k<7;k++)put32(b,uint32_t(k));}
    put32(b,7);for(uint32_t i=0;i<c1;i++)b.push_back(uint8_t(0xa0+i));
    for(uint32_t x:spv)put32(b,x);return b;}

static std::vector<uint8_t> file(const std::string &p){std::ifstream f(p,std::ios::binary);return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());}
static std::vector<uint32_t> words_of(const std::vector<uint8_t> &b){const size_t h=nr_sh_head(b.data(),b.size());std::vector<uint32_t> w((b.size()-h)/4);memcpy(w.data(),b.data()+h,w.size()*4);return w;}

static void real(const char *pack,const char *dir,const char *tsv){
    const std::vector<uint8_t> p=file(pack);CHECK(p.size()>16);if(p.size()<16)return;
    uint32_t n;memcpy(&n,p.data()+8,4);
    std::map<std::string,std::string> hashes;
    if(tsv){std::ifstream t(tsv);std::string line;std::getline(t,line);while(std::getline(t,line)){std::istringstream s(line);std::string pk,name,key,cls,proven,hh;std::getline(s,pk,'\t');std::getline(s,name,'\t');std::getline(s,key,'\t');std::getline(s,cls,'\t');std::getline(s,proven,'\t');std::getline(s,hh,'\t');hashes[pk]=hh;}}
    int seen=0;std::set<std::string> pks;
    for(uint32_t i=0;i<n;i++){
        char key[33]={0};memcpy(key,p.data()+16+56*i,32);uint64_t off;uint32_t size;memcpy(&off,p.data()+16+56*i+32,8);memcpy(&size,p.data()+16+56*i+40,4);
        std::vector<uint8_t> nb(p.begin()+off,p.begin()+off+size);
        const std::string pk=key;pks.insert(pk);
        CHECK(pk.size()>2 && pk[1]=='.');int e=nr_sh_find_name(pk.c_str()+2);if(e<0)e=nr_sh_find_key(pk.c_str()+2);CHECK(e>=0);if(e<0)continue;
        char want[32];CHECK(nr_sh_pack_key(e,pk[0],want,sizeof want) && pk==want);
        const std::vector<uint8_t> tb=file(std::string(dir)+"/"+nr_sh_table[e].key+".xgs");CHECK(!tb.empty());if(tb.empty())continue;
        CHECK(nr_sh_head_equal(tb.data(),tb.size(),nb.data(),nb.size()));
        CHECK(nr_sh_bindings_ok(nb.data(),nb.size()));
        std::vector<uint32_t> tw=words_of(tb),nw=words_of(nb);uint32_t ti=0,to=0,ni=1,no=1;
        CHECK(!nr_sh_io(tw.data(),tw.size(),&ti,&to) && !nr_sh_io(nw.data(),nw.size(),&ni,&no));CHECK(ti==ni && to==no);
        CHECK(to==(nr_sh_table[e].colors==2?3u:1u));   // the verified pass's colour count = the fragment outputs
        CHECK(nr_sh_depth_bindings(tw.data(),tw.size())==0);                                       // T11: no translated engine shader declares a depth image
        CHECK(nr_sh_depth_bindings(nw.data(),nw.size())==(pk[0]=='d'?1ull<<16:0ull));              //      only the dref bundles: the atlas, binding 16
        CHECK(nr_sh_head_hash(tb.data(),tb.size())==nr_sh_head_hash(nb.data(),nb.size()));
        if(!hashes.empty()){char hx[17];snprintf(hx,sizeof hx,"%016llx",(unsigned long long)nr_sh_head_hash(nb.data(),nb.size()));CHECK(hashes[pk]==hx);}
        std::vector<uint8_t> bad=nb;bad[40]^=1;CHECK(!nr_sh_head_equal(tb.data(),tb.size(),bad.data(),bad.size()));
        bad=nb;{uint32_t nbind;memcpy(&nbind,nb.data()+12,4);for(uint32_t k=0;k<nbind;k++)set32(bad,32+32*k,9000+k);}CHECK(!nr_sh_bindings_ok(bad.data(),bad.size()));   // table renamed: the SPIR-V's descriptors are unknown
        seen++;}
    int want_n=0;for(int e=0;e<nr_sh_count;e++)want_n+=int(strlen(nr_sh_table[e].variants));
    int oc=0;for(const std::string &k:pks)oc+=k[0]>='0' && k[0]<='9';   // NR-ONCHIP input-attachment variants ('2' / '3'.<name>) ride along in the pack
    CHECK(seen==want_n+oc && int(pks.size())==want_n+oc);
    printf("pack %s: %d bundles checked against %s\n",pack,seen,dir);}

int main(int argc,char **argv){
    // table
    CHECK(nr_sh_count==10+6);   // NR-04 10 + NR-33A (engine post) 6
    std::set<std::string> keys,names;
    for(int i=0;i<nr_sh_count;i++){const nr_sh_entry &e=nr_sh_table[i];CHECK(strlen(e.key)==16);for(int k=0;k<16;k++)CHECK(strchr("0123456789abcdef",e.key[k]));
        keys.insert(e.key);names.insert(e.name);CHECK(nr_sh_find_key(e.key)==i);CHECK(nr_sh_find_name(e.name)==i);
        CHECK(e.colors>=1 && e.colors<=2);CHECK(e.cls<NR_SH_CLASSES);CHECK(e.proven==(e.cls!=NR_SH_SHADOW));
        char pk[32];CHECK(nr_sh_pack_key(i,'x',pk,sizeof pk) && pk[0]=='x' && !strcmp(pk+2,strlen(e.name)+2>31?e.key:e.name) && strlen(pk)<32);}   // NR-33A: long names -> the key
    CHECK(int(keys.size())==nr_sh_count && int(names.size())==nr_sh_count);
    const int fade=nr_sh_find_name("FadeBlending"),pcf=nr_sh_find_name("CascadedPCF_3x768"),sky=nr_sh_find_name("DeferredSky");
    CHECK(fade>=0 && pcf>=0 && sky>=0);
    CHECK(nr_sh_find_name("Shaders/NX/CascadedPCF_3x768.bshd")==pcf);CHECK(nr_sh_find_name("EngineShaders:Shaders\\NX\\fadeblending.BSHD")==-1);   // only .bshd stripped, any case
    CHECK(nr_sh_find_name("EngineShaders:Shaders/NX/fadeblending.bshd")==fade);CHECK(nr_sh_find_name("FadeBlendingX")==-1);CHECK(nr_sh_find_name("")==-1);CHECK(nr_sh_find_name(nullptr)==-1);
    CHECK(nr_sh_find_key("62d683cc49953ad6")==fade);CHECK(nr_sh_find_key("62d683cc49953ad7")==-1);CHECK(nr_sh_find_key(nullptr)==-1);
    // variants
    CHECK(nr_sh_variant(fade,NR_SH_PCF_OFF)=='x' && nr_sh_variant(fade,NR_SH_PCF_MOBILE)=='x');
    CHECK(nr_sh_variant(pcf,NR_SH_PCF_OFF)==0 && nr_sh_variant(pcf,NR_SH_PCF_EXACT)=='x' && nr_sh_variant(pcf,NR_SH_PCF_MOBILE)=='m' && nr_sh_variant(pcf,7)==0);
    CHECK(nr_sh_variant(-1,1)==0 && nr_sh_variant(nr_sh_count,1)==0);
    // PERF-X19 T11: DM_NR_SH_PCF=3 -> 'd' (every shadow entry has it), post / light keep 'x'
    CHECK(nr_sh_variant(pcf,NR_SH_PCF_DREF)=='d' && nr_sh_variant(fade,NR_SH_PCF_DREF)=='x' && nr_sh_variant(pcf,4)==0);
    for(int i=0;i<nr_sh_count;i++)if(nr_sh_table[i].cls==NR_SH_SHADOW){char pk[32];CHECK(strchr(nr_sh_table[i].variants,'d') && nr_sh_pack_key(i,'d',pk,sizeof pk) && pk[0]=='d' && pk[1]=='.');}
    {// nr_sh_depth_bindings: %2 float, %3 image 2D Depth=1, %4 sampled image, %5 ptr UniformConstant, %6 var (set 0 binding 16);
     // %7 image Depth=0 -> %8 sampled -> %9 ptr -> %10 var (binding 10); %11 a Depth=1 variable in set 1 (binding 3) is not counted
     std::vector<uint32_t> w={0x07230203u,0x00010000u,0,100,0};
     auto op=[&](uint32_t code,std::vector<uint32_t> a){w.push_back(uint32_t((a.size()+1)<<16|code));w.insert(w.end(),a.begin(),a.end());};
     op(71,{6,33,16});op(71,{6,34,0});op(71,{10,33,10});op(71,{10,34,0});op(71,{11,33,3});op(71,{11,34,1});
     op(22,{2,32});op(25,{3,2,1,1,0,0,1,0});op(27,{4,3});op(32,{5,0,4});op(59,{5,6,0});
     op(25,{7,2,1,0,0,0,1,0});op(27,{8,7});op(32,{9,0,8});op(59,{9,10,0});op(59,{5,11,0});
     CHECK(nr_sh_depth_bindings(w.data(),w.size())==(1ull<<16));
     std::vector<uint32_t> nod=w;for(size_t i=5;i<nod.size();i+=nod[i]>>16)if((nod[i]&0xffff)==25)nod[i+4]=0;CHECK(nr_sh_depth_bindings(nod.data(),nod.size())==0);
     std::vector<uint32_t> bad=w;bad[0]=0;CHECK(nr_sh_depth_bindings(bad.data(),bad.size())==0);CHECK(nr_sh_depth_bindings(nullptr,0)==0);
}
    {char small[8];CHECK(!nr_sh_pack_key(fade,'x',small,sizeof small));CHECK(!nr_sh_pack_key(fade,0,small,32));}
    // bundles
    const std::vector<uint32_t> m=module(1,{0},0,10);
    std::vector<uint8_t> a=bundle(m,{4,10,22}),b=bundle(module(1,{0},0,10),{4,10,22});
    CHECK(nr_sh_head(a.data(),a.size())==32+3*32+4+8);
    CHECK(nr_sh_head_equal(a.data(),a.size(),b.data(),b.size()));
    {std::vector<uint32_t> longer=m;longer.push_back(1u<<16|0);std::vector<uint8_t> c=bundle(longer,{4,10,22});CHECK(c.size()==a.size()+4);
     CHECK(nr_sh_head_equal(a.data(),a.size(),c.data(),c.size()));   // word 6 (SPIR-V length) is the rewrite's own
     CHECK(nr_sh_head_hash(a.data(),a.size())==nr_sh_head_hash(c.data(),c.size()));}
    {std::vector<uint8_t> c=b;c[32+32]^=1;CHECK(!nr_sh_head_equal(a.data(),a.size(),c.data(),c.size()));}           // binding table
    {std::vector<uint8_t> c=b;c[32+3*32+4+3]^=1;CHECK(!nr_sh_head_equal(a.data(),a.size(),c.data(),c.size()));}      // c1 bytes
    {std::vector<uint8_t> c=bundle(m,{4,10,22},8,0xA1000000u);CHECK(!nr_sh_head_equal(a.data(),a.size(),c.data(),c.size()));}   // reserved (NR-02 layout word)
    {std::vector<uint8_t> c=a;c.pop_back();CHECK(nr_sh_head(c.data(),c.size())==0);CHECK(!nr_sh_head_equal(a.data(),a.size(),c.data(),c.size()));CHECK(nr_sh_head_hash(c.data(),c.size())==0);}
    {std::vector<uint8_t> c=a;set32(c,0,0x12345678);CHECK(nr_sh_head(c.data(),c.size())==0);}
    CHECK(nr_sh_head(nullptr,0)==0);
    CHECK(nr_sh_depth_bindings(m.data(),m.size())==0);   // T11: a module without image types
    {uint32_t in,out;CHECK(!nr_sh_io(m.data(),m.size(),&in,&out));CHECK(in==2 && out==1);
     std::vector<uint32_t> two=module(2,{0,1},0,10);CHECK(!nr_sh_io(two.data(),two.size(),&in,&out));CHECK(in==4 && out==3);
     std::vector<uint32_t> broken=m;broken[0]=0;CHECK(nr_sh_io(broken.data(),broken.size(),&in,&out)==-1);
     std::vector<uint32_t> cut=m;cut[5]=0xffff0000u|15;CHECK(nr_sh_io(cut.data(),cut.size(),&in,&out)==-1);}
    CHECK(nr_sh_bindings_ok(a.data(),a.size()));
    {std::vector<uint8_t> c=bundle(module(1,{0},0,16),{4,10,22});CHECK(!nr_sh_bindings_ok(c.data(),c.size()));}       // binding not in the table
    {std::vector<uint8_t> c=bundle(module(1,{0},1,10),{4,10,22});CHECK(!nr_sh_bindings_ok(c.data(),c.size()));}       // another set
    {std::vector<uint8_t> c=bundle(m,{4,10,22},7);CHECK(nr_sh_head(c.data(),c.size())!=0);CHECK(nr_sh_bindings_ok(c.data(),c.size()));}   // unaligned SPIR-V
    // engine names: the key decides, only a swap turns an entry off
    int idx=-9;
    CHECK(nr_sh_name_note("FadeBlending",nullptr,"62d683cc49953ad6",&idx)==NR_SH_NAME_AGREE && idx==fade);
    CHECK(nr_sh_name_note(nullptr,"EngineShaders:Shaders/NX/FadeBlending.bshd","62d683cc49953ad6",&idx)==NR_SH_NAME_AGREE);
    CHECK(nr_sh_name_note("SomeMaterial","Shaders/NX/FadeBlending.bshd","62d683cc49953ad6",&idx)==NR_SH_NAME_AGREE);   // the path names it
    CHECK(nr_sh_names_agreed()==3);
    CHECK(nr_sh_name_note("SomeMaterial","Materials/Foo.bshd","0123456789abcdef",&idx)==NR_SH_NAME_OTHER && idx==-1);
    CHECK(nr_sh_name_note("FadeBlending",nullptr,"0123456789abcdef",&idx)==NR_SH_NAME_KEY_DIFFERS && idx==fade);
    CHECK(nr_sh_name_note("FadeBlending",nullptr,nullptr,&idx)==NR_SH_NAME_OTHER);
    CHECK(nr_sh_name_note("PostProcess Fade",nullptr,"62d683cc49953ad6",&idx)==NR_SH_NAME_NAME_DIFFERS && idx==fade);
    CHECK(nr_sh_name_note(nullptr,nullptr,"62d683cc49953ad6",&idx)==NR_SH_NAME_OTHER);
    // NR-04 fix (phone run nr4_b2): names are path-qualified - the engine's Shaders/NX/Orthographic/LinearizeDepth.bshd is another shader
    // (fragment key a2cca3720ade8cc2, programs.tsv) than the table's Shaders/NX/LinearizeDepth.bshd (f23c09d198b55dc5): no contradiction
    {const int lin=nr_sh_find_name("LinearizeDepth");CHECK(lin>=0);
     const char *ortho="rom:/Data/Shaders/NX/Orthographic/LinearizeDepth.bshd",*plain="rom:/Data/Shaders/NX/LinearizeDepth.bshd";
     CHECK(nr_sh_find_name(plain)==lin);CHECK(nr_sh_find_name(ortho)==-1);
     CHECK(nr_sh_find_name("EngineShaders:Shaders/NX/Orthographic/LinearizeDepth.bshd")==-1);CHECK(nr_sh_find_name("Shaders\\NX\\Orthographic\\LinearizeDepth.bshd")==-1);
     CHECK(nr_sh_find_name("Data\\Shaders\\NX\\LinearizeDepth.bshd")==lin);CHECK(nr_sh_find_name("rom:/DATA/shaders/nx/linearizedepth.bshd")==lin);
     CHECK(nr_sh_find_name("Materials/LinearizeDepth.bshd")==-1);CHECK(nr_sh_find_name("Shaders/NXOrtho/LinearizeDepth.bshd")==-1);
     CHECK(nr_sh_find_name("MyShaders/NX/LinearizeDepth.bshd")==-1);CHECK(nr_sh_find_name("Shaders/NX/")==-1);CHECK(nr_sh_find_name("/LinearizeDepth.bshd")==-1);
     CHECK(nr_sh_name_note(ortho,ortho,"a2cca3720ade8cc2",&idx)==NR_SH_NAME_OTHER && idx==-1);                     // the phone's red line: now nothing
     CHECK(nr_sh_name_note(plain,plain,"a2cca3720ade8cc2",&idx)==NR_SH_NAME_KEY_DIFFERS && idx==lin);              // a real contradiction stays red
     CHECK(nr_sh_name_note(ortho,ortho,nr_sh_table[lin].key,&idx)==NR_SH_NAME_NAME_DIFFERS && idx==lin);           // our code in another folder: reported
     const uint64_t before=nr_sh_names_agreed();CHECK(nr_sh_name_note(plain,plain,nr_sh_table[lin].key,&idx)==NR_SH_NAME_AGREE && idx==lin);
     CHECK(nr_sh_names_agreed()==before+1);CHECK(nr_sh_enabled(lin));}
    CHECK(nr_sh_enabled(fade));
    CHECK(nr_sh_name_note("EoCFog",nullptr,nr_sh_table[sky].key,&idx)==NR_SH_NAME_SWAPPED && idx==sky);
    CHECK(!nr_sh_enabled(sky) && nr_sh_enabled(fade) && nr_sh_enabled(nr_sh_find_name("EoCFog")));
    CHECK(!nr_sh_enabled(-1) && !nr_sh_enabled(nr_sh_count));
    // passes
    {uint32_t f1[1]={0x3f},f2[2]={0x3f,0x25},g[1]={0x25};
     CHECK(nr_sh_pass_check(fade,1,f1,1)==0);CHECK(nr_sh_pass_check(fade,1,g,1)==NR_SH_P_FORMAT);CHECK(nr_sh_pass_check(fade,2,f2,1)==NR_SH_P_COLORS);
     CHECK(nr_sh_pass_check(fade,1,f1,4)==NR_SH_P_SAMPLES);CHECK(nr_sh_pass_check(fade,0,nullptr,1)==(NR_SH_P_COLORS|NR_SH_P_FORMAT));
     CHECK(nr_sh_pass_check(sky,2,f2,1)==0);CHECK(nr_sh_pass_check(sky,1,f1,1)==NR_SH_P_COLORS);
     uint32_t r8[1]={0x1};CHECK(nr_sh_pass_check(pcf,1,r8,1)==0);CHECK(nr_sh_pass_check(-1,1,r8,1)!=0);
     CHECK(nr_sh_pass_hash(1,f1,0,1)==nr_sh_pass_hash(1,f1,0,1));CHECK(nr_sh_pass_hash(1,f1,0,1)!=nr_sh_pass_hash(1,f1,0x35,1));
     CHECK(nr_sh_pass_hash(1,f1,0,1)!=nr_sh_pass_hash(1,g,0,1));CHECK(nr_sh_pass_hash(1,f1,0,1)!=nr_sh_pass_hash(2,f2,0,1));CHECK(nr_sh_pass_hash(1,f1,0,1)!=nr_sh_pass_hash(1,f1,0,2));}
    if(argc>=3)real(argv[1],argv[2],argc>=4?argv[3]:nullptr);
    printf("%s: %d checks, %d failures\n",failures?"FAIL":"PASS",checks,failures);
    return failures?1:0;}
