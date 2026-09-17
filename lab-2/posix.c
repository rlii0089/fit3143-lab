#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <limits.h>

/*
 * task2.c
 * Week 4 Lab 1 - Task 2
 * Parallel prime search using POSIX Threads.
 *
 * Usage:
 *   ./task2 <n> <threads>
 *
 * Example:
 *   ./task2 10000000 8
 */

typedef struct {
    int thread_number;
    int thread_count;
    long total_candidates;
    bool *prime_flags;
} ThreadData;


/* Print the prime list to filename. Overwrites existing file. */
int print_to_file(long *primes, long prime_count, const char *filename) {
    FILE *output_file = fopen(filename, "w");

    if (output_file == NULL) {
        fprintf(stderr, "Error: could not open %s for writing\n", filename);
        return 1;
    }

    fprintf(output_file,
            "Prime numbers found (%ld total):\n",
            prime_count);

    for (long i = 0; i < prime_count; i++) {
        fprintf(output_file, "%ld", primes[i]);

        if (i != prime_count - 1) {
            fprintf(output_file, ", ");
        }
    }

    fprintf(output_file, "\n");
    fclose(output_file);

    printf("Results written to %s\n", filename);

    return 0;
}


/* Check whether one number is prime. */
bool is_prime_number(long number) {
    if (number == 2) {
        return true;
    }

    if (number < 2 || number % 2 == 0) {
        return false;
    }

    /*
     * Only odd divisors need to be tested.
     * divisor <= number / divisor is equivalent to
     * divisor * divisor <= number, but avoids multiplication overflow.
     */
    for (long divisor = 3;
         divisor <= number / divisor;
         divisor += 2) {

        if (number % divisor == 0) {
            return false;
        }
    }

    return true;
}


/*
 * Return the number of prime candidates.
 *
 * Candidate 0 is 2.
 * All remaining candidates are odd numbers starting from 3.
 * Even numbers greater than 2 are skipped completely.
 */
long total_candidates(long n) {
    if (n <= 2) {
        return 0;
    }

    long odd_count = (n - 2) / 2;

    return 1 + odd_count;
}


/* Convert a candidate index back to the actual number. */
long candidate_to_number(long index) {
    if (index == 0) {
        return 2;
    }

    return 2 * index + 1;
}


/*
 * Function executed by every POSIX worker thread.
 *
 * Cyclic workload distribution is used:
 *
 * Thread 0 -> candidate indexes 0, T, 2T, ...
 * Thread 1 -> candidate indexes 1, T+1, 2T+1, ...
 * ...
 *
 * where T is the total number of threads.
 *
 * Each thread writes to different prime_flags indexes, so no mutex is
 * required for the prime-search loop.
 */
void *find_primes_for_thread(void *thread_information) {
    ThreadData *data = (ThreadData *)thread_information;

    for (long candidate_index = data->thread_number;
         candidate_index < data->total_candidates;
         candidate_index += data->thread_count) {

        long number =
            candidate_to_number(candidate_index);

        data->prime_flags[candidate_index] =
            is_prime_number(number);
    }

    return NULL;
}


/*
 * Find all prime numbers strictly less than input_number
 * using POSIX Threads.
 *
 * The expensive prime checks are performed in parallel.
 * After all worker threads finish, the main thread scans
 * the flag array in increasing candidate order so the final
 * prime array is automatically sorted.
 */
long *find_primes_posix(long input_number,
                        int thread_count,
                        long *prime_count_out) {

    long candidate_count =
        total_candidates(input_number);

    *prime_count_out = 0;

    if (candidate_count == 0) {
        return NULL;
    }

    bool *prime_flags =
        calloc(
            (size_t)candidate_count,
            sizeof(bool)
        );

    if (prime_flags == NULL) {
        fprintf(
            stderr,
            "Error: memory allocation failed for prime flags\n"
        );

        return NULL;
    }


    pthread_t *thread_ids =
        malloc(
            (size_t)thread_count
            * sizeof(pthread_t)
        );

    ThreadData *thread_data =
        malloc(
            (size_t)thread_count
            * sizeof(ThreadData)
        );


    if (thread_ids == NULL || thread_data == NULL) {
        fprintf(
            stderr,
            "Error: memory allocation failed for thread data\n"
        );

        free(prime_flags);
        free(thread_ids);
        free(thread_data);

        return NULL;
    }


    int created_threads = 0;


    /*
     * Create all POSIX worker threads.
     */
    for (int thread_index = 0;
         thread_index < thread_count;
         thread_index++) {

        thread_data[thread_index].thread_number =
            thread_index;

        thread_data[thread_index].thread_count =
            thread_count;

        thread_data[thread_index].total_candidates =
            candidate_count;

        thread_data[thread_index].prime_flags =
            prime_flags;


        int create_result =
            pthread_create(
                &thread_ids[thread_index],
                NULL,
                find_primes_for_thread,
                &thread_data[thread_index]
            );


        if (create_result != 0) {
            fprintf(
                stderr,
                "Error: failed to create thread %d\n",
                thread_index
            );

            break;
        }


        created_threads++;
    }


    /*
     * Main thread waits until every successfully-created
     * worker thread has finished.
     */
    for (int thread_index = 0;
         thread_index < created_threads;
         thread_index++) {

        pthread_join(
            thread_ids[thread_index],
            NULL
        );
    }


    if (created_threads != thread_count) {
        free(prime_flags);
        free(thread_ids);
        free(thread_data);

        return NULL;
    }


    /*
     * Count prime numbers after all threads have completed.
     */
    long prime_count = 0;

    for (long i = 0;
         i < candidate_count;
         i++) {

        if (prime_flags[i]) {
            prime_count++;
        }
    }


    if (prime_count == 0) {
        free(prime_flags);
        free(thread_ids);
        free(thread_data);

        return NULL;
    }


    long *primes =
        malloc(
            (size_t)prime_count
            * sizeof(long)
        );


    if (primes == NULL) {
        fprintf(
            stderr,
            "Error: memory allocation failed for primes array\n"
        );

        free(prime_flags);
        free(thread_ids);
        free(thread_data);

        return NULL;
    }


    /*
     * Read candidate indexes from low to high.
     *
     * This keeps the final result sorted without
     * requiring qsort().
     */
    long output_index = 0;


    for (long i = 0;
         i < candidate_count;
         i++) {

        if (prime_flags[i]) {

            primes[output_index++] =
                candidate_to_number(i);
        }
    }


    free(prime_flags);
    free(thread_ids);
    free(thread_data);


    *prime_count_out =
        prime_count;


    return primes;
}


int main(int argc, char *argv[]) {
    struct timespec start;
    struct timespec end;

    struct timespec start_computation;
    struct timespec end_computation;

    double computation_time;
    double total_time;

    long n;
    int thread_count;


    /*
     * Command-line arguments make repeated performance
     * experiments easier.
     *
     * argv[1] = upper limit n
     * argv[2] = number of POSIX threads
     */
    if (argc != 3) {

        fprintf(
            stderr,
            "Usage: %s <n> <threads>\n",
            argv[0]
        );

        return 1;
    }


    char *n_end_ptr;
    char *thread_end_ptr;


    n =
        strtol(
            argv[1],
            &n_end_ptr,
            10
        );


    long requested_threads =
        strtol(
            argv[2],
            &thread_end_ptr,
            10
        );


    if (*n_end_ptr != '\0') {

        fprintf(
            stderr,
            "Error: n must be an integer (got \"%s\")\n",
            argv[1]
        );

        return 1;
    }


    if (n < 2 || n > 100000000) {

        fprintf(
            stderr,
            "Error: n must be within [2, 100000000]\n"
        );

        return 1;
    }


    if (*thread_end_ptr != '\0'
        || requested_threads < 1
        || requested_threads > INT_MAX) {

        fprintf(
            stderr,
            "Error: threads must be a positive integer\n"
        );

        return 1;
    }


    thread_count =
        (int)requested_threads;


    /*
     * Total runtime begins after input validation.
     */
    clock_gettime(
        CLOCK_MONOTONIC,
        &start
    );


    long prime_count = 0;


    /*
     * Measure the POSIX prime-search computation.
     */
    clock_gettime(
        CLOCK_MONOTONIC,
        &start_computation
    );


    long *primes =
        find_primes_posix(
            n,
            thread_count,
            &prime_count
        );


    clock_gettime(
        CLOCK_MONOTONIC,
        &end_computation
    );


    computation_time =
        (end_computation.tv_sec
         - start_computation.tv_sec)
        * 1e9;


    computation_time =
        (
            computation_time
            + (
                end_computation.tv_nsec
                - start_computation.tv_nsec
            )
        )
        * 1e-9;


    if (primes == NULL || prime_count == 0) {

        fprintf(
            stderr,
            "Error: no primes found or prime search failed\n"
        );

        free(primes);

        return 1;
    }


    /*
     * Small input -> terminal.
     * Large input -> text file.
     */
    if (n < 100) {

        printf(
            "Prime numbers strictly less than %ld are:\n",
            n
        );


        for (long i = 0;
             i < prime_count;
             i++) {

            printf(
                "%ld%s",
                primes[i],
                i == prime_count - 1
                    ? "\n"
                    : " "
            );
        }
    }

    else {

        const char *filename =
            "primes.txt";


        if (
            print_to_file(
                primes,
                prime_count,
                filename
            ) != 0
        ) {

            free(primes);

            return 1;
        }
    }


    free(primes);


    clock_gettime(
        CLOCK_MONOTONIC,
        &end
    );


    total_time =
        (end.tv_sec - start.tv_sec)
        * 1e9;


    total_time =
        (
            total_time
            + (
                end.tv_nsec
                - start.tv_nsec
            )
        )
        * 1e-9;


    printf(
        "POSIX threads: %d\n",
        thread_count
    );

    printf(
        "Primes found: %ld\n",
        prime_count
    );

    printf(
        "Computation time taken: %f seconds\n",
        computation_time
    );

    printf(
        "Total time taken: %f seconds\n",
        total_time
    );


    return 0;
}