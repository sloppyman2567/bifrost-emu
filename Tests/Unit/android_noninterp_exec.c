// execve of images without a linker64 PT_INTERP, from a real Bionic process: a static image
// (android_static_image.S) and linker64 itself, which loads argv[1] when it is the program.
extern int fork(void);
extern int execve(const char*, char* const*, char* const*);
extern int waitpid(int, int*, int);
extern void _exit(int);
extern int strcmp(const char*, const char*);
extern int printf(const char*, ...);
extern int fflush(void*);
static int run(const char* path, char* const* arguments, char* const* envp, int expected) {
    fflush(0);
    int child = fork(), status = 0;
    if (child < 0) return 1;
    if (!child) {
        execve(path, arguments, envp);
        _exit(127);
    }
    if (waitpid(child, &status, 0) != child) return 2;
    return status == expected << 8 ? 0 : 3;
}
int main(int argc, char** argv, char** envp) {
    if (argc > 1 && !strcmp(argv[1], "child")) {
        printf("android_noninterp_exec: linker64 loaded its program\n");
        return 0;
    }
    char* static_arguments[] = {"android_static_image", 0};
    int result = run("/data/local/tmp/android_static_image", static_arguments, envp, 7);
    if (result) return 10 + result;
    char* linker_arguments[] = {"/system/bin/linker64", "/data/local/tmp/android_noninterp_exec", "child", 0};
    result = run("/system/bin/linker64", linker_arguments, envp, 0);
    if (result) return 20 + result;
    printf("android_noninterp_exec: ALL PASS\n");
    return 0;
}
