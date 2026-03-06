#include "libc/libc.h"

static int test_getpid(void) {
    printf("[pipetest] Test 1: getpid\n");
    long pid = sys_getpid();
    if (pid <= 0) {
        printf("[pipetest] FAIL: getpid returned %ld\n", pid);
        return -1;
    }
    printf("[pipetest] getpid = %ld — PASSED\n", pid);
    return 0;
}

static int test_pipe(void) {
    printf("[pipetest] Test 2: pipe\n");
    long rfd, wfd;
    if (sys_pipe(&rfd, &wfd) != 0) {
        printf("[pipetest] FAIL: sys_pipe returned error\n");
        return -1;
    }
    printf("[pipetest] pipe fds: read=%ld write=%ld\n", rfd, wfd);

    const char *msg = "Hello pipe!";
    long msg_len = (long)strlen(msg);
    long n = sys_fwrite(wfd, msg, (unsigned long)msg_len);
    if (n != msg_len) {
        printf("[pipetest] FAIL: write returned %ld\n", n);
        return -1;
    }

    /* Close write end so reader sees EOF after data */
    sys_close(wfd);

    char buf[64];
    n = sys_read(rfd, buf, 63);
    if (n != msg_len) {
        printf("[pipetest] FAIL: read returned %ld\n", n);
        sys_close(rfd);
        return -1;
    }
    buf[n] = '\0';

    sys_close(rfd);

    if (strcmp(buf, msg) != 0) {
        printf("[pipetest] FAIL: content mismatch: \"%s\"\n", buf);
        return -1;
    }
    printf("[pipetest] pipe read: \"%s\" — PASSED\n", buf);
    return 0;
}

static int test_exec_waitpid(void) {
    printf("[pipetest] Test 3: exec + waitpid\n");
    long child_pid = sys_exec("/hello.elf", NULL);
    if (child_pid <= 0) {
        printf("[pipetest] FAIL: sys_exec returned %ld\n", child_pid);
        return -1;
    }
    printf("[pipetest] exec'd /hello.elf (pid %ld), waiting...\n", child_pid);

    long status = sys_waitpid(child_pid);
    printf("[pipetest] waitpid returned status=%ld — PASSED\n", status);
    return 0;
}

static int test_fmmap(void) {
    printf("[pipetest] Test 4: fmmap\n");
    long fd = sys_open("/hello.txt", 0);
    if (fd < 0) {
        printf("[pipetest] FAIL: open /hello.txt failed\n");
        return -1;
    }

    long addr = sys_fmmap(fd);
    if (addr <= 0) {
        printf("[pipetest] FAIL: fmmap returned %ld\n", addr);
        sys_close(fd);
        return -1;
    }

    /* Read first few bytes from mapped region */
    const char *mapped = (const char *)addr;
    /* Print first 20 chars manually (our printf doesn't support %.Ns) */
    printf("[pipetest] fmmap addr=%lx, content: \"", (unsigned long)addr);
    for (int i = 0; i < 20 && mapped[i]; i++) {
        char c = mapped[i];
        sys_write(&c, 1);
    }
    printf("\"\n");

    sys_close(fd);
    sys_munmap((unsigned long)addr);

    printf("[pipetest] fmmap — PASSED\n");
    return 0;
}

int main(void) {
    printf("=== pipetest start ===\n");

    int failures = 0;
    if (test_getpid() != 0)    failures++;
    if (test_pipe() != 0)      failures++;
    if (test_exec_waitpid() != 0) failures++;
    if (test_fmmap() != 0)     failures++;

    if (failures == 0)
        printf("=== pipetest: ALL PASSED ===\n");
    else
        printf("=== pipetest: %d FAILED ===\n", failures);

    return failures;
}
