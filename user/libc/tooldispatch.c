#include "libc.h"

int tool_dispatch(const char *path, const char **argv, long caps,
                  long cpu_ticks, const char *input, uint32_t input_len,
                  tool_result_t *result) {
    if (!path || !result)
        return -1;

    memset(result, 0, sizeof(*result));
    (void)caps;       /* TODO: apply via sys_setcap on child when Limnx supports it */
    (void)cpu_ticks;  /* TODO: apply via sys_setrlimit on child when Limnx supports it */

    /* In Limnx, sys_exec spawns a new child (does not replace current process).
     * The child inherits our fd_table. We create pipes, mark parent-side fds
     * as FD_CLOEXEC so the child only gets the child-side ends. */

    /* Create stdin pipe (parent writes, child reads from fd 0) */
    long stdin_rfd = -1, stdin_wfd = -1;
    if (sys_pipe(&stdin_rfd, &stdin_wfd) < 0)
        return -1;

    /* Create stdout pipe (child writes to fd 1, parent reads) */
    long stdout_rfd = -1, stdout_wfd = -1;
    if (sys_pipe(&stdout_rfd, &stdout_wfd) < 0) {
        sys_close(stdin_rfd);
        sys_close(stdin_wfd);
        return -1;
    }

    /* Mark parent-side pipe ends as FD_CLOEXEC so child doesn't inherit them */
    sys_fcntl(stdin_wfd, F_SETFD, FD_CLOEXEC);
    sys_fcntl(stdout_rfd, F_SETFD, FD_CLOEXEC);

    /* Exec the tool — child inherits stdin_rfd and stdout_wfd (no cloexec) */
    long child_pid = sys_exec(path, argv);
    if (child_pid < 0) {
        sys_close(stdin_rfd);
        sys_close(stdin_wfd);
        sys_close(stdout_rfd);
        sys_close(stdout_wfd);
        return -1;
    }

    /* Close child-side pipe ends in parent */
    sys_close(stdin_rfd);
    sys_close(stdout_wfd);

    /* Write input to child via our pipe write end */
    if (input && input_len > 0) {
        sys_fwrite(stdin_wfd, input, input_len);
    }
    sys_close(stdin_wfd);  /* signal EOF to child */

    /* Read output from child via our pipe read end */
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

    /* Wait for child */
    long status = sys_waitpid(child_pid);
    result->exit_status = (int)status;

    return 0;
}
