#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>


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

/*
Only this function is edited from task1
*/
long *is_prime(long k, long *out_count) {
    // allocate memory for an array of size k, with each element being a _Bool (1 byte)
    _Bool *primeArray = calloc((size_t)k, sizeof *primeArray);
    if (primeArray == NULL) {
        fprintf(stderr, "Memory allocation failed for primeArray\n");
        *out_count = 0;
        return NULL;
    }

    /*
    Single parallelisation. Each i is independent of every other i
    since it only reads i and j and only updates to the i index of
    prime array, so there are no data races on primeArray. count is
    accumulated, each thread has its own private copy that OpenMP sums 
    at the end of the loop.
    */
    long count = 0;
    #pragma omp parallel for schedule(dynamic, 1) reduction(+:count)
    for (long i = 2; i < k; i++){
        _Bool isPrime = 1;

        for (long j = 2; j * j <= i; j++){
            if (i % j == 0){
                isPrime = 0;
                break;
            }
        }
        if (isPrime){
            primeArray[i] = 1;
            count++;
        }
        else {
            primeArray[i] = 0;
        }
    }

    // intialise array to hold primes
    if (count == 0) {
        free(primeArray);
        *out_count = 0;
        return NULL;
    }

    long *primes = malloc(sizeof(long) * count);
    if (primes == NULL) {
        fprintf(stderr, "Memory allocation failed for primes array\n");
        free(primeArray);
        *out_count = 0;
        return NULL;
    }

    long idx = 0;
    for (long i = 0; i < k; i++) {
        if (primeArray[i] == 1) {
            primes[idx++] = i;
        }
    }

    free(primeArray);
    *out_count = count;
    return primes;
}

int main(void) {
    omp_set_num_threads(8);
    struct timespec start, end, startComp, endComp; 
    double comp_time, total_time;

    long n;

    #pragma omp parallel
    {
        #pragma omp single
        printf("Using %d threads\n", omp_get_num_threads());
    }

    printf("Enter n (max 100,000,000): ");
    
    if (scanf("%ld", &n) != 1) {
        fprintf(stderr, "Error: failed to read n\n");
        return 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &start);
    if (n < 2) {
        printf("Please enter a number greater than or equal to 2.\n");
        return 0;
    }
    if (n > 100000000) {
        fprintf(stderr, "Error: n is too large (max 100,000,000)\n");
        return 1;
    }

    long prime_count = 0;
    clock_gettime(CLOCK_MONOTONIC, &startComp); 
    long *primes = is_prime(n, &prime_count);
    clock_gettime(CLOCK_MONOTONIC, &endComp); 

    comp_time = (endComp.tv_sec - startComp.tv_sec) * 1e9; 
    comp_time = (comp_time + (endComp.tv_nsec - startComp.tv_nsec)) * 1e-9;
 
    if (prime_count == 0) {
        printf("No primes found or error occurred.\n");
        return 0;
    }

    if (n < 100) {
        printf("Prime numbers that are strictly less than %ld are:\n", n);
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
    printf("Total time taken (including printing/file writing): %f seconds\n", total_time);
    return 0;
}