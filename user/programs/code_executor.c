/*
 * code_executor.c — Tool: execute a command and return its output
 *
 * stdin or argv[1]: command to execute (e.g. "ls /")
 * stdout: command output
 * exit 0 on success, 1 on error
 *
 * Uses fork+exec+pipe internally to capture command output.
 * Should be launched with seccomp sandbox.
 */
#include "../libc/libc.h"

int main(int argc, char **argv) {
    char cmd[256];
    int len = 0;

    if (argc >= 2) {
        const char *s = argv[1];
        while (*s && len < 255) cmd[len++] = *s++;
    } else {
        long n = sys_read(0, cmd, 255);
        if (n <= 0) {
            sys_write("[code_executor] no command\n", 27);
            return 1;
        }
        len = (int)n;
        while (len > 0 && (cmd[len-1] == '\n' || cmd[len-1] == '\r'))
            len--;
    }
    cmd[len] = '\0';

    if (len == 0) {
        sys_write("[code_executor] empty command\n", 30);
        return 1;
    }

    /* Parse command: first word is the binary, rest are args */
    char bin[128];
    int bi = 0;
    int ci = 0;
    while (cmd[ci] && cmd[ci] != ' ' && bi < 127)
        bin[bi++] = cmd[ci++];
    bin[bi] = '\0';
    while (cmd[ci] == ' ') ci++;

    /* Build path: try /bin/<cmd> first, then /<cmd>.elf */
    char path[256];
    int pi;

    /* Try /bin/<cmd> (busybox symlinks) */
    pi = 0;
    const char *pfx = "/bin/";
    while (*pfx) path[pi++] = *pfx++;
    for (int i = 0; i < bi; i++) path[pi++] = bin[i];
    path[pi] = '\0';

    /* Build argv */
    const char *exec_argv[4];
    exec_argv[0] = bin;
    exec_argv[1] = cmd[ci] ? &cmd[ci] : (void *)0;
    exec_argv[2] = (void *)0;

    /* Create pipe to capture output */
    long rfd = -1, wfd = -1;
    if (sys_pipe(&rfd, &wfd) < 0) {
        sys_write("[code_executor] pipe failed\n", 28);
        return 1;
    }

    long pid = sys_fork();
    if (pid == 0) {
        /* Child: redirect stdout to pipe write end */
        sys_close(rfd);
        sys_dup2(wfd, 1);
        sys_close(wfd);

        /* Try /bin/<cmd> first */
        sys_execve(path, exec_argv);

        /* Try /<cmd>.elf */
        pi = 0;
        path[pi++] = '/';
        for (int i = 0; i < bi; i++) path[pi++] = bin[i];
        const char *ext = ".elf";
        while (*ext) path[pi++] = *ext++;
        path[pi] = '\0';
        sys_execve(path, exec_argv);

        /* Failed */
        const char *err = "[code_executor] exec failed\n";
        sys_fwrite(2, err, 28);
        sys_exit(127);
    }

    if (pid < 0) {
        sys_write("[code_executor] fork failed\n", 28);
        sys_close(rfd);
        sys_close(wfd);
        return 1;
    }

    /* Parent: read child output from pipe */
    sys_close(wfd);
    char buf[512];
    long n;
    while ((n = sys_read(rfd, buf, sizeof(buf))) > 0) {
        sys_fwrite(1, buf, (unsigned long)n);
    }
    sys_close(rfd);

    sys_waitpid(pid);
    return 0;
}
