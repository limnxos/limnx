#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

int main(void) {
    /* Test 1: write */
    write(1, "TEST1: write ok\n", 16);

    /* Test 2: printf */
    printf("TEST2: printf ok\n");

    /* Test 3: stat */
    struct stat st;
    if (stat("/etc/inittab", &st) == 0) {
        printf("TEST3: stat ok (size=%ld, mode=0%o)\n", (long)st.st_size, st.st_mode & 07777);
    } else {
        printf("TEST3: stat FAILED\n");
    }

    /* Test 4: stat directory */
    if (stat("/etc", &st) == 0 && S_ISDIR(st.st_mode)) {
        printf("TEST4: stat dir ok\n");
    } else {
        printf("TEST4: stat dir FAILED\n");
    }

    /* Test 5: strlen */
    printf("TEST5: strlen=%d\n", (int)strlen("busybox ready"));

    return 0;
}
