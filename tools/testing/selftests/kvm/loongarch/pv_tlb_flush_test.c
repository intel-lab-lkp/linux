// SPDX-License-Identifier: GPL-2.0-only
/*
 * LoongArch PV TLB Flush Performance Test
 *
 * Measure the overhead of remote TLB flushes in a KVM guest by spawning
 * flusher threads that repeatedly mmap/munmap (triggering TLB shootdown
 * IPIs) alongside idle threads that either sleep or busy-spin.
 *
 * With PV TLB flush enabled, IPIs to preempted vCPUs are replaced by
 * deferred flags in the steal-time shared page, reducing flush latency.
 *
 * Usage:
 *   Compile on LoongArch guest:
 *     gcc -O2 -static -pthread -o pv_tlb_flush_test pv_tlb_flush_test.c
 *   Run (inside KVM guest):
 *     ./pv_tlb_flush_test <flushers> <idle> <iterations> <busy_idle>
 *   Examples:
 *     ./pv_tlb_flush_test 1 31 50000 0   # 1 flusher, 31 sleep, PV helps
 *     ./pv_tlb_flush_test 1 31 50000 1   # 1 flusher, 31 busy-spin, no PV
 *
 *   busy_idle=0: idle threads sleep, vCPUs get preempted, PV TLB flush
 *                can skip IPIs to them
 *   busy_idle=1: idle threads spin, all vCPUs stay active, PV TLB flush
 *                cannot optimize (baseline for comparison)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>

#define MEM_SIZE (2*1024*1024)
#define DEFAULT_ITERS 50000

static int nr_iters = DEFAULT_ITERS;
static volatile int start_barrier;
static volatile int stop_flag;
static int busy_idle = 0;

struct thread_args {
	int cpu;
	unsigned long *result;
	int *completed;
};

static inline unsigned long clock_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000000000UL + ts.tv_nsec;
}

static void pin_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    sched_setaffinity(0, sizeof(set), &set);
}

static void *idle_thread(void *arg) {
    struct thread_args *ta = arg;
    pin_cpu(ta->cpu);
    while (!__atomic_load_n(&start_barrier, __ATOMIC_ACQUIRE));
    if (busy_idle) {
        volatile long sink = 0;
        while (!__atomic_load_n(&stop_flag, __ATOMIC_ACQUIRE))
            sink++;
    } else {
        while (!__atomic_load_n(&stop_flag, __ATOMIC_ACQUIRE))
            usleep(1000);
    }
    return NULL;
}

static void *flush_thread(void *arg) {
    struct thread_args *ta = arg;
    unsigned long start, end;
    int i;
    size_t mem_size = MEM_SIZE;
    pin_cpu(ta->cpu);
    while (!__atomic_load_n(&start_barrier, __ATOMIC_ACQUIRE));
    start = clock_ns();
    for (i = 0; i < nr_iters && !__atomic_load_n(&stop_flag, __ATOMIC_ACQUIRE); i++) {
        void *p = mmap(NULL, mem_size, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) break;
        for (size_t off = 0; off < mem_size; off += 65536)
            ((volatile char*)p)[off] = 0;
        munmap(p, mem_size);
    }
    end = clock_ns();
    *ta->result = end - start;
    *ta->completed = i;
    return NULL;
}

int main(int argc, char **argv) {
    int nr_flush = 1, nr_idle = 3, i, run;
    int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (argc > 1) nr_flush = atoi(argv[1]);
    if (argc > 2) nr_idle = atoi(argv[2]);
    if (argc > 3) nr_iters = atoi(argv[3]);
    if (argc > 4) busy_idle = atoi(argv[4]);

    printf("=== TLB Flush Benchmark ===\n");
    printf("CPUs: %d  Flushers: %d  Idle: %d  Iters: %d  Mode: %s\n",
           ncpus, nr_flush, nr_idle, nr_iters,
           busy_idle ? "busy-spin" : "sleep");

    for (run = 0; run < 3; run++) {
        int total = nr_flush + nr_idle;
        int do_pin = (total <= ncpus);
        pthread_t threads[64];
        unsigned long results[64];
        int completed[64];
        struct thread_args args[64];
        start_barrier = 0; stop_flag = 0;

        for (i = 0; i < nr_idle; i++) {
            args[i].cpu = do_pin ? nr_flush + i : -1;
            args[i].result = NULL;
            args[i].completed = NULL;
            pthread_create(&threads[i], NULL, idle_thread, &args[i]);
        }
        for (i = 0; i < nr_flush; i++) {
            int idx = nr_idle + i;
            results[idx] = 0;
            completed[idx] = 0;
            args[idx].cpu = do_pin ? i : -1;
            args[idx].result = &results[idx];
            args[idx].completed = &completed[idx];
            pthread_create(&threads[idx], NULL, flush_thread, &args[idx]);
        }

        usleep(10000);
        __atomic_store_n(&start_barrier, 1, __ATOMIC_RELEASE);
        for (i = 0; i < nr_flush; i++)
            pthread_join(threads[nr_idle + i], NULL);
        __atomic_store_n(&stop_flag, 1, __ATOMIC_RELEASE);
        for (i = 0; i < nr_idle; i++)
            pthread_join(threads[i], NULL);

        unsigned long total_ns = 0;
        unsigned long total_done = 0;
        for (i = 0; i < nr_flush; i++) {
            int idx = nr_idle + i;
            unsigned long done = completed[idx];
            if (done == 0) {
                printf("  Run %d flusher %d: no iterations completed\n", run, i);
                continue;
            }
            printf("  Run %d flusher %d: %lu ns/flush (%lu iters)\n",
                   run, i, results[idx] / done, done);
            total_ns += results[idx];
            total_done += done;
        }
        if (total_done > 0)
            printf("  Run %d Avg: %lu ns/flush\n", run, total_ns / total_done);
    }
    return 0;
}
