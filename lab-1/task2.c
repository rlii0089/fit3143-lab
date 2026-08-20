#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define NUMBER_OF_THREADS 8

// Global variables for all threads to access
long upper_limit;
_Bool *prime_number_array;


void print_to_file(long *primes, long prime_count, const char *filename) {
    FILE *output_file = fopen(filename, "w");

    fprintf(output_file, "Prime numbers found (%ld total):\n", prime_count);
    for (long i = 0; i < prime_count; i++) {
        fprintf(output_file, "%ld", primes[i]);
        if (i != prime_count - 1) fprintf(output_file, ", ");
    }

    fclose(output_file);
    printf("Results written to %s\n", filename);
}


// Common function that will be executed by each POSIX thread.
void *find_primes_for_thread(void *thread_information) {
    // Access the thread number passed from main.
    int *thread_number_pointer = (int *)thread_information;
    // Store this thread's number.
    int thread_number = *thread_number_pointer;

    // Cyclic partitioning so each thread checks different numbers.
    for (long number = 2 + thread_number;
         number < upper_limit;
         number += NUMBER_OF_THREADS)
    {
        _Bool is_prime = 1;

        // Each thread performs the same prime check as the serial version.
        for (long divisor = 2; divisor * divisor <= number; divisor++)
        {
            if (number % divisor == 0){
                is_prime = 0;
                break;
            }
        }

        // Shared-array update for the number assigned to this thread.
        if (is_prime) {
            prime_number_array[number] = 1;
        } else {
            prime_number_array[number] = 0;
        }
    }

    // Return required by the pthread thread-function format.
    return 0;
}


long *is_prime(long input_number, long *is_prime_count) {
    // Added assignment so all threads can access the value of input_number.
    upper_limit = input_number;
    // Changed the prime array to shared memory accessible by all threads.
    prime_number_array = calloc((size_t)input_number, sizeof *prime_number_array);

    if (prime_number_array == NULL) {
        fprintf(stderr, "Memory allocation failed for prime array\n");
        *is_prime_count = 0;
        return NULL;
    }


    // Added array containing the POSIX thread identifiers.
    pthread_t thread_ids[NUMBER_OF_THREADS];
    // Added array containing the number assigned to each thread.
    int thread_numbers[NUMBER_OF_THREADS];
    // Added loop variable following the Week 2 pthread example style.
    int thread_index = 0;


    // Fork section to create all worker threads.
    for (thread_index = 0; thread_index < NUMBER_OF_THREADS; thread_index++) {
        // Unique number for each thread.
        thread_numbers[thread_index] = thread_index;

        // Creation of a thread running the common thread function.
        pthread_create(
            &thread_ids[thread_index],
            NULL,
            find_primes_for_thread,
            &thread_numbers[thread_index]
        );
    }


    // Join section so main waits until all threads finish.
    for (thread_index = 0; thread_index < NUMBER_OF_THREADS; thread_index++) {
        // Added pthread_join for each created thread.
        pthread_join(thread_ids[thread_index], NULL);
    }


    long count = 0;
    for (long i = 0; i < input_number; i++) {
        if (prime_number_array[i] == 1) count++;
    }

    // Intialise array to hold primes
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


    long idx = 0;
    // Main thread reads array from lowest index to keeps final list sorted.
    for (long i = 0; i < input_number; i++) {
        if (prime_number_array[i] == 1) {
            primes[idx++] = i;
        }
    }

    free(prime_number_array);
    *is_prime_count = count;
    return primes;
}


int main(void) {
    printf("Number of threads available: %d\n\n", NUMBER_OF_THREADS);

    struct timespec start, end, start_comp, end_comp; 
    double comp_time, total_time;

    long n;

    printf("Enter n (max 100,000,000): ");
    if (scanf("%ld", &n) != 1) {
        fprintf(stderr, "Error: failed to read n\n");
        return 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &start);
    if (n < 2 || n > 100000000) {
        printf("Please enter a number within range [2 to 100,000,000].\n");
        return 0;
    }

    long prime_count = 0;
    clock_gettime(CLOCK_MONOTONIC, &start_comp); 
    // The prime search now creates NUMBER_OF_THREADS POSIX threads internally.
    long *primes = is_prime(n, &prime_count);
    clock_gettime(CLOCK_MONOTONIC, &end_comp); 

    comp_time = (end_comp.tv_sec - start_comp.tv_sec) * 1e9; 
    comp_time = (comp_time + (end_comp.tv_nsec - start_comp.tv_nsec)) * 1e-9;

    if (prime_count == 0) {
        printf("No primes found or error occurred.\n");
        return 0;
    }

    if (n < 100)
    {
        printf("Prime numbers less than %ld:\n", n);

        for (long i = 0; i < prime_count; i++) {
            printf("%ld ", primes[i]);
        }
        printf("\n");
    } else {
        const char *filename = "primes.txt";
        print_to_file(primes, prime_count, filename);
    }

    free(primes);

    clock_gettime(CLOCK_MONOTONIC, &end);
    total_time = (end.tv_sec - start.tv_sec) * 1e9;
    total_time = (total_time + (end.tv_nsec - start.tv_nsec)) * 1e-9;
    printf("Computation time taken: %f seconds\n", comp_time);
    printf("Total time taken: %f seconds\n", total_time);
    return 0;
}