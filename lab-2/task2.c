#include <mpi.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <limits.h>

/*
 * task2.c
 * Week 8 Lab 2 - Task 2
 * Hybrid prime search using Open MPI + OpenMP.
 *
 * Usage:
 *   mpirun -np <processes> ./task2 <n> <threads> [block|cyclic|weighted] [static|dynamic|guided]
 *
 * Examples:
 *   mpirun -np 4 ./task2 10000000 2
 *   mpirun -np 4 ./task2 10000000 2 cyclic
 *   mpirun -np 4 ./task2 10000000 2 weighted guided
 *
 * The default MPI workload distribution is weighted, default OpenMP
 * schedule is dynamic.
 *
 * OMP_CHUNK_SIZE is fixed to 1024. This was found to be a good compromise between overhead and load balancing,
 * if chunk sise too small, overhead for asking for new work is too high, if chunk size too large, load balacning is poor,
 * after testing n values ranging from 10,000,000 to 100,000,000, 1024 was found to be a good balance and returned the 
 * best performance on average.
 */

#define OMP_CHUNK_SIZE 1024

enum Distribution { DISTRIBUTION_BLOCK = 0, DISTRIBUTION_CYCLIC = 1, DISTRIBUTION_WEIGHTED = 2 };

enum OmpSchedule { OMP_SCHEDULE_STATIC = 0, OMP_SCHEDULE_DYNAMIC = 1, OMP_SCHEDULE_GUIDED = 2 };

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
     * Only odd divisors need to be checked.
     *
     * divisor <= number / divisor is equivalent to:
     *
     * divisor * divisor <= number
     *
     * but avoids possible multiplication overflow.
     */
    for (long divisor = 3; divisor <= number / divisor; divisor += 2) {
        if (number % divisor == 0) {
            return false;
        }
    }

    return true;
}

/*
 * Return the number of prime candidates.
 *
 * 2 is included.
 * Even numbers greater than 2 are excluded.
 */
long total_candidates(long n) {
    if (n <= 2) {
        return 0;
    }

    long odd_count = (n - 2) / 2;

    return 1 + odd_count;
}

/* Convert candidate index back to the actual number. */
long candidate_to_number(long index) {
    if (index == 0) {
        return 2;
    }

    return 2 * index + 1;
}

/*
 * Return a weighted workload boundary.
 *
 * The approximate cost of checking candidate k is sqrt(k).
 * Therefore later ranges contain fewer candidates because
 * larger candidate values generally require more work.
 */
long weighted_boundary(long total, int size, int rank) {
    if (rank <= 0) {
        return 0;
    }

    if (rank >= size) {
        return total;
    }

    double fraction = (double)rank / (double)size;

    double scaled = pow(fraction, 2.0 / 3.0) * (double)total;

    return (long)(scaled + 0.5);
}

/*
 * Block workload distribution.
 *
 * Each MPI process receives one contiguous block.
 * Any remainder is distributed across the first ranks.
 */
void distribute_block(long n, int rank, int size, long *start_index, long *stride,
                      long *end_index) {
    long total = total_candidates(n);

    long block_size = total / size;
    long remainder = total % size;

    long count = block_size + (rank < remainder ? 1 : 0);

    long start = (long)rank * block_size + (rank < remainder ? rank : remainder);

    *start_index = start;
    *stride = 1;
    *end_index = start + count;
}

/*
 * Cyclic workload distribution.
 *
 * For four MPI processes:
 *
 * Rank 0 -> indexes 0, 4, 8, ...
 * Rank 1 -> indexes 1, 5, 9, ...
 * Rank 2 -> indexes 2, 6, 10, ...
 * Rank 3 -> indexes 3, 7, 11, ...
 */
void distribute_cyclic(long n, int rank, int size, long *start_index, long *stride,
                       long *end_index) {
    *start_index = rank;
    *stride = size;
    *end_index = total_candidates(n);
}

/*
 * Weighted contiguous workload distribution.
 *
 * Higher candidate values generally require more divisor tests,
 * so later ranks receive fewer candidate numbers.
 */
void distribute_weighted(long n, int rank, int size, long *start_index, long *stride,
                         long *end_index) {
    long total = total_candidates(n);

    *start_index = weighted_boundary(total, size, rank);

    *stride = 1;

    *end_index = weighted_boundary(total, size, rank + 1);
}

/* Calculate how many candidates belong to this MPI process. */
long count_local_candidates(long start_index, long stride, long end_index) {
    if (start_index >= end_index) {
        return 0;
    }

    return ((end_index - start_index - 1) / stride) + 1;
}

/*
 * Hybrid local prime search.
 *
 * Level 1:
 * MPI assigns part of the global candidate set to this process.
 *
 * Level 2:
 * OpenMP distributes this process's candidates across its threads.
 *
 * Each iteration writes to a unique prime_flags index,
 * so the expensive parallel loop does not require a mutex,
 * critical section or atomic operation.
 */
int find_local_primes_hybrid(long start_index, long stride, long end_index, int thread_count,
                             int omp_schedule, long **local_primes_out,
                             long *local_prime_count_out) {
    long local_candidate_count = count_local_candidates(start_index, stride, end_index);

    *local_primes_out = NULL;
    *local_prime_count_out = 0;

    if (local_candidate_count == 0) {
        return 0;
    }

    bool *prime_flags = calloc((size_t)local_candidate_count, sizeof(bool));

    if (prime_flags == NULL) {
        return 1;
    }

    if (omp_schedule == OMP_SCHEDULE_STATIC) {
        omp_set_schedule(omp_sched_static, OMP_CHUNK_SIZE);
    } else if (omp_schedule == OMP_SCHEDULE_DYNAMIC) {
        omp_set_schedule(omp_sched_dynamic, OMP_CHUNK_SIZE);
    } else {
        omp_set_schedule(omp_sched_guided, OMP_CHUNK_SIZE);
    }

#pragma omp parallel for num_threads(thread_count) schedule(runtime)

    for (long local_index = 0; local_index < local_candidate_count; local_index++) {
        long candidate_index = start_index + local_index * stride;

        long number = candidate_to_number(candidate_index);

        prime_flags[local_index] = is_prime_number(number);
    }

    /*
     * Count this process's prime numbers.
     *
     * This is done after the OpenMP region so the result
     * can be compacted in sorted candidate order.
     */
    long local_prime_count = 0;

    for (long i = 0; i < local_candidate_count; i++) {
        if (prime_flags[i]) {
            local_prime_count++;
        }
    }

    if (local_prime_count == 0) {
        free(prime_flags);
        return 0;
    }

    long *local_primes = malloc((size_t)local_prime_count * sizeof(long));

    if (local_primes == NULL) {
        free(prime_flags);
        return 1;
    }

    /*
     * Convert the boolean result back into an array
     * containing the actual prime values.
     */
    long output_index = 0;

    for (long i = 0; i < local_candidate_count; i++) {
        if (prime_flags[i]) {
            long candidate_index = start_index + i * stride;

            local_primes[output_index++] = candidate_to_number(candidate_index);
        }
    }

    free(prime_flags);

    *local_primes_out = local_primes;
    *local_prime_count_out = local_prime_count;

    return 0;
}

/* Comparator used by qsort. */
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

/* Convert distribution value to a printable name. */
const char *distribution_name(int distribution) {
    if (distribution == DISTRIBUTION_BLOCK) {
        return "block";
    }

    if (distribution == DISTRIBUTION_CYCLIC) {
        return "cyclic";
    }

    return "weighted";
}

/* Convert OpenMP schedule value to a printable name. */
const char *omp_schedule_name(int schedule) {
    if (schedule == OMP_SCHEDULE_STATIC) {
        return "static";
    }

    if (schedule == OMP_SCHEDULE_DYNAMIC) {
        return "dynamic";
    }

    return "guided";
}

int main(int argc, char *argv[]) {
    int provided_thread_level;

    /*
     * MPI_THREAD_FUNNELED means:
     *
     * - OpenMP threads may run in parallel.
     * - Only the main thread performs MPI operations.
     *
     * This is sufficient for this hybrid implementation because
     * all MPI calls occur outside the OpenMP parallel region.
     */
    MPI_Init_thread(&argc, &argv, MPI_THREAD_FUNNELED, &provided_thread_level);

    int rank;
    int size;

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    MPI_Comm_size(MPI_COMM_WORLD, &size);

    /*
     * Make sure the MPI implementation supports
     * the requested threading model.
     */
    if (provided_thread_level < MPI_THREAD_FUNNELED) {
        if (rank == 0) {
            fprintf(stderr,
                    "Error: MPI implementation does not "
                    "provide MPI_THREAD_FUNNELED support\n");
        }

        MPI_Finalize();

        return 1;
    }

    long n = 0;

    int thread_count = 0;

    int distribution = DISTRIBUTION_WEIGHTED;

    int omp_schedule = OMP_SCHEDULE_DYNAMIC;

    /*
     * Only the root MPI process reads and validates
     * the command-line arguments.
     *
     * Arguments:
     *
     * argv[1] = n
     * argv[2] = OpenMP threads per MPI process
     * argv[3] = optional MPI workload distribution
     * argv[4] = optional OpenMP schedule type
     */
    if (rank == 0) {
        if (argc < 3 || argc > 5) {
            fprintf(stderr,
                    "Usage: %s <n> <threads> "
                    "[block|cyclic|weighted] [static|dynamic|guided]\n",
                    argv[0]);

            n = -1;
        }

        else {
            char *n_end_ptr;
            char *thread_end_ptr;

            n = strtol(argv[1], &n_end_ptr, 10);

            long requested_threads = strtol(argv[2], &thread_end_ptr, 10);

            if (*n_end_ptr != '\0') {
                fprintf(stderr,
                        "Error: n must be an integer "
                        "(got \"%s\")\n",
                        argv[1]);

                n = -1;
            }

            else if (n < 2) {
                fprintf(stderr, "Error: n must be >= 2\n");

                n = -1;
            }

            else if (n > 100000000) {
                fprintf(stderr,
                        "Error: n is too large "
                        "(max 100,000,000)\n");

                n = -1;
            }

            else if (*thread_end_ptr != '\0' || requested_threads < 1 ||
                     requested_threads > INT_MAX) {
                fprintf(stderr,
                        "Error: threads must be "
                        "a positive integer\n");

                n = -1;
            }

            else {
                thread_count = (int)requested_threads;
            }
        }

        /*
         * Optional MPI workload distribution.
         */
        if (n >= 0 && argc >= 4) {
            if (strcmp(argv[3], "block") == 0) {
                distribution = DISTRIBUTION_BLOCK;
            }

            else if (strcmp(argv[3], "cyclic") == 0) {
                distribution = DISTRIBUTION_CYCLIC;
            }

            else if (strcmp(argv[3], "weighted") == 0) {
                distribution = DISTRIBUTION_WEIGHTED;
            }

            else {
                fprintf(stderr,
                        "Error: distribution must be "
                        "block, cyclic, or weighted\n");

                n = -1;
            }
        }

        /*
         * Optional OpenMP schedule type.
         */
        if (n >= 0 && argc >= 5) {
            if (strcmp(argv[4], "static") == 0) {
                omp_schedule = OMP_SCHEDULE_STATIC;
            }

            else if (strcmp(argv[4], "dynamic") == 0) {
                omp_schedule = OMP_SCHEDULE_DYNAMIC;
            }

            else if (strcmp(argv[4], "guided") == 0) {
                omp_schedule = OMP_SCHEDULE_GUIDED;
            }

            else {
                fprintf(stderr,
                        "Error: omp_schedule must be "
                        "static, dynamic, or guided\n");

                n = -1;
            }
        }
    }

    /*
     * Start overall timing before communication.
     *
     * Therefore the total time includes:
     *
     * - broadcasts
     * - computation
     * - gathering
     * - sorting
     * - file writing
     */
    MPI_Barrier(MPI_COMM_WORLD);

    double total_start = MPI_Wtime();

    /*
     * Root disseminates n, thread count and workload
     * distribution to every MPI process.
     */
    MPI_Bcast(&n, 1, MPI_LONG, 0, MPI_COMM_WORLD);

    MPI_Bcast(&thread_count, 1, MPI_INT, 0, MPI_COMM_WORLD);

    MPI_Bcast(&distribution, 1, MPI_INT, 0, MPI_COMM_WORLD);

    MPI_Bcast(&omp_schedule, 1, MPI_INT, 0, MPI_COMM_WORLD);

    if (n < 0) {
        MPI_Finalize();

        return 1;
    }

    /*
     * Prevent OpenMP from automatically reducing
     * the requested thread count.
     */
    omp_set_dynamic(0);

    omp_set_num_threads(thread_count);

    /*
     * MPI-level workload distribution.
     */
    long start_index;
    long stride;
    long end_index;

    if (distribution == DISTRIBUTION_BLOCK) {
        distribute_block(n, rank, size, &start_index, &stride, &end_index);
    }

    else if (distribution == DISTRIBUTION_CYCLIC) {
        distribute_cyclic(n, rank, size, &start_index, &stride, &end_index);
    }

    else {
        distribute_weighted(n, rank, size, &start_index, &stride, &end_index);
    }

    /*
     * HYBRID PARALLEL COMPUTATION
     *
     * MPI:
     *     splits work between processes.
     *
     * OpenMP:
     *     splits each process's work between threads.
     */
    double computation_start = MPI_Wtime();

    long *local_primes = NULL;

    long local_prime_count = 0;

    int local_error = find_local_primes_hybrid(start_index, stride, end_index, thread_count,
                                               omp_schedule, &local_primes, &local_prime_count);

    double computation_end = MPI_Wtime();

    double local_computation_time = computation_end - computation_start;

    /*
     * If one process fails to allocate memory,
     * every MPI process must terminate together.
     */
    int any_error = 0;

    MPI_Allreduce(&local_error, &any_error, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);

    if (any_error) {
        if (rank == 0) {
            fprintf(stderr,
                    "Error: memory allocation failed "
                    "on an MPI process\n");
        }

        free(local_primes);

        MPI_Finalize();

        return 1;
    }

    /*
     * Parallel computation is limited by the slowest MPI process.
     *
     * Therefore the maximum process computation time is the
     * meaningful computation wall-clock time.
     */
    double max_computation_time = 0.0;

    MPI_Reduce(&local_computation_time, &max_computation_time, 1, MPI_DOUBLE, MPI_MAX, 0,
               MPI_COMM_WORLD);

    /*
     * Root first collects how many primes each process found.
     */
    long *counts = NULL;

    if (rank == 0) {
        counts = malloc((size_t)size * sizeof(long));
    }

    int root_allocation_error = (rank == 0 && counts == NULL) ? 1 : 0;

    MPI_Bcast(&root_allocation_error, 1, MPI_INT, 0, MPI_COMM_WORLD);

    if (root_allocation_error) {
        if (rank == 0) {
            fprintf(stderr,
                    "Error: memory allocation failed "
                    "on root process\n");
        }

        free(local_primes);

        MPI_Finalize();

        return 1;
    }

    MPI_Gather(&local_prime_count, 1, MPI_LONG,

               counts, 1, MPI_LONG,

               0, MPI_COMM_WORLD);

    /*
     * Root prepares MPI_Gatherv.
     *
     * MPI_Gatherv is needed because each MPI process can
     * find a different number of prime numbers.
     */
    long total_prime_count = 0;

    long *all_primes = NULL;

    int *recv_counts = NULL;

    int *displacements = NULL;

    root_allocation_error = 0;

    if (rank == 0) {
        recv_counts = malloc((size_t)size * sizeof(int));

        displacements = malloc((size_t)size * sizeof(int));

        if (recv_counts == NULL || displacements == NULL) {
            root_allocation_error = 1;
        }

        else {
            long displacement = 0;

            for (int i = 0; i < size; i++) {
                if (counts[i] > INT_MAX || displacement > INT_MAX) {
                    root_allocation_error = 1;

                    break;
                }

                recv_counts[i] = (int)counts[i];

                displacements[i] = (int)displacement;

                displacement += counts[i];
            }

            total_prime_count = displacement;

            if (total_prime_count > INT_MAX) {
                root_allocation_error = 1;
            }

            else if (!root_allocation_error && total_prime_count > 0) {
                all_primes = malloc((size_t)total_prime_count * sizeof(long));

                if (all_primes == NULL) {
                    root_allocation_error = 1;
                }
            }
        }
    }

    MPI_Bcast(&root_allocation_error, 1, MPI_INT, 0, MPI_COMM_WORLD);

    if (root_allocation_error) {
        if (rank == 0) {
            fprintf(stderr,
                    "Error: root could not prepare "
                    "the gathered result\n");
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
     * Use a dummy buffer if a process found zero primes.
     */
    long dummy_value = 0;

    long *send_buffer = local_prime_count > 0 ? local_primes : &dummy_value;

    /*
     * Gather all prime arrays onto root.
     */
    MPI_Gatherv(send_buffer, (int)local_prime_count, MPI_LONG,

                all_primes, recv_counts, displacements, MPI_LONG,

                0, MPI_COMM_WORLD);

    free(local_primes);

    /*
     * Only the main thread of root reaches this section
     * and produces the final output.
     */
    if (rank == 0) {
        /*
         * Block and weighted partitioning use contiguous
         * increasing ranges.
         *
         * Therefore rank-order Gatherv already produces
         * sorted output.
         *
         * Cyclic partitioning interleaves candidate values,
         * so the final array must be sorted.
         */
        if (distribution == DISTRIBUTION_CYCLIC && total_prime_count > 1) {
            qsort(all_primes, (size_t)total_prime_count, sizeof(long), compare_longs);
        }

        /*
         * Task 2 requires the root process to write
         * the final result to a text file.
         */
        const char *filename = "primes.txt";

        print_to_file(all_primes, total_prime_count, filename);

        /*
         * Small inputs are also printed to the terminal
         * for easy correctness testing.
         */
        if (n < 100) {
            printf(
                "Prime numbers strictly less "
                "than %ld are:\n",
                n);

            for (long i = 0; i < total_prime_count; i++) {
                printf("%ld%s", all_primes[i], i == total_prime_count - 1 ? "\n" : " ");
            }

            if (total_prime_count == 0) {
                printf("None\n");
            }
        }
    }

    /*
     * Wait until root has finished sorting and file output.
     *
     * This means total runtime includes those serial stages.
     */
    MPI_Barrier(MPI_COMM_WORLD);

    double local_total_time = MPI_Wtime() - total_start;

    double max_total_time = 0.0;

    MPI_Reduce(&local_total_time, &max_total_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);

    /*
     * Root prints the configuration and timing results.
     */
    if (rank == 0) {
        printf("MPI processes: %d\n", size);

        printf("OpenMP threads per process: %d\n", thread_count);

        printf("Total worker threads: %lld\n", (long long)size * (long long)thread_count);

        printf("Distribution: %s\n", distribution_name(distribution));

        printf("OMP schedule: %s (chunk=%d)\n", omp_schedule_name(omp_schedule), OMP_CHUNK_SIZE);

        printf("Primes found: %ld\n", total_prime_count);

        printf(
            "Computation time taken: "
            "%f seconds\n",
            max_computation_time);

        printf(
            "Total time taken: "
            "%f seconds\n",
            max_total_time);
    }

    free(counts);
    free(recv_counts);
    free(displacements);
    free(all_primes);

    MPI_Finalize();

    return 0;
}