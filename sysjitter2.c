/*
 * sysjitter2
 *
 * Copyright 2010-2015 David Riddoch <david@riddoch.org.uk> (pre v1.3)
 * Copyright 2020 Red Hat Inc. (v2)
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of version 3 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>
#include <sched.h>
#include <ctype.h>
#include <assert.h>

#ifdef __GNUC__
# define atomic_inc(ptr)   __sync_add_and_fetch((ptr), 1)
# if defined(__x86_64__)
#  define relax()          __asm__ __volatile__("pause" ::: "memory") 
static inline void frc(uint64_t* pval)
{
    uint32_t low, high;
    __asm__ __volatile__("rdtsc" : "=a" (low) , "=d" (high));
    *pval = ((uint64_t) high << 32) | low;
}
# elif defined(__i386__)
#  define relax()          __asm__ __volatile__("pause" ::: "memory") 
static inline void frc(uint64_t* pval)
{
    __asm__ __volatile__("rdtsc" : "=A" (*pval));
}
# elif defined(__PPC64__)
#  define relax()          do{}while(0)
static inline void frc(uint64_t* pval)
{
    __asm__ __volatile__("mfspr %0, 268\n" : "=r" (*pval));
}
# else
#  error Need frc() for this platform.
# endif
#else
# error Need to add support for this compiler.
#endif


typedef uint64_t stamp_t;   /* timestamp */
typedef uint64_t cycles_t;  /* number of cycles */

enum command {
    WAIT,
    GO,
    STOP
};

/*
 * We'll have buckets 1us, 2us, ..., (BUCKET_SIZE) us.
 */
#define  BUCKET_SIZE  (32)

struct thread {
    int                  core_i;
    pthread_t            thread_id;

    /* NOTE! this is also how many ticks per us */
    unsigned             cpu_mhz;
    cycles_t             int_total;
    stamp_t              frc_start;
    stamp_t              frc_stop;
    cycles_t             runtime;
    stamp_t             *buckets;
    /* Maximum latency detected */
    stamp_t              max_jitter;
    /* Outliers that are bigger than the maximum bucket tick */
    stamp_t              outlier_ticks;
};

struct global {
    /* Configuration. */
    unsigned              runtime_secs;
    unsigned              n_threads;
    struct timeval        tv_start;
    int                   verbose;
    int                   rtprio;
    int                   bucket_size;

    /* Mutable state. */
    volatile enum command cmd;
    volatile unsigned     n_threads_started;
    volatile unsigned     n_threads_ready;
    volatile unsigned     n_threads_running;
    volatile unsigned     n_threads_finished;
};

static struct global g;

#define TEST(x)                                 \
    do {                                        \
        if( ! (x) )                             \
            test_fail(#x, __LINE__);            \
    } while( 0 )

#define TEST0(x)  TEST((x) == 0)

static void test_fail(const char* what, int line)
{
    fprintf(stderr, "ERROR:\n");
    fprintf(stderr, "ERROR: TEST(%s)\n", what);
    fprintf(stderr, "ERROR: at line %d\n", line);
    fprintf(stderr, "ERROR: errno=%d (%s)\n", errno, strerror(errno));
    fprintf(stderr, "ERROR:\n");
    exit(1);
}

static int move_to_core(int core_i)
{
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(core_i, &cpus);
    return sched_setaffinity(0, sizeof(cpus), &cpus);
}

static cycles_t __measure_cpu_hz(void)
{
    struct timeval tvs, tve;
    stamp_t s, e;
    double sec;

    frc(&s);
    e = s;
    gettimeofday(&tvs, NULL);
    while( e - s < 1000000 )
        frc(&e);
    gettimeofday(&tve, NULL);
    sec = tve.tv_sec - tvs.tv_sec + (tve.tv_usec - tvs.tv_usec) / 1e6;
    return (cycles_t) ((e - s) / sec);
}

static unsigned measure_cpu_mhz(void)
{
    cycles_t m, mprev, d;

    mprev = __measure_cpu_hz();
    do {
        m = __measure_cpu_hz();
        if( m > mprev )  d = m - mprev;
        else             d = mprev - m;
        mprev = m;
    } while( d > m / 1000 );

    return (unsigned) (m / 1000000);
}

static void thread_init(struct thread* t)
{
    t->cpu_mhz = measure_cpu_mhz();
    t->max_jitter = 0;
    t->outlier_ticks = t->cpu_mhz * g.bucket_size;
    TEST(t->buckets = calloc(1, sizeof(t->buckets[0]) * g.bucket_size));
}

static float cycles_to_sec(const struct thread* t, uint64_t cycles)
{
    return cycles / (t->cpu_mhz * 1e6);
}

static float cycles_to_usec(const struct thread* t, uint64_t cycles)
{
    return cycles / t->cpu_mhz;
}

static void insert_bucket(struct thread *t, stamp_t value)
{
    int index;

    if (value >= t->max_jitter) {
        t->max_jitter = value;
    }

    /* Too big!  Put also into the last bucket */
    if (value >= t->outlier_ticks) {
        t->buckets[g.bucket_size-1]++;
        return;
    }

    index = value / t->cpu_mhz;
    assert(index >= 0 && index < g.bucket_size);
    t->buckets[index]++;
    if (t->buckets[index] == 0) {
        printf("Bucket %d overflowed\n", index);
        exit(1);
    }
}

static void doit(struct thread* t)
{
    stamp_t ts1, ts2;

    frc(&ts2);
    do {
        frc(&ts1);
        insert_bucket(t, ts1 - ts2);
        frc(&ts2);
        insert_bucket(t, ts2 - ts1);
    } while (g.cmd == GO);
}

static int set_fifo_prio(int prio)
{
    struct sched_param param;

    memset(&param, 0, sizeof(param));
    param.sched_priority = prio;
    return sched_setscheduler(0, SCHED_FIFO, &param);
}

static void* thread_main(void* arg)
{
    /* Important thing to note here is that once we start bashing the CPU, we
     * need to keep doing so to prevent the core from changing frequency or
     * dropping into a low power state.
     */
    struct thread* t = arg;

    /* Alloc memory in the thread itself after setting affinity to get the
     * best chance of getting numa-local memory.  Doesn't matter so much for
     * the "struct thread" since we expect that to stay cache resident.
     */
    TEST(move_to_core(t->core_i) == 0);
    if (g.rtprio != -1)
        TEST(set_fifo_prio(g.rtprio) == 0);

    /* Don't bash the cpu until all threads have got going. */
    atomic_inc(&g.n_threads_started);
    while( g.cmd == WAIT )
        usleep(1000);

    thread_init(t);

    /* Last thread to get here starts the timer. */
    // if( atomic_inc(&g.n_threads_ready) == g.n_threads )
    //   alarm(g.runtime_secs);
    /* Ensure we all start at the same time. */
    atomic_inc(&g.n_threads_running);
    while( g.n_threads_running != g.n_threads )
        relax();

    frc(&t->frc_start);
    doit(t);
    frc(&t->frc_stop);

    t->runtime = t->frc_stop - t->frc_start;

    /* Wait for everyone to finish so we don't disturb them by exiting and
     * waking the main thread.
     */
    atomic_inc(&g.n_threads_finished);
    while( g.n_threads_finished != g.n_threads )
        relax();

    return NULL;
}

#define _putfield(label, val, fmt, end) do {    \
        printf("%12s:\t", label);               \
        for( i = 0; i < g.n_threads; ++i )      \
            printf(" %"fmt, val);               \
        printf("%s\n", end);                    \
    } while( 0 )

#define putfield(fn, fmt)  _putfield(#fn, t[i].fn, fmt, "")

#define putu(fn)  putfield(fn, "u")
#define putul(fn)  putfield(fn, "lu")
#define put_frc(fn)  putfield(fn, PRIx64)
#define put_cycles_s(fn)                                                \
    _putfield(#fn, cycles_to_sec(&(t[i]), t[i].fn), ".3f", " (sec)")
#define put_cycles_us(fn)                                               \
    _putfield(#fn, cycles_to_usec(&(t[i]), t[i].fn), ".0f", " (us)")

static void write_summary(struct thread* t)
{
    int i, j;

    putu(core_i);
    putu(cpu_mhz);

    for (i = 0; i < g.bucket_size; i++) {
        printf("    %03d (us):\t", i+1);
        for (j = 0; j < g.n_threads; j++) {
            printf(" %"PRIu64, t[j].buckets[i]);
        }
        printf("\n");
    }

    put_cycles_us(max_jitter);
    put_cycles_s(runtime);

    if( g.verbose ) {
        put_frc(frc_start);
        put_frc(frc_stop);
    }
}

static void run_expt(struct thread* threads, int runtime_secs)
{
    int i;

    g.runtime_secs = runtime_secs;
    g.n_threads_started = 0;
    g.n_threads_ready = 0;
    g.n_threads_running = 0;
    g.n_threads_finished = 0;
    g.cmd = WAIT;

    for( i = 0; i < g.n_threads; ++i )
        TEST0(pthread_create(&(threads[i].thread_id), NULL,
                             thread_main, &(threads[i])));
    while( g.n_threads_started != g.n_threads )
        usleep(1000);
    gettimeofday(&g.tv_start, NULL);
    g.cmd = GO;

    alarm(runtime_secs);

    /* Go to sleep until the threads have done their stuff. */
    for( i = 0; i < g.n_threads; ++i )
        pthread_join(threads[i].thread_id, NULL);
}

static void cleanup_expt(struct thread* threads)
{
    int i;
    for( i = 0; i < g.n_threads; ++i ) {
        free(threads[i].buckets);
        threads[i].buckets = NULL;
    }
}

static void handle_alarm(int code)
{
    g.cmd = STOP;
}

static void usage(const char* prog)
{
    fprintf(stderr, "usage:\n");
    fprintf(stderr, "  %s [options]\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "options:\n");
    fprintf(stderr, "  --runtime <seconds>\n");
    fprintf(stderr, "  --cpu-list <CPU-list>\n");
    fprintf(stderr, "  --rtprio <RT-prio>\n");
    fprintf(stderr, "  --verbose\n");
    exit(1);
}

static int cpu_list_is_valid(const char *cpu_list)
{
    const char *p;

    for (p = cpu_list; *p; p++)
        if (!isdigit(*p) && *p != ',')
            return 0;

    return 1;
}

/* TODO: use libnuma? */
static int parse_cpu_list(char *cpu_list, cpu_set_t *cpu_set)
{
    int i, n_cores;
    char *p;

    n_cores = sysconf(_SC_NPROCESSORS_CONF);

    if (!cpu_list) {
        for (i = 0; i < n_cores; i++)
            CPU_SET(i, cpu_set);
        return n_cores;
    }

    if (!cpu_list_is_valid(cpu_list))
        goto error;

    p = strtok(cpu_list, ",");
    while (p) {
        i = atoi(p);
        if (i > n_cores)
            goto error;
        CPU_SET(i, cpu_set);
        p = strtok(NULL, ",");
    }

    return n_cores;

error:
    fprintf(stderr, "ERROR: bad cpu list: %s\n", cpu_list);
    exit(1);
}

int main(int argc, char* argv[])
{
    struct thread* threads;
    const char* app = argv[0];
    char* cpu_list = NULL;
    char dummy;
    int i, n_cores, runtime = 70;
    cpu_set_t cpu_set;

    CPU_ZERO(&cpu_set);
    g.rtprio = -1;
    g.bucket_size = BUCKET_SIZE;

    --argc; ++argv;
    for( ; argc; --argc, ++argv ) {
        if( argv[0][0] != '-' ) {
            break;
        }
        else if( strcmp(argv[0], "--runtime") == 0 && argc > 1 &&
                sscanf(argv[1], "%u%c", &runtime, &dummy) == 1 ) {
            --argc, ++argv;
        }
        else if( strcmp(argv[0], "--verbose") == 0 ) {
            g.verbose = 1;
        }
        else if( strcmp(argv[0], "--cpu-list") == 0 ) {
            cpu_list = argv[1];
            --argc, ++argv;
        }
        else if( strcmp(argv[0], "--bucket-size") == 0 ) {
            sscanf(argv[1], "%i%c", &g.bucket_size, &dummy);
            assert(g.bucket_size > 0 && g.bucket_size <= 1024);
            --argc, ++argv;
        }
        else if( strcmp(argv[0], "--rtprio") == 0 ) {
            g.rtprio = atoi(argv[1]);
            --argc, ++argv;
        } else {
            usage(app);
        }
    }

    n_cores = parse_cpu_list(cpu_list, &cpu_set);

    TEST( threads = calloc(1, CPU_COUNT(&cpu_set) * sizeof(threads[0])) );
    for( i = 0; i < n_cores; ++i )
        if (CPU_ISSET(i, &cpu_set) && move_to_core(i) == 0)
            threads[g.n_threads++].core_i = i;
    TEST(move_to_core(0) == 0);

    signal(SIGALRM, handle_alarm);

    run_expt(threads, 1);
    cleanup_expt(threads);
    run_expt(threads, runtime);
    write_summary(threads);

    return 0;
}
