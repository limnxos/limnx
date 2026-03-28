#include "libc.h"

int tool_dispatch(const char *path, const char **argv, long caps,
                  long cpu_ticks, const char *input, uint32_t input_len,
                  tool_result_t *result) {
    if (!path || !result)
        return -1;

    memset(result, 0, sizeof(*result));
    (void)caps;
    (void)cpu_ticks;
    (void)input;
    (void)input_len;

    /* Create stdout pipe (child writes → parent reads) */
    long stdout_rfd = -1, stdout_wfd = -1;
    if (sys_pipe(&stdout_rfd, &stdout_wfd) < 0)
        return -1;

    /* Fork child */
    long child_pid = sys_fork();
    if (child_pid < 0) {
        sys_close(stdout_rfd);
        sys_close(stdout_wfd);
        return -1;
    }

    if (child_pid == 0) {
        /* Child: redirect stdout to pipe write end */
        sys_close(stdout_rfd);
        sys_dup2(stdout_wfd, 1);
        sys_close(stdout_wfd);

        /* Build argv for exec */
        const char *exec_argv[4];
        exec_argv[0] = path;
        exec_argv[1] = (argv && argv[1]) ? argv[1] : (void *)0;
        exec_argv[2] = (void *)0;

        /* sys_execve creates a new process — it does NOT replace us.
         * The new process inherits our fd table (fd 1 = pipe).
         * We must wait for it, then exit. But waiting here can deadlock
         * if the parent is also blocking on the pipe. So just exit
         * immediately — the grandchild continues with the pipe. */
        long gc = sys_execve(path, exec_argv);
        /* Exit with 0 — grandchild runs independently */
        sys_exit(gc > 0 ? 0 : 127);
    }

    /* Parent: close write end so we get EOF when child/grandchild close theirs */
    sys_close(stdout_wfd);

    /* Reap the child immediately (it exits right after execve) */
    sys_waitpid(child_pid);

    /* Read grandchild's output from pipe */
    uint32_t total = 0;
    while (total < sizeof(result->output) - 1) {
        long n = sys_read(stdout_rfd, result->output + total,
                          sizeof(result->output) - 1 - total);
        if (n <= 0) break;
        total += (uint32_t)n;
    }
    result->output[total] = '\0';
    result->output_len = total;
    sys_close(stdout_rfd);

    return 0;
}
