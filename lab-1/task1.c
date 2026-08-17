#include <stdio.h>
#include <math.h>
#include <stdlib.h>

void is_prime(long long k) {
    if (k < 2) {
        printf("Please enter a number greater than or equal to 2.\n");
        return;
    }

    _Bool *primeArray = calloc((size_t)k, sizeof *primeArray);
    if (primeArray == NULL) {
        fprintf(stderr, "Memory allocation failed for primeArray\n");
        return;
    }
    if (k > 0) {
        primeArray[0] = 0;
    }
    if (k > 1) {
        primeArray[1] = 0;
    }
    for (long long i = 2; i < k; i++){
        _Bool isPrime = 1;
        /* avoid calling sqrt() repeatedly; use integer bound */
        for (long long j = 2; j * j <= i; j++){
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

    long long count = 0;
    for (long long i = 0; i < k; i++) {
        if (primeArray[i] == 1) count++;
    }

    // intialise array to hold primes
    long long *primes = malloc(sizeof(long long) * (count > 0 ? count : 1)); // use count as length otherwise use 1
    if (primes == NULL) {
        fprintf(stderr, "Memory allocation failed\n");
        return;
    }

    long long idx = 0;
    for (long long i = 0; i < k; i++) {
        if (primeArray[i] == 1) {
            primes[idx++] = i;
        }
    }

    if (k < 100) {
        printf("Prime numbers that are strictly less than %lld are:\n", k);
        for (long long i = 0; i < count; i++) {
            printf("%lld ", primes[i]);
        }
        printf("\n");
    } else {
        char filename[64];
        snprintf(filename, sizeof(filename), "primes_%lld.txt", k);
        /* write primes to file for larger n */
        extern void print_to_file(long long *primes, long long count, const char *filename);
        print_to_file(primes, count, filename);
    }

    free(primes);
    free(primeArray);
}

void print_to_file(long long *primes, long long count, const char *filename) {
    FILE *fp = fopen(filename, "w");
    if (fp == NULL) {
        fprintf(stderr, "Error: could not open file %s for writing\n", filename);
        exit(EXIT_FAILURE);
    }

    fprintf(fp, "Prime numbers found (%lld total):\n", count);
    for (long long i = 0; i < count; i++) {
        fprintf(fp, "%lld", primes[i]);
        if (i != count - 1) fprintf(fp, ", ");
    }
    fprintf(fp, "\n");

    fclose(fp);
    printf("Results written to %s\n", filename);
}

int main(void) {
    long long n;

    printf("Enter n: ");
    if (scanf("%lld", &n) != 1) {
        fprintf(stderr, "Error: failed to read n\n");
        return 1;
    }

    is_prime(n);
    printf("\n");
    return 0;
}