/*
 * hello.c — Minimal hello world (portable C replacement for hello.asm).
 * Used by s100test fork+exec test on both architectures.
 */
#include "../libc/libc.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("Hello from user-space!\n");
    sys_exit(0);
    return 0;
}
