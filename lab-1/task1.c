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

void is_prime(long k) {
    if (k < 2) {
        return;
    } else if (k > 100000000) {
        fprintf(stderr, "Error: n must be less than or equal to 100,000,000\n");
        return;
    }
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
    long *primes = malloc(sizeof(long) * (count > 0 ? count : 1)); // use count as length otherwise use 1

    long idx = 0;
    for (long i = 0; i < k; i++) {
        if (primeArray[i] == 1) {
            primes[idx++] = i;
        }
    }

    if (k < 100) {
        printf("Prime numbers that are strictly less than %ld are:\n", k);
        for (long i = 0; i < count; i++) {
            printf("%ld ", primes[i]);
        }
        printf("\n");
    } else {
        const char *filename = "primes.txt";
        print_to_file(primes, count, filename);
    }

    free(primes);
    free(primeArray);
}

int main(void) {
    long n;

    printf("Enter n (max 100,000,000): ");
    if (scanf("%ld", &n) != 1) {
        fprintf(stderr, "Error: failed to read n\n");
        return 1;
    }

    is_prime(n);
    printf("\n");
    return 0;
}