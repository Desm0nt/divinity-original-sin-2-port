// Gather footprint snap (xbg_spirv_gather.h). Pure SPIR-V word rewrite, no Vulkan: host-tested by tests/spirv_gather_test.cpp.
#include "xbg_spirv_gather.h"
#include <cstring>
#include <map>
#include <set>

namespace {
enum : uint32_t {
    OpExtInstImport=11,OpExtInst=12,OpCapability=17,OpTypeVoid=19,OpTypeInt=21,OpTypeFloat=22,OpTypeVector=23,OpTypeImage=25,
    OpTypeSampledImage=27,OpTypeForwardPointer=39,OpConstant=43,OpConstantComposite=44,OpFunction=54,OpImageGather=96,OpImage=100,
    OpImageQuerySizeLod=103,OpConvertSToF=111,OpFAdd=129,OpFDiv=136,
    CapImageQuery=50,Dim2D=1,GlslFloor=8,GlslFma=50,
    WeightBits=0xbeff0001u,WeightBitsExact=0xbeff0000u,OneBits=0x3f800000u
};
void emit(std::vector<uint32_t> &o,uint32_t op,std::initializer_list<uint32_t> a){o.push_back((uint32_t(a.size()+1)<<16)|op);o.insert(o.end(),a.begin(),a.end());}
struct ImageType{uint32_t dim,arrayed,ms,sampled;};
}

const char *xbg_spirv_gather_snap(const uint32_t *w,size_t n,bool all,std::vector<uint32_t> &out,xbg_spirv_gather_info *info){
    out.clear();xbg_spirv_gather_info local;xbg_spirv_gather_info &inf=info?*info:local;inf=xbg_spirv_gather_info{};
    if(!w || n<5 || w[0]!=0x07230203)return "not a SPIR-V module";
    uint32_t f32=0,i32=0,v2f=0,v2i=0,glsl=0;bool query_cap=false,gathers=false;size_t first_function=0;
    std::set<uint32_t> types;std::map<uint32_t,ImageType> images;std::map<uint32_t,uint32_t> sampled;   // sampled image type -> image type
    // pass 1: declarations
    for(size_t i=5;i<n;){const uint32_t len=w[i]>>16,op=w[i]&65535;if(!len || i+len>n)return "malformed instruction stream";
        if(op>=OpTypeVoid && op<=OpTypeForwardPointer && len>=2)types.insert(w[i+1]);   // every OpType*: the result id comes first
        switch(op){
        case OpCapability:if(len>=2 && w[i+1]==CapImageQuery)query_cap=true;break;
        case OpExtInstImport:if(len>=5 && !memcmp(w+i+2,"GLSL.std.450",12))glsl=w[i+1];break;
        case OpTypeFloat:if(len>=3 && w[i+2]==32 && !f32)f32=w[i+1];break;
        case OpTypeInt:if(len>=4 && w[i+2]==32 && w[i+3]==1 && !i32)i32=w[i+1];break;
        case OpTypeVector:if(len>=4 && w[i+3]==2){if(f32 && w[i+2]==f32 && !v2f)v2f=w[i+1];else if(i32 && w[i+2]==i32 && !v2i)v2i=w[i+1];}break;
        case OpTypeImage:if(len>=8)images[w[i+1]]={w[i+3],w[i+5],w[i+6],w[i+7]};break;
        case OpTypeSampledImage:if(len>=3)sampled[w[i+1]]=w[i+2];break;
        case OpConstant:if(len==4 && f32 && w[i+1]==f32 && (w[i+3]==WeightBits || w[i+3]==WeightBitsExact))inf.signature=true;break;
        case OpFunction:if(!first_function)first_function=i;break;
        case OpImageGather:gathers=true;break;
        default:break;}
        i+=len;}
    if(!gathers)return "no OpImageGather";
    if(!all && !inf.signature)return "no gather weight constant";
    if(!first_function || !f32 || !v2f || !glsl)return "no float vec2 type or GLSL.std.450 import";
    // pass 2: result types inside functions (there every instruction whose first operand is a type id has its result id next)
    std::map<uint32_t,uint32_t> result_type;std::set<size_t> sites;
    for(size_t i=first_function;i<n;){const uint32_t len=w[i]>>16,op=w[i]&65535;
        if(len>=3 && types.count(w[i+1]))result_type[w[i+2]]=w[i+1];
        if(op==OpImageGather && len>=6){
            bool ok=len==6 || (len==7 && w[i+6]==0);   // no image operands (Bias / Lod / Offset / ...)
            auto st=result_type.find(w[i+3]);auto ct=result_type.find(w[i+4]);
            if(ok){ok=false;if(st!=result_type.end() && ct!=result_type.end() && ct->second==v2f){auto si=sampled.find(st->second);
                if(si!=sampled.end()){auto im=images.find(si->second);ok=im!=images.end() && im->second.dim==Dim2D && !im->second.arrayed && !im->second.ms && im->second.sampled!=2;}}}
            if(ok)sites.insert(i);else ++inf.skipped;}
        i+=len;}
    if(sites.empty())return "no plain 2D gather";
    uint32_t bound=w[3];
    const uint32_t new_i32=i32?0:(i32=bound++),new_v2i=v2i?0:(v2i=bound++);
    const uint32_t c_zero=bound++,c_weight=bound++,c_one=bound++,cv_weight=bound++,cv_one=bound++;
    out.reserve(n+32+sites.size()*40);out.insert(out.end(),w,w+5);
    if(!query_cap)emit(out,OpCapability,{CapImageQuery});
    for(size_t i=5;i<n;){const uint32_t len=w[i]>>16;
        if(i==first_function){
            if(new_i32)emit(out,OpTypeInt,{i32,32,1});
            if(new_v2i)emit(out,OpTypeVector,{v2i,i32,2});
            emit(out,OpConstant,{i32,c_zero,0});emit(out,OpConstant,{f32,c_weight,WeightBits});emit(out,OpConstant,{f32,c_one,OneBits});
            emit(out,OpConstantComposite,{v2f,cv_weight,c_weight,c_weight});emit(out,OpConstantComposite,{v2f,cv_one,c_one,c_one});}
        if(sites.count(i)){
            const uint32_t image_type=sampled[result_type[w[i+3]]];
            const uint32_t img=bound++,size=bound++,sizef=bound++,x=bound++,fl=bound++,corner=bound++,coord=bound++;
            emit(out,OpImage,{image_type,img,w[i+3]});
            emit(out,OpImageQuerySizeLod,{v2i,size,img,c_zero});
            emit(out,OpConvertSToF,{v2f,sizef,size});
            emit(out,OpExtInst,{v2f,x,glsl,GlslFma,w[i+4],sizef,cv_weight});   // the argument of the shader's own weights
            emit(out,OpExtInst,{v2f,fl,glsl,GlslFloor,x});                      // first texel of the footprint
            emit(out,OpFAdd,{v2f,corner,fl,cv_one});                            // the corner the four texels share
            emit(out,OpFDiv,{v2f,coord,corner,sizef});
            const size_t at=out.size();out.insert(out.end(),w+i,w+i+len);out[at+4]=coord;++inf.snapped;
            i+=len;continue;}
        out.insert(out.end(),w+i,w+i+len);i+=len;}
    out[3]=bound;return nullptr;
}
