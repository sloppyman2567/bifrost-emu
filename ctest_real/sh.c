/* sh.c — tiny interactive REPL shell.
 * Supports: simple commands, echo, exit, help, eval (arithmetic).
 * Demonstrates interactive use, stdin line buffering, manual tokenizer.
 *
 * NOTE: original version used strtok_r which exposed a latent NEON/SIMD
 * bug in musl's strspn/strcspn (used internally by strtok). The manual
 * tokenizer here avoids that path so the shell works end-to-end.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static int eval_arith(const char* expr) {
    long result = 0, term = 0;
    char op = '+';
    while (*expr) {
        while (isspace((unsigned char)*expr)) expr++;
        if (*expr == '+' || *expr == '-' || *expr == '*' || *expr == '/') {
            op = *expr;
            expr++;
            continue;
        }
        if (!isdigit((unsigned char)*expr)) break;
        long n = 0;
        while (isdigit((unsigned char)*expr)) {
            n = n * 10 + (*expr - '0');
            expr++;
        }
        switch (op) {
            case '+': result += n; break;
            case '-': result -= n; break;
            case '*': result *= n; break;
            case '/': result = n ? result / n : 0; break;
        }
    }
    return (int)result;
}

static void cmd_help(void) {
    puts("Available commands:");
    puts("  help          show this help");
    puts("  echo ARGS...  print args");
    puts("  eval EXPR     evaluate + - * / arithmetic");
    puts("  exit [N]      exit with code N (default 0)");
    puts("  (anything else gets echoed back)");
}

// Manual tokenizer: writes pointers into argv[] and returns argc.
// Modifies `line` in place (inserts NULs between tokens).
static int tokenize(char* line, char** argv, int max) {
    int argc = 0;
    char* p = line;
    while (*p && argc < max - 1) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) { *p = 0; p++; }
    }
    argv[argc] = NULL;
    return argc;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    char line[512];
    char* av[64];
    printf("bifrost-sh$ ");
    fflush(stdout);
    while (fgets(line, sizeof(line), stdin)) {
        size_t n = strlen(line);
        if (n > 0 && line[n-1] == '\n') line[n-1] = 0;
        int ac = tokenize(line, av, 64);
        if (ac == 0) { printf("bifrost-sh$ "); fflush(stdout); continue; }
        if (strcmp(av[0], "exit") == 0) {
            return ac > 1 ? atoi(av[1]) : 0;
        }
        if (strcmp(av[0], "help") == 0) {
            cmd_help();
        } else if (strcmp(av[0], "echo") == 0) {
            for (int i = 1; i < ac; i++) {
                fputs(i > 1 ? " " : "", stdout);
                fputs(av[i], stdout);
            }
            fputc('\n', stdout);
        } else if (strcmp(av[0], "eval") == 0) {
            // Rebuild a single expression string from av[1..]
            char expr[256] = {0};
            for (int i = 1; i < ac; i++) {
                strncat(expr, av[i], sizeof(expr) - strlen(expr) - 1);
            }
            int v = eval_arith(expr);
            printf("= %d\n", v);
        } else {
            printf("unknown command: %s\n", av[0]);
        }
        printf("bifrost-sh$ ");
        fflush(stdout);
    }
    return 0;
}
