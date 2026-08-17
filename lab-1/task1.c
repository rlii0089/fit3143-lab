#include <stdio.h>
#include <math.h>
#include <stdlib.h>

void print_to_file(long *primes, long count, const char *filename) {
    FILE *fp = fopen(filename, "w");

    fprintf(fp, "Prime numbers found (%ld total):\n", count);
    for (long i = 0; i < count; i++) {
        fprintf(fp, "%ld", primes[i]);
        if (i != count - 1) fprintf(fp, ", ");
    }
    fprintf(fp, "\n");

    fclose(fp);
    printf("Results written to %s\n", filename);
}

long *is_prime(long k, long *out_count) {
    // allocate memory for an array of size k, with each element being a _Bool (1 byte)
    _Bool *primeArray = calloc((size_t)k, sizeof *primeArray);

    for (long i = 2; i < k; i++){
        _Bool isPrime = 1;
        /* avoid calling sqrt() repeatedly; use integer bound */
        for (long j = 2; j * j <= i; j++){
            if (i % j == 0){
                isPrime = 0;
                break;
            }
        }
        if (isPrime){
            primeArray[i] = 1;
        }
        else {
            primeArray[i] = 0;
        }
    }

    long count = 0;
    for (long i = 0; i < k; i++) {
        if (primeArray[i] == 1) count++;
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

    long count = 0;
    long *primes = is_prime(n, &count);
    if (count == 0) {
        printf("No primes found or error occurred.\n");
        return 0;
    }

    if (n < 100) {
        printf("Prime numbers that are strictly less than %ld are:\n", n);
        for (long i = 0; i < count; i++) {
            printf("%ld ", primes[i]);
        }
        printf("\n");
    } else {
        const char *filename = "primes.txt";
        print_to_file(primes, count, filename);
    }

    free(primes);
    printf("\n");
    return 0;
}