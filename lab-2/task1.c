#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>


long weighted_boundary(long total, int size, int r);
/*
 * task1.c
 * Single-threaded prime finder (Week 8, Lab 2 base).
 * - `is_prime(input_number, &is_prime_count)` returns a dynamically
 *   allocated array of primes less than `input_number` and sets
 *   `is_prime_count` to the number of primes found.
 * - `print_to_file` writes the primes into `primes.txt` (overwrites).
 */

/* Print the prime list to `filename`. Overwrites existing file. */
void print_to_file(long *primes, long prime_count, const char *filename) {
    FILE *output_file = fopen(filename, "w");

    fprintf(output_file, "Prime numbers found (%ld total):\n", prime_count);
    for (long i = 0; i < prime_count; i++) {
        fprintf(output_file, "%ld", primes[i]);
        if (i != prime_count - 1) fprintf(output_file, ", ");
    }
    fprintf(output_file, "\n");

    fclose(output_file);
    printf("Results written to %s\n", filename);
}

long *is_prime(long input_number, long *is_prime_count) {
    /* allocate the boolean array on the heap so large inputs don't overflow stack */
    bool *prime_number_array = calloc((size_t)input_number, sizeof *prime_number_array);
    if (prime_number_array == NULL) {
        fprintf(stderr, "Memory allocation failed for prime array\n");
        *is_prime_count = 0;
        return NULL;
    }

    long count = 0;
    
    if (input_number > 2) {
        prime_number_array[2] = true;
        count++;
    }

    for (long number = 3; number < input_number; number += 2){
        bool is_prime = true;
        for (long divisor = 3; divisor * divisor <= number; divisor += 2){
            if (number % divisor == 0){
                is_prime = false;
                break;
            }
        }
        prime_number_array[number] = is_prime;
        if (is_prime) count++;
    }

    if (count == 0) {
        free(prime_number_array);
        *is_prime_count = 0;
        return NULL;
    }

    long *primes = malloc(sizeof(long) * count);
    if (primes == NULL) {
        fprintf(stderr, "Memory allocation failed for primes array\n");
        free(prime_number_array);
        *is_prime_count = 0;
        return NULL;
    }

    long index = 0;
    for (long i = 0; i < input_number; i++) {
        if (prime_number_array[i] == 1) {
            primes[index++] = i;
        }
    }

    free(prime_number_array);
    *is_prime_count = count;
    return primes;
}

/* returns total number of prime candidates, evens filtered out */
long total_candidates(long n) {
    if (n <= 2){
        return 0;
    }
    long odd_count = (n - 2) / 2; // count of odd integers in [3, n)
    return 1 + odd_count;
}

/* Maps candidate index back to actual number */
long candidate_to_number(long index) {
    if (index == 0) {
        return 2;
    }
    return 2 * index + 1;
}

/*
 * Workload distribution strategies. We test for which one is the best.
 */

/* Contiguous block allocatoin, where each process gets equal sized block of the work*/
void distribute_block(long n, int rank, int size, long *start_index, long *stride, long *end_index) {
    long total = total_candidates(n);
    long block_size = total / size;
    long extra = total - block_size * size;

    long start = 0;
    long count = 0;
    if (rank == 0) {
        count = block_size + extra; // we dump any remainders onto first process as it gets the lowest numbers
    } else {
        start = (long)rank * block_size + extra ;
        count = block_size;
    }

    *start_index = start;
    *stride = 1;
    *end_index = start + count;
}

/* Cyclic allocation */
void distribute_cyclic(long n, int rank, int size, long *start_index, long *stride, long *end_index) {
    *start_index = rank;
    *stride = size;
    *end_index = total_candidates(n);
}

/* Estimate workload to check if a number, k, is prime to be sqrt(k) 
We then split blocks by cost instead of by size*/
void distribute_weighted(long n, int rank, int size, long *start_index, long *stride, long *end_index) {
    long total = total_candidates(n);
    *start_index = weighted_boundary(total, size, rank);
    *stride = 1;
    *end_index = weighted_boundary(total, size, rank + 1);
}

/* returns the weighted boundary for a given rank */
long weighted_boundary(long total, int size, int r) {
    if (r <= 0) {
        return 0;
    }
    if (r >= size) {
        return total;
    }

    // given we estimate the cost of checking a number k for primality to be sqrt(k), we can use the integral of sqrt(x) 
    // to estimate the total cost of checking all numbers up to n.
    double fraction = (double)r / (double)size;
    double scaled = pow(fraction, 2.0 / 3.0) * (double)total;
    return (long)(scaled + 0.5); // rounding
}



int main(int argc, char *argv[]) {
    MPI_Init(&argc, &argv);

    int rank;
    int size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    long n = 0;

    if (rank == 0) { // serial
        if (argc != 2) {
            fprintf(stderr, "Incorrect number of arguments. Usage: %s <n>\n", argv[0]);
            n = -1; 
        } else {
            char *end_ptr;
            n = strtol(argv[1], &end_ptr, 10); // convert input string to long in base 10
            if (*end_ptr != '\0') { // makes sure entire string was a number
                fprintf(stderr, "Error: n must be an integer (got \"%s\")\n", argv[1]);
                n = -1;
            } else if (n < 2) {
                fprintf(stderr, "Error: n must be >= 2 (got \"%s\")\n", argv[1]);
                n = -1;
            } else if (n > 100000000) {
                fprintf(stderr, "Error: n is too large (max 100,000,000)\n");
                n = -1;
            }
        }
    }

    MPI_Bcast(&n, 1, MPI_LONG, 0, MPI_COMM_WORLD);

    if (n < 0) {
        MPI_Finalize();
        return 1;
    }

    printf("Rank %d of %d received n = %ld\n", rank, size, n);

    MPI_Finalize();
    return 0;
}