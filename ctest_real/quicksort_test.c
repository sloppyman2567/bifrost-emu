#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void quicksort(int *arr, int lo, int hi) {
    if (lo >= hi) return;
    int pivot = arr[(lo + hi) / 2];
    int i = lo, j = hi;
    while (i <= j) {
        while (arr[i] < pivot) i++;
        while (arr[j] > pivot) j--;
        if (i <= j) {
            int tmp = arr[i]; arr[i] = arr[j]; arr[j] = tmp;
            i++; j--;
        }
    }
    quicksort(arr, lo, j);
    quicksort(arr, i, hi);
}

int main() {
    printf("Quicksort benchmark (10000 elements)\n");
    int n = 10000;
    int *arr = malloc(n * sizeof(int));
    
    // Fill with pseudo-random values
    srand(42);
    for (int i = 0; i < n; i++) arr[i] = rand() % 100000;
    
    quicksort(arr, 0, n - 1);
    
    // Verify sorted
    for (int i = 1; i < n; i++) {
        if (arr[i] < arr[i-1]) {
            printf("FAIL: arr[%d]=%d < arr[%d]=%d\n", i, arr[i], i-1, arr[i-1]);
            free(arr);
            return 1;
        }
    }
    printf("  First: %d, Last: %d, Count: %d\n", arr[0], arr[n-1], n);
    printf("PASS\n");
    free(arr);
    return 0;
}
