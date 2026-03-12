#include "libc/libc.h"

#define RESP_BUF 2048

/* Read all data from pipe until EOF using O_NONBLOCK. */
static int read_all(long fd, char *buf, int max) {
    sys_fcntl(fd, F_SETFL, O_NONBLOCK);
    int total = 0;
    while (total < max - 1) {
        char c;
        long n = sys_read(fd, &c, 1);
        if (n > 0) {
            buf[total++] = c;
        } else if (n == 0) {
            break;  /* EOF — writer closed pipe */
        } else {
            /* n == -1: would block, writer still alive */
            sys_yield();
        }
    }
    buf[total] = '\0';
    return total;
}

int main(void) {
    printf("multiagent: self-test start\n");

    /* Create pipe A: coordinator writes, worker reads (worker's stdin) */
    long rA, wA;
    if (sys_pipe(&rA, &wA) != 0) {
        printf("multiagent: FAIL pipe A\n");
        return 1;
    }

    /* Create pipe B: worker writes, coordinator reads (worker's stdout) */
    long rB, wB;
    if (sys_pipe(&rB, &wB) != 0) {
        printf("multiagent: FAIL pipe B\n");
        return 1;
    }

    /* Save coordinator's write end before rearranging fds */
    long saved_wA = sys_dup(wA);
    if (saved_wA < 0) {
        printf("multiagent: FAIL dup wA\n");
        return 1;
    }

    /* Set FD_CLOEXEC on fds that shouldn't be inherited by worker */
    sys_fcntl(rB, F_SETFD, FD_CLOEXEC);
    sys_fcntl(wA, F_SETFD, FD_CLOEXEC);
    sys_fcntl(saved_wA, F_SETFD, FD_CLOEXEC);

    /* Set up child's fd 0 = read end of pipe A (worker's stdin) */
    sys_dup2(rA, 0);
    /* Set up child's fd 1 = write end of pipe B (worker's stdout) */
    sys_dup2(wB, 1);

    /* Exec worker — child inherits fds: fd 0 = rA, fd 1 = wB */
    long child_pid = sys_exec("/worker.elf", NULL);
    if (child_pid < 0) {
        printf("multiagent: FAIL exec worker\n");
        return 1;
    }
    printf("multiagent: worker spawned (pid %ld)\n", child_pid);

    /* Close coordinator's copies of child ends */
    sys_close(0);   /* rA — child has it */
    sys_close(1);   /* wB — child has it */
    sys_close(rA);
    sys_close(wA);
    sys_close(wB);

    /* Send both commands, then close write pipe to signal EOF */
    sys_fwrite(saved_wA, "stat /hello.txt\n", 16);
    sys_fwrite(saved_wA, "ls\n", 3);
    sys_close(saved_wA);

    /* Read all output (worker processes both commands, then exits on EOF) */
    char resp[RESP_BUF];
    int n = read_all(rB, resp, RESP_BUF);

    /* Wait for worker to exit */
    long status = sys_waitpid(child_pid);
    printf("multiagent: worker exited (status %ld)\n", status);

    /* Check responses */
    int got_stat = (strstr(resp, "size=") != (void *)0);
    int got_ls = (strstr(resp, ".elf") != (void *)0);

    if (got_stat)
        printf("[worker] stat: OK (found size=)\n");
    if (got_ls)
        printf("[worker] ls: OK (%d bytes)\n", n);

    if (got_stat && got_ls)
        printf("multiagent: self-test PASSED\n");
    else
        printf("multiagent: self-test FAILED (stat=%d ls=%d)\n", got_stat, got_ls);

    sys_close(rB);
    return (got_stat && got_ls) ? 0 : 1;
}
