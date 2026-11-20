// [NR-16] host test of dm/host/cpuprof.h (the pure part of hle_cpuprof.c, DM_NR_CPU): /proc/<tid>/stat with a comm holding spaces and
// parentheses, schedstat, /proc/stat cpu lines, clusters by capacity (Snapdragon 8 Gen 3 1+3+2+2, Helio G99 2+6, Tensor 1+3+4, one
// cluster, equal frequencies without cpu_capacity), letters, SVC names and the blocking set.
// Build: clang -std=c11 -Wall -Wextra -I.. t_cpuprof.c -o t_cpuprof && ./t_cpuprof   (Windows LLVM clang, WSL gcc or the Mac)
#include "../cpuprof.h"
#include <stdio.h>
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_stat(void) {
    // field 3 = state ... 14 utime, 15 stime ... 39 processor (Linux proc(5)); the comm has spaces and a ')'
    const char *l = "4242 (Thread (x) 1) S 4200 4200 0 0 -1 4194368 100 0 0 0 1234 567 0 0 10 -10 52 0 9000 0 0 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 6 0 0 0 0 0\n";
    uint64_t ut = 0, st = 0; int cpu = -2;
    CHECK(cpf_parse_stat(l, &ut, &st, &cpu) == 1, "stat parsed");
    CHECK(ut == 1234 && st == 567, "utime %llu stime %llu", (unsigned long long)ut, (unsigned long long)st);
    CHECK(cpu == 6, "processor %d", cpu);
    CHECK(!cpf_parse_stat("no parenthesis here", &ut, &st, &cpu), "garbage refused");
    CHECK(!cpf_parse_stat("1 (a) S 2 3", &ut, &st, &cpu), "short line refused");
    // a kernel without field 39 (old): parsed, processor -1
    const char *old = "7 (nvn-translate) R 1 1 0 0 -1 0 0 0 0 0 99 1 0 0 20 0 1 0 5 0 0\n";
    CHECK(cpf_parse_stat(old, &ut, &st, &cpu) == 1 && ut == 99 && st == 1 && cpu == -1, "short stat: %llu %llu %d", (unsigned long long)ut, (unsigned long long)st, cpu);
}
static void test_schedstat(void) {
    uint64_t r = 0, w = 0;
    CHECK(cpf_parse_schedstat("123456789012 3456789 4242\n", &r, &w) && r == 123456789012ull && w == 3456789, "schedstat %llu %llu", (unsigned long long)r, (unsigned long long)w);
    CHECK(!cpf_parse_schedstat("", &r, &w), "empty refused");
    CHECK(!cpf_parse_schedstat("12", &r, &w), "one number refused");
}
static void test_procstat(void) {
    const char *s = "cpu  100 0 50 1000 10 0 0 0 0 0\n"
                    "cpu0 10 1 5 100 2 1 1 0 0 0\n"
                    "cpu1 20 0 10 200 0 0 0 0 0 0\n"
                    "cpu3 1 0 0 10 0 0 0 0 0 0\n"
                    "intr 1 2 3\nctxt 99\n";
    uint64_t b[8] = {0}, t[8] = {0};
    int n = cpf_parse_procstat(s, 8, b, t);
    CHECK(n == 4, "cpus %d", n);
    CHECK(t[0] == 120 && b[0] == 18, "cpu0 busy %llu total %llu", (unsigned long long)b[0], (unsigned long long)t[0]);
    CHECK(t[1] == 230 && b[1] == 30, "cpu1 busy %llu total %llu", (unsigned long long)b[1], (unsigned long long)t[1]);
    CHECK(t[2] == 0 && b[2] == 0, "cpu2 offline stays 0");
    CHECK(t[3] == 11 && b[3] == 1, "cpu3");
    uint64_t b2[2] = {0}, t2[2] = {0};
    CHECK(cpf_parse_procstat(s, 2, b2, t2) == 2 && t2[1] == 230, "max respected");
}
static void test_clusters(void) {
    int cl[16], first[4], count[4]; unsigned cap[4];
    { const unsigned c[8] = {379, 379, 870, 870, 870, 861, 861, 1024};   // SD 8 Gen 3 (A520 x2, A720 x3 + x2, X4)
      int n = cpf_clusters(c, 8, cl, cap, first, count, 4);
      CHECK(n == 4, "8g3 clusters %d", n);
      CHECK(cl[0] == 0 && cl[1] == 0 && cl[5] == 1 && cl[6] == 1 && cl[2] == 2 && cl[4] == 2 && cl[7] == 3, "8g3 order %d %d %d %d", cl[0], cl[5], cl[2], cl[7]);
      CHECK(first[1] == 5 && count[1] == 2 && first[2] == 2 && count[2] == 3 && count[3] == 1, "8g3 first/count");
      CHECK(cpf_cluster_letter(0, n) == 'L' && cpf_cluster_letter(1, n) == 'M' && cpf_cluster_letter(2, n) == 'B' && cpf_cluster_letter(3, n) == 'X', "letters LMBX"); }
    { const unsigned c[8] = {294, 294, 294, 294, 294, 294, 1024, 1024};   // Helio G99
      int n = cpf_clusters(c, 8, cl, cap, first, count, 4);
      CHECK(n == 2 && cl[0] == 0 && cl[6] == 1 && count[0] == 6 && count[1] == 2, "g99");
      CHECK(cpf_cluster_letter(0, n) == 'L' && cpf_cluster_letter(1, n) == 'B', "letters LB"); }
    { const unsigned c[8] = {1024, 600, 600, 600, 250, 250, 250, 250};   // big first in cpu order
      int n = cpf_clusters(c, 8, cl, cap, first, count, 4);
      CHECK(n == 3 && cl[0] == 2 && cl[1] == 1 && cl[4] == 0 && cap[0] == 250 && cap[2] == 1024, "reverse order");
      CHECK(cpf_cluster_letter(2, n) == 'B', "letter B of 3"); }
    { const unsigned c[4] = {1024, 1024, 1024, 1024};
      int n = cpf_clusters(c, 4, cl, cap, first, count, 4);
      CHECK(n == 1 && cpf_cluster_letter(0, n) == 'A' && count[0] == 4, "one cluster"); }
    { const unsigned c[6] = {1, 2, 3, 4, 5, 6};   // more groups than slots: the rest go to the last cluster
      int n = cpf_clusters(c, 6, cl, cap, first, count, 4);
      CHECK(n == 4 && cl[0] == 0 && cl[3] == 3 && cl[4] == 3 && cl[5] == 3, "overflow into the last: %d %d %d", n, cl[4], cl[5]); }
    CHECK(cpf_cluster_letter(5, 4) == '?' && cpf_cluster_letter(0, 0) == '?', "bad letters");
}
static void test_svc(void) {
    CHECK(!strcmp(cpf_svc_name(0x0B), "SleepThread") && !strcmp(cpf_svc_name(0x1A), "ArbitrateLock") && !strcmp(cpf_svc_name(0x34), "WaitForAddress"), "names");
    CHECK(!strcmp(cpf_svc_name(0x90), "host-call") && !strcmp(cpf_svc_name(0x7F), "?"), "host calls and unknown");
    CHECK(cpf_svc_waits(0x0B) && cpf_svc_waits(0x1C) && cpf_svc_waits(0x18) && !cpf_svc_waits(0x1B) && !cpf_svc_waits(0x1D) && !cpf_svc_waits(0x35), "blocking set");
}
int main(void) {
    test_stat(); test_schedstat(); test_procstat(); test_clusters(); test_svc();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
