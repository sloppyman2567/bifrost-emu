#include <stdio.h>
#include <stdlib.h>

typedef struct Node {
    int val;
    struct Node *left, *right;
} Node;

Node* insert(Node *root, int val) {
    if (!root) {
        root = malloc(sizeof(Node));
        root->val = val; root->left = root->right = NULL;
        return root;
    }
    if (val < root->val) root->left = insert(root->left, val);
    else if (val > root->val) root->right = insert(root->right, val);
    return root;
}

Node* search(Node *root, int val) {
    while (root) {
        if (val == root->val) return root;
        root = (val < root->val) ? root->left : root->right;
    }
    return NULL;
}

int height(Node *root) {
    if (!root) return 0;
    int l = height(root->left), r = height(root->right);
    return 1 + (l > r ? l : r);
}

void inorder(Node *root, int *prev, int *ok) {
    if (!root) return;
    inorder(root->left, prev, ok);
    if (root->val < *prev) *ok = 0;
    *prev = root->val;
    inorder(root->right, prev, ok);
}

int main() {
    printf("Binary search tree test\n");
    Node *root = NULL;
    int vals[] = {50, 30, 70, 20, 40, 60, 80, 10, 25, 35, 45};
    for (int i = 0; i < 11; i++) root = insert(root, vals[i]);
    
    for (int i = 0; i < 11; i++) {
        Node *n = search(root, vals[i]);
        if (!n) { printf("FAIL: can't find %d\n", vals[i]); return 1; }
    }
    printf("  All 11 values found: OK\n");
    
    if (search(root, 999)) { printf("FAIL: found 999\n"); return 1; }
    printf("  999 not found: OK\n");
    
    int prev = -1, ok = 1;
    inorder(root, &prev, &ok);
    printf("  Inorder sorted: %s\n", ok ? "OK" : "FAIL");
    if (!ok) return 1;
    
    printf("  Height: %d\n", height(root));
    printf("PASS\n");
    return 0;
}
