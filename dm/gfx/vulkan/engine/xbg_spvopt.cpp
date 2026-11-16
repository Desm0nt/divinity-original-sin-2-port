// [DOS2] DM_GFX_SPV_OPT: on-device SPIR-V optimizer + persistent cache (design in xbg_spvopt.h). Built only with DM_SPVOPT=1
// (dm/android/CMakeLists.txt links SPIRV-Tools for DM_GAME_DOS2).
#include "xbg_spvopt.h"
#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/optimizer.hpp>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <dirent.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <ctime>
extern "C" void xbg_logf(const char *format,...);   // xbg_game_internal.h (not included: this file only needs the log)

namespace {
// Changing the passes or the library changes this seed, so old cache files are simply never named again (delete the folder).
constexpr const char *seed="dm-spvopt-v1 SPIRV-Tools v2022.4 RegisterPerformancePasses";
constexpr size_t max_queue_bytes=64u<<20,max_queue_jobs=8192;
uint64_t now_ns(){timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000000000ull+uint64_t(t.tv_nsec);}
std::string name_of(const uint32_t *w,size_t n){
    uint64_t h=1469598103934665603ull;for(const char *s=seed;*s;s++){h^=uint8_t(*s);h*=1099511628211ull;}
    const uint8_t *b=reinterpret_cast<const uint8_t*>(w);for(size_t i=0;i<n*4;i++){h^=b[i];h*=1099511628211ull;}
    char t[48];snprintf(t,sizeof t,"%016llx-%zu",(unsigned long long)h,n);return t;}
spv_target_env env_of(const uint32_t *w){   // Vulkan 1.1 (the port's device API); newer SPIR-V versions get the env that allows them
    const uint32_t v=w[1];if(v<=0x10300)return SPV_ENV_VULKAN_1_1;if(v==0x10400)return SPV_ENV_VULKAN_1_1_SPIRV_1_4;if(v==0x10500)return SPV_ENV_VULKAN_1_2;return SPV_ENV_VULKAN_1_3;}
struct Job{std::string name,what;std::vector<uint32_t> words;};
struct State{
    int mode=0;std::string dir;std::mutex mu;std::condition_variable cv;std::deque<Job> queue;size_t queued_bytes=0;bool thread_started=false;
    std::unordered_set<std::string> cached,bad,pending;
    uint64_t hits=0,misses=0,queued=0,dropped=0,done=0,failed=0,rejected=0,in_words=0,out_words=0,opt_ns=0,hit_in=0,hit_out=0;
};
State *st=nullptr;   // never freed: the worker thread is detached and may outlive static destructors at exit
bool read_words(const std::string &path,std::vector<uint32_t> &out){
    FILE *f=fopen(path.c_str(),"rb");if(!f)return false;fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);bool ok=n>=20 && n%4==0 && n<=(64l<<20);
    if(ok){out.resize(size_t(n)/4);ok=fread(out.data(),1,size_t(n),f)==size_t(n) && out[0]==0x07230203u;}fclose(f);return ok;}
void add_bad(const std::string &name){if(FILE *f=fopen((st->dir+"/bad.txt").c_str(),"ab")){fprintf(f,"%s\n",name.c_str());fclose(f);}}
void summary_locked(const char *when){
    xbg_logf("NVN spvopt %s: mode=%d cache=%zu bad=%zu hits=%llu (words %llu->%llu) misses=%llu queued=%llu dropped=%llu optimized=%llu (words %llu->%llu, %.2fx, %.0f ms) failed=%llu driver-rejected=%llu pending=%zu",
        when,st->mode,st->cached.size(),st->bad.size(),(unsigned long long)st->hits,(unsigned long long)st->hit_in,(unsigned long long)st->hit_out,(unsigned long long)st->misses,
        (unsigned long long)st->queued,(unsigned long long)st->dropped,(unsigned long long)st->done,(unsigned long long)st->in_words,(unsigned long long)st->out_words,
        st->in_words?double(st->out_words)/double(st->in_words):0.0,st->opt_ns/1e6,(unsigned long long)st->failed,(unsigned long long)st->rejected,st->queue.size());}
void worker(){
    setpriority(PRIO_PROCESS,gettid(),19);   // lowest: shader loads, warm-up and the game threads come first
    for(;;){
        Job j;{std::unique_lock<std::mutex> l(st->mu);st->cv.wait(l,[]{return !st->queue.empty();});j=std::move(st->queue.front());st->queue.pop_front();st->queued_bytes-=j.words.size()*4;}
        const uint64_t t0=now_ns();const spv_target_env env=env_of(j.words.data());std::string err;
        auto consumer=[&err](spv_message_level_t level,const char*,const spv_position_t &pos,const char *msg){
            if(level<=SPV_MSG_ERROR && err.empty()){char b[32];snprintf(b,sizeof b," (word %zu)",pos.index);err=std::string(msg?msg:"?")+b;}};
        std::vector<uint32_t> out;bool ok;const char *stage="optimize";
        {spvtools::Optimizer opt(env);opt.SetMessageConsumer(consumer);opt.RegisterPerformancePasses();
            spvtools::OptimizerOptions oo;oo.set_run_validator(true);   // the input is validated first: an invalid input is "bad", not optimized
            ok=opt.Run(j.words.data(),j.words.size(),&out,oo);}
        if(ok){stage="validate";spvtools::SpirvTools tools(env);tools.SetMessageConsumer(consumer);spvtools::ValidatorOptions vo;ok=tools.Validate(out.data(),out.size(),vo);}
        if(ok && (out.size()<5 || out[0]!=0x07230203u)){ok=false;err="empty result";}
        bool wrote=false;
        if(ok){const std::string path=st->dir+"/"+j.name+".spv",tmp=path+".tmp";
            if(FILE *f=fopen(tmp.c_str(),"wb")){wrote=fwrite(out.data(),4,out.size(),f)==out.size();wrote=fclose(f)==0 && wrote;if(wrote)wrote=rename(tmp.c_str(),path.c_str())==0;if(!wrote)remove(tmp.c_str());}
            if(!wrote){stage="write";err="cache file not written";}}
        const uint64_t dt=now_ns()-t0;
        std::lock_guard<std::mutex> l(st->mu);st->pending.erase(j.name);st->opt_ns+=dt;
        if(ok && wrote){st->cached.insert(j.name);++st->done;st->in_words+=j.words.size();st->out_words+=out.size();
            if(st->mode>=2)xbg_logf("NVN spvopt %s %s: words %zu -> %zu (%.2fx) valid=1 ms=%.1f env=%d",j.what.c_str(),j.name.c_str(),j.words.size(),out.size(),double(out.size())/double(j.words.size()),dt/1e6,int(env));}
        else{++st->failed;if(ok==false){st->bad.insert(j.name);add_bad(j.name);}   // a write failure is retried next launch
            if(st->mode>=2 || st->failed<=16)xbg_logf("NVN spvopt %s %s: %s failed (%s): the original module is kept%s",j.what.c_str(),j.name.c_str(),stage,err.empty()?"?":err.c_str(),ok?"":" (bad.txt)");}
        if(((st->done+st->failed)&255)==0)summary_locked("progress");
    }
}
}  // namespace

void xbg_spvopt_init(const char *dir,int mode){
    if(st || mode<=0 || !dir || !*dir)return;
    st=new State;st->mode=mode;st->dir=dir;mkdir(dir,0775);
    if(DIR *d=opendir(dir)){while(dirent *e=readdir(d)){std::string n=e->d_name;
            if(n.size()>4 && n.compare(n.size()-4,4,".spv")==0)st->cached.insert(n.substr(0,n.size()-4));
            else if(n.size()>4 && n.compare(n.size()-4,4,".tmp")==0)remove((st->dir+"/"+n).c_str());}closedir(d);}
    else{xbg_logf("NVN spvopt DM_GFX_SPV_OPT=%d: cache folder %s cannot be opened: off",mode,dir);st->mode=0;return;}
    if(FILE *f=fopen((st->dir+"/bad.txt").c_str(),"rb")){char line[96];while(fgets(line,sizeof line,f)){line[strcspn(line,"\r\n")]=0;if(*line)st->bad.insert(line);}fclose(f);}
    xbg_logf("NVN spvopt DM_GFX_SPV_OPT=%d: cache %s has %zu optimized module(s), %zu bad (SPIRV-Tools v2022.4 -O, background thread, nice 19)",mode,dir,st->cached.size(),st->bad.size());
}
int xbg_spvopt_mode(){return st?st->mode:0;}
bool xbg_spvopt_lookup(const uint32_t *words,size_t n,std::vector<uint32_t> &out,const char *what){
    if(!st || !st->mode || !words || n<5 || words[0]!=0x07230203u)return false;
    const std::string name=name_of(words,n);bool have;
    {std::lock_guard<std::mutex> l(st->mu);if(st->bad.count(name))return false;have=st->cached.count(name)!=0;}
    if(have){if(read_words(st->dir+"/"+name+".spv",out)){std::lock_guard<std::mutex> l(st->mu);++st->hits;st->hit_in+=n;st->hit_out+=out.size();
            if(st->hits==1)xbg_logf("NVN spvopt first cache hit %s %s: words %zu -> %zu",what?what:"?",name.c_str(),n,out.size());return true;}
        std::lock_guard<std::mutex> l(st->mu);st->cached.erase(name);}   // unreadable / truncated: optimize again
    std::lock_guard<std::mutex> l(st->mu);++st->misses;
    if(st->pending.count(name))return false;
    if(st->queue.size()>=max_queue_jobs || st->queued_bytes+n*4>max_queue_bytes){++st->dropped;return false;}   // the next launch queues it again
    Job j;j.name=name;j.what=what?what:"?";j.words.assign(words,words+n);st->queued_bytes+=n*4;st->pending.insert(name);st->queue.push_back(std::move(j));++st->queued;
    if(!st->thread_started){st->thread_started=true;std::thread(worker).detach();}
    st->cv.notify_one();return false;
}
void xbg_spvopt_reject(const uint32_t *words,size_t n,const char *what){
    if(!st)return;const std::string name=name_of(words,n);std::lock_guard<std::mutex> l(st->mu);++st->rejected;st->cached.erase(name);
    if(st->bad.insert(name).second){add_bad(name);remove((st->dir+"/"+name+".spv").c_str());}
    xbg_logf("NVN spvopt %s %s: the driver refused the optimized module: the original is used, marked bad",what?what:"?",name.c_str());
}
void xbg_spvopt_summary(const char *when){if(!st || !st->mode)return;std::lock_guard<std::mutex> l(st->mu);summary_locked(when);}
