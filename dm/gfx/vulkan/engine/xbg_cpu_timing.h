#pragma once
#include <stdint.h>

// CPU-side elapsed time, not GPU timestamps or thread CPU utilization.
// Caller serializes a recorder; scopes must close before reporting/resetting.
struct XbgCpuTiming {
    // [DOS2] NR-CPU (docs/dos2/nr/NR-CPU.md): NbState .. MtEnd are timed only with DM_NR_CPU=1 (NRCPU_SCOPE in xbg_game.cpp) and printed
    // only when they were timed, so without the switch every line is as before. They split what was "other" (NbState, NbDraw, MtEnd) and
    // "draw" (NrDesc, NrLane).
    enum Kind { Other, Draw, Dispatch, Descriptor, Pass, Buffer, Pipeline,
        Submit, Fence, Present, Cleanup, Hash, Image, Log, Program, CpuWrite, Snapshot,
        NbState, NbDraw, NrDesc, NrLane, MtEnd, NrZ3, Count };   // NrZ3: [NR-ZERO Z3] a draw answered by nrz3 (lookup to the end of the shared tail)
    static constexpr int FirstOptional = NbState;
    struct Counter { uint64_t calls=0, exclusive_ns=0, inclusive_ns=0, max_ns=0; } counters[Count];
    struct Scope;
    uint64_t (*clock)();
    Scope *top=nullptr;
    explicit XbgCpuTiming(uint64_t (*read_clock)()):clock(read_clock){}
    struct Scope {
        XbgCpuTiming *recorder; Scope *parent=nullptr; Kind kind;
        uint64_t start=0,children=0;
        Scope(XbgCpuTiming *r,Kind k):recorder(r),kind(k){
            if(r){start=r->clock();parent=r->top;r->top=this;}
        }
        ~Scope(){
            if(!recorder)return;
            uint64_t elapsed=recorder->clock()-start;
            auto &c=recorder->counters[kind];++c.calls;
            c.inclusive_ns+=elapsed;c.exclusive_ns+=elapsed-children;
            if(elapsed>c.max_ns)c.max_ns=elapsed;
            if(parent)parent->children+=elapsed;
            recorder->top=parent;
        }
        Scope(const Scope&)=delete;
        Scope& operator=(const Scope&)=delete;
    };
};
