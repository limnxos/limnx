/* Test musl libc linked with Limnx _start instead of musl's crt1 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    write(1, "TEST1: write\n", 13);
    printf("TEST2: printf works!\n");
    printf("TEST3: strlen=%d\n", (int)strlen("musl"));
    return 0;
}
