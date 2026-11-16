// Gather footprint snap (xbg_spirv_gather.h).
// 1. The arithmetic: for random coordinates and sizes the snapped coordinate selects, under an exact floor(u * size - 0.5) footprint
//    and under Maxwell's floor(u * size - 0.5 + 1/512), the texel the shader's own weights assume (floor(fma(u, size, -0.498046905))),
//    and sits at fraction 0.5 of it; the unsnapped coordinate disagrees with the exact rule in a band about 1/512 texel wide.
// 2. With arguments <bundle dir> [out dir] [all]: every .xgs fragment bundle is rewritten; the modules go to <out dir>/<key>.spv for
//    spirv-val --target-env vulkan1.1 (for f in out/*.spv; do spirv-val --target-env vulkan1.1 $f || echo FAIL $f; done).
// Host build: g++ -std=c++17 -O1 tests/spirv_gather_test.cpp xbg_spirv_gather.cpp -o spirv_gather_test (or cl /std:c++17 /EHsc ...).
#include "../xbg_spirv_gather.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

static int failures=0;
static void require(bool ok,const char *what){if(!ok){++failures;printf("FAIL %s\n",what);}}

static void arithmetic(){
    std::mt19937 rng(20261116);const float weight=-0.498046905f;
    const float sizes[]={4,64,204,412,616,1228,2048,4096};
    unsigned long long n=0,band=0,snapped_bad=0;
    for(float size:sizes)for(int k=0;k<400000;k++){
        // half of the samples sit right at a texel border, where the band is
        float texel=float(rng()%unsigned(size));float fr=(k&1)?(0.5f-float(rng()%4096)/4096.f/128.f):float(rng()%65536)/65536.f;
        float u=(texel+fr)/size;
        const float want=std::floor(std::fma(u,size,weight));                    // the shader's footprint (first texel)
        const float exact=std::floor(u*size-0.5f);                               // a GPU without Maxwell's rounding
        if(exact!=want)++band;
        const float snapped=(want+1.f)/size;                                     // what the rewrite passes to the gather
        const float sx=snapped*size-0.5f;                                        // texel-space position of the snapped coordinate
        if(std::floor(sx)!=want || std::floor(sx+1.f/512.f)!=want || std::fabs(sx-std::floor(sx)-0.5f)>0.01f)++snapped_bad;
        ++n;}
    printf("arithmetic: %llu samples, unsnapped footprint differs from the shader's in %llu (%.3f%% of the border samples), snapped differs in %llu\n",
           n,band,200.0*double(band)/double(n),snapped_bad);
    require(band>0,"the test never hit the 1/512 band");require(snapped_bad==0,"snapped coordinate picks another footprint");
}

int main(int argc,char **argv){
    arithmetic();
    if(argc>=2){
        const bool all=argc>=4 && !strcmp(argv[3],"all");const std::string outdir=argc>=3?argv[2]:"";
        if(!outdir.empty())std::filesystem::create_directories(outdir);
        unsigned long long files=0,fragment=0,rewritten=0,sites=0,skipped=0,signature=0;
        for(auto &e:std::filesystem::directory_iterator(argv[1])){
            if(e.path().extension()!=".xgs")continue;++files;
            std::ifstream f(e.path(),std::ios::binary);std::vector<char> b((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
            if(b.size()<32)continue;uint32_t h[8];memcpy(h,b.data(),32);if(h[0]!=0x31534758 || h[6]%4)continue;
            const size_t off=32+size_t(h[3])*32+size_t(h[4])*4+h[5];if(off+h[6]>b.size())continue;
            if(h[2]==4)++fragment;
            std::vector<uint32_t> in(h[6]/4),out;memcpy(in.data(),b.data()+off,h[6]);xbg_spirv_gather_info info;
            const char *why=xbg_spirv_gather_snap(in.data(),in.size(),all,out,&info);
            if(info.signature)++signature;skipped+=info.skipped;
            if(why){require(out.empty(),"refused but out is not empty");continue;}
            ++rewritten;sites+=info.snapped;require(info.snapped>0 && out.size()>in.size(),"rewritten without a site");
            if(!outdir.empty()){std::ofstream o(outdir+"/"+e.path().stem().string()+".spv",std::ios::binary);o.write(reinterpret_cast<const char*>(out.data()),std::streamsize(out.size()*4));}
        }
        printf("bundles: %llu files, %llu fragment, %llu with the weight constant, %llu rewritten (%llu gather sites, %llu gathers left alone)%s\n",
               files,fragment,signature,rewritten,sites,skipped,all?" [all]":"");
    }
    printf(failures?"FAILED (%d)\n":"PASS\n",failures);return failures?1:0;
}
