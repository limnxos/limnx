#include "libc/libc.h"

static int passed = 0, failed = 0;

static void check(int ok, const char *name) {
    if (ok) {
        printf("  PASS: %s\n", name);
        passed++;
    } else {
        printf("  FAIL: %s\n", name);
        failed++;
    }
}

int main(void) {
    printf("=== Stage 58: errno + perror ===\n");

    /* Test 1: errno starts at 0 */
    check(errno == 0, "T1: errno initially 0");

    /* Test 2: strerror known codes */
    {
        check(strcmp(strerror(0), "Success") == 0, "T2: strerror(0)");
        check(strcmp(strerror(ENOENT), "No such file or directory") == 0,
              "T2: strerror(ENOENT)");
        check(strcmp(strerror(EACCES), "Permission denied") == 0,
              "T2: strerror(EACCES)");
        check(strcmp(strerror(EINVAL), "Invalid argument") == 0,
              "T2: strerror(EINVAL)");
        check(strcmp(strerror(ENOMEM), "Cannot allocate memory") == 0,
              "T2: strerror(ENOMEM)");
        check(strcmp(strerror(EBADF), "Bad file descriptor") == 0,
              "T2: strerror(EBADF)");
        check(strcmp(strerror(EEXIST), "File exists") == 0,
              "T2: strerror(EEXIST)");
        check(strcmp(strerror(EMFILE), "Too many open files") == 0,
              "T2: strerror(EMFILE)");
    }

    /* Test 3: strerror unknown code */
    {
        check(strcmp(strerror(9999), "Unknown error") == 0,
              "T3: strerror unknown returns Unknown error");
    }

    /* Test 4: __set_errno with negative (error) */
    {
        errno = 0;
        long ret = __set_errno(-ENOENT);
        check(ret == -1, "T4: __set_errno(-ENOENT) returns -1");
        check(errno == ENOENT, "T4: errno set to ENOENT");
    }

    /* Test 5: __set_errno with positive (success) */
    {
        errno = 0;
        long ret = __set_errno(5);
        check(ret == 5, "T5: __set_errno(5) returns 5");
        check(errno == 0, "T5: errno unchanged on success");
    }

    /* Test 6: __set_errno with zero (success) */
    {
        errno = 42;
        long ret = __set_errno(0);
        check(ret == 0, "T6: __set_errno(0) returns 0");
    }

    /* Test 7: Real syscall error — open nonexistent file */
    {
        errno = 0;
        long fd = __set_errno(sys_open("/nonexistent_xyz_123", O_RDONLY));
        check(fd == -1, "T7: open nonexistent returns -1");
        check(errno != 0, "T7: errno set after failed open");
        check(strcmp(strerror(errno), "Unknown error") != 0,
              "T7: strerror gives known message");
    }

    /* Test 8: perror output (write to file, verify) */
    {
        sys_mkdir("/tmp");
        const char *testfile = "/tmp/perror_test.txt";

        /* Redirect stderr to a file temporarily */
        FILE *fp = fopen(testfile, "w");
        check(fp != NULL, "T8: open file for perror test");
        if (fp) {
            /* Manually set errno and call fprintf to simulate perror */
            errno = ENOENT;
            fprintf(fp, "%s: %s\n", "myapp", strerror(errno));
            fclose(fp);

            /* Read back and verify */
            fp = fopen(testfile, "r");
            if (fp) {
                char buf[128];
                fgets(buf, sizeof(buf), fp);
                check(strcmp(buf, "myapp: No such file or directory\n") == 0,
                      "T8: perror-style output correct");
                fclose(fp);
            }
            sys_unlink(testfile);
        }
    }

    /* Test 9: Multiple errno codes from strerror */
    {
        int codes[] = {EPERM, ESRCH, EINTR, EIO, EAGAIN, EFAULT,
                       ENOSYS, EADDRINUSE, ENOTCONN, ECONNREFUSED};
        int ok = 1;
        for (int i = 0; i < 10; i++) {
            const char *s = strerror(codes[i]);
            if (strcmp(s, "Unknown error") == 0) { ok = 0; break; }
        }
        check(ok, "T9: all defined codes have known messages");
    }

    /* Test 10: errno persists until explicitly changed */
    {
        errno = EACCES;
        check(errno == EACCES, "T10: errno persists");
        /* Successful operation should not clear errno (C standard behavior) */
        long fd = sys_open("/hello.txt", O_RDONLY);
        if (fd >= 0) {
            check(errno == EACCES, "T10: errno not cleared by success");
            sys_close(fd);
        }
    }

    /* Test 11: __set_errno with various error codes */
    {
        int codes[] = {EPERM, EACCES, ENOMEM, EMFILE, EBADF, EAGAIN};
        int ok = 1;
        for (int i = 0; i < 6; i++) {
            long ret = __set_errno(-codes[i]);
            if (ret != -1 || errno != codes[i]) { ok = 0; break; }
        }
        check(ok, "T11: __set_errno works for all error codes");
    }

    printf("\n=== Stage 58 Results: %d/%d passed ===\n",
           passed, passed + failed);

    return (failed > 0) ? 1 : 0;
}
