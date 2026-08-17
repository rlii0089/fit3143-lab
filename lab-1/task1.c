#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

void print_to_file(const long long *primes, long long count, const char *filename);

void generate_primes_below(long long limit) {
    if (limit < 2) return;

    size_t n = (size_t)limit;
    bool *is_prime = calloc(n, sizeof *is_prime);
    if (is_prime == NULL) {
        fprintf(stderr, "Memory allocation failed\n");
        return;
    }

    for (size_t i = 2; i < n; i++) is_prime[i] = true;

    for (size_t p = 2; p * p < n; p++) {
        if (!is_prime[p]) continue;
        for (size_t m = p * p; m < n; m += p) is_prime[m] = false;
    }

    size_t prime_count = 0;
    for (size_t i = 2; i < n; i++) if (is_prime[i]) prime_count++;

    long long *prime_list = malloc(sizeof *prime_list * (prime_count > 0 ? prime_count : 1));
    if (prime_list == NULL) {
        fprintf(stderr, "Memory allocation failed\n");
        free(is_prime);
        return;
    }

    size_t idx = 0;
    for (size_t i = 2; i < n; i++) if (is_prime[i]) prime_list[idx++] = (long long)i;

    if (limit < 100) {
        printf("Prime numbers that are strictly less than %lld are:\n", limit);
        for (size_t i = 0; i < prime_count; i++) printf("%lld ", prime_list[i]);
        printf("\n");
    } else {
        char filename[64];
        snprintf(filename, sizeof filename, "primes_%lld.txt", limit);
        print_to_file(prime_list, (long long)prime_count, filename);
    }

    free(prime_list);
    free(is_prime);
}

void print_to_file(const long long *primes, long long count, const char *filename) {
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
    long long n = 0;
    printf("Enter n: ");
    if (scanf("%lld", &n) != 1) {
        fprintf(stderr, "Error: failed to read n\n");
        return 1;
    }

    generate_primes_below(n);
    printf("\n");
    return 0;
}