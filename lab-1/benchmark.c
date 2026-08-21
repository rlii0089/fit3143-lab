/*
Benchmark harness for comparing:
  - task1: serial
  - task2: pthreads (num_threads = 2, 4, 8)
  - task3: OpenMP static (num_threads = 2, 4, 8)
  - task3: OpenMP dynamic (num_threads = 2, 4, 8)

n values are generated with a fixed step increment within each of
three ranges (not a fixed point count), inclusive of both ends:
  100,000     ->   1,000,000   step     50,000
  1,000,000   ->  10,000,000   step    500,000
  10,000,000  ->  50,000,000   step  5,000,000
Shared range boundaries (1,000,000 and 10,000,000) are only counted
once, so no n value is repeated.

Only *computation* time is measured (array allocation + prime-finding
+ counting) -- no file I/O or printing of results is included in the
timed region, matching "comp_time" in the original task1/2/3.c files.

Writes results.csv with columns: n,config,threads,time_seconds

Progress is printed to stdout after every single trial so you can
tell the program is alive during a long run.
*/

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <omp.h>
#include <stdbool.h>

/* ---------------- Configuration ---------------- */
#define MAX_N_VALUES 60
static const int THREAD_COUNTS[] = {2, 4, 8};
#define NUM_THREAD_COUNTS 3

/* Builds the step-based n distribution described above.
   Returns the number of values written into out[]. */
static int build_n_values(long *out) {
    long range_starts[] = {100000L,   1000000L,  10000000L};
    long range_ends[]   = {1000000L,  10000000L, 50000000L};
    long range_steps[]  = {50000L,    500000L,   5000000L};
    int count = 0;
    long last = -1;

    for (int r = 0; r < 3; r++) {
        for (long val = range_starts[r]; val <= range_ends[r]; val += range_steps[r]) {
            if (val == last) continue; /* skip repeated range boundary */
            out[count++] = val;
            last = val;
        }
    }
    return count;
}

static double elapsed_seconds(struct timespec t0, struct timespec t1) {
    double dt = (double)(t1.tv_sec - t0.tv_sec) * 1e9;
    dt += (double)(t1.tv_nsec - t0.tv_nsec);
    return dt * 1e-9;
}

/* ================= Serial (task1 style) ================= */
static double run_serial(long k) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    volatile _Bool *primeArray = calloc((size_t)k, sizeof *primeArray);
    long count = 0;
    for (long i = 2; i < k; i++) {
        _Bool isPrime = 1;
        for (long j = 2; j * j <= i; j++) {
            if (i % j == 0) { isPrime = 0; break; }
        }
        if (isPrime) { primeArray[i] = 1; count++; }
    }

    /* Extraction step -- present in task1.c's is_prime and timed as
       part of comp_time there, so it must be timed here too. */
    long *primes = NULL;
    if (count > 0) {
        primes = malloc(sizeof(long) * (size_t)count);
        long idx = 0;
        for (long i = 0; i < k; i++) {
            if (primeArray[i] == 1) primes[idx++] = i;
        }
    }
    free((void *)primeArray);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = elapsed_seconds(t0, t1);
    free(primes); /* matches main() freeing primes *after* comp_time in task1.c */
    return dt;
}

/* ================= Pthreads (task2 style, generalised thread count) ================= */
typedef struct {
    long start_offset;
    long upper_limit;
    int stride;
    volatile _Bool *arr;
    long local_count;
} PThreadArg;

static void *pthread_worker(void *arg_) {
    PThreadArg *arg = (PThreadArg *)arg_;
    long local_count = 0;
    for (long number = arg->start_offset; number < arg->upper_limit; number += arg->stride) {
        _Bool is_prime = 1;
        for (long divisor = 2; divisor * divisor <= number; divisor++) {
            if (number % divisor == 0) { is_prime = 0; break; }
        }
        arg->arr[number] = is_prime;
        if (is_prime) local_count++;
    }
    arg->local_count = local_count;
    return NULL;
}

static double run_pthread(long k, int num_threads) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    volatile _Bool *arr = calloc((size_t)k, sizeof *arr);
    pthread_t *ids = malloc(sizeof(pthread_t) * (size_t)num_threads);
    PThreadArg *args = malloc(sizeof(PThreadArg) * (size_t)num_threads);

    for (int t = 0; t < num_threads; t++) {
        args[t].start_offset = 2 + t;
        args[t].upper_limit = k;
        args[t].stride = num_threads;
        args[t].arr = arr;
        args[t].local_count = 0;
        pthread_create(&ids[t], NULL, pthread_worker, &args[t]);
    }
    long count = 0;
    for (int t = 0; t < num_threads; t++) {
        pthread_join(ids[t], NULL);
        count += args[t].local_count;
    }
    free(ids); free(args);

    /* Extraction step -- present in task2.c's is_prime (single-threaded,
       main-thread-only pass) and timed as part of computation_time there,
       so it must be timed here too. */
    long *primes = NULL;
    if (count > 0) {
        primes = malloc(sizeof(long) * (size_t)count);
        long idx = 0;
        for (long i = 0; i < k; i++) {
            if (arr[i] == 1) primes[idx++] = i;
        }
    }
    free((void *)arr);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = elapsed_seconds(t0, t1);
    free(primes); /* matches main() freeing primes *after* computation_time in task2.c */
    return dt;
}

/* ================= OpenMP (task3 style, static or dynamic via runtime schedule) ================= */
static double run_omp(long k, int num_threads, int use_dynamic) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    volatile _Bool *primeArray = calloc((size_t)k, sizeof *primeArray);
    omp_set_num_threads(num_threads);
    if (use_dynamic) omp_set_schedule(omp_sched_dynamic, 1);
    else              omp_set_schedule(omp_sched_static, 1);

    long count = 0;
    #pragma omp parallel for schedule(runtime) reduction(+:count)
    for (long i = 2; i < k; i++) {
        _Bool isPrime = 1;
        for (long j = 2; j * j <= i; j++) {
            if (i % j == 0) { isPrime = 0; break; }
        }
        if (isPrime) { primeArray[i] = 1; count++; }
    }

    /* Extraction step -- present in task3.c's is_prime and timed as
       part of comp_time there, so it must be timed here too. */
    long *primes = NULL;
    if (count > 0) {
        primes = malloc(sizeof(long) * (size_t)count);
        long idx = 0;
        for (long i = 0; i < k; i++) {
            if (primeArray[i] == 1) primes[idx++] = i;
        }
    }
    free((void *)primeArray);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = elapsed_seconds(t0, t1);
    free(primes); /* matches main() freeing primes *after* comp_time in task3.c */
    return dt;
}

int main(void) {
    long n_values[MAX_N_VALUES];
    int num_n = build_n_values(n_values);

    FILE *f = fopen("results.csv", "w");
    if (!f) { fprintf(stderr, "Could not open results.csv for writing\n"); return 1; }
    fprintf(f, "n,config,threads,time\n");

    int total_trials = num_n * (1 + NUM_THREAD_COUNTS * 3);
    int trial = 0;

    for (int i = 0; i < num_n; i++) {
        long n = n_values[i];

        double t = run_serial(n);
        trial++;
        fprintf(f, "%ld,serial,1,%f\n", n, t); fflush(f);
        printf("[%d/%d] n=%ld serial          -> %.4f s\n", trial, total_trials, n, t);
        fflush(stdout);

        for (int tc = 0; tc < NUM_THREAD_COUNTS; tc++) {
            int nt = THREAD_COUNTS[tc];

            double tp = run_pthread(n, nt);
            trial++;
            fprintf(f, "%ld,pthread,%d,%f\n", n, nt, tp); fflush(f);
            printf("[%d/%d] n=%ld pthread threads=%d -> %.4f s\n", trial, total_trials, n, nt, tp);
            fflush(stdout);

            double ts = run_omp(n, nt, 0);
            trial++;
            fprintf(f, "%ld,omp_static,%d,%f\n", n, nt, ts); fflush(f);
            printf("[%d/%d] n=%ld omp_static threads=%d -> %.4f s\n", trial, total_trials, n, nt, ts);
            fflush(stdout);

            double td = run_omp(n, nt, 1);
            trial++;
            fprintf(f, "%ld,omp_dynamic,%d,%f\n", n, nt, td); fflush(f);
            printf("[%d/%d] n=%ld omp_dynamic threads=%d -> %.4f s\n", trial, total_trials, n, nt, td);
            fflush(stdout);
        }
    }

    fclose(f);
    printf("\nDone. Results written to results.csv\n");
    return 0;
}