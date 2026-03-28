#include "libc.h"

int tool_dispatch(const char *path, const char **argv, long caps,
                  long cpu_ticks, const char *input, uint32_t input_len,
                  tool_result_t *result) {
    if (!path || !result)
        return -1;

    memset(result, 0, sizeof(*result));
    (void)caps;
    (void)cpu_ticks;

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
        sys_close(stdout_rfd);  /* parent's read end */
        sys_dup2(stdout_wfd, 1);
        sys_close(stdout_wfd);

        /* Write input to a temp file if needed (tool reads via argv) */
        /* For now, tools get input via argv[1] */

        /* Exec the tool — sys_execve creates a NEW process that
         * inherits our fd table (including the dup2'd fd 1).
         * We then exit so the parent only waits for us. */
        const char *exec_argv[4];
        exec_argv[0] = path;
        exec_argv[1] = (argv && argv[1]) ? argv[1] : (void *)0;
        exec_argv[2] = (void *)0;

        long grandchild = sys_execve(path, exec_argv);
        if (grandchild > 0) {
            /* Wait for grandchild to finish */
            sys_waitpid(grandchild);
        }
        sys_exit(grandchild > 0 ? 0 : 127);
    }

    /* Parent: close write end, read child's output */
    sys_close(stdout_wfd);

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
