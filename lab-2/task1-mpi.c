#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <limits.h>

/*
 * task1.c
 * Week 8 Lab 2 - Task 1
 * Parallel prime search using Open MPI.
 *
 * Usage:
 *   mpirun -np <processes> ./task1 <n> [block|cyclic|weighted]
 *
 * Examples:
 *   mpirun -np 4 ./task1 10000000
 *   mpirun -np 4 ./task1 10000000 cyclic
 *
 * The default workload distribution is weighted.
 */

enum Distribution {
    DISTRIBUTION_BLOCK = 0,
    DISTRIBUTION_CYCLIC = 1,
    DISTRIBUTION_WEIGHTED = 2
};

/* Print the prime list to filename. Overwrites existing file. */
int print_to_file(long *primes, long prime_count, const char *filename) {
    FILE *output_file = fopen(filename, "w");

    if (output_file == NULL) {
        fprintf(stderr, "Error: could not open %s for writing\n", filename);
        return 1;
    }

    fprintf(output_file, "Prime numbers found (%ld total):\n", prime_count);

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
     * Only test odd divisors.
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
 * Returns total number of candidates.
 * Even numbers greater than 2 are not included.
 */
long total_candidates(long n) {
    if (n <= 2) {
        return 0;
    }

    long odd_count = (n - 2) / 2;

    /* +1 includes the prime candidate 2. */
    return 1 + odd_count;
}


/* Maps a candidate index back to the actual integer. */
long candidate_to_number(long index) {
    if (index == 0) {
        return 2;
    }

    return 2 * index + 1;
}


/*
 * Returns a weighted boundary.
 *
 * The approximate cost of checking number k is sqrt(k).
 * Therefore cumulative work grows approximately as x^(3/2).
 * Solving for equal work gives a boundary proportional to x^(2/3).
 */
long weighted_boundary(long total, int size, int rank) {
    if (rank <= 0) {
        return 0;
    }

    if (rank >= size) {
        return total;
    }

    double fraction = (double)rank / (double)size;

    double scaled =
        pow(fraction, 2.0 / 3.0) * (double)total;

    return (long)(scaled + 0.5);
}


/*
 * Block allocation.
 *
 * Each process receives a contiguous range of candidates.
 * Any remainder is distributed across the first processes.
 */
void distribute_block(long n,
                      int rank,
                      int size,
                      long *start_index,
                      long *stride,
                      long *end_index) {

    long total = total_candidates(n);

    long block_size = total / size;
    long remainder = total % size;

    long count =
        block_size + (rank < remainder ? 1 : 0);

    long start =
        (long)rank * block_size
        + (rank < remainder ? rank : remainder);

    *start_index = start;
    *stride = 1;
    *end_index = start + count;
}


/*
 * Cyclic allocation.
 *
 * Example with four processes:
 *
 * Rank 0 -> candidate indexes 0, 4, 8, ...
 * Rank 1 -> candidate indexes 1, 5, 9, ...
 * Rank 2 -> candidate indexes 2, 6, 10, ...
 * Rank 3 -> candidate indexes 3, 7, 11, ...
 */
void distribute_cyclic(long n,
                       int rank,
                       int size,
                       long *start_index,
                       long *stride,
                       long *end_index) {

    *start_index = rank;
    *stride = size;
    *end_index = total_candidates(n);
}


/*
 * Weighted contiguous allocation.
 *
 * Higher candidate values generally require more divisor tests,
 * so later processes receive fewer candidate values.
 */
void distribute_weighted(long n,
                         int rank,
                         int size,
                         long *start_index,
                         long *stride,
                         long *end_index) {

    long total = total_candidates(n);

    *start_index =
        weighted_boundary(total, size, rank);

    *stride = 1;

    *end_index =
        weighted_boundary(total, size, rank + 1);
}


/*
 * Search only the candidates allocated to this MPI process.
 *
 * The local array grows dynamically instead of allocating an
 * n-sized boolean array on every process.
 */
long *find_local_primes(long start_index,
                        long stride,
                        long end_index,
                        long *local_prime_count) {

    long capacity = 1024;
    long count = 0;

    long *local_primes =
        malloc((size_t)capacity * sizeof(long));

    if (local_primes == NULL) {
        *local_prime_count = 0;
        return NULL;
    }

    for (long index = start_index;
         index < end_index;
         index += stride) {

        long number =
            candidate_to_number(index);

        if (is_prime_number(number)) {

            if (count == capacity) {

                long new_capacity =
                    capacity * 2;

                long *resized =
                    realloc(
                        local_primes,
                        (size_t)new_capacity
                        * sizeof(long)
                    );

                if (resized == NULL) {
                    free(local_primes);

                    *local_prime_count = 0;

                    return NULL;
                }

                local_primes = resized;
                capacity = new_capacity;
            }

            local_primes[count++] = number;
        }
    }

    *local_prime_count = count;

    return local_primes;
}


/* Comparator used by qsort for cyclic distribution. */
int compare_longs(const void *a, const void *b) {
    long value_a = *(const long *)a;
    long value_b = *(const long *)b;

    if (value_a < value_b) {
        return -1;
    }

    if (value_a > value_b) {
        return 1;
    }

    return 0;
}


/* Convert distribution enum to printable name. */
const char *distribution_name(int distribution) {
    if (distribution == DISTRIBUTION_BLOCK) {
        return "block";
    }

    if (distribution == DISTRIBUTION_CYCLIC) {
        return "cyclic";
    }

    return "weighted";
}


int main(int argc, char *argv[]) {

    MPI_Init(&argc, &argv);

    int rank;
    int size;

    MPI_Comm_rank(
        MPI_COMM_WORLD,
        &rank
    );

    MPI_Comm_size(
        MPI_COMM_WORLD,
        &size
    );

    long n = 0;

    /*
     * Weighted distribution is the default.
     */
    int distribution =
        DISTRIBUTION_WEIGHTED;


    /*
     * Only root process reads the command-line argument.
     */
    if (rank == 0) {

        if (argc < 2 || argc > 3) {

            fprintf(
                stderr,
                "Usage: %s <n> [block|cyclic|weighted]\n",
                argv[0]
            );

            n = -1;
        }

        else {

            char *end_ptr;

            n =
                strtol(
                    argv[1],
                    &end_ptr,
                    10
                );


            if (*end_ptr != '\0') {

                fprintf(
                    stderr,
                    "Error: n must be an integer "
                    "(got \"%s\")\n",
                    argv[1]
                );

                n = -1;
            }

            else if (n < 2) {

                fprintf(
                    stderr,
                    "Error: n must be >= 2\n"
                );

                n = -1;
            }

            else if (n > 100000000) {

                fprintf(
                    stderr,
                    "Error: n is too large "
                    "(max 100,000,000)\n"
                );

                n = -1;
            }
        }


        /*
         * Optional workload-distribution argument.
         */
        if (n >= 0 && argc == 3) {

            if (
                strcmp(argv[2], "block") == 0
            ) {

                distribution =
                    DISTRIBUTION_BLOCK;
            }

            else if (
                strcmp(argv[2], "cyclic") == 0
            ) {

                distribution =
                    DISTRIBUTION_CYCLIC;
            }

            else if (
                strcmp(argv[2], "weighted") == 0
            ) {

                distribution =
                    DISTRIBUTION_WEIGHTED;
            }

            else {

                fprintf(
                    stderr,
                    "Error: distribution must be "
                    "block, cyclic, or weighted\n"
                );

                n = -1;
            }
        }
    }


    /*
     * Start timing before communication so total time contains
     * communication as required for Task 1 performance analysis.
     */
    MPI_Barrier(MPI_COMM_WORLD);

    double total_start =
        MPI_Wtime();


    /*
     * Root disseminates n and the selected workload strategy.
     */
    MPI_Bcast(
        &n,
        1,
        MPI_LONG,
        0,
        MPI_COMM_WORLD
    );

    MPI_Bcast(
        &distribution,
        1,
        MPI_INT,
        0,
        MPI_COMM_WORLD
    );


    if (n < 0) {
        MPI_Finalize();
        return 1;
    }


    /*
     * Determine the work assigned to this MPI process.
     */
    long start_index;
    long stride;
    long end_index;


    if (
        distribution ==
        DISTRIBUTION_BLOCK
    ) {

        distribute_block(
            n,
            rank,
            size,
            &start_index,
            &stride,
            &end_index
        );
    }

    else if (
        distribution ==
        DISTRIBUTION_CYCLIC
    ) {

        distribute_cyclic(
            n,
            rank,
            size,
            &start_index,
            &stride,
            &end_index
        );
    }

    else {

        distribute_weighted(
            n,
            rank,
            size,
            &start_index,
            &stride,
            &end_index
        );
    }


    /*
     * PARALLEL PRIME SEARCH.
     *
     * Every MPI process independently searches its own share.
     */
    double computation_start =
        MPI_Wtime();


    long local_prime_count = 0;

    long *local_primes =
        find_local_primes(
            start_index,
            stride,
            end_index,
            &local_prime_count
        );


    double computation_end =
        MPI_Wtime();


    double local_computation_time =
        computation_end
        - computation_start;


    /*
     * Check whether any process had an allocation failure.
     */
    int local_error =
        (local_primes == NULL)
        ? 1
        : 0;

    int any_error = 0;


    MPI_Allreduce(
        &local_error,
        &any_error,
        1,
        MPI_INT,
        MPI_MAX,
        MPI_COMM_WORLD
    );


    if (any_error) {

        if (rank == 0) {
            fprintf(
                stderr,
                "Error: memory allocation failed "
                "on an MPI process\n"
            );
        }

        free(local_primes);

        MPI_Finalize();

        return 1;
    }


    /*
     * Parallel computation finishes when the slowest process
     * finishes, so use MPI_MAX.
     */
    double max_computation_time = 0.0;


    MPI_Reduce(
        &local_computation_time,
        &max_computation_time,
        1,
        MPI_DOUBLE,
        MPI_MAX,
        0,
        MPI_COMM_WORLD
    );


    /*
     * Gather how many primes each process found.
     */
    long *counts = NULL;


    if (rank == 0) {

        counts =
            malloc(
                (size_t)size
                * sizeof(long)
            );
    }


    int root_allocation_error =
        (rank == 0 && counts == NULL)
        ? 1
        : 0;


    MPI_Bcast(
        &root_allocation_error,
        1,
        MPI_INT,
        0,
        MPI_COMM_WORLD
    );


    if (root_allocation_error) {

        if (rank == 0) {
            fprintf(
                stderr,
                "Error: memory allocation failed "
                "on root process\n"
            );
        }

        free(local_primes);

        MPI_Finalize();

        return 1;
    }


    MPI_Gather(
        &local_prime_count,
        1,
        MPI_LONG,

        counts,
        1,
        MPI_LONG,

        0,
        MPI_COMM_WORLD
    );


    /*
     * Root builds the receive counts and displacements needed
     * for MPI_Gatherv.
     */
    long total_prime_count = 0;

    long *all_primes = NULL;

    int *recv_counts = NULL;
    int *displacements = NULL;


    root_allocation_error = 0;


    if (rank == 0) {

        recv_counts =
            malloc(
                (size_t)size
                * sizeof(int)
            );

        displacements =
            malloc(
                (size_t)size
                * sizeof(int)
            );


        if (
            recv_counts == NULL
            || displacements == NULL
        ) {

            root_allocation_error = 1;
        }

        else {

            long displacement = 0;


            for (int i = 0;
                 i < size;
                 i++) {

                if (
                    counts[i] > INT_MAX
                    || displacement > INT_MAX
                ) {

                    root_allocation_error = 1;
                    break;
                }


                recv_counts[i] =
                    (int)counts[i];

                displacements[i] =
                    (int)displacement;


                displacement +=
                    counts[i];
            }


            total_prime_count =
                displacement;


            if (
                total_prime_count > INT_MAX
            ) {

                root_allocation_error = 1;
            }

            else if (
                !root_allocation_error
                && total_prime_count > 0
            ) {

                all_primes =
                    malloc(
                        (size_t)total_prime_count
                        * sizeof(long)
                    );


                if (all_primes == NULL) {

                    root_allocation_error = 1;
                }
            }
        }
    }


    /*
     * Tell all MPI processes whether root is ready for Gatherv.
     */
    MPI_Bcast(
        &root_allocation_error,
        1,
        MPI_INT,
        0,
        MPI_COMM_WORLD
    );


    if (root_allocation_error) {

        if (rank == 0) {

            fprintf(
                stderr,
                "Error: root could not prepare "
                "the gathered result\n"
            );
        }


        free(local_primes);

        free(counts);

        free(recv_counts);

        free(displacements);

        free(all_primes);


        MPI_Finalize();

        return 1;
    }


    /*
     * Each MPI process may have found a different number of primes,
     * therefore MPI_Gatherv is used rather than MPI_Gather.
     */
    long dummy_value = 0;


    long *send_buffer =
        local_prime_count > 0
        ? local_primes
        : &dummy_value;


    MPI_Gatherv(
        send_buffer,
        (int)local_prime_count,
        MPI_LONG,

        all_primes,
        recv_counts,
        displacements,
        MPI_LONG,

        0,
        MPI_COMM_WORLD
    );


    free(local_primes);


    /*
     * Root now owns the complete result.
     */
    if (rank == 0) {

        /*
         * Block and weighted partitions contain contiguous candidate
         * ranges, so Gatherv already produces globally sorted results.
         *
         * Cyclic distribution interleaves candidates between ranks,
         * therefore the gathered result must be sorted.
         */
        if (
            distribution ==
                DISTRIBUTION_CYCLIC
            && total_prime_count > 1
        ) {

            qsort(
                all_primes,
                (size_t)total_prime_count,
                sizeof(long),
                compare_longs
            );
        }


        /*
         * Task specification requires root to output the final
         * sorted prime list to a text file.
         */
        const char *filename =
            "primes.txt";


        print_to_file(
            all_primes,
            total_prime_count,
            filename
        );


        /*
         * Useful for checking correctness with small n.
         */
        if (n < 100) {

            printf(
                "Prime numbers strictly less "
                "than %ld are:\n",
                n
            );


            for (long i = 0;
                 i < total_prime_count;
                 i++) {

                printf(
                    "%ld%s",
                    all_primes[i],
                    i ==
                        total_prime_count - 1
                        ? "\n"
                        : " "
                );
            }
        }
    }


    /*
     * Root's file output is part of overall Task 1 runtime.
     */
    MPI_Barrier(MPI_COMM_WORLD);


    double local_total_time =
        MPI_Wtime()
        - total_start;


    double max_total_time = 0.0;


    MPI_Reduce(
        &local_total_time,
        &max_total_time,
        1,
        MPI_DOUBLE,
        MPI_MAX,
        0,
        MPI_COMM_WORLD
    );


    if (rank == 0) {

        printf(
            "MPI processes: %d\n",
            size
        );

        printf(
            "Distribution: %s\n",
            distribution_name(
                distribution
            )
        );

        printf(
            "Primes found: %ld\n",
            total_prime_count
        );

        printf(
            "Computation time taken: "
            "%f seconds\n",
            max_computation_time
        );

        printf(
            "Total time taken: "
            "%f seconds\n",
            max_total_time
        );
    }


    /*
     * Root-only pointers are NULL on the other processes,
     * and free(NULL) is safe.
     */
    free(counts);
    free(recv_counts);
    free(displacements);
    free(all_primes);


    MPI_Finalize();

    return 0;
}