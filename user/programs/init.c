/*
 * init.c — Init system (pid 1)
 *
 * First userspace process. Reads /etc/inittab, spawns services,
 * reaps zombies, respawns crashed services.
 *
 * /etc/inittab format (one entry per line):
 *   name:path:flags
 *   flags: respawn, once, wait
 *   Lines starting with '#' are comments, empty lines ignored.
 *
 * Example:
 *   serviced:/serviced.elf:respawn
 *   shell:/shell.elf:wait
 */

#include "../libc/libc.h"

#define MAX_SERVICES 16
#define MAX_LINE     256

/* Service flags */
#define SVC_RESPAWN  (1 << 0)   /* auto-restart on exit */
#define SVC_WAIT     (1 << 1)   /* wait for exit before continuing */
#define SVC_ONCE     (1 << 2)   /* run once, don't respawn */

typedef struct {
    char name[32];
    char path[128];
    uint32_t flags;
    long pid;           /* current pid, 0 if not running */
    int started;        /* has been started at least once */
} service_t;

static service_t services[MAX_SERVICES];
static int service_count = 0;

/* Parse a single inittab line: name:path:flags */
static int parse_line(const char *line) {
    if (service_count >= MAX_SERVICES) return -1;
    if (line[0] == '#' || line[0] == '\0' || line[0] == '\n') return 0;

    service_t *svc = &services[service_count];
    memset(svc, 0, sizeof(*svc));

    /* Parse name */
    int i = 0, j = 0;
    while (line[i] && line[i] != ':' && j < 31)
        svc->name[j++] = line[i++];
    svc->name[j] = '\0';
    if (line[i] != ':') return -1;
    i++;

    /* Parse path */
    j = 0;
    while (line[i] && line[i] != ':' && j < 127)
        svc->path[j++] = line[i++];
    svc->path[j] = '\0';
    if (line[i] != ':') return -1;
    i++;

    /* Parse flags */
    if (strstr(&line[i], "respawn")) svc->flags |= SVC_RESPAWN;
    if (strstr(&line[i], "wait"))    svc->flags |= SVC_WAIT;
    if (strstr(&line[i], "once"))    svc->flags |= SVC_ONCE;

    /* Default to once if no flags */
    if (svc->flags == 0) svc->flags = SVC_ONCE;

    service_count++;
    return 0;
}

/* Read and parse /etc/inittab */
static int read_inittab(void) {
    long fd = sys_open("/etc/inittab", 0);
    if (fd < 0) {
        printf("[init] /etc/inittab not found, using defaults\n");
        return -1;
    }

    char buf[2048];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';

    /* Parse line by line */
    char line[MAX_LINE];
    int li = 0, bi = 0;
    while (bi <= n) {
        if (buf[bi] == '\n' || buf[bi] == '\0') {
            line[li] = '\0';
            if (li > 0) parse_line(line);
            li = 0;
            bi++;
        } else {
            if (li < MAX_LINE - 1)
                line[li++] = buf[bi];
            bi++;
        }
    }
    return 0;
}

/* Spawn a service via fork + execve */
static long spawn_service(service_t *svc) {
    long pid = sys_fork();
    if (pid < 0) {
        printf("[init] fork failed for %s\n", svc->name);
        return -1;
    }
    if (pid == 0) {
        /* Child: exec the service */
        const char *argv[2];
        argv[0] = svc->path;
        argv[1] = (void *)0;
        sys_execve(svc->path, argv);
        /* execve failed */
        printf("[init] exec failed: %s\n", svc->path);
        sys_exit(127);
    }
    /* Parent */
    svc->pid = pid;
    svc->started = 1;
    printf("[init] started %s (pid %ld)\n", svc->name, pid);
    return pid;
}

/* Find service by pid */
static service_t *find_service_by_pid(long pid) {
    for (int i = 0; i < service_count; i++) {
        if (services[i].pid == pid)
            return &services[i];
    }
    return (void *)0;
}

int main(void) {
    printf("[init] Limnx init (pid %ld)\n", sys_getpid());

    /* Set up environment */
    sys_setenv("LIMNX_VERSION", "0.110");
    sys_setenv("PATH", "/");

    /* Read config */
    if (read_inittab() < 0) {
        /* No inittab — use hardcoded defaults */
        service_t *svc;

        svc = &services[service_count++];
        memset(svc, 0, sizeof(*svc));
        strcpy(svc->name, "serviced");
        strcpy(svc->path, "/serviced.elf");
        svc->flags = SVC_RESPAWN;

        svc = &services[service_count++];
        memset(svc, 0, sizeof(*svc));
        strcpy(svc->name, "shell");
        strcpy(svc->path, "/shell.elf");
        svc->flags = SVC_WAIT;
    }

    printf("[init] %d services configured\n", service_count);

    /* Start all services */
    for (int i = 0; i < service_count; i++) {
        spawn_service(&services[i]);

        /* If wait flag, block until this service exits before next */
        if (services[i].flags & SVC_WAIT) {
            sys_waitpid(services[i].pid);
            printf("[init] %s exited\n", services[i].name);
            services[i].pid = 0;
        }
    }

    /* Main loop: reap children, respawn if needed */
    printf("[init] entering main loop\n");
    for (;;) {
        /* Poll all running services with WNOHANG */
        int any_running = 0;
        for (int i = 0; i < service_count; i++) {
            if (services[i].pid <= 0) continue;
            any_running = 1;

            /* WNOHANG returns 0 for both "not exited" and "exited with status 0".
             * Use /proc/<pid> to disambiguate: if gone, child was reaped. */
            long status = sys_waitpid_flags(services[i].pid, WNOHANG);
            if (status == 0) {
                /* Check if process still exists via /proc */
                char ppath[32];
                int pp = 0;
                const char *pfx = "/proc/";
                while (*pfx) ppath[pp++] = *pfx++;
                long ptmp = services[i].pid;
                char rev[24]; int rp = 0;
                if (ptmp == 0) rev[rp++] = '0';
                else while (ptmp > 0) { rev[rp++] = '0' + (ptmp % 10); ptmp /= 10; }
                while (rp > 0) ppath[pp++] = rev[--rp];
                ppath[pp] = '\0';
                long pfd = sys_open(ppath, 0);
                if (pfd >= 0) {
                    sys_close(pfd);
                    continue;  /* Still running */
                }
                /* Process gone — was reaped with status 0 */
            }
            if (status == -1) {
                /* Process not found — already reaped elsewhere */
                services[i].pid = 0;
                continue;
            }

            /* Child exited */
            printf("[init] %s (pid %ld) exited with status %ld\n",
                   services[i].name, services[i].pid, status);
            services[i].pid = 0;

            /* Respawn if configured */
            if (services[i].flags & SVC_RESPAWN) {
                printf("[init] respawning %s\n", services[i].name);
                spawn_service(&services[i]);
            }
        }

        if (!any_running) break;  /* All services done */

        /* Brief sleep to avoid busy-waiting */
        long ts[2] = {0, 100000000};  /* 100ms */
        sys_nanosleep(ts);
    }

    return 0;
}
