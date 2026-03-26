#include "libc.h"

int tool_dispatch(const char *path, const char **argv, long caps,
                  long cpu_ticks, const char *input, uint32_t input_len,
                  tool_result_t *result) {
    if (!path || !result)
        return -1;

    memset(result, 0, sizeof(*result));
    (void)caps;
    (void)cpu_ticks;

    /* Create stdin pipe (parent writes → child reads) */
    long stdin_rfd = -1, stdin_wfd = -1;
    if (sys_pipe(&stdin_rfd, &stdin_wfd) < 0)
        return -1;

    /* Create stdout pipe (child writes → parent reads) */
    long stdout_rfd = -1, stdout_wfd = -1;
    if (sys_pipe(&stdout_rfd, &stdout_wfd) < 0) {
        sys_close(stdin_rfd);
        sys_close(stdin_wfd);
        return -1;
    }

    /* Fork child */
    long child_pid = sys_fork();
    if (child_pid < 0) {
        sys_close(stdin_rfd);
        sys_close(stdin_wfd);
        sys_close(stdout_rfd);
        sys_close(stdout_wfd);
        return -1;
    }

    if (child_pid == 0) {
        /* Child: redirect fd 0 and fd 1 to pipes */
        sys_close(stdin_wfd);   /* parent's write end */
        sys_close(stdout_rfd);  /* parent's read end */

        sys_dup2(stdin_rfd, 0);    /* stdin ← pipe read end */
        sys_dup2(stdout_wfd, 1);   /* stdout → pipe write end */

        sys_close(stdin_rfd);   /* close original after dup */
        sys_close(stdout_wfd);

        /* Exec the tool */
        sys_execve(path, argv);
        /* If exec fails, exit */
        sys_exit(127);
    }

    /* Parent: close child-side pipe ends */
    sys_close(stdin_rfd);
    sys_close(stdout_wfd);

    /* Write input to child's stdin */
    if (input && input_len > 0) {
        sys_fwrite(stdin_wfd, input, input_len);
    }
    sys_close(stdin_wfd);  /* signal EOF */

    /* Read child's stdout */
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
