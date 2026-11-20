// [DOS2] NR-UIX host test (dm/gfx/nr/nr_uix.h, used by xbg_nb_uiarea.inc / xbg_nr_uix.inc under DM_NR_UIX_*). Pure parts + a model of the
// depth-stencil detach history: the old one (DM_NB_UIAREA alone: keyed by the first draw's state, sticky) against NR-UIX (ordinal keys, decay),
// on the Iggy pass shapes of the owner's play-1118 log (Fort Joy: 14..120 filter passes a frame on one ping-pong target, same fill shader, full
// scissor; one of them uses the depth-stencil every frame, another one rarely).
// Build, from this folder:  clang++ -std=c++17 -O2 -Wall -D_CRT_SECURE_NO_WARNINGS nr_uix_test.cpp -o nr_uix_test   (or g++)
#include "../../../nr/nr_uix.h"
#include "../../../nr/nr_cover.h"
#include <stdio.h>
#include <vector>
#include <chrono>

static int failures;
#define CHECK(expression) do { if(!(expression)){printf("FAIL line %d: %s\n",__LINE__,#expression);++failures;} } while(0)
static bool eq(nb_ua_rect r,int x0,int y0,int x1,int y1){return r.x0==x0 && r.y0==y0 && r.x1==x1 && r.y1==y1;}
// a box from float clip coordinates: never smaller than the exact one (conservative), at most 1 px larger per side (float rounding of NDC)
static bool near(nb_ua_rect r,int x0,int y0,int x1,int y1){return r.x0<=x0 && r.y0<=y0 && r.x1>=x1 && r.y1>=y1 && r.x0>=x0-1 && r.y0>=y0-1 && r.x1<=x1+1 && r.y1<=y1+1;}

// One frame of Iggy pass starts as the backend sees them: the state key of each pass's first draw and whether a draw of it uses the DS.
struct PassStart{uint64_t key;bool uses_ds;};
struct Model{   // old = DM_NB_UIAREA history (key, sticky ds), new = NR-UIX (key2 = key + ordinal, ds refreshed, 120-frame decay)
    nb_ua_hist old_h,new_h;nr_uix_ord ord;uint32_t frame=0;bool use_new;
    uint64_t passes=0,detached=0,reattach=0,kept_unused=0;
    explicit Model(bool n):use_new(n){nb_ua_hist_reset(&old_h);nb_ua_hist_reset(&new_h);nr_uix_ord_reset(&ord);}
    void run(const std::vector<PassStart> &f){++frame;nr_uix_ord_frame(&ord);
        for(const PassStart &p:f){++passes;
            const uint32_t o=nr_uix_ord_peek(&ord,p.key);const uint64_t k2=nr_uix_key2(p.key,o);
            bool det;
            if(use_new)det=!nr_uix_ds_keep(nb_ua_hist_find(&new_h,k2),frame,120);
            else{const nb_ua_hent *e=nb_ua_hist_find(&old_h,p.key);det=!(e && e->ds);}
            CHECK(nr_uix_ord_take(&ord,p.key)==(o<255?o:255u));
            if(det){if(p.uses_ds){++reattach;   // the DS-using draw ends the DS-less pass, the host begins it with the DS (LOAD)
                    if(nb_ua_hent *e=nb_ua_hist_put(&old_h,p.key))e->ds=1;
                    if(nb_ua_hent *e=nb_ua_hist_put(&new_h,k2)){e->ds=1;e->frame_ds=frame;}}
                else ++detached;}
            else{if(p.uses_ds){if(nb_ua_hent *e=nb_ua_hist_put(&new_h,k2)){e->ds=1;e->frame_ds=frame;}}else ++kept_unused;}}}
};

int main(){
    const uint32_t W=2400,H=1080;
    // ---- ordinals: per frame, per key; peek does not count
    {nr_uix_ord o;nr_uix_ord_reset(&o);nr_uix_ord_frame(&o);
        CHECK(nr_uix_ord_peek(&o,7)==0);CHECK(nr_uix_ord_peek(&o,7)==0);CHECK(nr_uix_ord_take(&o,7)==0);CHECK(nr_uix_ord_peek(&o,7)==1);
        CHECK(nr_uix_ord_take(&o,7)==1);CHECK(nr_uix_ord_take(&o,9)==0);CHECK(nr_uix_ord_take(&o,7)==2);
        nr_uix_ord_frame(&o);CHECK(nr_uix_ord_peek(&o,7)==0);CHECK(nr_uix_ord_take(&o,7)==0);
        for(int i=0;i<300;i++)nr_uix_ord_take(&o,11);CHECK(nr_uix_ord_peek(&o,11)==300);CHECK(nr_uix_ord_take(&o,11)==255);   // saturates: still a stable key
        // many keys in one frame: every one still gets ordinals 0, 1 (or the shared overflow 255, never a crash)
        int ok=0;for(uint64_t k=100;k<400;k++){const uint32_t a=nr_uix_ord_take(&o,k),b=nr_uix_ord_take(&o,k);if(a==0 && b==1)++ok;else CHECK(a==255 || b==255);}
        CHECK(ok>=200);
        nr_uix_ord_frame(&o);CHECK(nr_uix_ord_take(&o,150)==0);
        CHECK(nr_uix_key2(5,0)!=nr_uix_key2(5,1));CHECK(nr_uix_key2(5,0)!=nr_uix_key2(6,0));CHECK(nr_uix_key2(0,0)!=0);}
    // ---- decay
    {nb_ua_hent e{};CHECK(!nr_uix_ds_keep(nullptr,10,120));CHECK(!nr_uix_ds_keep(&e,10,120));e.ds=1;e.frame_ds=10;
        CHECK(nr_uix_ds_keep(&e,10,120));CHECK(nr_uix_ds_keep(&e,129,120));CHECK(!nr_uix_ds_keep(&e,130,120));
        e.frame_ds=0xfffffff0u;CHECK(nr_uix_ds_keep(&e,5,120));}   // wraps
    // ---- vertex boxes
    {nr_uix_bb b;nr_uix_bb_init(&b);nb_ua_rect r;CHECK(!nr_uix_bb_rect(&b,1,nb_ua_full(W,H),&r));   // no vertex: unusable
        // a quad from pixel 100..300 x 50..80 in NDC of a 2400x1080 viewport
        auto ndc=[&](double px,double py,float *p){p[0]=float(px/W*2-1);p[1]=float(py/H*2-1);p[2]=0.5f;p[3]=1;};
        float v[4];ndc(100,50,v);nr_uix_bb_add(&b,v,0,0,float(W),float(H));ndc(300,50,v);nr_uix_bb_add(&b,v,0,0,float(W),float(H));
        ndc(300,80,v);nr_uix_bb_add(&b,v,0,0,float(W),float(H));ndc(100,80,v);nr_uix_bb_add(&b,v,0,0,float(W),float(H));
        CHECK(nr_uix_bb_rect(&b,1,nb_ua_full(W,H),&r));CHECK(near(r,99,49,301,81));
        CHECK(nr_uix_bb_rect(&b,1,nb_ua_rect{0,0,200,1080},&r));CHECK(near(r,99,49,200,81));   // clipped to viewport x scissor
        CHECK(nr_uix_bb_rect(&b,1,nb_ua_rect{500,0,600,1080},&r));CHECK(nb_ua_empty(r));     // outside: touches nothing
        // a projective w (Iggy's matrix programs divide by a computed term): the same pixels
        nr_uix_bb b2;nr_uix_bb_init(&b2);ndc(100,50,v);for(float &c:v)c*=2;nr_uix_bb_add(&b2,v,0,0,float(W),float(H));ndc(300,80,v);for(float &c:v)c*=2;nr_uix_bb_add(&b2,v,0,0,float(W),float(H));
        CHECK(nr_uix_bb_rect(&b2,1,nb_ua_full(W,H),&r));CHECK(near(r,99,49,301,81));
        // negative viewport height (y flipped): rows 1080-80 .. 1080-50
        nr_uix_bb b3;nr_uix_bb_init(&b3);ndc(100,50,v);nr_uix_bb_add(&b3,v,0,float(H),float(W),-float(H));ndc(300,80,v);nr_uix_bb_add(&b3,v,0,float(H),float(W),-float(H));
        CHECK(nr_uix_bb_rect(&b3,0,nb_ua_full(W,H),&r));CHECK(near(r,100,1000,300,1030));
        // w <= 0 or NaN: unusable (the caller keeps viewport x scissor)
        nr_uix_bb b4;nr_uix_bb_init(&b4);ndc(100,50,v);nr_uix_bb_add(&b4,v,0,0,float(W),float(H));v[3]=0;nr_uix_bb_add(&b4,v,0,0,float(W),float(H));CHECK(!nr_uix_bb_rect(&b4,1,nb_ua_full(W,H),&r));
        nr_uix_bb b5;nr_uix_bb_init(&b5);v[0]=0.f/0.f;v[3]=1;nr_uix_bb_add(&b5,v,0,0,float(W),float(H));CHECK(!nr_uix_bb_rect(&b5,1,nb_ua_full(W,H),&r));
        CHECK(nr_uix_prim_margin(4,1)==1);CHECK(nr_uix_prim_margin(7,1)==1);CHECK(nr_uix_prim_margin(4,0)==4);CHECK(nr_uix_prim_margin(1,1)==4);CHECK(nr_uix_prim_margin(0,1)<0);CHECK(nr_uix_prim_margin(14,1)<0);}
    // ---- the Iggy rect program (NR-10b census: x = in.x * c[0].x + c[0].z, y = in.y * c[0].y + c[0].w, z 0, w 1) through nr_cv_eval + the box
    {nr_cv_prog pr{};pr.ok=1;pr.in_mask=1;pr.nubo=1;pr.ubo[0]=6;pr.ubo_bytes[0]=16;
        auto ins=[&](uint8_t op,uint16_t a,uint16_t b,uint16_t c,float k){pr.ins[pr.n]=nr_cv_ins{op,0,a,b,c,k};return uint8_t(pr.n++);};
        const uint8_t ix=ins(NR_CV_INPUT,0,0,0,0),iy=ins(NR_CV_INPUT,0,1,0,0),c0=ins(NR_CV_UBO,6,0,0,0),c1=ins(NR_CV_UBO,6,4,0,0),c2=ins(NR_CV_UBO,6,8,0,0),c3=ins(NR_CV_UBO,6,12,0,0);
        const uint8_t x=ins(NR_CV_FMA,ix,c0,c2,0),y=ins(NR_CV_FMA,iy,c1,c3,0),z=ins(NR_CV_CONST,0,0,0,0),w=ins(NR_CV_CONST,0,0,0,1);
        pr.out[0]=x;pr.out[1]=y;pr.out[2]=z;pr.out[3]=w;
        const float c[4]={2.f/W,2.f/H,-1,-1};const uint8_t *ub[NR_CV_MAX_UBO]={reinterpret_cast<const uint8_t*>(c)};const size_t ubn[NR_CV_MAX_UBO]={16};
        const float quad[4][2]={{640,300},{900,300},{900,420},{640,420}};   // pixel coordinates (Iggy's own space)
        nr_uix_bb b;nr_uix_bb_init(&b);
        for(auto &q:quad){float in[32][4]{};in[0][0]=q[0];in[0][1]=q[1];in[0][3]=1;float P[4];CHECK(nr_cv_eval(&pr,in,ub,ubn,P));nr_uix_bb_add(&b,P,0,0,float(W),float(H));}
        nb_ua_rect r;CHECK(nr_uix_bb_rect(&b,1,nb_ua_full(W,H),&r));CHECK(near(r,639,299,901,421));
        // the area a pass of this one fill would begin with (32 px grid): 608..928 x 288..448 of 2400x1080 = 1.98 % of the target
        nb_ua_rect sd;CHECK(nb_ua_seed(r,nullptr,W,H,32,100,&sd));CHECK(eq(sd,608,288,928,448));
        printf("Iggy fill box %dx%d -> area %dx%d: load+store %.2f MB instead of %.1f MB (colour + D24S8 at 4 B/px)\n",r.x1-r.x0,r.y1-r.y0,sd.x1-sd.x0,sd.y1-sd.y0,
            double(nb_ua_px(sd))*8*2/1048576.,double(W)*H*8*2/1048576.);}
    // ---- CPU cost of a box: a 42-instruction program (the size of 4bd09f50's), 500 Iggy draws x 4 vertices a frame
    {nr_cv_prog pr{};pr.ok=1;pr.in_mask=1;pr.nubo=1;pr.ubo[0]=6;pr.ubo_bytes[0]=128;
        auto ins=[&](uint8_t op,uint16_t a,uint16_t b,uint16_t c,float k){pr.ins[pr.n]=nr_cv_ins{op,0,a,b,c,k};return uint8_t(pr.n++);};
        uint8_t last=ins(NR_CV_INPUT,0,0,0,0);const uint8_t iy=ins(NR_CV_INPUT,0,1,0,0);
        for(int i=0;i<19;i++){const uint8_t u=ins(NR_CV_UBO,6,uint16_t(4*(i%32)),0,0);last=ins(NR_CV_FMA,last,u,iy,0);}
        const uint8_t one=ins(NR_CV_CONST,0,0,0,1),y=ins(NR_CV_MUL,iy,one,0,0);
        pr.out[0]=last;pr.out[1]=y;pr.out[2]=one;pr.out[3]=one;CHECK(pr.n<=NR_CV_MAX_INS);
        float c[32];for(int i=0;i<32;i++)c[i]=0.999f;const uint8_t *ub[NR_CV_MAX_UBO]={reinterpret_cast<const uint8_t*>(c)};const size_t ubn[NR_CV_MAX_UBO]={128};
        const int frames=200;double sink=0;const auto t0=std::chrono::steady_clock::now();
        for(int f=0;f<frames;f++)for(int d=0;d<500;d++){nr_uix_bb b;nr_uix_bb_init(&b);
            for(int k=0;k<4;k++){float in[32][4];in[0][0]=float((d+k)%7)*0.01f;in[0][1]=float(k)*0.1f;in[0][2]=0;in[0][3]=1;float P[4];nr_cv_eval(&pr,in,ub,ubn,P);nr_uix_bb_add(&b,P,0,0,float(W),float(H));}
            nb_ua_rect r;if(nr_uix_bb_rect(&b,1,nb_ua_full(W,H),&r))sink+=r.x0;}
        const double us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t0).count()/frames;
        printf("box cost on this PC: %.1f us a frame for 500 draws x 4 vertices (%.0f ns a box; the phone's big core ~1-2x) [%g]\n",us,us*1000/500,sink>0?0.:1.);}
    // ---- the detach history on play-1118 shapes
    {const uint64_t KF=0x1234;   // fill-first filter pass on the ping-pong target #83: same key for all of them
        auto frame_of=[&](int n,int f){std::vector<PassStart> v;v.push_back({0x99,true});   // the main UI pass p21 (uses stencil masks)
            for(int i=0;i<n;i++)v.push_back({KF+(i&1),i==2 || (i==9 && f%500==7)});   // #83 / #85 alternate; pass 2 always uses the DS, pass 9 once in 500 frames
            return v;};
        Model old_m(false),new_m(true);
        for(int f=0;f<3000;f++){const int n=f<1000?14:f<2000?113:60;const auto v=frame_of(n,f);old_m.run(v);new_m.run(v);}
        const double fr=3000;
        printf("detach model, 3000 frames (14 / 113 / 60 filter passes): old history detached %.2f a frame, kept unused %.2f, re-attached %.3f; "
               "NR-UIX detached %.2f, kept unused %.2f, re-attached %.3f\n",old_m.detached/fr,old_m.kept_unused/fr,old_m.reattach/fr,new_m.detached/fr,new_m.kept_unused/fr,new_m.reattach/fr);
        CHECK(old_m.detached<5*fr);   // the old one collapses after the first re-attach (play-1118: 14.0 -> 0.09)
        CHECK(new_m.detached>0.97*(14*1000+111*1000+58*1000));   // the new one keeps every pass that never uses the DS detached
        CHECK(new_m.reattach<40);    // learning + the rare user once per 500 frames (+ the decay of the always-user: never, it refreshes)
        CHECK(new_m.kept_unused<0.03*(14*1000+111*1000+58*1000));}
    // ---- the stress mode in the model: always detach = every DS user re-attaches every frame (the cost 3 trades for coverage)
    // ---- cache accounting
    {nr_uix_cache_acc a{};nr_uix_frame p{0,0,0,0},c{5,6,100,10};
        nr_uix_cache_close(&a,&p,&c);CHECK(a.ui_frames==1 && a.same_strict==0);p=c;
        nr_uix_cache_close(&a,&p,&c);CHECK(a.same_strict==1 && a.same_loose==1 && a.draws_same_strict==100 && a.best_strict==1);
        nr_uix_frame d{7,6,100,10};p=c;nr_uix_cache_close(&a,&p,&d);CHECK(a.same_strict==1 && a.same_loose==2 && a.run_strict==0 && a.best_loose==2);
        nr_uix_frame e{0,0,0,0};nr_uix_cache_close(&a,&d,&e);CHECK(a.frames==4 && a.ui_frames==3 && a.run_loose==0);
        CHECK(nr_uix_fnv(NR_UIX_FNV0,"a",1)!=nr_uix_fnv(NR_UIX_FNV0,"b",1));}
    if(failures){printf("%d failures\n",failures);return 1;}
    printf("nr_uix_test: all passed\n");return 0;
}
