#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>

#define NUMBER_OF_THREADS 8

// Global variables for all threads to access
long upper_limit;
bool *prime_number_array;
long partial_counts[NUMBER_OF_THREADS]; 

/*
 * task2.c
 * POSIX threads prime finder.
 * - `is_prime(input_number, &is_prime_count)` spawns `NUMBER_OF_THREADS`
 *   threads which mark `prime_number_array` in a cyclic partitioning.
 * - `print_to_file` writes the primes into `primes.txt` (overwrites).
 */

void print_to_file(long *primes, long prime_count, const char *filename) {
    FILE *output_file = fopen(filename, "w");

    fprintf(output_file, "Prime numbers found (%ld total):\n", prime_count);
    for (long i = 0; i < prime_count; i++) {
        fprintf(output_file, "%ld", primes[i]);
        if (i != prime_count - 1) {
            fprintf(output_file, ", ");
        }
    }

    fclose(output_file);
    printf("Results written to %s file\n\n", filename);
}

// Function to be executed by each POSIX thread.
void *find_primes_for_thread(void *thread_information) {
    // Access the thread number passed from main.
    int *thread_number_pointer = (int *)thread_information;
    int thread_number = *thread_number_pointer;

    // Private count variable for each thread
    long local_count = 0;

    // Cyclic partitioning.
    for (long number = 2 + thread_number;
         number < upper_limit;
         number += NUMBER_OF_THREADS)
    {
        bool is_prime = true;
        for (long divisor = 2; divisor * divisor <= number; divisor++) {
            if (number % divisor == 0) {
                is_prime = false;
                break;
            }
        }

        // Update shared array with the result of the prime check.
        prime_number_array[number] = is_prime;
        if (is_prime) {
            local_count++;
        }
    }

    partial_counts[thread_number] = local_count;
    return 0;
}

long *is_prime(long input_number, long *is_prime_count) {
    upper_limit = input_number;
    // Changed the prime array to shared memory accessible by all threads.
    prime_number_array = calloc((size_t)input_number, sizeof *prime_number_array);

    if (prime_number_array == NULL) {
        fprintf(stderr, "Error: Memory allocation failed for prime array.\n");
        *is_prime_count = 0;
        return NULL;
    }

    pthread_t thread_ids[NUMBER_OF_THREADS];
    int thread_numbers[NUMBER_OF_THREADS];
    int thread_index = 0;


    // Fork to create all worker threads.
    for (thread_index = 0; thread_index < NUMBER_OF_THREADS; thread_index++) {
        thread_numbers[thread_index] = thread_index;

        pthread_create(
            &thread_ids[thread_index],
            NULL,
            find_primes_for_thread,
            &thread_numbers[thread_index]
        );
    }

    // Join so main waits until all threads finish.
    for (thread_index = 0; thread_index < NUMBER_OF_THREADS; thread_index++) {
        pthread_join(thread_ids[thread_index], NULL);
    }

    long count = 0;
    for (int t = 0; t < NUMBER_OF_THREADS; t++) {
        count += partial_counts[t];
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
    // Main thread reads array from lowest index to keeps final list sorted.
    for (long i = 0; i < input_number; i++) {
        if (prime_number_array[i] == 1) {
            primes[index++] = i;
        }
    }
    free(prime_number_array);

    *is_prime_count = count;
    return primes;
}

int main(void) {
    struct timespec start, end, start_computation, end_computation;
    double computation_time, total_time;
    long n;

    printf("Number of threads available: %d\n\n", NUMBER_OF_THREADS);
    printf("Enter n (max 100,000,000): ");
    if (scanf("%ld", &n) != 1) {
        fprintf(stderr, "Error: Failed to read n\n");
        return 1;
    }
    if (n < 2 || n > 100000000) {
        fprintf(stderr, "Error: Enter number within range [2 to 100,000,000].\n");
        return 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &start);
    long prime_count = 0;

    clock_gettime(CLOCK_MONOTONIC, &start_computation);
    long *primes = is_prime(n, &prime_count);
    clock_gettime(CLOCK_MONOTONIC, &end_computation);

    computation_time = (end_computation.tv_sec - start_computation.tv_sec) * 1e9; 
    computation_time = (computation_time + (end_computation.tv_nsec - start_computation.tv_nsec)) * 1e-9;

    if (prime_count == 0) {
        fprintf(stderr, "Error: No primes found.\n");
        return 1;
    }
    if (n < 100)
    {
        printf("Prime numbers less than %ld:\n", n);
        for (long i = 0; i < prime_count; i++) {
            printf("%ld ", primes[i]);
        }
        printf("\n\n");
    } else {
        const char *filename = "primes.txt";
        print_to_file(primes, prime_count, filename);
    }
    free(primes);
    clock_gettime(CLOCK_MONOTONIC, &end);

    total_time = (end.tv_sec - start.tv_sec) * 1e9;
    total_time = (total_time + (end.tv_nsec - start.tv_nsec)) * 1e-9;
    printf("Computation time taken: %f seconds\n", computation_time);
    printf("Total time taken: %f seconds\n", total_time);

    return 0;
}