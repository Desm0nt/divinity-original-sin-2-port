// [NR-16] DM_NR_CPU: the pure parts of the CPU attribution sampler (hle_cpuprof.c) - /proc text parsing, core clusters by capacity, SVC
// names. No I/O, no globals: the host test dm/host/tests/t_cpuprof.c runs these on fixed text.
#ifndef DM_CPUPROF_H
#define DM_CPUPROF_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// /proc/<pid>/task/<tid>/stat: "tid (comm) S ppid ..." - comm may hold spaces and ')', so fields count from the last ')'.
// utime = field 14, stime = 15 (clock ticks), processor = 39 (the cpu it last ran on). 1 = parsed.
static inline int cpf_parse_stat(const char *buf, uint64_t *utime, uint64_t *stime, int *processor) {
    const char *p = strrchr(buf, ')'); if (!p) return 0;
    p++;
    int field = 2; uint64_t ut = 0, st = 0; int cpu = -1;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p || *p == '\n') break;
        field++;
        char *end; unsigned long long v = strtoull(p, &end, 10);
        if (field == 14) ut = v; else if (field == 15) st = v; else if (field == 39) { cpu = (int)v; break; }
        p = end; while (*p && *p != ' ') p++;
    }
    if (field < 15) return 0;
    *utime = ut; *stime = st; *processor = cpu; return 1;
}
// /proc/<pid>/task/<tid>/schedstat: "run_ns wait_ns timeslices" (time on a cpu, time runnable on a run queue). 1 = parsed.
static inline int cpf_parse_schedstat(const char *buf, uint64_t *run, uint64_t *wait) {
    char *end; unsigned long long r = strtoull(buf, &end, 10); if (end == buf) return 0;
    const char *q = end; unsigned long long w = strtoull(q, &end, 10); if (end == q) return 0;
    *run = r; *wait = w; return 1;
}
// /proc/stat: the "cpuN user nice system idle iowait irq softirq steal ..." lines -> busy (all but idle + iowait) and total ticks of
// cpu N (index N; cpus the text does not list stay 0). Returns 1 + the highest cpu index seen, at most max.
static inline int cpf_parse_procstat(const char *buf, int max, uint64_t *busy, uint64_t *total) {
    int top = 0;
    for (const char *l = buf; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        if (strncmp(l, "cpu", 3) || l[3] < '0' || l[3] > '9') continue;
        char *end; long n = strtol(l + 3, &end, 10); if (n < 0 || n >= max) continue;
        uint64_t v[10] = {0}; const char *p = end;
        for (int i = 0; i < 10; i++) { char *e2; unsigned long long x = strtoull(p, &e2, 10); if (e2 == p) break; v[i] = x; p = e2; }
        uint64_t t = 0; for (int i = 0; i < 8; i++) t += v[i];
        busy[n] = t - v[3] - v[4]; total[n] = t;
        if ((int)n + 1 > top) top = (int)n + 1;
    }
    return top;
}
// Clusters: cpus of equal capacity (cpu_capacity, or the max frequency where the kernel has none) form one cluster; clusters are
// numbered by rising capacity. cl[c] = cluster of cpu c, cap_of[k] / first[k] / count[k] per cluster. Returns the number of clusters.
// Letter: with 4 clusters L M B X, with 3 L M B, with 2 L B, with 1 A.
static inline int cpf_clusters(const unsigned *cap, int n, int *cl, unsigned *cap_of, int *first, int *count, int maxcl) {
    int ncl = 0;
    for (int c = 0; c < n; c++) {
        int k = 0; while (k < ncl && cap_of[k] != cap[c]) k++;
        if (k == ncl) { if (ncl == maxcl) { cl[c] = ncl - 1; continue; } cap_of[ncl++] = cap[c]; }
    }
    for (int i = 1; i < ncl; i++) for (int j = i; j > 0 && cap_of[j] < cap_of[j - 1]; j--) { unsigned t = cap_of[j]; cap_of[j] = cap_of[j - 1]; cap_of[j - 1] = t; }
    for (int k = 0; k < ncl; k++) { first[k] = -1; count[k] = 0; }
    for (int c = 0; c < n; c++) {
        int k = 0; while (k < ncl - 1 && cap_of[k] != cap[c]) k++;
        cl[c] = k; if (first[k] < 0) first[k] = c; count[k]++;
    }
    return ncl;
}
static inline char cpf_cluster_letter(int k, int ncl) {
    static const char *const l[5] = {"", "A", "LB", "LMB", "LMBX"};
    return ncl >= 1 && ncl <= 4 && k >= 0 && k < ncl ? l[ncl][k] : '?';
}
// Horizon SVC numbers the game reaches (public names, Switchbrew / mesosphere)
static inline const char *cpf_svc_name(int n) {
    switch (n) {
    case 0x01: return "SetHeapSize"; case 0x03: return "SetMemoryAttribute"; case 0x04: return "MapMemory"; case 0x05: return "UnmapMemory";
    case 0x06: return "QueryMemory"; case 0x08: return "CreateThread"; case 0x09: return "StartThread"; case 0x0A: return "ExitThread";
    case 0x0B: return "SleepThread"; case 0x0C: return "GetThreadPriority"; case 0x0D: return "SetThreadPriority"; case 0x0E: return "GetThreadCoreMask";
    case 0x0F: return "SetThreadCoreMask"; case 0x10: return "GetCurrentProcessorNumber"; case 0x11: return "SignalEvent"; case 0x12: return "ClearEvent";
    case 0x13: return "MapSharedMemory"; case 0x15: return "CreateTransferMemory"; case 0x16: return "CloseHandle"; case 0x17: return "ResetSignal";
    case 0x18: return "WaitSynchronization"; case 0x19: return "CancelSynchronization"; case 0x1A: return "ArbitrateLock"; case 0x1B: return "ArbitrateUnlock";
    case 0x1C: return "WaitProcessWideKeyAtomic"; case 0x1D: return "SignalProcessWideKey"; case 0x1E: return "GetSystemTick"; case 0x1F: return "ConnectToNamedPort";
    case 0x21: return "SendSyncRequest"; case 0x22: return "SendSyncRequestWithUserBuffer"; case 0x24: return "GetProcessId"; case 0x25: return "GetThreadId";
    case 0x26: return "Break"; case 0x27: return "OutputDebugString"; case 0x29: return "GetInfo"; case 0x2C: return "MapPhysicalMemory";
    case 0x2D: return "UnmapPhysicalMemory"; case 0x32: return "SetThreadActivity"; case 0x34: return "WaitForAddress"; case 0x35: return "SignalToAddress";
    case 0x36: return "SynchronizePreemptionState"; case 0x45: return "CreateEvent";
    default: return n >= 0x80 ? "host-call" : "?";
    }
}
// The SVCs that block by design (their wall time is waiting, not work)
static inline int cpf_svc_waits(int n) { return n == 0x0B || n == 0x18 || n == 0x1A || n == 0x1C || n == 0x34; }
#endif
