#include "libc/libc.h"

#define MAX_LINE    512
#define MAX_ARGS    32
#define MAX_CMDS    8     /* max pipeline stages */
#define HISTORY_MAX 32

/* --- Globals --- */
static int stdin_is_pty = 0;
static int last_exit_status = 0;
static volatile long fg_pid = 0;  /* PID of foreground job (0 = none) */

/* Retry waitpid on -EINTR */
static long waitpid_retry(long pid) {
    long st;
    while ((st = sys_waitpid(pid)) == -EINTR)
        ;
    return st;
}

/* --- History --- */
static char history[HISTORY_MAX][MAX_LINE];
static int history_count = 0;
static int history_pos = 0;  /* next write slot */

static void history_add(const char *line) {
    if (!line[0]) return;
    /* Don't add duplicates of previous entry */
    if (history_count > 0) {
        int prev = (history_pos - 1 + HISTORY_MAX) % HISTORY_MAX;
        if (strcmp(history[prev], line) == 0) return;
    }
    int len = 0;
    while (line[len] && len < MAX_LINE - 1) {
        history[history_pos][len] = line[len];
        len++;
    }
    history[history_pos][len] = '\0';
    history_pos = (history_pos + 1) % HISTORY_MAX;
    if (history_count < HISTORY_MAX) history_count++;
}

static const char *history_get(int idx) {
    if (idx < 0 || idx >= history_count) return NULL;
    int pos = (history_pos - history_count + idx + HISTORY_MAX) % HISTORY_MAX;
    return history[pos];
}

/* --- String helpers --- */

static char *strchr_local(const char *s, char c) {
    while (*s) {
        if (*s == c) return (char *)s;
        s++;
    }
    return NULL;
}

static int isspace_local(char c) {
    return c == ' ' || c == '\t';
}

/* --- Glob pattern matching --- */

/* Match a string against a glob pattern (* and ? wildcards) */
static int glob_match(const char *pattern, const char *str) {
    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return 1;  /* trailing * matches all */
            while (*str) {
                if (glob_match(pattern, str)) return 1;
                str++;
            }
            return 0;
        } else if (*pattern == '?') {
            if (!*str) return 0;
            pattern++; str++;
        } else {
            if (*pattern != *str) return 0;
            pattern++; str++;
        }
    }
    return *str == '\0';
}

/* Check if a string contains glob characters */
static int has_glob(const char *s) {
    while (*s) {
        if (*s == '*' || *s == '?') return 1;
        s++;
    }
    return 0;
}

/* Expand glob pattern into argv slots. Returns number of matches.
 * Fills expanded[] up to max entries. */
static int glob_expand(const char *pattern, char expanded[][128], int max) {
    /* Extract directory and file pattern */
    char dir[256] = ".";
    char fpat[128];
    int last_slash = -1;
    for (int i = 0; pattern[i]; i++)
        if (pattern[i] == '/') last_slash = i;

    if (last_slash >= 0) {
        int di = 0;
        for (int i = 0; i < last_slash && di < 255; i++)
            dir[di++] = pattern[i];
        dir[di] = '\0';
        if (di == 0) { dir[0] = '/'; dir[1] = '\0'; }
        int pi = 0;
        for (int i = last_slash + 1; pattern[i] && pi < 127; i++)
            fpat[pi++] = pattern[i];
        fpat[pi] = '\0';
    } else {
        int pi = 0;
        for (int i = 0; pattern[i] && pi < 127; i++)
            fpat[pi++] = pattern[i];
        fpat[pi] = '\0';
    }

    int count = 0;
    dirent_t ent;
    for (unsigned long i = 0; sys_readdir(dir, i, &ent) == 0 && count < max; i++) {
        if (glob_match(fpat, ent.name)) {
            /* Build full path if pattern had directory prefix */
            if (last_slash >= 0) {
                int pos = 0;
                for (int j = 0; dir[j] && pos < 126; j++)
                    expanded[count][pos++] = dir[j];
                if (pos > 0 && expanded[count][pos-1] != '/')
                    expanded[count][pos++] = '/';
                for (int j = 0; ent.name[j] && pos < 127; j++)
                    expanded[count][pos++] = ent.name[j];
                expanded[count][pos] = '\0';
            } else {
                int pos = 0;
                for (int j = 0; ent.name[j] && pos < 127; j++)
                    expanded[count][pos++] = ent.name[j];
                expanded[count][pos] = '\0';
            }
            count++;
        }
    }
    return count;
}

/* --- Tab completion --- */

/* Find completions for a prefix in the given directory.
 * Returns number of matches, fills matches[] with names. */
static int find_completions(const char *prefix, const char *dir,
                            char matches[][256], int max) {
    int plen = 0;
    while (prefix[plen]) plen++;

    int count = 0;
    dirent_t ent;
    for (unsigned long i = 0; sys_readdir(dir, i, &ent) == 0 && count < max; i++) {
        /* Check if name starts with prefix */
        int match = 1;
        for (int j = 0; j < plen; j++) {
            if (ent.name[j] != prefix[j]) { match = 0; break; }
        }
        if (match) {
            int ni = 0;
            while (ent.name[ni] && ni < 255) {
                matches[count][ni] = ent.name[ni];
                ni++;
            }
            matches[count][ni] = '\0';
            count++;
        }
    }
    return count;
}

/* Find the longest common prefix among completions */
static int common_prefix_len(char matches[][256], int count) {
    if (count <= 1) return count ? (int)strlen(matches[0]) : 0;
    int len = 0;
    for (;;) {
        char c = matches[0][len];
        if (!c) break;
        int all_match = 1;
        for (int i = 1; i < count; i++) {
            if (matches[i][len] != c) { all_match = 0; break; }
        }
        if (!all_match) break;
        len++;
    }
    return len;
}

/* --- Input --- */

/* Read a line with history support. Uses raw PTY mode for arrow keys. */
static int getline_pty_history(char *buf, int max) {
    /* Switch to raw mode for arrow key support */
    termios_t saved, raw;
    sys_ioctl(0, TCGETS, (long)&saved);
    raw = saved;
    raw.c_lflag &= ~(TERMIOS_ICANON | TERMIOS_ECHO);
    sys_ioctl(0, TCSETS, (long)&raw);

    int pos = 0;
    int hist_browse = -1;  /* -1 = not browsing */
    char saved_line[MAX_LINE];
    saved_line[0] = '\0';

    for (;;) {
        char ch;
        long n = sys_read(0, &ch, 1);
        if (n <= 0) continue;

        if (ch == '\n' || ch == '\r') {
            sys_write("\n", 1);
            break;
        }

        /* Escape sequence (arrow keys: ESC [ A/B) */
        if (ch == 0x1B) {
            char seq[2];
            long r = sys_read(0, &seq[0], 1);
            if (r <= 0) continue;
            if (seq[0] == '[') {
                r = sys_read(0, &seq[1], 1);
                if (r <= 0) continue;

                if (seq[1] == 'A') {
                    /* Up arrow - previous history */
                    if (history_count == 0) continue;
                    if (hist_browse < 0) {
                        hist_browse = history_count - 1;
                        /* Save current line */
                        for (int i = 0; i < pos; i++)
                            saved_line[i] = buf[i];
                        saved_line[pos] = '\0';
                    } else if (hist_browse > 0) {
                        hist_browse--;
                    } else {
                        continue;
                    }
                    goto show_history;
                } else if (seq[1] == 'B') {
                    /* Down arrow - next history */
                    if (hist_browse < 0) continue;
                    hist_browse++;
                    if (hist_browse >= history_count) {
                        /* Restore saved line */
                        hist_browse = -1;
                        /* Erase current display */
                        while (pos > 0) {
                            sys_write("\b \b", 3);
                            pos--;
                        }
                        int slen = 0;
                        while (saved_line[slen]) slen++;
                        for (int i = 0; i < slen && i < max - 1; i++)
                            buf[i] = saved_line[i];
                        pos = slen;
                        buf[pos] = '\0';
                        sys_write(buf, pos);
                        continue;
                    }
                    goto show_history;
                }
            }
            continue;

        show_history: {
                const char *h = history_get(hist_browse);
                if (!h) continue;
                /* Erase current display */
                while (pos > 0) {
                    sys_write("\b \b", 3);
                    pos--;
                }
                int hlen = 0;
                while (h[hlen]) hlen++;
                if (hlen > max - 1) hlen = max - 1;
                for (int i = 0; i < hlen; i++)
                    buf[i] = h[i];
                pos = hlen;
                buf[pos] = '\0';
                sys_write(buf, pos);
                continue;
            }
        }

        /* Backspace / DEL */
        if (ch == 0x7F || ch == 0x08) {
            if (pos > 0) {
                pos--;
                sys_write("\b \b", 3);
            }
            continue;
        }

        /* ^C - cancel line */
        if (ch == 0x03) {
            sys_write("^C\n", 3);
            pos = 0;
            break;
        }

        /* ^U - kill line */
        if (ch == 0x15) {
            while (pos > 0) {
                sys_write("\b \b", 3);
                pos--;
            }
            continue;
        }

        /* ^D on empty line = EOF */
        if (ch == 0x04 && pos == 0) {
            sys_ioctl(0, TCSETS, (long)&saved);
            return -1;
        }

        /* TAB — tab completion */
        if (ch == '\t') {
            buf[pos] = '\0';
            /* Find the last token (word being typed) */
            int tok_start = pos;
            while (tok_start > 0 && buf[tok_start - 1] != ' ')
                tok_start--;
            char *token = &buf[tok_start];
            int tok_len = pos - tok_start;

            /* Split into directory and prefix */
            char comp_dir[256] = ".";
            const char *comp_prefix = token;
            int last_slash = -1;
            for (int ci = 0; ci < tok_len; ci++)
                if (token[ci] == '/') last_slash = ci;

            if (last_slash >= 0) {
                int di = 0;
                for (int ci = 0; ci < last_slash && di < 255; ci++)
                    comp_dir[di++] = token[ci];
                comp_dir[di] = '\0';
                if (di == 0) { comp_dir[0] = '/'; comp_dir[1] = '\0'; }
                comp_prefix = &token[last_slash + 1];
            }

            static char tab_matches[16][256];
            int nmatches = find_completions(comp_prefix, comp_dir, tab_matches, 16);

            if (nmatches == 1) {
                /* Single match — complete it */
                int mlen = strlen(tab_matches[0]);
                int plen = strlen(comp_prefix);
                for (int ci = plen; ci < mlen && pos < max - 1; ci++) {
                    buf[pos++] = tab_matches[0][ci];
                    sys_write(&tab_matches[0][ci], 1);
                }
            } else if (nmatches > 1) {
                /* Multiple matches — complete common prefix, show options */
                int cplen = common_prefix_len(tab_matches, nmatches);
                int plen = strlen(comp_prefix);
                if (cplen > plen) {
                    for (int ci = plen; ci < cplen && pos < max - 1; ci++) {
                        buf[pos++] = tab_matches[0][ci];
                        sys_write(&tab_matches[0][ci], 1);
                    }
                } else {
                    /* Show all matches */
                    sys_write("\n", 1);
                    for (int ci = 0; ci < nmatches; ci++) {
                        sys_write(tab_matches[ci], strlen(tab_matches[ci]));
                        sys_write("  ", 2);
                    }
                    sys_write("\n", 1);
                    /* Re-display prompt and current input */
                    char cwdbuf2[128];
                    sys_getcwd(cwdbuf2, sizeof(cwdbuf2));
                    printf("%s> ", cwdbuf2);
                    buf[pos] = '\0';
                    sys_write(buf, pos);
                }
            }
            continue;
        }

        /* Printable character */
        if (ch >= 32 && ch < 127 && pos < max - 1) {
            buf[pos++] = ch;
            sys_write(&ch, 1);
            hist_browse = -1;
        }
    }

    buf[pos] = '\0';

    /* Restore canonical mode */
    sys_ioctl(0, TCSETS, (long)&saved);
    return pos;
}

/* Legacy serial input (no PTY) */
static int getline_serial(char *buf, int max) {
    int pos = 0;
    while (pos < max - 1) {
        long ch = sys_getchar();
        if (ch == '\n' || ch == '\r') {
            sys_write("\n", 1);
            break;
        } else if (ch == '\b' || ch == 127) {
            if (pos > 0) {
                pos--;
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
        return getline_pty_history(buf, max);
    return getline_serial(buf, max);
}

/* --- Tokenizer with quoting and variable expansion --- */

/* Expand $VAR, $?, $$ in a token. Returns pointer to static buffer. */
static char expand_buf[MAX_LINE];

static char *expand_vars(const char *token) {
    int out = 0;
    int i = 0;
    while (token[i] && out < MAX_LINE - 1) {
        if (token[i] == '$') {
            i++;
            if (token[i] == '?') {
                /* $? = last exit status */
                i++;
                /* itoa into expand_buf */
                int val = last_exit_status;
                char tmp[16];
                int ti = 0;
                if (val < 0) { expand_buf[out++] = '-'; val = -val; }
                if (val == 0) { tmp[ti++] = '0'; }
                else { while (val > 0 && ti < 15) { tmp[ti++] = '0' + (val % 10); val /= 10; } }
                while (ti > 0 && out < MAX_LINE - 1) expand_buf[out++] = tmp[--ti];
            } else if (token[i] == '$') {
                /* $$ = PID */
                i++;
                long pid = sys_getpid();
                char tmp[16];
                int ti = 0;
                if (pid == 0) { tmp[ti++] = '0'; }
                else { while (pid > 0 && ti < 15) { tmp[ti++] = '0' + (int)(pid % 10); pid /= 10; } }
                while (ti > 0 && out < MAX_LINE - 1) expand_buf[out++] = tmp[--ti];
            } else {
                /* $VARNAME */
                char varname[64];
                int vi = 0;
                while ((token[i] >= 'A' && token[i] <= 'Z') ||
                       (token[i] >= 'a' && token[i] <= 'z') ||
                       (token[i] >= '0' && token[i] <= '9') ||
                       token[i] == '_') {
                    if (vi < 63) varname[vi++] = token[i];
                    i++;
                }
                varname[vi] = '\0';
                if (vi > 0) {
                    char val[128];
                    long r = sys_getenv(varname, val, sizeof(val));
                    if (r >= 0) {
                        for (int j = 0; val[j] && out < MAX_LINE - 1; j++)
                            expand_buf[out++] = val[j];
                    }
                }
            }
        } else {
            expand_buf[out++] = token[i++];
        }
    }
    expand_buf[out] = '\0';
    return expand_buf;
}

/* A single command in a pipeline */
typedef struct {
    char *argv[MAX_ARGS];
    int argc;
    char *redir_in;     /* < file */
    char *redir_out;    /* > file */
    int redir_append;   /* >> */
    int background;     /* & */
} command_t;

/* Parse a line into semicolon-separated segments, then pipes within each.
 * Returns number of pipeline stages in cmds[]. */
static int parse_pipeline(char *line, command_t *cmds, int max_cmds) {
    int ncmds = 0;

    /* Split on pipe '|' */
    char *segments[MAX_CMDS];
    int nseg = 0;
    char *p = line;
    segments[nseg++] = p;
    int in_single = 0, in_double = 0;
    while (*p) {
        if (*p == '\'' && !in_double) { in_single = !in_single; }
        else if (*p == '"' && !in_single) { in_double = !in_double; }
        else if (*p == '|' && !in_single && !in_double) {
            *p = '\0';
            p++;
            if (nseg < MAX_CMDS) segments[nseg++] = p;
            continue;
        }
        p++;
    }

    /* Parse each segment into a command */
    for (int s = 0; s < nseg && ncmds < max_cmds; s++) {
        command_t *cmd = &cmds[ncmds];
        cmd->argc = 0;
        cmd->redir_in = NULL;
        cmd->redir_out = NULL;
        cmd->redir_append = 0;
        cmd->background = 0;

        char *cp = segments[s];
        while (*cp) {
            while (isspace_local(*cp)) cp++;
            if (*cp == '\0') break;

            /* I/O redirection */
            if (*cp == '<') {
                cp++;
                while (isspace_local(*cp)) cp++;
                cmd->redir_in = cp;
                while (*cp && !isspace_local(*cp)) cp++;
                if (*cp) *cp++ = '\0';
                continue;
            }
            if (*cp == '>') {
                cp++;
                if (*cp == '>') {
                    cmd->redir_append = 1;
                    cp++;
                }
                while (isspace_local(*cp)) cp++;
                cmd->redir_out = cp;
                while (*cp && !isspace_local(*cp)) cp++;
                if (*cp) *cp++ = '\0';
                continue;
            }

            /* Background */
            if (*cp == '&') {
                cmd->background = 1;
                cp++;
                continue;
            }

            /* Token (with quoting) */
            if (cmd->argc >= MAX_ARGS - 1) { cp++; continue; }

            /* Collect token */
            static char token_buf[MAX_CMDS][MAX_ARGS][128];
            char *tok = token_buf[s][cmd->argc];
            int ti = 0;

            while (*cp && ti < 126) {
                if (*cp == '\'' && !in_double) {
                    cp++;
                    while (*cp && *cp != '\'') {
                        if (ti < 126) tok[ti++] = *cp;
                        cp++;
                    }
                    if (*cp == '\'') cp++;
                } else if (*cp == '"') {
                    cp++;
                    while (*cp && *cp != '"') {
                        if (*cp == '$') {
                            /* Variable expansion inside double quotes */
                            tok[ti++] = '$';
                        } else {
                            if (ti < 126) tok[ti++] = *cp;
                        }
                        cp++;
                    }
                    if (*cp == '"') cp++;
                } else if (isspace_local(*cp) || *cp == '|' || *cp == '<' || *cp == '>' || *cp == '&') {
                    break;
                } else {
                    tok[ti++] = *cp++;
                }
            }
            tok[ti] = '\0';

            /* Expand variables */
            if (strchr_local(tok, '$')) {
                char *expanded = expand_vars(tok);
                /* Copy expanded back to token buffer */
                int elen = 0;
                while (expanded[elen] && elen < 126) {
                    tok[elen] = expanded[elen];
                    elen++;
                }
                tok[elen] = '\0';
            }

            /* Glob expansion */
            if (has_glob(tok)) {
                static char glob_results[32][128];
                int nglob = glob_expand(tok, glob_results, 32);
                if (nglob > 0) {
                    for (int gi = 0; gi < nglob && cmd->argc < MAX_ARGS - 1; gi++) {
                        /* Copy glob result into a stable buffer */
                        static char glob_bufs[MAX_CMDS * MAX_ARGS][128];
                        static int glob_buf_idx = 0;
                        char *gbuf = glob_bufs[glob_buf_idx++ % (MAX_CMDS * MAX_ARGS)];
                        int gbi = 0;
                        while (glob_results[gi][gbi] && gbi < 127) {
                            gbuf[gbi] = glob_results[gi][gbi];
                            gbi++;
                        }
                        gbuf[gbi] = '\0';
                        cmd->argv[cmd->argc++] = gbuf;
                    }
                } else {
                    /* No match — keep literal */
                    cmd->argv[cmd->argc++] = tok;
                }
            } else {
                cmd->argv[cmd->argc++] = tok;
            }
        }
        cmd->argv[cmd->argc] = NULL;
        if (cmd->argc > 0) ncmds++;
    }

    return ncmds;
}

/* --- Built-in commands --- */

/* --- Test runner --- */

typedef struct {
    const char *name;
    const char *elf;
    const char *arg;  /* optional extra arg (e.g. "--test") */
} test_entry_t;

static const test_entry_t tests[] = {
    {"hello",      "/hello.elf",       NULL},
    {"cat",        "/cat.elf",         NULL},
    {"writetest",  "/writetest.elf",   NULL},
    {"mathtest",   "/mathtest.elf",    NULL},
    {"agenttest",  "/agenttest.elf",   NULL},
    {"agentrt",    "/agentrt.elf",     NULL},
    {"infertest",  "/infertest.elf",   NULL},
    {"pipetest",   "/pipetest.elf",    NULL},
    {"generate",   "/generate.elf",    "--test"},
    {"toolagent",  "/toolagent.elf",   "--test"},
    {"memtest",    "/memtest.elf",     NULL},
    {"ragtest",    "/ragtest.elf",     NULL},
    {"fstest",     "/fstest.elf",      NULL},
    {"fstest2",    "/fstest2.elf",     NULL},
    {"lmstest",    "/lmstest.elf",     NULL},
    {"gguftest",   "/gguftest.elf",    NULL},
    {"gguf2test",  "/gguf2test.elf",   NULL},
    {"agenttest2", "/agenttest2.elf",  NULL},
    {"ostest",     "/ostest.elf",      NULL},
    {"s25",        "/s25test.elf",     NULL},
    {"multiagent", "/multiagent.elf",  NULL},
    {"s26",        "/s26test.elf",     NULL},
    {"s27",        "/s27test.elf",     NULL},
    {"s28",        "/s28test.elf",     "--test"},
    {"s29",        "/s29test.elf",     NULL},
    {"s30",        "/s30test.elf",     NULL},
    {"s31",        "/s31test.elf",     NULL},
    {"s32",        "/s32test.elf",     NULL},
    {"s33",        "/s33test.elf",     NULL},
    {"s34",        "/s34test.elf",     NULL},
    {"s35",        "/s35test.elf",     NULL},
    {"s36",        "/s36test.elf",     NULL},
    {"s37",        "/s37test.elf",     NULL},
    {"s38",        "/s38test.elf",     NULL},
    {"s39",        "/s39test.elf",     NULL},
    {"s41",        "/s41test.elf",     NULL},
    {"s42",        "/s42test.elf",     NULL},
    {"s44",        "/s44test.elf",     NULL},
    {"s45",        "/s45test.elf",     NULL},
    {"s47",        "/s47test.elf",     NULL},
    {"s48",        "/s48test.elf",     NULL},
    {"s49",        "/s49test.elf",     NULL},
    {"s50",        "/s50test.elf",     NULL},
    {"s51",        "/s51test.elf",     NULL},
    {"s52",        "/s52test.elf",     NULL},
    {"s53",        "/s53test.elf",     NULL},
    {"s54",        "/s54test.elf",     NULL},
    {"s55",        "/s55test.elf",     NULL},
    {"s56",        "/s56test.elf",     NULL},
    {"s57",        "/s57test.elf",     NULL},
    {"s58",        "/s58test.elf",     NULL},
    {"s59",        "/s59test.elf",     NULL},
    {"s61",        "/s61test.elf",     NULL},
    {"s63",        "/s63test.elf",     NULL},
    {"s64",        "/s64test.elf",     NULL},
    {"s65",        "/s65test.elf",     NULL},
    {"s66",        "/s66test.elf",     NULL},
    {"s67",        "/s67test.elf",     NULL},
    {"s68",        "/s68test.elf",     NULL},
    {"s69",        "/s69test.elf",     NULL},
    {"s70",        "/s70test.elf",     NULL},
    {"s71",        "/s71test.elf",     NULL},
    {"s72",        "/s72test.elf",     NULL},
    {"s73",        "/s73test.elf",     NULL},
    {"s74",        "/s74test.elf",     NULL},
    {"s75",        "/s75test.elf",     NULL},
    {"s76",        "/s76test.elf",     NULL},
    {"s77",        "/s77test.elf",     NULL},
    {"s78",        "/s78test.elf",     NULL},
    {NULL, NULL, NULL}
};

static int run_one_test(const test_entry_t *t) {
    const char *argv[3];
    /* Use basename for argv[0] */
    const char *base = t->elf + 1;  /* skip leading '/' */
    argv[0] = base;
    argv[1] = t->arg;
    argv[2] = NULL;

    long pid = sys_exec(t->elf, argv);
    if (pid <= 0) {
        printf("  SKIP %s (not found)\n", t->name);
        return -1;
    }
    long st = waitpid_retry(pid);
    return (int)st;
}

static void cmd_test(int argc, char **argv) {
    if (argc < 2) {
        /* List available tests */
        printf("Usage: test <name>    Run a single test\n");
        printf("       test all       Run all tests\n");
        printf("\nAvailable tests:\n");
        int col = 0;
        for (int i = 0; tests[i].name; i++) {
            printf("  %-12s", tests[i].name);
            col++;
            if (col == 5) { printf("\n"); col = 0; }
        }
        if (col) printf("\n");
        return;
    }

    if (strcmp(argv[1], "all") == 0) {
        int passed = 0, failed = 0, skipped = 0;
        for (int i = 0; tests[i].name; i++) {
            printf("[test] Running %s...\n", tests[i].name);
            int st = run_one_test(&tests[i]);
            if (st < 0)
                skipped++;
            else if (st == 0)
                passed++;
            else
                failed++;
        }
        printf("\n=== Test Summary: %d passed, %d failed, %d skipped ===\n",
               passed, failed, skipped);
        return;
    }

    /* Find and run a single test */
    for (int i = 0; tests[i].name; i++) {
        if (strcmp(argv[1], tests[i].name) == 0) {
            run_one_test(&tests[i]);
            return;
        }
    }
    printf("test: unknown test '%s'\n", argv[1]);
}

static int is_builtin(const char *cmd) {
    return strcmp(cmd, "help") == 0 || strcmp(cmd, "echo") == 0 ||
           strcmp(cmd, "ls") == 0 || strcmp(cmd, "cd") == 0 ||
           strcmp(cmd, "pwd") == 0 || strcmp(cmd, "mv") == 0 ||
           strcmp(cmd, "mkdir") == 0 || strcmp(cmd, "cat") == 0 ||
           strcmp(cmd, "pid") == 0 || strcmp(cmd, "exit") == 0 ||
           strcmp(cmd, "export") == 0 || strcmp(cmd, "env") == 0 ||
           strcmp(cmd, "history") == 0 || strcmp(cmd, "rm") == 0 ||
           strcmp(cmd, "touch") == 0 || strcmp(cmd, "stat") == 0 ||
           strcmp(cmd, "kill") == 0 || strcmp(cmd, "jobs") == 0 ||
           strcmp(cmd, "write") == 0 || strcmp(cmd, "ps") == 0 ||
           strcmp(cmd, "df") == 0 || strcmp(cmd, "mount") == 0 ||
           strcmp(cmd, "test") == 0 || strcmp(cmd, "cp") == 0 ||
           strcmp(cmd, "head") == 0 || strcmp(cmd, "tail") == 0 ||
           strcmp(cmd, "wc") == 0 || strcmp(cmd, "wait") == 0 ||
           strcmp(cmd, "fg") == 0 || strcmp(cmd, "service") == 0;
}

static void cmd_help(void) {
    printf("Limnx Shell — built-in commands:\n");
    printf("  help                 Show this help\n");
    printf("  echo [text...]       Print text\n");
    printf("  ls [dir]             List directory\n");
    printf("  cat <file>           Show file contents\n");
    printf("  write <file> <text>  Write text to file\n");
    printf("  cp <src> <dst>       Copy file\n");
    printf("  head [-n N] <file>   Show first N lines\n");
    printf("  tail [-n N] <file>   Show last N lines\n");
    printf("  wc <file>            Count lines/words/bytes\n");
    printf("  touch <file>         Create empty file\n");
    printf("  rm <file>            Remove file\n");
    printf("  mkdir <dir>          Create directory\n");
    printf("  cd <dir>             Change directory\n");
    printf("  pwd                  Print working directory\n");
    printf("  mv <src> <dst>       Rename/move file\n");
    printf("  stat <path>          Show file info\n");
    printf("  env                  Show environment\n");
    printf("  export VAR=value     Set environment variable\n");
    printf("  ps                   List running processes\n");
    printf("  df                   Show disk usage\n");
    printf("  mount                Show mounted filesystems\n");
    printf("  kill <pid> [sig]     Send signal to process\n");
    printf("  pid                  Show shell PID\n");
    printf("  history              Show command history\n");
    printf("  jobs                 Show background jobs\n");
    printf("  wait [pid|%%job]      Wait for background job(s)\n");
    printf("  fg [%%job]            Bring job to foreground\n");
    printf("  service <cmd> [...]  Manage services (start/stop/list/restart)\n");
    printf("  test [name|all]      Run test(s)\n");
    printf("  exit                 Exit shell\n");
    printf("\nFeatures: pipes (|), redirection (> >> <),\n");
    printf("  variables ($VAR $? $$), quoting ('...' \"...\"),\n");
    printf("  semicolons (;), background (&), history (up/down),\n");
    printf("  tab completion, globbing (*.elf, s3?test.*)\n");
    printf("\nExternal: /path/to/program [args...]\n");
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
    if (count == 0)
        printf("  (empty or not found)\n");
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

static void cmd_write(const char *path, int argc, char **argv) {
    long fd = sys_create(path);
    if (fd < 0) {
        printf("write: cannot create '%s'\n", path);
        return;
    }
    /* Write remaining args as content, space-separated */
    for (int i = 0; i < argc; i++) {
        if (i > 0) sys_fwrite(fd, " ", 1);
        int len = 0;
        while (argv[i][len]) len++;
        sys_fwrite(fd, argv[i], len);
    }
    sys_fwrite(fd, "\n", 1);
    sys_close(fd);
}

static void cmd_stat(const char *path) {
    uint8_t st[16];
    if (sys_stat(path, st) != 0) {
        printf("stat: cannot stat '%s'\n", path);
        return;
    }
    uint64_t size = *(uint64_t *)&st[0];
    uint8_t type = st[8];
    uint16_t mode = *(uint16_t *)&st[10];
    printf("  %s  type=%s  size=%lu  mode=%03lo\n",
           path,
           type == 1 ? "dir" : "file",
           size,
           (unsigned long)mode);
}

static void cmd_cp(const char *src, const char *dst) {
    long sfd = sys_open(src, O_RDONLY);
    if (sfd < 0) {
        printf("cp: cannot open '%s'\n", src);
        return;
    }
    long dfd = sys_open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (dfd < 0) {
        dfd = sys_create(dst);
        if (dfd < 0) {
            printf("cp: cannot create '%s'\n", dst);
            sys_close(sfd);
            return;
        }
    }
    char buf[512];
    long n;
    while ((n = sys_read(sfd, buf, sizeof(buf))) > 0) {
        sys_fwrite(dfd, buf, n);
    }
    sys_close(sfd);
    sys_close(dfd);
}

static void cmd_head(int argc, char **argv) {
    int nlines = 10;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            nlines = atoi(argv[++i]);
        } else {
            path = argv[i];
        }
    }
    if (!path) { printf("usage: head [-n N] <file>\n"); return; }

    long fd = sys_open(path, O_RDONLY);
    if (fd < 0) { printf("head: cannot open '%s'\n", path); return; }

    int lines = 0;
    char ch;
    while (lines < nlines && sys_read(fd, &ch, 1) > 0) {
        sys_write(&ch, 1);
        if (ch == '\n') lines++;
    }
    if (lines > 0) {
        /* ensure trailing newline */
    } else {
        printf("\n");
    }
    sys_close(fd);
}

static void cmd_tail(int argc, char **argv) {
    int nlines = 10;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            nlines = atoi(argv[++i]);
        } else {
            path = argv[i];
        }
    }
    if (!path) { printf("usage: tail [-n N] <file>\n"); return; }

    long fd = sys_open(path, O_RDONLY);
    if (fd < 0) { printf("tail: cannot open '%s'\n", path); return; }

    /* Read entire file to find line positions */
    char buf[4096];
    int line_starts[1024];
    int total_lines = 0;
    long total_bytes = 0;
    line_starts[0] = 0;

    long n;
    while ((n = sys_read(fd, buf, sizeof(buf))) > 0) {
        for (long j = 0; j < n; j++) {
            if (buf[j] == '\n') {
                total_lines++;
                if (total_lines < 1024)
                    line_starts[total_lines] = (int)(total_bytes + j + 1);
            }
        }
        total_bytes += n;
    }

    /* Seek to start of the last N lines */
    int start_line = total_lines - nlines;
    if (start_line < 0) start_line = 0;
    int start_offset = (start_line < 1024) ? line_starts[start_line] : 0;

    sys_seek(fd, start_offset, 0);  /* SEEK_SET */
    while ((n = sys_read(fd, buf, sizeof(buf))) > 0) {
        sys_write(buf, n);
    }
    sys_close(fd);
}

static void cmd_wc(const char *path) {
    long fd = sys_open(path, O_RDONLY);
    if (fd < 0) { printf("wc: cannot open '%s'\n", path); return; }

    long lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    char buf[512];
    long n;
    while ((n = sys_read(fd, buf, sizeof(buf))) > 0) {
        for (long j = 0; j < n; j++) {
            bytes++;
            if (buf[j] == '\n') lines++;
            if (buf[j] == ' ' || buf[j] == '\t' || buf[j] == '\n' || buf[j] == '\r') {
                in_word = 0;
            } else {
                if (!in_word) words++;
                in_word = 1;
            }
        }
    }
    printf("  %ld  %ld  %ld %s\n", lines, words, bytes, path);
    sys_close(fd);
}

/* --- Background jobs tracking --- */
#define MAX_JOBS 8
static struct {
    long pid;
    char name[64];
    int active;
} jobs[MAX_JOBS];

static void jobs_add(long pid, const char *name) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].active) {
            jobs[i].pid = pid;
            jobs[i].active = 1;
            int len = 0;
            while (name[len] && len < 63) {
                jobs[i].name[len] = name[len];
                len++;
            }
            jobs[i].name[len] = '\0';
            printf("[%d] %ld\n", i + 1, pid);
            return;
        }
    }
    printf("shell: too many background jobs\n");
}

static void jobs_check(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].active) {
            long st = sys_waitpid_flags(jobs[i].pid, WNOHANG);
            if (st != 0) {
                printf("[%d] Done (%ld)  %s\n", i + 1, st, jobs[i].name);
                jobs[i].active = 0;
            }
        }
    }
}

static void cmd_jobs(void) {
    int any = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].active) {
            /* Check if still running */
            long st = sys_waitpid_flags(jobs[i].pid, WNOHANG);
            if (st != 0) {
                printf("[%d] Done (%ld)  %s\n", i + 1, st, jobs[i].name);
                jobs[i].active = 0;
            } else {
                printf("[%d] Running    %s  (pid %ld)\n", i + 1, jobs[i].name, jobs[i].pid);
            }
            any = 1;
        }
    }
    if (!any) printf("No background jobs.\n");
}

/* Parse a job specifier: %N → job index, or raw PID number. Returns -1 on error. */
static int parse_job_spec(const char *arg, long *out_pid) {
    if (arg[0] == '%') {
        /* Job ID */
        int job_id = 0;
        for (int i = 1; arg[i]; i++) {
            if (arg[i] < '0' || arg[i] > '9') return -1;
            job_id = job_id * 10 + (arg[i] - '0');
        }
        job_id--;  /* 1-based → 0-based */
        if (job_id < 0 || job_id >= MAX_JOBS || !jobs[job_id].active)
            return -1;
        *out_pid = jobs[job_id].pid;
        return job_id;
    }
    /* Raw PID */
    long pid = 0;
    for (int i = 0; arg[i]; i++) {
        if (arg[i] < '0' || arg[i] > '9') return -1;
        pid = pid * 10 + (arg[i] - '0');
    }
    *out_pid = pid;
    /* Find matching job */
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].active && jobs[i].pid == pid) return i;
    }
    return -2;  /* valid PID but not in jobs table */
}

static void cmd_wait(int argc, char **argv) {
    if (argc < 2) {
        /* Wait for all background jobs */
        int any = 0;
        for (int i = 0; i < MAX_JOBS; i++) {
            if (jobs[i].active) {
                any = 1;
                long st = sys_waitpid(jobs[i].pid);
                printf("[%d] Done (%ld)  %s\n", i + 1, st, jobs[i].name);
                jobs[i].active = 0;
            }
        }
        if (!any) printf("wait: no background jobs\n");
    } else {
        long pid;
        int idx = parse_job_spec(argv[1], &pid);
        if (idx == -1) {
            printf("wait: invalid job spec '%s'\n", argv[1]);
            return;
        }
        long st = sys_waitpid(pid);
        if (idx >= 0) {
            printf("[%d] Done (%ld)  %s\n", idx + 1, st, jobs[idx].name);
            jobs[idx].active = 0;
        } else {
            printf("pid %ld exited (%ld)\n", pid, st);
        }
    }
}

static void cmd_fg(int argc, char **argv) {
    int job_idx = -1;

    if (argc < 2) {
        /* Find the last active job */
        for (int i = MAX_JOBS - 1; i >= 0; i--) {
            if (jobs[i].active) { job_idx = i; break; }
        }
        if (job_idx < 0) {
            printf("fg: no background jobs\n");
            return;
        }
    } else {
        long pid;
        job_idx = parse_job_spec(argv[1], &pid);
        if (job_idx < 0) {
            printf("fg: invalid job spec '%s'\n", argv[1]);
            return;
        }
    }

    printf("%s\n", jobs[job_idx].name);
    fg_pid = jobs[job_idx].pid;
    long st = waitpid_retry(jobs[job_idx].pid);
    fg_pid = 0;
    jobs[job_idx].active = 0;
    last_exit_status = (int)st;
}

/* ---- Service IPC protocol (must match serviced.c) ---- */

#define SVC_CMD_START    1
#define SVC_CMD_STOP     2
#define SVC_CMD_LIST     3
#define SVC_CMD_RESTART  4

#define SVC_RESP_OK      0
#define SVC_RESP_ERR     1
#define SVC_RESP_LIST    2

typedef struct svc_request {
    unsigned char cmd;
    unsigned char policy;
    unsigned char reserved[2];
    char reply_agent[32];
    char name[32];
    char elf_path[128];
    unsigned long caps;
    long ns_id;
} svc_request_t;

typedef struct svc_response {
    unsigned char status;
    unsigned char count;
    unsigned char health;    /* 0=DOWN, 1=UP */
    unsigned char reserved;
    int result;
    unsigned int id;
    unsigned char policy;
    unsigned char child_count;
    unsigned char pad[2];
    unsigned int restart_count;
    unsigned long owner_pid;
    char name[32];
    unsigned long child_pids[8];
} svc_response_t;

/* Format PID-based reply agent name: "svc-<pid>" */
static void svc_reply_name(char *buf, long pid) {
    buf[0] = 's'; buf[1] = 'v'; buf[2] = 'c'; buf[3] = '-';
    /* Convert PID to string */
    char digits[16];
    int n = 0;
    long v = pid;
    if (v == 0) { digits[n++] = '0'; }
    else { while (v > 0) { digits[n++] = '0' + (v % 10); v /= 10; } }
    for (int i = 0; i < n; i++) buf[4 + i] = digits[n - 1 - i];
    buf[4 + n] = '\0';
}

/* Wait for a response from serviced with yield-based polling */
static int svc_recv_response(svc_response_t *resp) {
    long sender = 0, tok = 0;
    for (int i = 0; i < 2000; i++) {
        long r = sys_agent_recv(resp, sizeof(*resp), &sender, &tok);
        if (r > 0) return 0;
        sys_yield();
    }
    return -1;  /* timeout */
}

static void cmd_service(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: service <command> [args...]\n");
        printf("  service start <name> <path>    Start a supervised service\n");
        printf("  service stop <name>            Stop a supervised service\n");
        printf("  service list                   List all services\n");
        printf("  service restart <name>         Restart a service\n");
        printf("  service enable <name> <path>   Add to boot config\n");
        printf("  service disable <name>         Remove from boot config\n");
        return;
    }

    /* Register temporary reply agent */
    char reply_name[32];
    svc_reply_name(reply_name, sys_getpid());
    sys_agent_register(reply_name);

    /* Build request */
    svc_request_t req;
    memset(&req, 0, sizeof(req));
    int i = 0;
    while (reply_name[i] && i < 31) { req.reply_agent[i] = reply_name[i]; i++; }
    req.reply_agent[i] = '\0';

    if (strcmp(argv[1], "list") == 0) {
        req.cmd = SVC_CMD_LIST;

        if (sys_agent_send("serviced", &req, sizeof(req), 0) < 0) {
            printf("service: serviced not running\n");
            return;
        }

        svc_response_t resp;
        if (svc_recv_response(&resp) < 0) {
            printf("service: timeout\n");
            return;
        }

        if (resp.count == 0) {
            printf("No active services.\n");
            return;
        }

        printf("  ID  NAME                       STATUS  POLICY         CHILDREN  RESTARTS  OWNER\n");

        /* First response is already received */
        int total = resp.count;
        for (int idx = 0; idx < total; idx++) {
            if (idx > 0) {
                if (svc_recv_response(&resp) < 0) break;
            }
            const char *pol = (resp.policy == SUPER_ONE_FOR_ALL) ? "one-for-all" : "one-for-one";
            const char *health = resp.health ? "UP" : "DOWN";
            printf("  %-3u %-26s %-6s  %-14s %-9u %-9u pid %lu\n",
                   resp.id, resp.name, health, pol, resp.child_count,
                   resp.restart_count, resp.owner_pid);
            for (int j = 0; j < 8; j++) {
                if (resp.child_pids[j] != 0)
                    printf("       child pid %lu\n", resp.child_pids[j]);
            }
        }

    } else if (strcmp(argv[1], "start") == 0) {
        if (argc < 4) {
            printf("usage: service start <name> <path> [policy]\n");
            return;
        }

        req.cmd = SVC_CMD_START;

        /* Copy service name */
        int ni = 0;
        while (argv[2][ni] && ni < 31) { req.name[ni] = argv[2][ni]; ni++; }
        req.name[ni] = '\0';

        /* Resolve ELF path */
        const char *path = argv[3];
        char elf_path[128];
        if (path[0] == '/') {
            int pi = 0;
            while (path[pi] && pi < 126) { elf_path[pi] = path[pi]; pi++; }
            elf_path[pi] = '\0';
        } else {
            elf_path[0] = '/';
            int pi = 0;
            while (path[pi] && pi < 125) { elf_path[pi + 1] = path[pi]; pi++; }
            elf_path[pi + 1] = '\0';
        }

        /* Add .elf extension if missing */
        int has_dot = 0;
        for (int ei = 0; elf_path[ei]; ei++) if (elf_path[ei] == '.') has_dot = 1;
        if (!has_dot) {
            int len = 0;
            while (elf_path[len]) len++;
            if (len < 123) {
                elf_path[len] = '.'; elf_path[len+1] = 'e';
                elf_path[len+2] = 'l'; elf_path[len+3] = 'f';
                elf_path[len+4] = '\0';
            }
        }

        int ei = 0;
        while (elf_path[ei] && ei < 127) { req.elf_path[ei] = elf_path[ei]; ei++; }
        req.elf_path[ei] = '\0';

        req.caps = 0xFFF;
        if (argc >= 5 && strcmp(argv[4], "one-for-all") == 0)
            req.policy = SUPER_ONE_FOR_ALL;

        if (sys_agent_send("serviced", &req, sizeof(req), 0) < 0) {
            printf("service: serviced not running\n");
            return;
        }

        svc_response_t resp;
        if (svc_recv_response(&resp) < 0) {
            printf("service: timeout\n");
            return;
        }

        if (resp.status == SVC_RESP_OK)
            printf("Service '%s' started (supervisor %d)\n", argv[2], resp.result);
        else
            printf("service: failed to start '%s' (error %d)\n", argv[2], resp.result);

    } else if (strcmp(argv[1], "stop") == 0) {
        if (argc < 3) {
            printf("usage: service stop <name>\n");
            return;
        }

        req.cmd = SVC_CMD_STOP;
        int ni = 0;
        while (argv[2][ni] && ni < 31) { req.name[ni] = argv[2][ni]; ni++; }
        req.name[ni] = '\0';

        if (sys_agent_send("serviced", &req, sizeof(req), 0) < 0) {
            printf("service: serviced not running\n");
            return;
        }

        svc_response_t resp;
        if (svc_recv_response(&resp) < 0) {
            printf("service: timeout\n");
            return;
        }

        if (resp.status == SVC_RESP_OK)
            printf("Service '%s' stopped.\n", argv[2]);
        else
            printf("service: '%s' not found or failed (%d)\n", argv[2], resp.result);

    } else if (strcmp(argv[1], "restart") == 0) {
        if (argc < 3) {
            printf("usage: service restart <name>\n");
            return;
        }

        req.cmd = SVC_CMD_RESTART;
        int ni = 0;
        while (argv[2][ni] && ni < 31) { req.name[ni] = argv[2][ni]; ni++; }
        req.name[ni] = '\0';

        if (sys_agent_send("serviced", &req, sizeof(req), 0) < 0) {
            printf("service: serviced not running\n");
            return;
        }

        svc_response_t resp;
        if (svc_recv_response(&resp) < 0) {
            printf("service: timeout\n");
            return;
        }

        if (resp.status == SVC_RESP_OK)
            printf("Service '%s' restarting (%d child(ren) killed, supervisor will restart)\n",
                   argv[2], resp.result);
        else
            printf("service: '%s' not found (%d)\n", argv[2], resp.result);

    } else if (strcmp(argv[1], "enable") == 0) {
        if (argc < 4) {
            printf("usage: service enable <name> <path> [policy] [after]\n");
            return;
        }
        const char *sname = argv[2];
        const char *spath = argv[3];
        const char *spolicy = (argc >= 5) ? argv[4] : "one-for-one";
        const char *safter = (argc >= 6) ? argv[5] : "none";

        /* Resolve path */
        char elf_path[128];
        if (spath[0] == '/') {
            int pi = 0;
            while (spath[pi] && pi < 126) { elf_path[pi] = spath[pi]; pi++; }
            elf_path[pi] = '\0';
        } else {
            elf_path[0] = '/';
            int pi = 0;
            while (spath[pi] && pi < 125) { elf_path[pi + 1] = spath[pi]; pi++; }
            elf_path[pi + 1] = '\0';
        }
        int has_dot = 0;
        for (int di = 0; elf_path[di]; di++) if (elf_path[di] == '.') has_dot = 1;
        if (!has_dot) {
            int len = 0;
            while (elf_path[len]) len++;
            if (len < 123) {
                elf_path[len] = '.'; elf_path[len+1] = 'e';
                elf_path[len+2] = 'l'; elf_path[len+3] = 'f';
                elf_path[len+4] = '\0';
            }
        }

        /* Read existing config to check for duplicates */
        long fd = sys_open("/etc/services", 0);
        if (fd >= 0) {
            char cfgbuf[2048];
            long n = sys_read(fd, cfgbuf, sizeof(cfgbuf) - 1);
            sys_close(fd);
            if (n > 0) {
                cfgbuf[n] = '\0';
                /* Check if service name already exists */
                char needle[64];
                int ni = 0;
                while (sname[ni] && ni < 62) { needle[ni] = sname[ni]; ni++; }
                needle[ni] = '|';
                needle[ni + 1] = '\0';
                if (strstr(cfgbuf, needle) != NULL) {
                    printf("service: '%s' already in config\n", sname);
                    return;
                }
            }
        }

        /* Append to config */
        char line[256];
        int lp = 0;
        for (int ci = 0; sname[ci] && lp < 250; ci++) line[lp++] = sname[ci];
        line[lp++] = '|';
        for (int ci = 0; elf_path[ci] && lp < 250; ci++) line[lp++] = elf_path[ci];
        line[lp++] = '|';
        for (int ci = 0; spolicy[ci] && lp < 250; ci++) line[lp++] = spolicy[ci];
        line[lp++] = '|';
        for (int ci = 0; safter[ci] && lp < 250; ci++) line[lp++] = safter[ci];
        line[lp++] = '\n';
        line[lp] = '\0';

        fd = sys_open("/etc/services", 0);
        if (fd < 0) {
            sys_mkdir("/etc");
            sys_create("/etc/services");
            fd = sys_open("/etc/services", 0);
        }
        if (fd >= 0) {
            /* Find end of file */
            long end = sys_seek(fd, 0, 2);  /* SEEK_END */
            if (end < 0) end = 0;
            sys_fwrite(fd, line, lp);
            sys_close(fd);
            printf("Service '%s' enabled (will start on next boot)\n", sname);
        } else {
            printf("service: failed to write config\n");
        }

    } else if (strcmp(argv[1], "disable") == 0) {
        if (argc < 3) {
            printf("usage: service disable <name>\n");
            return;
        }
        const char *sname = argv[2];

        /* Read config */
        long fd = sys_open("/etc/services", 0);
        if (fd < 0) {
            printf("service: no config file\n");
            return;
        }
        char cfgbuf[2048];
        long n = sys_read(fd, cfgbuf, sizeof(cfgbuf) - 1);
        sys_close(fd);
        if (n <= 0) {
            printf("service: config is empty\n");
            return;
        }
        cfgbuf[n] = '\0';

        /* Filter out matching line */
        char outbuf[2048];
        int outlen = 0;
        int found = 0;
        int pos = 0;
        while (pos < n) {
            int line_start = pos;
            while (pos < n && cfgbuf[pos] != '\n') pos++;
            int line_end = pos;
            if (pos < n) pos++;

            /* Check if this line starts with "name|" */
            int match = 1;
            int si = 0;
            while (sname[si]) {
                if (line_start + si >= line_end || cfgbuf[line_start + si] != sname[si]) {
                    match = 0;
                    break;
                }
                si++;
            }
            if (match && line_start + si < line_end && cfgbuf[line_start + si] == '|') {
                found = 1;
                continue;  /* skip this line */
            }

            /* Copy line to output */
            for (int j = line_start; j < line_end && outlen < 2046; j++)
                outbuf[outlen++] = cfgbuf[j];
            if (outlen < 2047) outbuf[outlen++] = '\n';
        }

        if (!found) {
            printf("service: '%s' not in config\n", sname);
            return;
        }

        /* Rewrite config file */
        sys_truncate("/etc/services", 0);
        fd = sys_open("/etc/services", 0);
        if (fd >= 0) {
            if (outlen > 0)
                sys_fwrite(fd, outbuf, outlen);
            sys_close(fd);
        }
        printf("Service '%s' disabled (removed from boot config)\n", sname);

    } else {
        printf("service: unknown command '%s'\n", argv[1]);
        printf("usage: service <start|stop|list|restart|enable|disable> [args...]\n");
    }
}

static void cmd_ps(void) {
    printf("  PID  PPID  UID  STATE    TYPE    NAME\n");
    proc_info_t info;
    for (long i = 0; sys_procinfo(i, &info) == 0; i++) {
        const char *state = info.state == 1 ? "stopped" : "running";
        const char *type = info.daemon ? "daemon" : "user";
        printf("%5ld %5ld %4u  %-7s  %-6s  %s\n",
               info.pid, info.parent_pid, info.uid, state, type, info.name);
    }
}

static void cmd_df(void) {
    fs_stat_t st;
    if (sys_fsstat(&st) < 0 || !st.mounted) {
        printf("No filesystem mounted.\n");
        return;
    }
    unsigned int used = st.total_blocks - st.free_blocks;
    unsigned int total_kb = (st.total_blocks * (st.block_size / 1024));
    unsigned int used_kb = (used * (st.block_size / 1024));
    unsigned int free_kb = (st.free_blocks * (st.block_size / 1024));
    unsigned int pct = st.total_blocks ? (used * 100 / st.total_blocks) : 0;
    printf("Filesystem      Size    Used    Free    Use%%  Mounted on\n");
    printf("LimnFS      %5uKB %5uKB %5uKB    %3u%%  /\n",
           total_kb, used_kb, free_kb, pct);
    printf("Inodes: %u/%u used\n",
           st.total_inodes - st.free_inodes, st.total_inodes);
}

static void cmd_mount(void) {
    fs_stat_t st;
    if (sys_fsstat(&st) < 0 || !st.mounted) {
        printf("No filesystem mounted.\n");
        return;
    }
    printf("LimnFS on / (rw, %uKB blocks)\n", st.block_size / 1024);
}

/* --- Execute a builtin command, returns 1 if handled, 0 if not builtin.
 *     Sets *should_exit = 1 if the shell should exit. */
static int run_builtin(command_t *cmd, int *should_exit) {
    char *name = cmd->argv[0];
    int argc = cmd->argc;
    char **argv = cmd->argv;

    if (strcmp(name, "help") == 0) {
        cmd_help();
    } else if (strcmp(name, "echo") == 0) {
        for (int i = 1; i < argc; i++) {
            if (i > 1) printf(" ");
            printf("%s", argv[i]);
        }
        printf("\n");
    } else if (strcmp(name, "ls") == 0) {
        cmd_ls(argc > 1 ? argv[1] : ".");
    } else if (strcmp(name, "cd") == 0) {
        if (argc < 2)
            printf("usage: cd <dir>\n");
        else if (sys_chdir(argv[1]) < 0)
            printf("cd: cannot change to '%s'\n", argv[1]);
    } else if (strcmp(name, "pwd") == 0) {
        char cwdbuf[256];
        sys_getcwd(cwdbuf, sizeof(cwdbuf));
        printf("%s\n", cwdbuf);
    } else if (strcmp(name, "mv") == 0) {
        if (argc < 3) printf("usage: mv <src> <dst>\n");
        else if (sys_rename(argv[1], argv[2]) < 0)
            printf("mv: failed\n");
    } else if (strcmp(name, "mkdir") == 0) {
        if (argc < 2) printf("usage: mkdir <dir>\n");
        else if (sys_mkdir(argv[1]) < 0)
            printf("mkdir: failed to create '%s'\n", argv[1]);
    } else if (strcmp(name, "cat") == 0) {
        if (argc < 2) printf("usage: cat <file>\n");
        else cmd_cat(argv[1]);
    } else if (strcmp(name, "write") == 0) {
        if (argc < 3) printf("usage: write <file> <text...>\n");
        else cmd_write(argv[1], argc - 2, &argv[2]);
    } else if (strcmp(name, "touch") == 0) {
        if (argc < 2) printf("usage: touch <file>\n");
        else {
            long fd = sys_create(argv[1]);
            if (fd >= 0) sys_close(fd);
            else printf("touch: failed to create '%s'\n", argv[1]);
        }
    } else if (strcmp(name, "rm") == 0) {
        if (argc < 2) printf("usage: rm <file>\n");
        else if (sys_unlink(argv[1]) < 0)
            printf("rm: cannot remove '%s'\n", argv[1]);
    } else if (strcmp(name, "stat") == 0) {
        if (argc < 2) printf("usage: stat <path>\n");
        else cmd_stat(argv[1]);
    } else if (strcmp(name, "pid") == 0) {
        printf("PID: %ld\n", sys_getpid());
    } else if (strcmp(name, "export") == 0) {
        if (argc < 2) {
            printf("usage: export VAR=value\n");
        } else {
            /* Parse VAR=value */
            char *eq = strchr_local(argv[1], '=');
            if (eq) {
                *eq = '\0';
                sys_setenv(argv[1], eq + 1);
            } else {
                printf("export: expected VAR=value\n");
            }
        }
    } else if (strcmp(name, "env") == 0) {
        char envbuf[1024];
        long len = sys_environ(envbuf, sizeof(envbuf));
        if (len > 0) {
            int pos = 0;
            while (pos < len) {
                printf("%s\n", &envbuf[pos]);
                while (pos < len && envbuf[pos] != '\0') pos++;
                pos++;  /* skip NUL */
            }
        }
    } else if (strcmp(name, "kill") == 0) {
        if (argc < 2) {
            printf("usage: kill <pid> [signal]\n");
        } else {
            long pid = 0;
            for (int i = 0; argv[1][i]; i++)
                pid = pid * 10 + (argv[1][i] - '0');
            long sig = SIGTERM;
            if (argc >= 3) {
                sig = 0;
                for (int i = 0; argv[2][i]; i++)
                    sig = sig * 10 + (argv[2][i] - '0');
            }
            if (sys_kill(pid, sig) < 0)
                printf("kill: failed\n");
        }
    } else if (strcmp(name, "history") == 0) {
        for (int i = 0; i < history_count; i++) {
            const char *h = history_get(i);
            if (h) printf("  %d  %s\n", i + 1, h);
        }
    } else if (strcmp(name, "jobs") == 0) {
        cmd_jobs();
    } else if (strcmp(name, "wait") == 0) {
        cmd_wait(argc, argv);
    } else if (strcmp(name, "fg") == 0) {
        cmd_fg(argc, argv);
    } else if (strcmp(name, "service") == 0) {
        cmd_service(argc, argv);
    } else if (strcmp(name, "ps") == 0) {
        cmd_ps();
    } else if (strcmp(name, "df") == 0) {
        cmd_df();
    } else if (strcmp(name, "mount") == 0) {
        cmd_mount();
    } else if (strcmp(name, "cp") == 0) {
        if (argc < 3) printf("usage: cp <src> <dst>\n");
        else cmd_cp(argv[1], argv[2]);
    } else if (strcmp(name, "head") == 0) {
        cmd_head(argc, argv);
    } else if (strcmp(name, "tail") == 0) {
        cmd_tail(argc, argv);
    } else if (strcmp(name, "wc") == 0) {
        if (argc < 2) printf("usage: wc <file>\n");
        else cmd_wc(argv[1]);
    } else if (strcmp(name, "test") == 0) {
        cmd_test(argc, argv);
    } else if (strcmp(name, "exit") == 0) {
        printf("Goodbye.\n");
        *should_exit = 1;
    } else {
        return 0;
    }
    last_exit_status = 0;
    return 1;
}

/* --- Execute an external command via fork+exec --- */
static long run_external(command_t *cmd) {
    /* Resolve path: if it doesn't start with '/', prepend '/' */
    char path[256];
    if (cmd->argv[0][0] == '/') {
        int i = 0;
        while (cmd->argv[0][i] && i < 255) { path[i] = cmd->argv[0][i]; i++; }
        path[i] = '\0';
    } else {
        path[0] = '/';
        int i = 0;
        while (cmd->argv[0][i] && i < 254) { path[i + 1] = cmd->argv[0][i]; i++; }
        path[i + 1] = '\0';
    }

    /* Try with .elf extension if no extension */
    char path_elf[256];
    int has_dot = 0;
    for (int i = 0; path[i]; i++) if (path[i] == '.') has_dot = 1;
    if (!has_dot) {
        int i = 0;
        while (path[i]) { path_elf[i] = path[i]; i++; }
        path_elf[i] = '.'; path_elf[i+1] = 'e'; path_elf[i+2] = 'l';
        path_elf[i+3] = 'f'; path_elf[i+4] = '\0';
    } else {
        int i = 0;
        while (path[i]) { path_elf[i] = path[i]; i++; }
        path_elf[i] = '\0';
    }

    /* Use sys_exec which does fork internally in Limnx */
    long pid = sys_exec(path_elf, (const char **)cmd->argv);
    if (pid <= 0) {
        /* Try without .elf extension */
        pid = sys_exec(path, (const char **)cmd->argv);
    }
    return pid;
}

/* --- Execute a pipeline --- */
static void execute_pipeline(command_t *cmds, int ncmds) {
    /* Single command — check for builtin first */
    if (ncmds == 1 && !cmds[0].redir_in && !cmds[0].redir_out) {
        int should_exit = 0;
        if (is_builtin(cmds[0].argv[0])) {
            run_builtin(&cmds[0], &should_exit);
            if (should_exit) sys_exit(0);
            return;
        }

        /* External command */
        long pid = run_external(&cmds[0]);
        if (pid <= 0) {
            printf("%s: command not found\n", cmds[0].argv[0]);
            last_exit_status = 127;
            return;
        }
        if (cmds[0].background) {
            jobs_add(pid, cmds[0].argv[0]);
            last_exit_status = 0;
        } else {
            long st = waitpid_retry(pid);
            last_exit_status = (int)st;
        }
        return;
    }

    /* Pipeline or command with redirections: use fork for each stage */
    long pids[MAX_CMDS];
    long pipe_fds[MAX_CMDS - 1][2]; /* pipe_fds[i] = pipe between cmd i and i+1 */

    /* Create all pipes */
    for (int i = 0; i < ncmds - 1; i++) {
        if (sys_pipe(&pipe_fds[i][0], &pipe_fds[i][1]) < 0) {
            printf("shell: pipe creation failed\n");
            return;
        }
    }

    for (int i = 0; i < ncmds; i++) {
        /* For builtins in a pipeline, we still fork so redirections work */
        long pid = sys_fork();
        if (pid < 0) {
            printf("shell: fork failed\n");
            return;
        }

        if (pid == 0) {
            /* Child process */

            /* Set up pipe redirections */
            if (i > 0) {
                /* Read from previous pipe */
                sys_dup2(pipe_fds[i - 1][0], 0);
            }
            if (i < ncmds - 1) {
                /* Write to next pipe */
                sys_dup2(pipe_fds[i][1], 1);
            }

            /* Close all pipe fds in child */
            for (int j = 0; j < ncmds - 1; j++) {
                sys_close(pipe_fds[j][0]);
                sys_close(pipe_fds[j][1]);
            }

            /* File redirections */
            if (cmds[i].redir_in) {
                long fd = sys_open(cmds[i].redir_in, O_RDONLY);
                if (fd < 0) {
                    printf("shell: cannot open '%s'\n", cmds[i].redir_in);
                    sys_exit(1);
                }
                sys_dup2(fd, 0);
                sys_close(fd);
            }
            if (cmds[i].redir_out) {
                long flags = O_WRONLY | O_CREAT;
                if (cmds[i].redir_append)
                    flags |= O_APPEND;
                else
                    flags |= O_TRUNC;
                long fd = sys_open(cmds[i].redir_out, flags);
                if (fd < 0) {
                    /* Try create */
                    fd = sys_create(cmds[i].redir_out);
                }
                if (fd < 0) {
                    printf("shell: cannot open '%s' for writing\n", cmds[i].redir_out);
                    sys_exit(1);
                }
                sys_dup2(fd, 1);
                sys_close(fd);
            }

            /* Execute */
            if (is_builtin(cmds[i].argv[0])) {
                int should_exit = 0;
                run_builtin(&cmds[i], &should_exit);
                sys_exit(0);
            }

            /* External — use true exec to replace fork child */
            {
                char epath[256];
                if (cmds[i].argv[0][0] == '/') {
                    int ei = 0;
                    while (cmds[i].argv[0][ei] && ei < 255) { epath[ei] = cmds[i].argv[0][ei]; ei++; }
                    epath[ei] = '\0';
                } else {
                    epath[0] = '/';
                    int ei = 0;
                    while (cmds[i].argv[0][ei] && ei < 254) { epath[ei + 1] = cmds[i].argv[0][ei]; ei++; }
                    epath[ei + 1] = '\0';
                }
                /* Try with .elf extension */
                char epath_elf[256];
                int has_dot = 0;
                for (int j = 0; epath[j]; j++) if (epath[j] == '.') has_dot = 1;
                if (!has_dot) {
                    int j = 0;
                    while (epath[j]) { epath_elf[j] = epath[j]; j++; }
                    epath_elf[j] = '.'; epath_elf[j+1] = 'e'; epath_elf[j+2] = 'l';
                    epath_elf[j+3] = 'f'; epath_elf[j+4] = '\0';
                } else {
                    int j = 0;
                    while (epath[j]) { epath_elf[j] = epath[j]; j++; }
                    epath_elf[j] = '\0';
                }
                /* sys_execve replaces this process — only returns on error */
                sys_execve(epath_elf, (const char **)cmds[i].argv);
                sys_execve(epath, (const char **)cmds[i].argv);
                printf("%s: command not found\n", cmds[i].argv[0]);
                sys_exit(127);
            }
        }

        pids[i] = pid;
    }

    /* Parent: close all pipe fds */
    for (int i = 0; i < ncmds - 1; i++) {
        sys_close(pipe_fds[i][0]);
        sys_close(pipe_fds[i][1]);
    }

    /* Wait for all children (unless background) */
    int bg = cmds[ncmds - 1].background;
    if (bg) {
        jobs_add(pids[ncmds - 1], cmds[0].argv[0]);
        last_exit_status = 0;
    } else {
        for (int i = 0; i < ncmds; i++) {
            long st = waitpid_retry(pids[i]);
            if (i == ncmds - 1)
                last_exit_status = (int)st;
        }
    }
}

/* --- Main --- */

/* SIGINT handler: forward to foreground job if active, otherwise ignore */
static void shell_sigint_handler(int sig) {
    (void)sig;
    if (fg_pid > 0)
        sys_kill(fg_pid, SIGINT);
    sys_sigreturn();
}

int main(int argc_init, char **argv_init) {
    /* Detect PTY */
    termios_t t;
    if (sys_ioctl(0, TCGETS, (long)&t) == 0)
        stdin_is_pty = 1;

    /* Install SIGINT handler (forward to fg job instead of dying) */
    sys_sigaction(SIGINT, shell_sigint_handler);

    /* Init jobs */
    for (int i = 0; i < MAX_JOBS; i++)
        jobs[i].active = 0;

    /* Auto-test mode: shell --test-all */
    if (argc_init >= 2 && strcmp(argv_init[1], "--test-all") == 0) {
        char *test_argv[] = {"test", "all", NULL};
        cmd_test(2, test_argv);
        return last_exit_status;
    }

    printf("\n");
    printf("========================================\n");
    printf("  Limnx Shell v3.0\n");
    printf("========================================\n");
    printf("Type 'help' for commands.\n\n");

    char line[MAX_LINE];

    for (;;) {
        /* Check background jobs */
        jobs_check();

        /* Print prompt */
        char cwdbuf[128];
        sys_getcwd(cwdbuf, sizeof(cwdbuf));
        printf("%s> ", cwdbuf);

        int len = getline_input(line, MAX_LINE);
        if (len < 0) {
            /* EOF (^D) */
            printf("\nGoodbye.\n");
            break;
        }
        if (len == 0) continue;

        history_add(line);

        /* Split on semicolons (respecting quotes) */
        char *stmts[16];
        int nstmts = 0;
        char *sp = line;
        stmts[nstmts++] = sp;
        int sq = 0, dq = 0;
        while (*sp) {
            if (*sp == '\'' && !dq) sq = !sq;
            else if (*sp == '"' && !sq) dq = !dq;
            else if (*sp == ';' && !sq && !dq) {
                *sp = '\0';
                sp++;
                if (nstmts < 16) stmts[nstmts++] = sp;
                continue;
            }
            sp++;
        }

        /* Execute each semicolon-separated statement */
        for (int si = 0; si < nstmts; si++) {
            command_t cmds[MAX_CMDS];
            int ncmds = parse_pipeline(stmts[si], cmds, MAX_CMDS);
            if (ncmds == 0) continue;

            execute_pipeline(cmds, ncmds);
        }
    }

    return 0;
}
