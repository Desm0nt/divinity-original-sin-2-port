// NVN alpha test rewrite (xbg_spirv_alpha.h) on synthetic fragment modules: the test before every OpReturn of the entry point, the
// comparison per func, the reference constant, NEVER, EarlyFragmentTests removed, OpTypeBool reused, demote when declared, and the
// refusals (no Location 0 / integer target / .w never written / ALWAYS).
// With arguments: <bundle dir> <out dir> [func [ref]] rewrites every fragment shader of the .xgs bundles (XGS1 header, xbg_game.h)
// and writes <out>/<key>.f<func>.spv for spirv-val; prints the refusal reasons. With one argument <dir>: the synthetic run also
// writes its input and outputs there as .spv for spirv-val.
// Host build: cl /std:c++17 /EHsc tests\spirv_alpha_test.cpp xbg_spirv_alpha.cpp (or g++ -std=c++17 tests/spirv_alpha_test.cpp xbg_spirv_alpha.cpp).
#include "../xbg_spirv_alpha.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Module=std::vector<uint32_t>;
void require(bool ok,const char *why){if(!ok)throw std::runtime_error(why);}
void op(Module &m,uint32_t opcode,std::initializer_list<uint32_t> args){m.push_back((uint32_t(args.size()+1)<<16)|opcode);m.insert(m.end(),args);}
uint32_t fbits(float f){uint32_t b;memcpy(&b,&f,4);return b;}
enum : uint32_t {tVoid=1,tFn=2,tFloat=3,tVec4=4,tPtrOutV4=5,tBool=6,tInt=7,tPtrOutF=8,tIVec4=9,tPtrOutIV4=10,
    cOne=20,cHalf=21,cTrue=22,cColor=23,cI3=24,cI0=25,
    vOut=30,vOut1=31,fMain=40,lEntry=41,lThen=42,lElse=43,ac=44,BOUND=50};
struct Opt{bool early=true,has_bool=false,two_returns=true,int_target=false,no_location0=false,write_w=true,chain3=false,
    dual=false,demote=false,mrt_index=false;};
// main(): store a colour to the Location 0 output, then if(true) return; else return; (two OpReturn sites)
Module fixture(const Opt &o){
    Module m={0x07230203,0x00010300,0,BOUND,0};
    op(m,17,{1});if(o.demote)op(m,17,{5379});
    if(o.demote)op(m,10,{0x5f565053,0x5f545845,0x6f6d6564,0x745f6574,0x65685f6f,0x7265706c,0x766e695f,0x7461636f,0x006e6f69});   // "SPV_EXT_demote_to_helper_invocation"
    op(m,14,{0,1});
    if(o.dual)op(m,15,{4,fMain,0x6e69616d,0,vOut,vOut1});else op(m,15,{4,fMain,0x6e69616d,0,vOut});
    op(m,16,{fMain,7});if(o.early)op(m,16,{fMain,9});
    op(m,71,{vOut,30,o.no_location0?1u:0u});if(o.dual){op(m,71,{vOut,32,o.mrt_index?1u:0u});op(m,71,{vOut1,30,0});op(m,71,{vOut1,32,o.mrt_index?0u:1u});}
    op(m,19,{tVoid});op(m,33,{tFn,tVoid});op(m,22,{tFloat,32});op(m,23,{tVec4,tFloat,4});op(m,32,{tPtrOutV4,3,tVec4});
    if(o.has_bool)op(m,20,{tBool});
    op(m,21,{tInt,32,1});op(m,32,{tPtrOutF,3,tFloat});op(m,23,{tIVec4,tInt,4});op(m,32,{tPtrOutIV4,3,tIVec4});
    op(m,43,{tFloat,cOne,fbits(1.f)});op(m,43,{tFloat,cHalf,fbits(.5f)});op(m,43,{tInt,cI3,3});op(m,43,{tInt,cI0,0});
    if(o.has_bool)op(m,41,{tBool,cTrue});
    op(m,44,{tVec4,cColor,cOne,cHalf,cOne,cHalf});
    op(m,59,{o.int_target?tPtrOutIV4:tPtrOutV4,vOut,3});if(o.dual)op(m,59,{tPtrOutV4,vOut1,3});
    op(m,54,{tVoid,fMain,0,tFn});op(m,248,{lEntry});
    if(o.chain3 || !o.write_w){op(m,65,{tPtrOutF,ac,vOut,o.chain3?cI3:cI0});op(m,62,{ac,cHalf});}
    else if(!o.int_target)op(m,62,{vOut,cColor});
    if(o.dual)op(m,62,{vOut1,cColor});
    if(o.two_returns){
        // with a bool type: if(true); without one (the rewrite must add it): switch(0) { case 0: return; default: return; }
        op(m,247,{lElse,0});if(o.has_bool)op(m,250,{cTrue,lThen,lElse});else op(m,251,{cI0,lElse,0,lThen});
        op(m,248,{lThen});op(m,253,{});op(m,248,{lElse});op(m,253,{});}
    else op(m,253,{});
    op(m,56,{});return m;}
struct Scan{std::map<uint32_t,unsigned> ops;std::vector<std::vector<uint32_t>> ins;};
Scan scan(const Module &m){Scan s;for(size_t i=5;i<m.size();){uint32_t n=m[i]>>16;require(n && i+n<=m.size(),"bad stream");s.ops[m[i]&65535]++;s.ins.emplace_back(m.begin()+i,m.begin()+i+n);i+=n;}return s;}
bool has(const Scan &s,std::initializer_list<uint32_t> words){for(auto &i:s.ins)if(i.size()==words.size() && std::equal(i.begin(),i.end(),words.begin()))return true;return false;}

const char *dump_dir=nullptr;
void dump(const Module &m,const char *name){if(!dump_dir)return;std::ofstream o(std::string(dump_dir)+"/"+name+".spv",std::ios::binary);o.write(reinterpret_cast<const char*>(m.data()),std::streamsize(m.size()*4));}
void synthetic(){
    const uint32_t cmp[8]={0,0,184,180,188,186,182,190};
    for(uint32_t func=2;func<=7;func++){
        Opt o;Module in=fixture(o),out;xbg_spirv_alpha_info info;
        const char *why=xbg_spirv_alpha_test(in.data(),in.size(),func,.25f,out,&info);require(!why,why?why:"");
        if(func==2){dump(in,"synthetic_in");}dump(out,("synthetic_f"+std::to_string(func)).c_str());
        Scan s=scan(out);require(info.returns==2 && s.ops[253]==2,"one test per OpReturn, OpReturn kept");
        require(s.ops[252]==2 && s.ops[cmp[func]]==2 && s.ops[247]==2+1 && s.ops[61]==2 && s.ops[81]==2,"load / extract / compare / kill per return");
        require(info.early_removed && !has(s,{(3u<<16)|16,fMain,9}),"EarlyFragmentTests removed");
        require(has(s,{(3u<<16)|16,fMain,7}),"other execution modes kept");
        require(s.ops[20]==1,"one OpTypeBool added");
        require(out[3]==BOUND+2+5*2,"bound grows by bool, constant and 5 ids per return");
        bool ref=false;for(auto &i:s.ins)if((i[0]&65535)==43 && i.size()==4 && i[1]==tFloat && i[3]==fbits(.25f))ref=true;require(ref,"reference constant");
        // the compare reads the extracted alpha against the reference; the branch passes to the merge and fails to the kill block
        for(size_t k=0;k<s.ins.size();k++)if((s.ins[k][0]&65535)==cmp[func]){require(s.ins[k+2][0]==((4u<<16)|250) && s.ins[k+2][1]==s.ins[k][2] && s.ins[k+3][1]==s.ins[k+2][3] && (s.ins[k+4][0]&65535)==252 && s.ins[k+5][1]==s.ins[k+2][2],"pass -> merge, fail -> kill");}}
    {Opt o;o.has_bool=true;o.early=false;Module in=fixture(o),out;xbg_spirv_alpha_info info;
        require(!xbg_spirv_alpha_test(in.data(),in.size(),5,.5f,out,&info),"existing bool");Scan s=scan(out);dump(out,"synthetic_bool");
        require(s.ops[20]==1 && !info.early_removed && out[3]==BOUND+1+5*2,"OpTypeBool reused");
        for(auto &i:s.ins)if((i[0]&65535)==186)require(i[1]==tBool,"compare typed with the module's bool");}
    {Opt o;Module in=fixture(o),out;xbg_spirv_alpha_info info;   // NEVER: every return becomes a kill, nothing else added
        require(!xbg_spirv_alpha_test(in.data(),in.size(),1,0.f,out,&info),"never");Scan s=scan(out);dump(out,"synthetic_never");
        require(info.returns==2 && s.ops[253]==0 && s.ops[252]==2 && s.ops[20]==0 && out[3]==BOUND,"NEVER kills");}
    {Opt o;o.demote=true;Module in=fixture(o),out;xbg_spirv_alpha_info info;
        require(!xbg_spirv_alpha_test(in.data(),in.size(),5,.5f,out,&info),"demote");Scan s=scan(out);dump(out,"synthetic_demote");
        require(info.demote && s.ops[5380]==2 && s.ops[252]==0 && s.ops[249]==2,"demote + branch to merge");}
    {Opt o;o.chain3=true;Module in=fixture(o),out;require(!xbg_spirv_alpha_test(in.data(),in.size(),5,.5f,out),"store through an access chain to .w");}
    {Opt o;o.dual=true;Module in=fixture(o),out;require(!xbg_spirv_alpha_test(in.data(),in.size(),5,.5f,out),"dual source");dump(out,"synthetic_dual");
        Scan s=scan(out);for(auto &i:s.ins)if((i[0]&65535)==61)require(i[3]==vOut,"Index 0 output tested");}
    {Opt o;o.dual=true;o.mrt_index=true;Module in=fixture(o),out;require(!xbg_spirv_alpha_test(in.data(),in.size(),5,.5f,out),"dual source swapped");
        Scan s=scan(out);for(auto &i:s.ins)if((i[0]&65535)==61)require(i[3]==vOut1,"Index 0 output tested (second variable)");}
    {Opt o;o.two_returns=false;Module in=fixture(o),out;xbg_spirv_alpha_info info;require(!xbg_spirv_alpha_test(in.data(),in.size(),7,1.f,out,&info) && info.returns==1,"single return");}
    // refusals leave out empty
    struct R{Opt o;uint32_t func;};std::vector<R> refuse;
    {R r;r.func=8;refuse.push_back(r);}{R r;r.func=0;refuse.push_back(r);}
    {R r;r.func=5;r.o.no_location0=true;refuse.push_back(r);}
    {R r;r.func=5;r.o.int_target=true;refuse.push_back(r);}
    {R r;r.func=5;r.o.write_w=false;refuse.push_back(r);}
    for(auto &r:refuse){Module in=fixture(r.o),out={1,2,3};const char *why=xbg_spirv_alpha_test(in.data(),in.size(),r.func,.5f,out);require(why && out.empty(),"refused");}
    {Opt o;o.no_location0=true;Module in=fixture(o),out;require(!xbg_spirv_alpha_test(in.data(),in.size(),1,0.f,out),"NEVER needs no output");}
    std::cout<<"PASS: alpha test before each OpReturn (6 funcs), reference constant, bool reuse, NEVER, demote, chain .w, dual source, EarlyFragmentTests removal, 6 refusals\n";
}

int bundles(const char *dir,const char *outdir,uint32_t func,float ref){
    namespace fs=std::filesystem;fs::create_directories(outdir);std::map<std::string,unsigned> why_count;unsigned done=0,total=0;
    for(auto &e:fs::directory_iterator(dir)){if(e.path().extension()!=".xgs")continue;
        std::ifstream f(e.path(),std::ios::binary);std::vector<char> b((std::istreambuf_iterator<char>(f)),{});if(b.size()<32)continue;
        uint32_t h[8];memcpy(h,b.data(),32);if(h[0]!=0x31534758 || h[2]!=4)continue;
        size_t off=32+size_t(h[3])*32+size_t(h[4])*4+h[5];if(off+h[6]!=b.size())continue;++total;
        Module in(h[6]/4),out;memcpy(in.data(),b.data()+off,h[6]);xbg_spirv_alpha_info info;
        const char *why=xbg_spirv_alpha_test(in.data(),in.size(),func,ref,out,&info);
        if(why){why_count[why]++;continue;}
        std::ofstream o((fs::path(outdir)/(e.path().stem().string()+".f"+std::to_string(func)+".spv")),std::ios::binary);o.write(reinterpret_cast<const char*>(out.data()),std::streamsize(out.size()*4));++done;}
    std::cout<<"fragment bundles "<<total<<", rewritten "<<done<<" (func "<<func<<", ref "<<ref<<")\n";for(auto &w:why_count)std::cout<<"  refused "<<w.second<<": "<<w.first<<"\n";
    return 0;}
}

int main(int argc,char **argv){
    try{if(argc==2)dump_dir=argv[1];if(argc>=3)return bundles(argv[1],argv[2],argc>3?uint32_t(atoi(argv[3])):5u,argc>4?float(atof(argv[4])):.5f);synthetic();return 0;}
    catch(const std::exception &e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
