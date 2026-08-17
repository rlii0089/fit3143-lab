#include <stdio.h>
#include <math.h>

void is_prime(long long k) {
    if (k < 2) {
        printf("Please enter a number greater than or equal to 2.\n");
        return;
    }

    _Bool primeArray[k];
    primeArray[0] = 0;
    primeArray[1] = 0;
    for (long long i = 2; i < k; i++){
        _Bool isPrime = 1;
        for (long long j = 2; j <= sqrt(k); j++){
            if (i%j == 0){
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

    printf("Prime numbers that are strictly less than %lld are:\n", k);
    for (long long i = 0; i < k; i++){
        if (primeArray[i] == 1){
            printf("%lld ", i);
        }
    }
}

// void print_to_file(long long *primes, long long count, const char *filename) {
//     FILE *fp = fopen(filename, "w");
//     if (fp == NULL) {
//         fprintf(stderr, "Error: could not open file %s for writing\n", filename);
//         exit(EXIT_FAILURE);
//     }

//     fprintf(fp, "Prime numbers found (%lld total):\n", count);
//     for (long long i = 0; i < count; i++) {
//         fprintf(fp, "%lld", primes[i]);
//         if (i != count - 1) fprintf(fp, ", ");
//     }
//     fprintf(fp, "\n");

//     fclose(fp);
//     printf("Results written to %s\n", filename);
// }

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