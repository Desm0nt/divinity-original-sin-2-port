// N1 (docs/dos2/nr/NR-NSHADER.md): offline Adreno compiler statistics. Loads the host build of Turnip (the turnip27 Mesa tree compiled for
// x86_64 Linux, kgsl kmd) with fakekgsl.so preloaded, builds one graphics pipeline per manifest line and prints the driver's own
// VK_KHR_pipeline_executable_properties statistics (instructions, nops, registers, max waves, sync stalls, ...) of every stage.
// Nothing runs on a GPU. Descriptor layouts / vertex inputs / colour attachments come from the SPIR-V itself (reflection below).
//
//   tu_stats <libvulkan_freedreno.so> <manifest>      manifest line: <name> <vs.spv> <fs.spv|-> [colour format (VkFormat number) ...]
//   output (stdout): <name>\t<stage>\t<stat>=<value>\t...   one line a stage; "ERR <name> <what>" lines on failure
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst; static VkDevice dev;
#define IPROC(n) auto n = (PFN_##n)gipa(inst, #n)

struct Var { uint32_t id, type, sc; int set = -1, binding = -1, loc = -1; bool block = false, bufferblock = false, builtin = false; };
struct Refl {
    std::map<uint32_t, std::vector<uint32_t>> types;    // id -> instruction words (op first)
    std::map<uint32_t, Var> vars; std::map<uint32_t, std::set<uint32_t>> deco_block; uint32_t exec = 0;
    std::map<uint32_t, int> set, binding, loc; std::set<uint32_t> blocks, bufblocks, builtins;
};
static bool read_file(const std::string &p, std::vector<uint32_t> &w){
    std::ifstream f(p, std::ios::binary); if (!f) return false;
    std::string s((std::istreambuf_iterator<char>(f)), {}); w.resize(s.size() / 4); memcpy(w.data(), s.data(), w.size() * 4); return w.size() > 5;
}
static Refl reflect(const std::vector<uint32_t> &w){
    Refl r;
    for (size_t i = 5; i < w.size();) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff; if (!n) break;
        const uint32_t *a = &w[i + 1];
        if (op == 71) { // OpDecorate
            if (a[1] == 34) r.set[a[0]] = a[2]; else if (a[1] == 33) r.binding[a[0]] = a[2]; else if (a[1] == 30) r.loc[a[0]] = a[2];
            else if (a[1] == 2) r.blocks.insert(a[0]); else if (a[1] == 3) r.bufblocks.insert(a[0]); else if (a[1] == 11) r.builtins.insert(a[0]);
        } else if (op == 72 && a[2] == 11) r.builtins.insert(a[0]);   // OpMemberDecorate BuiltIn -> the struct
        else if (op == 15) r.exec = a[0];
        else if (op >= 19 && op <= 39) r.types[a[0]] = std::vector<uint32_t>(&w[i], &w[i] + n);   // OpType*
        else if (op == 59) { Var v; v.type = a[0]; v.id = a[1]; v.sc = a[2]; r.vars[v.id] = v; }
        i += n;
    }
    return r;
}
static const std::vector<uint32_t> *ty(const Refl &r, uint32_t id){auto it = r.types.find(id); return it == r.types.end() ? nullptr : &it->second;}
static uint32_t pointee(const Refl &r, uint32_t ptr){auto t = ty(r, ptr); return t && ((*t)[0] & 0xffff) == 32 ? (*t)[3] : 0;}
// strip arrays; return array count (0 = runtime array)
static uint32_t strip_arr(const Refl &r, uint32_t t, uint32_t &count){
    count = 1;
    for (;;) {auto d = ty(r, t); if (!d) return t; uint32_t op = (*d)[0] & 0xffff; if (op == 28) {count = 1; t = (*d)[2];} else if (op == 29) {count = 0; t = (*d)[2];} else return t;}
}
static VkDescriptorType desc_type(const Refl &r, const Var &v, bool &ok, uint32_t &count){
    ok = true; uint32_t t = strip_arr(r, pointee(r, v.type), count); auto d = ty(r, t); uint32_t op = d ? ((*d)[0] & 0xffff) : 0;
    if (v.sc == 2) return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;                          // Uniform (Block)
    if (v.sc == 12) return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;                         // StorageBuffer
    if (op == 27) return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;                    // SampledImage
    if (op == 26) return VK_DESCRIPTOR_TYPE_SAMPLER;
    if (op == 25) {uint32_t dim = (*d)[3], sampled = (*d)[7];
        if (dim == 5) return sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;   // Buffer
        if (dim == 6) return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        return sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;}
    ok = false; return VK_DESCRIPTOR_TYPE_MAX_ENUM;
}
static VkFormat attr_format(const Refl &r, uint32_t t){
    auto d = ty(r, t); if (!d) return VK_FORMAT_R32G32B32A32_SFLOAT;
    uint32_t op = (*d)[0] & 0xffff, comp = 1, st = t;
    if (op == 23) {comp = (*d)[3]; st = (*d)[2];}
    auto s = ty(r, st); uint32_t sop = s ? ((*s)[0] & 0xffff) : 22; bool sgn = sop == 21 && (*s)[3];
    static const VkFormat F[4] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT};
    static const VkFormat I[4] = {VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT};
    static const VkFormat U[4] = {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT};
    comp = comp < 1 ? 1 : comp > 4 ? 4 : comp;
    return sop == 22 ? F[comp - 1] : sgn ? I[comp - 1] : U[comp - 1];
}

int main(int argc, char **argv){
    if (argc < 3) {fprintf(stderr, "usage: tu_stats <libvulkan_freedreno.so> <manifest>\n"); return 2;}
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL); if (!lib) {fprintf(stderr, "dlopen: %s\n", dlerror()); return 1;}
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr"); if (!gipa) {fprintf(stderr, "no vk_icdGetInstanceProcAddr\n"); return 1;}
    auto neg = (VkResult (*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion"); if (neg) {uint32_t v = 5; neg(&v);}
    auto ci = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion = VK_API_VERSION_1_3; ai.pApplicationName = "tu_stats";
    VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ic.pApplicationInfo = &ai;
    if (ci(&ic, nullptr, &inst) != VK_SUCCESS) {fprintf(stderr, "vkCreateInstance failed\n"); return 1;}
    IPROC(vkEnumeratePhysicalDevices); IPROC(vkCreateDevice); IPROC(vkGetPhysicalDeviceProperties); IPROC(vkGetDeviceProcAddr);
    uint32_t n = 1; VkPhysicalDevice pd; if (vkEnumeratePhysicalDevices(inst, &n, &pd) != VK_SUCCESS || !n) {fprintf(stderr, "no device\n"); return 1;}
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp); fprintf(stderr, "tu_stats: %s driver 0x%x\n", pp.deviceName, pp.driverVersion);
    float prio = 1; VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; q.queueCount = 1; q.pQueuePriorities = &prio;
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pe{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR}; pe.pipelineExecutableInfo = 1;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; f13.dynamicRendering = 1; f13.pNext = &pe;
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES}; f11.pNext = &f13;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; f2.pNext = &f11;
    auto gf2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(inst, "vkGetPhysicalDeviceFeatures2"); gf2(pd, &f2);   // everything the device has
    pe.pipelineExecutableInfo = 1; f13.dynamicRendering = 1; f13.pNext = &pe;
    const char *ext[] = {"VK_KHR_pipeline_executable_properties"};
    VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dc.pNext = &f2; dc.queueCreateInfoCount = 1; dc.pQueueCreateInfos = &q;
    dc.enabledExtensionCount = 1; dc.ppEnabledExtensionNames = ext;
    if (vkCreateDevice(pd, &dc, nullptr, &dev) != VK_SUCCESS) {fprintf(stderr, "vkCreateDevice failed\n"); return 1;}
#define DPROC(n) auto n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    DPROC(vkCreateShaderModule); DPROC(vkCreateDescriptorSetLayout); DPROC(vkCreatePipelineLayout); DPROC(vkCreateGraphicsPipelines);
    DPROC(vkGetPipelineExecutablePropertiesKHR); DPROC(vkGetPipelineExecutableStatisticsKHR); DPROC(vkDestroyPipeline);
    DPROC(vkDestroyPipelineLayout); DPROC(vkDestroyDescriptorSetLayout); DPROC(vkDestroyShaderModule);

    std::ifstream man(argv[2]); std::string line;
    while (std::getline(man, line)) {
        std::istringstream ss(line); std::string name, vsp, fsp; if (!(ss >> name >> vsp >> fsp) || name[0] == '#') continue;
        std::vector<VkFormat> cfmt; int fv; while (ss >> fv) cfmt.push_back((VkFormat)fv);
        std::vector<uint32_t> vw, fw; if (!read_file(vsp, vw)) {printf("ERR %s read %s\n", name.c_str(), vsp.c_str()); continue;}
        bool has_fs = fsp != "-"; if (has_fs && !read_file(fsp, fw)) {printf("ERR %s read %s\n", name.c_str(), fsp.c_str()); continue;}
        Refl rv = reflect(vw), rf; if (has_fs) rf = reflect(fw);
        // descriptors: union of both stages, set -> binding -> (type, count, stages)
        std::map<int, std::map<int, VkDescriptorSetLayoutBinding>> sets; bool bad = false;
        auto add = [&](const Refl &r, VkShaderStageFlags st){
            for (auto &kv : r.vars) {const Var &v = kv.second; if (v.sc != 0 && v.sc != 2 && v.sc != 12) continue;
                auto si = r.set.find(v.id), bi = r.binding.find(v.id); if (si == r.set.end() || bi == r.binding.end()) continue;
                bool ok; uint32_t cnt; VkDescriptorType t = desc_type(r, v, ok, cnt); if (!ok) {bad = true; continue;}
                auto &b = sets[si->second][bi->second]; if (b.stageFlags) {b.stageFlags |= st; continue;}
                b.binding = bi->second; b.descriptorType = t; b.descriptorCount = cnt ? cnt : 1; b.stageFlags = st;}
        };
        add(rv, VK_SHADER_STAGE_VERTEX_BIT); if (has_fs) add(rf, VK_SHADER_STAGE_FRAGMENT_BIT);
        if (bad) {printf("ERR %s descriptor type\n", name.c_str()); continue;}
        int maxset = sets.empty() ? -1 : sets.rbegin()->first; std::vector<VkDescriptorSetLayout> dsl(maxset + 1);
        for (int s = 0; s <= maxset; s++) {std::vector<VkDescriptorSetLayoutBinding> bs; for (auto &kv : sets[s]) bs.push_back(kv.second);
            VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = bs.size(); li.pBindings = bs.data();
            vkCreateDescriptorSetLayout(dev, &li, nullptr, &dsl[s]);}
        VkPushConstantRange pcr{VK_SHADER_STAGE_ALL_GRAPHICS, 0, 128};
        VkPipelineLayoutCreateInfo plc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; plc.setLayoutCount = dsl.size(); plc.pSetLayouts = dsl.data();
        plc.pushConstantRangeCount = 1; plc.pPushConstantRanges = &pcr;
        VkPipelineLayout pl; vkCreatePipelineLayout(dev, &plc, nullptr, &pl);
        // vertex inputs
        std::vector<VkVertexInputAttributeDescription> at; uint32_t off = 0;
        for (auto &kv : rv.vars) {const Var &v = kv.second; if (v.sc != 1) continue; auto li = rv.loc.find(v.id); if (li == rv.loc.end()) continue;
            uint32_t cnt; uint32_t t = strip_arr(rv, pointee(rv, v.type), cnt);
            at.push_back({(uint32_t)li->second, 0, attr_format(rv, t), off}); off += 16;}
        VkVertexInputBindingDescription vb{0, off ? off : 16, VK_VERTEX_INPUT_RATE_VERTEX};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb; vi.vertexAttributeDescriptionCount = at.size(); vi.pVertexAttributeDescriptions = at.data();
        // colour outputs
        int maxloc = -1; if (has_fs) for (auto &kv : rf.vars) if (kv.second.sc == 3) {auto li = rf.loc.find(kv.first); if (li != rf.loc.end() && li->second > maxloc) maxloc = li->second;}
        if (cfmt.size() == 1 && cfmt[0] == 0) maxloc = -1;   // "0" = a depth-only pass: no colour attachments
        std::vector<VkFormat> cf(maxloc + 1); for (int i = 0; i <= maxloc; i++) cf[i] = i < (int)cfmt.size() ? cfmt[i] : (cfmt.empty() ? VK_FORMAT_R8G8B8A8_UNORM : cfmt.back());
        std::vector<VkPipelineColorBlendAttachmentState> cb(cf.size()); for (auto &c : cb) {memset(&c, 0, sizeof c); c.colorWriteMask = 0xf;}
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; ri.colorAttachmentCount = cf.size(); ri.pColorAttachmentFormats = cf.data();
        ri.depthAttachmentFormat = VK_FORMAT_D24_UNORM_S8_UINT; ri.stencilAttachmentFormat = VK_FORMAT_D24_UNORM_S8_UINT;
        VkShaderModule mv, mf = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo smc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smc.codeSize = vw.size() * 4; smc.pCode = vw.data(); vkCreateShaderModule(dev, &smc, nullptr, &mv);
        if (has_fs) {smc.codeSize = fw.size() * 4; smc.pCode = fw.data(); vkCreateShaderModule(dev, &smc, nullptr, &mf);}
        VkPipelineShaderStageCreateInfo st[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = mv; st[0].pName = "main"; st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = mf; st[1].pName = "main";
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = 1; vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; ds.depthTestEnable = 1; ds.depthWriteEnable = 1; ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cbs.attachmentCount = cb.size(); cbs.pAttachments = cb.data();
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; gp.pNext = &ri; gp.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
        gp.stageCount = has_fs ? 2 : 1; gp.pStages = st; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp;
        gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds; gp.pColorBlendState = &cbs; gp.pDynamicState = &dy; gp.layout = pl;
        VkPipeline pipe; VkResult res = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe);
        if (res != VK_SUCCESS) {printf("ERR %s pipeline %d\n", name.c_str(), res); fflush(stdout);}
        else {
            VkPipelineInfoKHR pi{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR}; pi.pipeline = pipe; uint32_t ne = 0;
            vkGetPipelineExecutablePropertiesKHR(dev, &pi, &ne, nullptr);
            std::vector<VkPipelineExecutablePropertiesKHR> ep(ne, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR}); vkGetPipelineExecutablePropertiesKHR(dev, &pi, &ne, ep.data());
            for (uint32_t e = 0; e < ne; e++) {
                VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR}; ei.pipeline = pipe; ei.executableIndex = e; uint32_t ns = 0;
                vkGetPipelineExecutableStatisticsKHR(dev, &ei, &ns, nullptr);
                std::vector<VkPipelineExecutableStatisticKHR> sv(ns, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR}); vkGetPipelineExecutableStatisticsKHR(dev, &ei, &ns, sv.data());
                printf("%s\t%s\tSubgroup_size=%u", name.c_str(), ep[e].name, ep[e].subgroupSize);
                for (auto &s : sv) {std::string k = s.name; for (auto &c : k) if (c == ' ' || c == '\t') c = '_';
                    if (s.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR) printf("\t%s=%llu", k.c_str(), (unsigned long long)s.value.u64);
                    else if (s.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR) printf("\t%s=%lld", k.c_str(), (long long)s.value.i64);
                    else if (s.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR) printf("\t%s=%g", k.c_str(), s.value.f64);
                    else printf("\t%s=%d", k.c_str(), (int)s.value.b32);}
                printf("\n");
            }
            fflush(stdout); vkDestroyPipeline(dev, pipe, nullptr);
        }
        vkDestroyShaderModule(dev, mv, nullptr); if (mf) vkDestroyShaderModule(dev, mf, nullptr);
        vkDestroyPipelineLayout(dev, pl, nullptr); for (auto l : dsl) vkDestroyDescriptorSetLayout(dev, l, nullptr);
    }
    return 0;
}
