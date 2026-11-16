// [dos2-nb] NB-18b-6: host test of the render-area / depth-stencil-detach logic (dm/gfx/nb/nb_uiarea.h, used by xbg_nb_uiarea.inc under
// DM_NB_UIAREA=1 / 2). Shapes from P2 (NB-18a findings): Iggy filter passes 2400x1080 with one small draw, the UI pass with 95 draws.
// Build, from this folder:  clang++ -std=c++17 -Wall -D_CRT_SECURE_NO_WARNINGS nb_uiarea_test.cpp -o nb_uiarea_test   (or g++)
#include "../../../nb/nb_uiarea.h"
#include <stdio.h>

static int failures;
#define CHECK(expression) do { if(!(expression)){printf("FAIL line %d: %s\n",__LINE__,#expression);++failures;} } while(0)
static bool eq(nb_ua_rect r,int x0,int y0,int x1,int y1){return r.x0==x0 && r.y0==y0 && r.x1==x1 && r.y1==y1;}

int main(){
    const uint32_t W=2400,H=1080;
    // ---- draw rect: viewport (+ margin) ∩ scissor ∩ framebuffer
    CHECK(eq(nb_ua_draw_rect(0,0,2400,1080,0,0,2400,1080,W,H,4),0,0,2400,1080));               // full: clamped to the target
    CHECK(eq(nb_ua_draw_rect(0,0,2400,1080,100,200,300,50,W,H,4),100,200,400,250));            // scissor inside
    CHECK(eq(nb_ua_draw_rect(100.5f,200,50,40,0,0,2400,1080,W,H,4),96,196,155,244));           // viewport + margin, floor / ceil
    CHECK(eq(nb_ua_draw_rect(100,240,50,-40,0,0,2400,1080,W,H,0),100,200,150,240));            // negative height: rows y+h .. y
    CHECK(nb_ua_empty(nb_ua_draw_rect(0,0,100,100,500,500,10,10,W,H,4)));                      // disjoint: touches nothing
    CHECK(nb_ua_empty(nb_ua_draw_rect(0,0,0,100,0,0,2400,1080,W,H,0)));                         // zero width
    CHECK(eq(nb_ua_draw_rect(-50,-50,100,100,0,0,0xffffffffu,0xffffffffu,W,H,0),0,0,50,50));   // huge scissor extent does not overflow
    CHECK(nb_ua_empty(nb_ua_draw_rect(0.f/0.f,0,10,10,0,0,2400,1080,W,H,0)));                   // NaN viewport
    // ---- union / contains
    nb_ua_rect u=nb_ua_none();CHECK(nb_ua_empty(u));
    u=nb_ua_union(u,nb_ua_draw_rect(0,0,2400,1080,10,10,20,20,W,H,0));CHECK(eq(u,10,10,30,30));
    u=nb_ua_union(u,nb_ua_none());CHECK(eq(u,10,10,30,30));
    u=nb_ua_union(u,nb_ua_draw_rect(0,0,2400,1080,100,5,10,10,W,H,0));CHECK(eq(u,10,5,110,30));
    CHECK(nb_ua_contains(u,nb_ua_none()));CHECK(nb_ua_contains(u,u));
    {nb_ua_rect in={20,10,100,20},out={20,10,111,20};CHECK(nb_ua_contains(u,in));CHECK(!nb_ua_contains(u,out));}
    // ---- align to the 32 px tile grid, clamped
    CHECK(eq(nb_ua_align(u,32,W,H),0,0,128,32));
    {nb_ua_rect r={2390,1070,2400,1080};CHECK(eq(nb_ua_align(r,32,W,H),2368,1056,2400,1080));}   // 2400 / 1080 are not multiples of 32
    {nb_ua_rect r={33,64,64,65};CHECK(eq(nb_ua_align(r,32,W,H),32,64,64,96));}
    // ---- seed: first draw ∪ history, aligned; full area (0) when saving < 10 %
    nb_ua_rect s{};
    {nb_ua_rect f={1000,400,1400,700};CHECK(nb_ua_seed(f,nullptr,W,H,32,100,&s)==1);CHECK(eq(s,992,384,1408,704));}   // a filter pass: 1 small draw
    {nb_ua_rect f={0,0,2400,1080};CHECK(nb_ua_seed(f,nullptr,W,H,32,100,&s)==0);}                                       // full-screen draw
    {nb_ua_rect f={0,0,2400,1000};CHECK(nb_ua_seed(f,nullptr,W,H,32,100,&s)==0);}                                       // 7 % saving: not worth it
    {nb_ua_rect f={0,0,2400,900};CHECK(nb_ua_seed(f,nullptr,W,H,32,100,&s)==1);CHECK(eq(s,0,0,2400,928));}             // 14 %
    {nb_ua_rect f={10,10,20,20},h={2000,900,2100,1000};CHECK(nb_ua_seed(f,&h,W,H,32,100,&s)==1);CHECK(eq(s,0,0,2112,1024));}   // history widens it
    {nb_ua_rect f={10,10,20,20},h={0,0,2400,1080};CHECK(nb_ua_seed(f,&h,W,H,32,100,&s)==0);}                            // history: full -> full
    CHECK(nb_ua_seed(nb_ua_none(),nullptr,W,H,32,100,&s)==0);                                                           // nothing known
    CHECK(nb_ua_seed(nb_ua_none(),nullptr,0,0,32,100,&s)==0);
    // grow-only restart: the area of the first draw does not contain a later one -> restart (caller: end the pass, full area)
    {nb_ua_rect f={1000,400,1400,700};nb_ua_seed(f,nullptr,W,H,32,100,&s);
        nb_ua_rect later={1100,500,1300,600},outside={1300,600,1450,650};CHECK(nb_ua_contains(s,later));CHECK(!nb_ua_contains(s,outside));}
    // ---- DS use
    CHECK(!nb_ua_uses_ds(0,0,0));CHECK(nb_ua_uses_ds(1,0,0));CHECK(nb_ua_uses_ds(0,1,0));CHECK(nb_ua_uses_ds(0,0,1));
    // ---- history
    static nb_ua_hist h;nb_ua_hist_reset(&h);
    CHECK(!nb_ua_hist_find(&h,42));
    nb_ua_hent *e=nb_ua_hist_put(&h,42);CHECK(e && e->key==42 && nb_ua_empty(e->uni) && !e->ds);
    e->uni=nb_ua_full(10,10);e->ds=1;CHECK(nb_ua_hist_find(&h,42)==e && nb_ua_hist_find(&h,42)->ds==1);
    CHECK(nb_ua_hist_put(&h,42)==e && h.n==1);
    CHECK(nb_ua_hist_put(&h,0)!=nullptr && nb_ua_hist_find(&h,0)==nb_ua_hist_find(&h,1));   // key 0 = 1
    for(uint64_t k=100;k<100+2000;k++)CHECK(nb_ua_hist_put(&h,k*0x10001ull)!=nullptr);      // fills and empties itself, never fails
    CHECK(h.n<=NB_UA_HIST*3/4+1);
    // ---- commit / counting (campaign f=2509 shapes: filter pass with one draw, DS attached unused; UI pass 95 draws full)
    nb_ua_acc a{};nb_ua_pass p;
    nb_ua_pass_init(&p);p.w=W;p.h=H;p.natt=2;p.draws=1;p.cls=NB_UA_CLS_FILTER;p.uni={1000,400,1400,700};p.ds_att=1;nb_ua_commit(&a,&p,32,100);
    CHECK(a.passes==1 && a.ui_passes==1 && a.flt_passes==1 && a.area_q==1 && a.area_q_flt==1 && a.ds_q==1 && a.ds_q_flt==1);
    CHECK(a.area_px_saved==uint64_t(W)*H-uint64_t(416)*320);CHECK(a.area_bytes_saved==a.area_px_saved*4*2*2);CHECK(a.ds_bytes_saved==uint64_t(W)*H*8);
    nb_ua_pass_init(&p);p.w=W;p.h=H;p.natt=2;p.draws=95;p.cls=NB_UA_CLS_IGGY;p.uni={0,0,2400,1080};p.ds_att=1;p.ds_used=1;nb_ua_commit(&a,&p,32,100);
    CHECK(a.ui_passes==2 && a.flt_passes==1 && a.area_q==1 && a.ds_q==1 && a.ui_draws==96);
    nb_ua_pass_init(&p);p.w=W;p.h=H;p.natt=1;p.draws=1;p.cls=NB_UA_CLS_FILTER;p.uni={0,0,64,64};p.clear_start=1;nb_ua_commit(&a,&p,32,100);
    CHECK(a.area_q==1 && a.no_area_clear==1);                                                 // begun with a clear: never a smaller area
    nb_ua_pass_init(&p);p.w=W;p.h=H;p.draws=3;p.cls=NB_UA_CLS_OTHER;p.uni={0,0,64,64};p.ds_att=1;nb_ua_commit(&a,&p,32,100);
    CHECK(a.passes==4 && a.ui_passes==3 && a.ds_q==1);                                        // non-Iggy passes: counted as host passes only
    nb_ua_pass_init(&p);p.w=W;p.h=H;p.natt=1;p.draws=2;p.cls=NB_UA_CLS_FILTER;p.reduced=1;p.area={992,384,1408,704};p.detached=1;p.uni={1000,400,1400,700};
    p.fills=2;p.fills_opaque=1;p.fills_first_full=0;nb_ua_commit(&a,&p,32,100);
    CHECK(a.reduced==1 && a.reduced_px_saved==uint64_t(W)*H-uint64_t(416)*320 && a.detached==1 && a.fills==2 && a.fills_opaque==1);
    if(failures){printf("%d failures\n",failures);return 1;}
    printf("nb_uiarea_test: all passed\n");return 0;
}
