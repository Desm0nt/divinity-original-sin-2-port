"""[NR-19] VK-UBO-MEMO + VK-DRIVER-KEY: run the production descriptor-memo key code (xbg_game.cpp descriptors()) in a CPU model.

The two cases of the memo key loop that port-common/desc-memo-keys fixes are cut out of xbg_game.cpp as they are (the
UNIFORM / STORAGE case, the DRIVER0 / DRIVER2 case and the UBO_DYN macro), compiled for wasm32 with the NDK clang and run under
node (the texel_memo_lifetime_test.py runner; no Vulkan, no device). The model: the game writes its constants into guest memory, a
descriptor set built for a draw bakes a static uniform buffer's contents (DM_GFX_UBO_DYNAMIC=0, the PowerVR policy), a dynamic one
is copied again on every memo hit (dyn_ubos), and a memo hit reuses the set of an equal key.

Checks (production must give 0):
  10+m  mode m: the game rewrites the same address between two draws and the second draw sees the old contents (D3 issue #22)
  20    mode 0: a set with a static guest UBO was memoized, or the ubosnap counter (desc_memo_why[5]) did not count both draws
  30+m  mode 1 / 2: the memo was lost for a dynamic UBO (a cost the fix must not add)
  40/41 mode 0: an unbound UBO (address 0) / a size-0 range lost its memo (e6e9611 refinement)
  50    mode 0: a storage buffer lost its memo
  60-63 mode 0: the non-dynamic driver block key does not follow buf_gen (DRIVER0, SSBO table) / tex_gen (DRIVER2, handles)
  64    mode 1: a dynamic driver block is keyed by a counter
Negative controls: the same code without the VK-UBO-MEMO line must fail with 10, the swapped driver line of the shared core
(before d2ffc82) with 60.

  python ubo_memo_snapshot_test.py --compiler <NDK clang++.exe> [--node node]
"""
import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from texture_cache_regression import balanced

PREFIX = r'''
typedef __SIZE_TYPE__ size_t;typedef unsigned long long uint64_t;typedef unsigned uint32_t;typedef unsigned char uint8_t;
namespace std{template<class T> T min(T a,T b){return b<a?b:a;}}
enum {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER=6,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER=7};
enum {XBG_SOURCE_UNIFORM=1,XBG_SOURCE_STORAGE,XBG_SOURCE_SUPPORT,XBG_SOURCE_C1,XBG_SOURCE_DRIVER0,XBG_SOURCE_DRIVER2,XBG_SOURCE_TEXTURE,XBG_SOURCE_IMAGE};
template<class T,int N=64> struct Vec{T v[N];int n=0;void push_back(T x){if(n<N)v[n++]=x;}void clear(){n=0;}};
struct DynUbo{uint64_t order,address;size_t copy;};
struct Binding{int type;unsigned source,binding,bytes,slot,count,flags;};
// nr-all: the UBO_DYN macro of the merged tree also asks NR-08 (mode 3: nrdesc::split_has) and NR-08b (nrubd::ro + the binding's
// scan flags); both are off here (DM_NR_DESC / DM_NR_UBODYN 0), as in the production default.
namespace nrdesc{inline bool split_has(const Binding*){return false;}}
namespace nrubd{bool ro=false;}
constexpr unsigned XBG_BIND_UNREAD=1u<<29,XBG_BIND_SCANNED=1u<<28;
struct Range{uint64_t address,size;};
struct State{Range ub[6][16],sb[6][16];} state;
uint64_t tex_gen[6],buf_gen[6];
'''

MODEL = r'''
struct KeyOut{bool ok;Vec<uint64_t> key;Vec<DynUbo,8> dyn;unsigned why[6];};
void keyed(const Binding &b,unsigned dmode,unsigned ns,KeyOut &o){
    o.ok=true;o.key.clear();o.dyn.clear();for(auto &w:o.why)w=0;
    bool ok=true;const bool tags=true;Vec<uint8_t> ktag;auto &key=o.key;auto &dyn_ubos=o.dyn;unsigned (&desc_memo_why)[6]=o.why;
    for(unsigned element=0;element<b.count;element++){unsigned slot=b.slot+element;
        switch(b.source){
@@CASES@@
        default:ok=false;}
        if(!ok)break;}
    (void)ktag;o.ok=ok;}
bool same(const Vec<uint64_t> &a,const Vec<uint64_t> &b){if(a.n!=b.n)return false;for(int i=0;i<a.n;i++)if(a.v[i]!=b.v[i])return false;return true;}
// Guest memory (word per address) and the descriptor memo of one shader set.
uint64_t mem[256];
struct Entry{Vec<uint64_t> key;bool dyn;uint64_t snap;};
struct Memo{Entry e[8];int n=0;int hits=0;unsigned ubosnap=0;void clear(){n=0;hits=0;ubosnap=0;}} memo;
// One draw: the contents the GPU reads from the binding (static: baked at build; dynamic: copied again from the current address).
uint64_t draw(const Binding &b,unsigned dmode,unsigned ns){KeyOut o;keyed(b,dmode,ns,o);memo.ubosnap+=o.why[5];
    const Range r=b.source==XBG_SOURCE_UNIFORM?state.ub[ns][b.slot]:state.sb[ns][b.slot];
    const bool dyn=o.dyn.n>0;
    if(o.ok)for(int i=0;i<memo.n;i++)if(same(memo.e[i].key,o.key)){++memo.hits;return memo.e[i].dyn?mem[o.dyn.v[0].address]:memo.e[i].snap;}
    const uint64_t seen=mem[r.address];   // build: snapshot / dynamic copy now
    if(o.ok && memo.n<8){Entry &e=memo.e[memo.n++];e.key=o.key;e.dyn=dyn;e.snap=seen;}
    return seen;}
const unsigned NS=4;
int ubo_case(unsigned mode){
    memo.clear();Binding b{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,XBG_SOURCE_UNIFORM,0,256,3,1};
    state.ub[NS][3]={0x40,256};mem[0x40]=111;
    if(draw(b,mode,NS)!=111)return 10+int(mode);
    mem[0x40]=222;   // skinning matrices / per-object constants rewritten at the same address
    if(draw(b,mode,NS)!=222)return 10+int(mode);
    if(mode==0 && (memo.hits!=0 || memo.ubosnap!=2))return 20;
    if(mode!=0 && memo.hits!=1)return 30+int(mode);
    return 0;}
int keep_cases(){
    Binding b{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,XBG_SOURCE_UNIFORM,0,256,5,1};
    memo.clear();state.ub[NS][5]={0,0};draw(b,0,NS);draw(b,0,NS);if(memo.hits!=1 || memo.ubosnap)return 40;
    memo.clear();state.ub[NS][5]={0x50,0};draw(b,0,NS);draw(b,0,NS);if(memo.hits!=1 || memo.ubosnap)return 41;
    Binding s{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,XBG_SOURCE_STORAGE,1,0,2,1};
    memo.clear();state.sb[NS][2]={0x60,64};draw(s,0,NS);draw(s,0,NS);if(memo.hits!=1 || memo.ubosnap)return 50;
    return 0;}
int driver_cases(){
    const unsigned srcs[2]={XBG_SOURCE_DRIVER0,XBG_SOURCE_DRIVER2};
    for(unsigned src:srcs){
        Binding d{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,src,2,0x1000,0,1};const bool d0=src==XBG_SOURCE_DRIVER0;
        KeyOut a,c;keyed(d,0,NS,a);if(d0)++buf_gen[NS];else ++tex_gen[NS];keyed(d,0,NS,c);
        if(!a.ok || !c.ok || same(a.key,c.key))return d0?60:62;   // the generation the block is built from moved: new key
        keyed(d,0,NS,a);if(d0)++tex_gen[NS];else ++buf_gen[NS];keyed(d,0,NS,c);
        if(!same(a.key,c.key))return d0?61:63;                    // the other one: no needless miss
        keyed(d,1,NS,a);++tex_gen[NS];++buf_gen[NS];keyed(d,1,NS,c);
        if(!same(a.key,c.key) || a.key.n!=1 || a.key.v[0]!=uint64_t(0xD0D0+src))return 64;}
    return 0;}
extern "C" int run_tests(){
    for(unsigned m=0;m<3;m++)if(int r=ubo_case(m))return r;
    if(int r=keep_cases())return r;
    return driver_cases();}
'''

# The shared core's DRIVER line before d2ffc82 (port-common desc-memo-keys/patches/drv-key.patch, '-' side).
OLD_DRIVER = ('case XBG_SOURCE_DRIVER0:case XBG_SOURCE_DRIVER2:key.push_back(UBO_DYN(dmode,b)?uint64_t(0xD0D0+b.source):'
              '(b.source==XBG_SOURCE_DRIVER0?tex_gen[ns]:buf_gen[ns]));if(tags)ktag.push_back(7);break;')


def extract(source):
    macro = re.search(r'^#define UBO_DYN\(mode,b\).*$', source, re.M)
    assert macro, 'UBO_DYN macro not found'
    start = source.index('case XBG_SOURCE_UNIFORM:case XBG_SOURCE_STORAGE:{')
    uni = balanced(source, start)
    assert 'key.push_back(r.address)' in uni and 'dyn_ubos.push_back(' in uni, 'the UNIFORM / STORAGE memo key case changed shape'
    drv = re.search(r'^\s*(case XBG_SOURCE_DRIVER0:case XBG_SOURCE_DRIVER2:key\.push_back\(.*)$', source, re.M)
    assert drv, 'the DRIVER0 / DRIVER2 memo key case not found'
    return macro.group(0), uni, drv.group(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--node', default='node')
    args = parser.parse_args()
    source = (pathlib.Path(__file__).resolve().parent.parent / 'xbg_game.cpp').read_text(encoding='utf-8')
    macro, uni, drv = extract(source)
    rule = re.compile(r'\n[ \t]*else if\(!UBO_DYN\(dmode,b\) && b\.source==XBG_SOURCE_UNIFORM && r\.address && r\.size\)\{ok=false;\+\+desc_memo_why\[5\];break;\}[^\n]*(\n[ \t]*//[^\n]*)*')
    assert len(rule.findall(uni)) == 1, 'VK-UBO-MEMO: the static-UBO rule (ok=false, desc_memo_why[5]) is not in the UNIFORM case once'
    old_uni = rule.sub('', uni)
    variants = [('production', uni, drv, 0), ('without_VK-UBO-MEMO', old_uni, drv, 10), ('swapped_driver_key', uni, OLD_DRIVER, 60)]
    with tempfile.TemporaryDirectory(prefix='xbg-ubo-memo-') as temp:
        for name, u, d, expected in variants:
            cpp, wasm = pathlib.Path(temp) / (name + '.cpp'), pathlib.Path(temp) / (name + '.wasm')
            cpp.write_text(PREFIX + macro + '\n' + MODEL.replace('@@CASES@@', u + '\n' + d + '\n'), encoding='utf-8')
            subprocess.run([args.compiler, '--target=wasm32', '-std=c++17', '-ffreestanding', '-fno-exceptions', '-fno-rtti',
                            '-nostdlib', '-Wl,--no-entry', '-Wl,--export=run_tests', '-O1', str(cpp), '-o', str(wasm)], check=True)
            js = ('const fs=require("fs");WebAssembly.instantiate(fs.readFileSync(process.argv[1]))'
                  '.then(({instance})=>{let actual=instance.exports.run_tests();'
                  'let expected=Number(process.argv[2]);if(actual!==expected)'
                  'throw Error(process.argv[3]+": unexpected CPU result "+actual+", expected "+expected);'
                  'console.log(process.argv[3]+": "+actual);});')
            subprocess.run([args.node, '-e', js, str(wasm), str(expected), name], check=True)
    print('PASS: NR-19 VK-UBO-MEMO / VK-DRIVER-KEY in the production memo key code: mode 0 never reuses a static guest UBO snapshot '
          '(rewrite at the same address seen, ubosnap counted per draw), modes 1 / 2 keep the memo and see the rewrite, unbound / size-0 '
          'UBO and storage keep the memo, driver block keys follow buf_gen (DRIVER0) / tex_gen (DRIVER2); both negative controls fail as '
          'expected. No Vulkan executed.')


if __name__ == '__main__':
    main()
