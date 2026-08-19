#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#define NUM_THREADS 4

// Added shared value so every thread knows the upper limit n.
long upperLimit;

// Added shared array where threads mark prime numbers.
_Bool *primeNumberArray;


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


// Common function that will be executed by each POSIX thread.
void *findPrimesForThread(void *threadArgument) {
    // Access the thread number passed from main.
    int *threadNumberPointer = (int *)threadArgument;
    // Store this thread's number.
    int threadNumber = *threadNumberPointer;

    // Cyclic partitioning so each thread checks different numbers.
    for (long number = 2 + threadNumber;
         number < upperLimit;
         number += NUM_THREADS)
    {
        _Bool isPrime = 1;

        // Each thread performs the same prime check as the serial version.
        for (long divisor = 2; divisor * divisor <= number; divisor++)
        {
            if (number % divisor == 0){
                isPrime = 0;
                break;
            }
        }

        // Shared-array update for the number assigned to this thread.
        if (isPrime) {
            primeNumberArray[number] = 1;
        } else {
            primeNumberArray[number] = 0;
        }
    }

    // Return required by the pthread thread-function format.
    return 0;
}


long *is_prime(long k, long *out_count) {
    // Added assignment so all threads can access the value of k.
    upperLimit = k;
    // Changed the prime array to shared memory accessible by all threads.
    primeNumberArray = calloc((size_t)k, sizeof *primeNumberArray);

    if (primeNumberArray == NULL) {
        fprintf(stderr, "Memory allocation failed for prime array\n");
        *out_count = 0;
        return NULL;
    }


    // Added array containing the POSIX thread identifiers.
    pthread_t threadIDs[NUM_THREADS];
    // Added array containing the number assigned to each thread.
    int threadNumbers[NUM_THREADS];
    // Added loop variable following the Week 2 pthread example style.
    int threadIndex = 0;


    // Fork section to create all worker threads.
    for (threadIndex = 0; threadIndex < NUM_THREADS; threadIndex++) {
        // Unique number for each thread.
        threadNumbers[threadIndex] = threadIndex;

        // Creation of a thread running the common thread function.
        pthread_create(
            &threadIDs[threadIndex],
            NULL,
            findPrimesForThread,
            &threadNumbers[threadIndex]
        );
    }


    // Join section so main waits until all threads finish.
    for (threadIndex = 0; threadIndex < NUM_THREADS; threadIndex++) {
        // Added pthread_join for each created thread.
        pthread_join(threadIDs[threadIndex], NULL);
    }


    long count = 0;
    for (long i = 0; i < k; i++) {
        if (primeNumberArray[i] == 1) count++;
    }

    // Intialise array to hold primes
    if (count == 0) {
        free(primeNumberArray);
        *out_count = 0;
        return NULL;
    }


    long *primes = malloc(sizeof(long) * count);

    if (primes == NULL) {
        fprintf(stderr, "Memory allocation failed for primes array\n");
        free(primeNumberArray);
        *out_count = 0;
        return NULL;
    }


    long idx = 0;
    // Main thread reads array from lowest index to keeps final list sorted.
    for (long i = 0; i < k; i++) {
        if (primeNumberArray[i] == 1) {
            primes[idx++] = i;
        }
    }

    free(primeNumberArray);
    *out_count = count;
    return primes;
}


int main(void) {
    long n;

    printf("Enter n (max 100,000,000): ");

    if (scanf("%ld", &n) != 1) {
        fprintf(stderr, "Error: failed to read n\n");
        return 1;
    }
    if (n < 2) {
        printf("Please enter a number greater than or equal to 2.\n");
        return 0;
    }
    if (n > 100000000) {
        fprintf(stderr, "Error: n is too large (max 100000000)\n");
        return 1;
    }

    long prime_count = 0;
    // The prime search now creates NUM_THREADS POSIX threads internally.
    long *primes = is_prime(n, &prime_count);
    if (prime_count == 0) {
        printf("No primes found or error occurred.\n");
        return 0;
    }


    if (n < 100)
    {
        printf("Prime numbers that are strictly less than %ld are:\n", n);

        for (long i = 0; i < prime_count; i++) {
            printf("%ld ", primes[i]);
        }
        printf("\n");
    } else {
        const char *filename = "primes.txt";
        print_to_file(primes, prime_count, filename);
    }
    printf("Number of threads used: %d\n", NUM_THREADS);

    free(primes);
    return 0;
}