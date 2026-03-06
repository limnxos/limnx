#include "libc/libc.h"

#define MAX_LINE 256
#define MAX_ARGS 16

/* Check if fd 0 is a PTY (ioctl TCGETS succeeds) */
static int stdin_is_pty = 0;

/* Read a line from PTY stdin (canonical mode handles echo + backspace) */
static int getline_pty(char *buf, int max) {
    long n = sys_read(0, buf, max - 1);
    if (n <= 0) return 0;

    /* Strip trailing newline */
    int len = (int)n;
    if (len > 0 && buf[len - 1] == '\n')
        len--;
    buf[len] = '\0';
    return len;
}

/* Read a line from keyboard input, echoing chars back (legacy) */
static int getline_serial(char *buf, int max) {
    int pos = 0;
    while (pos < max - 1) {
        long ch = sys_getchar();
        if (ch == '\n' || ch == '\r') {
            /* Echo newline */
            char nl = '\n';
            sys_write(&nl, 1);
            break;
        } else if (ch == '\b' || ch == 127) {
            if (pos > 0) {
                pos--;
                /* Erase character on terminal: backspace, space, backspace */
                sys_write("\b \b", 3);
            }
        } else if (ch >= 32 && ch < 127) {
            buf[pos++] = (char)ch;
            char c = (char)ch;
            sys_write(&c, 1);
        }
    }
    buf[pos] = '\0';
    return pos;
}

static int getline_input(char *buf, int max) {
    if (stdin_is_pty)
        return getline_pty(buf, max);
    return getline_serial(buf, max);
}

/* Split line on spaces into argv[] */
static int parse_args(char *line, char **argv, int max_args) {
    int argc = 0;
    char *p = line;

    while (*p && argc < max_args - 1) {
        /* Skip leading spaces */
        while (*p == ' ') p++;
        if (*p == '\0') break;

        argv[argc++] = p;

        /* Find end of token */
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

static void cmd_help(void) {
    printf("Limnx Shell commands:\n");
    printf("  help             Show this help\n");
    printf("  echo <text>      Print text\n");
    printf("  ls [dir]         List files (default: cwd)\n");
    printf("  cat <file>       Show file contents\n");
    printf("  mkdir <dir>      Create directory\n");
    printf("  cd <dir>         Change directory\n");
    printf("  pwd              Print working directory\n");
    printf("  mv <src> <dst>   Rename/move file\n");
    printf("  exec <program>   Run program and wait\n");
    printf("  pid              Show current PID\n");
    printf("  exit             Exit shell\n");
}

static void cmd_ls(const char *dir) {
    if (!dir || !*dir)
        dir = ".";
    dirent_t ent;
    int count = 0;
    for (unsigned long i = 0; sys_readdir(dir, i, &ent) == 0; i++) {
        const char *type_str = (ent.type == 1) ? "d" : "-";
        printf("  %s %-24s %lu bytes\n", type_str, ent.name, ent.size);
        count++;
    }
    if (count == 0) {
        printf("  (empty or not found)\n");
    }
}

static void cmd_cat(const char *path) {
    long fd = sys_open(path, 0);
    if (fd < 0) {
        printf("cat: cannot open '%s'\n", path);
        return;
    }

    char buf[512];
    long n;
    while ((n = sys_read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }
    printf("\n");
    sys_close(fd);
}

static void cmd_exec(int argc, char **argv) {
    (void)argc;
    /* argv[0] = program path, argv[1..] = arguments for child */
    long child_pid = sys_exec(argv[0], (const char **)argv);
    if (child_pid <= 0) {
        printf("exec: failed to run '%s'\n", argv[0]);
        return;
    }
    printf("[shell] running %s (pid %ld)...\n", argv[0], child_pid);
    long status = sys_waitpid(child_pid);
    printf("[shell] %s exited with status %ld\n", argv[0], status);
}

int main(int argc_init, char **argv_init) {
    (void)argc_init; (void)argv_init;

    /* Detect if stdin (fd 0) is a PTY */
    termios_t t;
    if (sys_ioctl(0, TCGETS, (long)&t) == 0) {
        stdin_is_pty = 1;
    }

    printf("\n");
    printf("========================================\n");
    printf("  Limnx Shell\n");
    printf("========================================\n");
    printf("Type 'help' for commands.\n\n");

    char line[MAX_LINE];
    char *argv[MAX_ARGS];

    for (;;) {
        printf("limnx> ");
        int len = getline_input(line, MAX_LINE);
        if (len == 0)
            continue;

        int argc = parse_args(line, argv, MAX_ARGS);
        if (argc == 0)
            continue;

        if (strcmp(argv[0], "help") == 0) {
            cmd_help();
        } else if (strcmp(argv[0], "echo") == 0) {
            for (int i = 1; i < argc; i++) {
                if (i > 1) printf(" ");
                printf("%s", argv[i]);
            }
            printf("\n");
        } else if (strcmp(argv[0], "ls") == 0) {
            cmd_ls(argc > 1 ? argv[1] : ".");
        } else if (strcmp(argv[0], "cd") == 0) {
            if (argc < 2)
                printf("usage: cd <dir>\n");
            else {
                long rc = sys_chdir(argv[1]);
                if (rc < 0)
                    printf("cd: cannot change to '%s'\n", argv[1]);
            }
        } else if (strcmp(argv[0], "pwd") == 0) {
            char cwdbuf[256];
            sys_getcwd(cwdbuf, sizeof(cwdbuf));
            printf("%s\n", cwdbuf);
        } else if (strcmp(argv[0], "mv") == 0) {
            if (argc < 3)
                printf("usage: mv <src> <dst>\n");
            else {
                long rc = sys_rename(argv[1], argv[2]);
                if (rc < 0)
                    printf("mv: failed to rename '%s' to '%s'\n", argv[1], argv[2]);
                else
                    printf("mv: renamed '%s' -> '%s'\n", argv[1], argv[2]);
            }
        } else if (strcmp(argv[0], "mkdir") == 0) {
            if (argc < 2)
                printf("usage: mkdir <dir>\n");
            else {
                long rc = sys_mkdir(argv[1]);
                if (rc < 0)
                    printf("mkdir: failed to create '%s'\n", argv[1]);
                else
                    printf("mkdir: created '%s'\n", argv[1]);
            }
        } else if (strcmp(argv[0], "cat") == 0) {
            if (argc < 2)
                printf("usage: cat <file>\n");
            else
                cmd_cat(argv[1]);
        } else if (strcmp(argv[0], "exec") == 0) {
            if (argc < 2)
                printf("usage: exec <program> [args...]\n");
            else
                cmd_exec(argc - 1, &argv[1]);
        } else if (strcmp(argv[0], "pid") == 0) {
            printf("PID: %ld\n", sys_getpid());
        } else if (strcmp(argv[0], "exit") == 0) {
            printf("Goodbye.\n");
            break;
        } else {
            printf("Unknown command: %s\n", argv[0]);
        }
    }

    return 0;
}
