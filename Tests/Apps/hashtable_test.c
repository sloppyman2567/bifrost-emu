#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TABLE_SIZE 256

typedef struct Node {
    char key[64];
    int value;
    struct Node *next;
} Node;

Node *table[TABLE_SIZE];

unsigned int hash(const char *key) {
    unsigned int h = 5381;
    while (*key) h = ((h << 5) + h) + *key++;
    return h % TABLE_SIZE;
}

void ht_set(const char *key, int value) {
    unsigned int h = hash(key);
    Node *n = table[h];
    while (n) {
        if (strcmp(n->key, key) == 0) { n->value = value; return; }
        n = n->next;
    }
    n = malloc(sizeof(Node));
    strcpy(n->key, key);
    n->value = value;
    n->next = table[h];
    table[h] = n;
}

int ht_get(const char *key, int *found) {
    unsigned int h = hash(key);
    Node *n = table[h];
    while (n) {
        if (strcmp(n->key, key) == 0) { *found = 1; return n->value; }
        n = n->next;
    }
    *found = 0;
    return 0;
}

int main() {
    printf("Hash table test (djb2 hash)\n");
    
    ht_set("alpha", 1);
    ht_set("beta", 2);
    ht_set("gamma", 3);
    ht_set("delta", 4);
    ht_set("epsilon", 5);
    
    int found;
    int v;
    
    v = ht_get("gamma", &found);
    printf("  gamma = %d %s\n", v, found && v == 3 ? "OK" : "FAIL");
    if (!found || v != 3) return 1;
    
    v = ht_get("alpha", &found);
    printf("  alpha = %d %s\n", v, found && v == 1 ? "OK" : "FAIL");
    if (!found || v != 1) return 1;
    
    v = ht_get("missing", &found);
    printf("  missing = %s %s\n", found ? "found" : "not found", !found ? "OK" : "FAIL");
    if (found) return 1;
    
    ht_set("gamma", 30);
    v = ht_get("gamma", &found);
    printf("  gamma(updated) = %d %s\n", v, found && v == 30 ? "OK" : "FAIL");
    if (!found || v != 30) return 1;
    
    // Stress test: insert 1000 keys
    for (int i = 0; i < 1000; i++) {
        char key[32];
        sprintf(key, "key_%d", i);
        ht_set(key, i * 10);
    }
    for (int i = 0; i < 1000; i++) {
        char key[32];
        sprintf(key, "key_%d", i);
        v = ht_get(key, &found);
        if (!found || v != i * 10) {
            printf("FAIL: key_%d expected %d got %d\n", i, i*10, v);
            return 1;
        }
    }
    printf("  1000 keys: OK\n");
    printf("PASS\n");
    return 0;
}
