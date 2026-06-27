#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

typedef union {
    int i;
    float f;
    uint8_t bytes[4];
} Variant;

typedef struct {
    char name[32];
    int type;
    Variant value;
    int (*compare)(const void *, const void *);
} Field;

int cmp_int(const void *a, const void *b) {
    return *(const int*)a - *(const int*)b;
}

int cmp_str(const void *a, const void *b) {
    return strcmp((const char*)a, (const char*)b);
}

int main() {
    printf("Complex struct/union/function-pointer test\n");
    
    // Test union
    Variant v;
    v.i = 0x41424344;
    printf("  union.i=0x%08X bytes=%02x%02x%02x%02x f=%f\n",
           v.i, v.bytes[0], v.bytes[1], v.bytes[2], v.bytes[3], v.f);
    if (v.bytes[0] != 0x44) { printf("FAIL: union byte 0\n"); return 1; }
    printf("  Union: OK\n");
    
    // Test struct with function pointer
    Field f1;
    strcpy(f1.name, "age");
    f1.type = 0;
    f1.value.i = 42;
    f1.compare = cmp_int;
    
    int a = 10, b = 20;
    int r = f1.compare(&a, &b);
    printf("  compare(10, 20) = %d %s\n", r, r < 0 ? "OK" : "FAIL");
    if (r >= 0) return 1;
    
    // Test struct array with qsort
    Field fields[3];
    strcpy(fields[0].name, "charlie"); fields[0].value.i = 3;
    strcpy(fields[1].name, "alpha"); fields[1].value.i = 1;
    strcpy(fields[2].name, "bravo"); fields[2].value.i = 2;
    
    // Sort by name using qsort
    qsort(fields, 3, sizeof(Field), cmp_str);
    
    printf("  Sorted: %s, %s, %s\n", fields[0].name, fields[1].name, fields[2].name);
    if (strcmp(fields[0].name, "alpha") != 0 ||
        strcmp(fields[1].name, "bravo") != 0 ||
        strcmp(fields[2].name, "charlie") != 0) {
        printf("FAIL: qsort\n");
        return 1;
    }
    printf("  qsort: OK\n");
    
    // Test struct alignment / padding
    printf("  sizeof(Field) = %zu\n", sizeof(Field));
    printf("  sizeof(Variant) = %zu\n", sizeof(Variant));
    printf("PASS\n");
    return 0;
}
