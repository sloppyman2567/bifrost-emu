#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Minimal JSON value parser
typedef enum { JSON_NULL, JSON_BOOL, JSON_NUM, JSON_STR, JSON_ARR, JSON_OBJ } JsonType;

typedef struct JsonValue {
    JsonType type;
    double num;
    int boolean;
    char str[256];
} JsonValue;

const char* parse_value(const char *p, JsonValue *v);

const char* skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

const char* parse_string(const char *p, char *out, int max) {
    if (*p != '"') return NULL;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < max-1) {
        if (*p == '\\') { p++; if (*p) out[i++] = *p++; }
        else out[i++] = *p++;
    }
    out[i] = 0;
    if (*p == '"') p++;
    return p;
}

const char* parse_value(const char *p, JsonValue *v) {
    p = skip_ws(p);
    if (*p == '"') {
        v->type = JSON_STR;
        return parse_string(p, v->str, sizeof(v->str));
    }
    if (*p == 't') {
        v->type = JSON_BOOL; v->boolean = 1;
        return p + 4;
    }
    if (*p == 'f') {
        v->type = JSON_BOOL; v->boolean = 0;
        return p + 5;
    }
    if (*p == 'n') {
        v->type = JSON_NULL;
        return p + 4;
    }
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        v->type = JSON_NUM;
        v->num = strtod(p, (char**)&p);
        return p;
    }
    return NULL;
}

int main() {
    const char *json = "{\"name\":\"bifrost\",\"version\":1.4,\"fast\":true,\"null_val\":null}";
    JsonValue v;
    
    printf("JSON parser test\n");
    const char *p = json;
    p = skip_ws(p);
    if (*p != '{') { printf("FAIL: expected {\n"); return 1; }
    p++;
    
    while (*p && *p != '}') {
        p = skip_ws(p);
        char key[256];
        p = parse_string(p, key, sizeof(key));
        if (!p) { printf("FAIL: bad key\n"); return 1; }
        p = skip_ws(p);
        if (*p != ':') { printf("FAIL: expected :\n"); return 1; }
        p++;
        p = parse_value(p, &v);
        if (!p) { printf("FAIL: bad value for %s\n", key); return 1; }
        
        switch (v.type) {
            case JSON_STR: printf("  %s = \"%s\"\n", key, v.str); break;
            case JSON_NUM: printf("  %s = %f\n", key, v.num); break;
            case JSON_BOOL: printf("  %s = %s\n", key, v.boolean ? "true" : "false"); break;
            case JSON_NULL: printf("  %s = null\n", key); break;
            default: printf("  %s = ???\n", key); break;
        }
        p = skip_ws(p);
        if (*p == ',') p++;
    }
    printf("PASS: JSON parsed successfully\n");
    return 0;
}
