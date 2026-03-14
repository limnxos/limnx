/* kill — send signal to process */
#include "../libc/libc.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: kill [-signal] pid\n");
        return 1;
    }
    int sig = 15;  /* SIGTERM */
    int pid_arg = 1;

    if (argv[1][0] == '-') {
        sig = atoi(&argv[1][1]);
        if (sig <= 0) {
            printf("kill: invalid signal '%s'\n", argv[1]);
            return 1;
        }
        pid_arg = 2;
    }
    if (pid_arg >= argc) {
        printf("usage: kill [-signal] pid\n");
        return 1;
    }
    long pid = atoi(argv[pid_arg]);
    if (pid <= 0) {
        printf("kill: invalid pid '%s'\n", argv[pid_arg]);
        return 1;
    }
    if (sys_kill(pid, sig) < 0) {
        printf("kill: failed to signal pid %ld\n", pid);
        return 1;
    }
    return 0;
}
