// [NR-24] Linux (WSL / Android-like) stubs for building hle_thread.c in a unit test, next to t_condvar_stub.c (which covers the
// host facilities the Mac build has): the Mach thread calls only the state dump uses, pthread_gettid_np, the pc_note of the
// thread-exit module. Build: see the head of t_nr24_arb.c.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <mach/mach.h>
pid_t pthread_gettid_np(pthread_t t) { (void)t; return (pid_t)syscall(SYS_gettid); }
void pc_note(const char *id, int on, const char *fmt, ...) { (void)id; (void)on; (void)fmt; }
kern_return_t thread_suspend(mach_port_t t) { (void)t; return 1; }
kern_return_t thread_resume(mach_port_t t) { (void)t; return 1; }
kern_return_t thread_get_state(mach_port_t t, int f, thread_state_t s, mach_msg_type_number_t *n) { (void)t; (void)f; (void)s; (void)n; return 1; }
kern_return_t thread_info(mach_port_t t, int f, thread_info_t i, mach_msg_type_number_t *n) { (void)t; (void)f; (void)i; (void)n; return 1; }
kern_return_t task_threads(mach_port_t task, thread_act_array_t *a, mach_msg_type_number_t *n) { (void)task; (void)a; *n = 0; return 1; }
