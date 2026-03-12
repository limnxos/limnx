#include "libc/libc.h"

#define BUF_SIZE 512

/* Read a line from fd 0 (pipe) into buf, returns length or -1 on EOF */
static int read_line(char *buf, int max) {
    int pos = 0;
    while (pos < max - 1) {
        char c;
        long n = sys_read(0, &c, 1);
        if (n <= 0) {
            if (pos > 0) break;  /* return partial line */
            return -1;            /* EOF */
        }
        if (c == '\n') break;
        buf[pos++] = c;
    }
    buf[pos] = '\0';
    return pos;
}

/* Write string to fd 1 (pipe) */
static void write_response(const char *s) {
    int len = (int)strlen(s);
    sys_fwrite(1, s, (unsigned long)len);
}

/* Write string + newline to fd 1 */
static void write_line(const char *s) {
    write_response(s);
    sys_fwrite(1, "\n", 1);
}

static void cmd_ls(void) {
    dirent_t ent;
    char line[300];
    int found = 0;
    for (unsigned long i = 0; i < 256; i++) {
        if (sys_readdir("/", i, &ent) != 0) break;
        /* Format: "name size=N" */
        int pos = 0;
        const char *name = ent.name;
        while (*name && pos < 250) line[pos++] = *name++;
        line[pos++] = ' ';
        line[pos++] = 's';
        line[pos++] = 'i';
        line[pos++] = 'z';
        line[pos++] = 'e';
        line[pos++] = '=';
        /* Convert size to string */
        char num[21];
        int ni = 0;
        uint64_t sz = ent.size;
        if (sz == 0) {
            num[ni++] = '0';
        } else {
            while (sz > 0) {
                num[ni++] = '0' + (char)(sz % 10);
                sz /= 10;
            }
        }
        for (int j = ni - 1; j >= 0; j--)
            line[pos++] = num[j];
        line[pos++] = '\n';
        line[pos] = '\0';
        write_response(line);
        found = 1;
    }
    if (!found)
        write_line("(empty)");
}

static void cmd_cat(const char *path) {
    long fd = sys_open(path, O_RDONLY);
    if (fd < 0) {
        write_line("error: cannot open file");
        return;
    }
    char buf[256];
    long n;
    while ((n = sys_read(fd, buf, 255)) > 0) {
        sys_fwrite(1, buf, (unsigned long)n);
    }
    sys_fwrite(1, "\n", 1);
    sys_close(fd);
}

static void cmd_stat(const char *path) {
    typedef struct {
        uint64_t size;
        uint8_t  type;
        uint8_t  mode;
        uint8_t  pad[6];
    } stat_t;

    stat_t st;
    long sr = sys_stat(path, &st);
    if (sr != 0) {
        write_line("error: stat failed");
        return;
    }

    /* Format: "size=N type=T" */
    char line[64];
    int pos = 0;
    const char *prefix = "size=";
    while (*prefix) line[pos++] = *prefix++;

    char num[21];
    int ni = 0;
    uint64_t sz = st.size;
    if (sz == 0) {
        num[ni++] = '0';
    } else {
        while (sz > 0) {
            num[ni++] = '0' + (char)(sz % 10);
            sz /= 10;
        }
    }
    for (int j = ni - 1; j >= 0; j--)
        line[pos++] = num[j];

    const char *mid = " type=";
    while (*mid) line[pos++] = *mid++;
    line[pos++] = '0' + st.type;
    line[pos] = '\0';
    write_line(line);
}

static void process_command(const char *line) {
    if (strcmp(line, "ls") == 0) {
        cmd_ls();
    } else if (strncmp(line, "cat ", 4) == 0) {
        cmd_cat(line + 4);
    } else if (strncmp(line, "stat ", 5) == 0) {
        cmd_stat(line + 5);
    } else {
        write_line("error: unknown command");
    }
}

int main(void) {
    char line[BUF_SIZE];

    for (;;) {
        int len = read_line(line, BUF_SIZE);
        if (len < 0) break;  /* EOF — pipe closed */
        if (len == 0) continue;
        process_command(line);
    }

    return 0;
}
