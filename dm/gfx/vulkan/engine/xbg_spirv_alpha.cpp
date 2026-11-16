// NVN alpha test -> fragment shader kill (xbg_spirv_alpha.h). Pure SPIR-V word rewrite, no Vulkan: host-tested by
// tests/spirv_alpha_test.cpp (synthetic modules; with arguments every fragment shader of a bundle directory for spirv-val).
#include "xbg_spirv_alpha.h"
#include <cstring>
#include <map>
#include <set>

namespace {
enum : uint32_t {
    OpEntryPoint=15,OpExecutionMode=16,OpCapability=17,OpTypeBool=20,OpTypeFloat=22,OpTypeVector=23,
    OpTypePointer=32,OpConstant=43,OpFunction=54,OpFunctionEnd=56,OpFunctionCall=57,OpVariable=59,OpLoad=61,OpStore=62,
    OpCopyMemory=63,OpCopyMemorySized=64,OpAccessChain=65,OpInBoundsAccessChain=66,OpPtrAccessChain=67,
    OpInBoundsPtrAccessChain=70,OpDecorate=71,OpCompositeExtract=81,OpSelectionMerge=247,OpLabel=248,OpBranch=249,
    OpBranchConditional=250,OpKill=252,OpReturn=253,OpTerminateInvocation=4416,OpDemoteToHelperInvocation=5380,
    CapDemote=5379,ModelFragment=4,ModeEarlyFragmentTests=9,DecLocation=30,DecComponent=31,DecIndex=32,StorageOutput=3
};
// NVN alpha func -> ordered float comparison (Ryujinx: FOrd* for every op, NotEqual included)
uint32_t compare_op(uint32_t func){
    switch(func){
    case 2:return 184;   // LESS     OpFOrdLessThan
    case 3:return 180;   // EQUAL    OpFOrdEqual
    case 4:return 188;   // LEQUAL   OpFOrdLessThanEqual
    case 5:return 186;   // GREATER  OpFOrdGreaterThan
    case 6:return 182;   // NOTEQUAL OpFOrdNotEqual
    case 7:return 190;   // GEQUAL   OpFOrdGreaterThanEqual
    default:return 0;}
}
void emit(std::vector<uint32_t> &o,uint32_t op,std::initializer_list<uint32_t> a){o.push_back((uint32_t(a.size()+1)<<16)|op);o.insert(o.end(),a.begin(),a.end());}
}

const char *xbg_spirv_alpha_test(const uint32_t *w,size_t n,uint32_t func,float ref,std::vector<uint32_t> &out,xbg_spirv_alpha_info *info){
    out.clear();xbg_spirv_alpha_info local;xbg_spirv_alpha_info &inf=info?*info:local;inf=xbg_spirv_alpha_info{};
    if(func<1 || func>7)return "func is ALWAYS or not an NVN alpha func";
    if(!w || n<5 || w[0]!=0x07230203)return "not a SPIR-V module";
    const bool never=func==1;const uint32_t cmp=compare_op(func);
    uint32_t entry=0,entries=0,bool_type=0;bool demote_cap=false;
    std::map<uint32_t,uint32_t> location,index,component,float_width,pointer_sc,pointer_to,constant;
    std::map<uint32_t,std::pair<uint32_t,uint32_t>> vector;   // id -> (component type, count)
    std::map<uint32_t,uint32_t> output_var;                   // Output variable -> pointer type
    std::set<size_t> drop;                                    // EarlyFragmentTests of the entry point
    size_t first_function=0,entry_begin=0,entry_end=0;
    // pass 1: module structure
    for(size_t i=5;i<n;){const uint32_t len=w[i]>>16,op=w[i]&65535;if(!len || i+len>n)return "malformed instruction stream";
        switch(op){
        case OpCapability:if(len>=2 && w[i+1]==CapDemote)demote_cap=true;break;
        case OpEntryPoint:if(len>=3 && w[i+1]==ModelFragment){entry=w[i+2];++entries;}else if(len>=3)return "module has a non-fragment entry point";break;
        case OpExecutionMode:if(len>=3 && w[i+1]==entry && w[i+2]==ModeEarlyFragmentTests)drop.insert(i);break;
        case OpDecorate:if(len>=4){if(w[i+2]==DecLocation)location[w[i+1]]=w[i+3];else if(w[i+2]==DecIndex)index[w[i+1]]=w[i+3];else if(w[i+2]==DecComponent)component[w[i+1]]=w[i+3];}break;
        case OpTypeBool:if(len>=2 && !bool_type)bool_type=w[i+1];break;
        case OpTypeFloat:if(len>=3)float_width[w[i+1]]=w[i+2];break;
        case OpTypeVector:if(len>=4)vector[w[i+1]]={w[i+2],w[i+3]};break;
        case OpTypePointer:if(len>=4){pointer_sc[w[i+1]]=w[i+2];pointer_to[w[i+1]]=w[i+3];}break;
        case OpConstant:if(len==4)constant[w[i+2]]=w[i+3];break;
        case OpVariable:if(len>=4 && w[i+3]==StorageOutput)output_var[w[i+2]]=w[i+1];break;
        case OpFunction:if(len>=3){if(!first_function)first_function=i;if(w[i+2]==entry)entry_begin=i;}break;
        case OpFunctionEnd:if(entry_begin && !entry_end)entry_end=i;break;
        default:break;}
        i+=len;}
    if(entries!=1 || !entry)return "not a single fragment entry point";
    if(!entry_begin || !entry_end || !first_function)return "entry point function not found";
    // colour target 0: a float32 vec4 Output at Location 0, Index 0 (dual-source blending's Index 1 is not tested), no Component
    uint32_t var=0,vec4=0,f32=0;
    if(!never){
        for(auto &v:output_var){auto l=location.find(v.first);if(l==location.end() || l->second!=0)continue;
            auto x=index.find(v.first);if(x!=index.end() && x->second!=0)continue;
            if(component.count(v.first))return "Location 0 output has a Component decoration";
            auto pt=pointer_to.find(v.second);if(pt==pointer_to.end())return "Location 0 output pointer type unknown";
            auto vt=vector.find(pt->second);if(vt==vector.end() || vt->second.second!=4)return "Location 0 output is not a vec4";
            auto ft=float_width.find(vt->second.first);if(ft==float_width.end() || ft->second!=32)return "Location 0 output is not float32 (integer colour target)";
            var=v.first;vec4=pt->second;f32=vt->second.first;break;}
        if(!var)return "no colour output at Location 0";
        // is .w written at all? (Ryujinx tests only when the shader writes target 0 alpha.) Whole stores, copies, calls taking the
        // pointer and access chains to component 3 or a dynamic component count as writes.
        bool written=false;
        for(size_t i=first_function;i<n && !written;){const uint32_t len=w[i]>>16,op=w[i]&65535;
            if(op==OpStore && len>=3 && w[i+1]==var)written=true;
            else if((op==OpCopyMemory || op==OpCopyMemorySized) && len>=3 && w[i+1]==var)written=true;
            else if(op==OpFunctionCall && len>=4){for(uint32_t k=4;k<len;k++)if(w[i+k]==var)written=true;}
            else if((op==OpAccessChain || op==OpInBoundsAccessChain) && len>=5 && w[i+3]==var){auto c=constant.find(w[i+4]);if(c==constant.end() || c->second==3)written=true;}
            else if((op==OpPtrAccessChain || op==OpInBoundsPtrAccessChain) && len>=4 && w[i+3]==var)written=true;
            i+=len;}
        if(!written)return "the shader never writes Location 0 .w";
    }
    inf.demote=demote_cap;inf.early_removed=!drop.empty();
    const bool terminate=!demote_cap && w[1]>=0x00010600;   // OpKill is deprecated from SPIR-V 1.6: OpTerminateInvocation there
    uint32_t bound=w[3];
    uint32_t new_bool=0,ref_const=0;
    if(!never){if(!bool_type)new_bool=bool_type=bound++;ref_const=bound++;}
    out.reserve(n+64);out.insert(out.end(),w,w+5);
    for(size_t i=5;i<n;){const uint32_t len=w[i]>>16,op=w[i]&65535;
        if(drop.count(i)){i+=len;continue;}
        if(i==first_function && !never){
            if(new_bool)emit(out,OpTypeBool,{new_bool});
            uint32_t bits;memcpy(&bits,&ref,4);emit(out,OpConstant,{f32,ref_const,bits});}
        if(op==OpReturn && i>entry_begin && i<entry_end){
            ++inf.returns;
            if(never){
                if(demote_cap){emit(out,OpDemoteToHelperInvocation,{});emit(out,OpReturn,{});}
                else emit(out,terminate?OpTerminateInvocation:OpKill,{});
            }else{
                const uint32_t v=bound++,a=bound++,c=bound++,k=bound++,m=bound++;
                emit(out,OpLoad,{vec4,v,var});emit(out,OpCompositeExtract,{f32,a,v,3});emit(out,cmp,{bool_type,c,a,ref_const});
                emit(out,OpSelectionMerge,{m,0});emit(out,OpBranchConditional,{c,m,k});
                emit(out,OpLabel,{k});
                if(demote_cap){emit(out,OpDemoteToHelperInvocation,{});emit(out,OpBranch,{m});}
                else emit(out,terminate?OpTerminateInvocation:OpKill,{});
                emit(out,OpLabel,{m});emit(out,OpReturn,{});}
            i+=len;continue;}
        out.insert(out.end(),w+i,w+i+len);i+=len;}
    if(!inf.returns){out.clear();return "no OpReturn in the entry point";}
    out[3]=bound;return nullptr;
}
