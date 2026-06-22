#include <stdlib.h>
#include <stdio.h>

int cmp(const void *a, const void *b) {
    return *(int*)a - *(int*)b;
}

int main() {
    printf("malloc test start\n");
    int *p = malloc(100 * sizeof(int));
    if (!p) { printf("malloc failed\n"); return 1; }
    for (int i = 0; i < 100; i++) p[i] = 100 - i;
    qsort(p, 100, sizeof(int), cmp);
    printf("first=%d last=%d\n", p[0], p[99]);
    free(p);
    printf("malloc test done\n");
    return 0;
}
