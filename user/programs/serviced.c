/* serviced — system-level service daemon
 *
 * Launched at boot before the shell. Owns all supervisors so services
 * persist across shell sessions. Shell's "service" command communicates
 * with serviced via agent IPC (sys_agent_send / sys_agent_recv).
 *
 * Features:
 * - Boot-time auto-start from /etc/services config
 * - Dependency ordering (multi-pass with circular detection)
 * - Periodic health monitoring
 * - IPC command interface (start/stop/list/restart)
 */

#include "libc/libc.h"

/* ---- IPC Protocol ---- */

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
    unsigned char health;   /* 0=DOWN, 1=UP */
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

/* ---- Service registry ---- */

#define MAX_SERVICES     16
#define SVC_NAME_MAX     32
#define SVC_PATH_MAX     128

#define SVC_STATE_DOWN    0
#define SVC_STATE_UP      1

typedef struct svc_entry {
    char     name[SVC_NAME_MAX];
    char     elf_path[SVC_PATH_MAX];
    char     after[SVC_NAME_MAX];   /* dependency name, empty = none */
    unsigned char policy;
    unsigned char state;            /* SVC_STATE_DOWN or SVC_STATE_UP */
    unsigned char used;
    long     supervisor_id;         /* -1 if not started */
} svc_entry_t;

static svc_entry_t svc_table[MAX_SERVICES];
static int svc_count = 0;

/* ---- Helpers ---- */

static int svc_find(const char *name) {
    for (int i = 0; i < svc_count; i++) {
        if (svc_table[i].used && strcmp(svc_table[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int is_pid_alive(long pid) {
    if (pid == 0) return 0;
    proc_info_t info;
    for (long i = 0; sys_procinfo(i, &info) == 0; i++) {
        if (info.pid == pid) return 1;
    }
    return 0;
}

static void svc_register(const char *name, const char *elf_path,
                          unsigned char policy, const char *after) {
    if (svc_count >= MAX_SERVICES) return;

    svc_entry_t *e = &svc_table[svc_count];
    memset(e, 0, sizeof(*e));
    e->used = 1;
    e->state = SVC_STATE_DOWN;
    e->supervisor_id = -1;

    int i = 0;
    while (name[i] && i < SVC_NAME_MAX - 1) { e->name[i] = name[i]; i++; }
    e->name[i] = '\0';

    i = 0;
    while (elf_path[i] && i < SVC_PATH_MAX - 1) { e->elf_path[i] = elf_path[i]; i++; }
    e->elf_path[i] = '\0';

    e->policy = policy;

    if (after && strcmp(after, "none") != 0) {
        i = 0;
        while (after[i] && i < SVC_NAME_MAX - 1) { e->after[i] = after[i]; i++; }
        e->after[i] = '\0';
    }

    svc_count++;
}

/* Start a service by creating a supervisor. Returns supervisor ID or -1. */
static long svc_start_one(svc_entry_t *e) {
    long sid = sys_super_create(e->name);
    if (sid < 0) return -1;

    if (e->policy == SUPER_ONE_FOR_ALL)
        sys_super_set_policy(sid, SUPER_ONE_FOR_ALL);

    long child = sys_super_add(sid, e->elf_path, 0, 0xFFF);
    if (child < 0) return -1;

    long launched = sys_super_start(sid);
    if (launched <= 0) return -1;

    e->supervisor_id = sid;
    e->state = SVC_STATE_UP;
    return sid;
}

/* ---- Config file parser ---- */

static void parse_config(void) {
    long fd = sys_open("/etc/services", 0);
    if (fd < 0) return;

    char buf[2048];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    /* Parse line by line */
    int pos = 0;
    while (pos < n) {
        /* Skip leading whitespace */
        while (pos < n && (buf[pos] == ' ' || buf[pos] == '\t')) pos++;

        /* Find end of line */
        int line_start = pos;
        while (pos < n && buf[pos] != '\n') pos++;
        int line_end = pos;
        if (pos < n) pos++;  /* skip newline */

        /* Skip empty lines and comments */
        if (line_end == line_start) continue;
        if (buf[line_start] == '#') continue;

        /* Parse: name|elf_path|policy|after */
        char fields[4][128];
        int field = 0;
        int fi = 0;
        memset(fields, 0, sizeof(fields));

        for (int j = line_start; j < line_end && field < 4; j++) {
            if (buf[j] == '|') {
                fields[field][fi] = '\0';
                field++;
                fi = 0;
            } else if (fi < 127) {
                fields[field][fi++] = buf[j];
            }
        }
        fields[field][fi] = '\0';

        if (field < 2) continue;  /* need at least name and path */

        /* Determine policy */
        unsigned char policy = SUPER_ONE_FOR_ONE;
        if (field >= 2 && strcmp(fields[2], "one-for-all") == 0)
            policy = SUPER_ONE_FOR_ALL;

        /* Dependency */
        const char *after = (field >= 3) ? fields[3] : "none";

        svc_register(fields[0], fields[1], policy, after);
    }
}

/* ---- Dependency-ordered boot start ---- */

static void boot_start_services(void) {
    if (svc_count == 0) return;

    int started = 0;
    int last_started = -1;

    /* Multi-pass: keep going until all started or no progress */
    while (started < svc_count) {
        if (started == last_started) {
            /* No progress — remaining services have unresolvable deps */
            for (int i = 0; i < svc_count; i++) {
                if (svc_table[i].used && svc_table[i].state == SVC_STATE_DOWN)
                    printf("[serviced] ERROR: circular/missing dep for '%s' (after '%s')\n",
                           svc_table[i].name, svc_table[i].after);
            }
            break;
        }
        last_started = started;

        for (int i = 0; i < svc_count; i++) {
            svc_entry_t *e = &svc_table[i];
            if (!e->used || e->state != SVC_STATE_DOWN) continue;

            /* Check dependency */
            if (e->after[0] != '\0') {
                int dep = svc_find(e->after);
                if (dep < 0) {
                    printf("[serviced] WARNING: '%s' depends on unknown '%s', skipping\n",
                           e->name, e->after);
                    e->used = 0;  /* remove invalid entry */
                    started++;
                    continue;
                }
                if (svc_table[dep].state != SVC_STATE_UP) {
                    continue;  /* dep not ready yet, try next pass */
                }
            }

            /* Deps satisfied — start it */
            long sid = svc_start_one(e);
            if (sid >= 0) {
                printf("[serviced] Started '%s' (supervisor %ld)\n", e->name, sid);
            } else {
                printf("[serviced] Failed to start '%s'\n", e->name);
                e->used = 0;  /* don't block dependents */
            }
            started++;
        }
    }
}

/* ---- Health monitoring ---- */

static void health_check(void) {
    super_info_t infos[8];
    long count = sys_super_list(infos, 8);

    for (int i = 0; i < svc_count; i++) {
        svc_entry_t *e = &svc_table[i];
        if (!e->used || e->supervisor_id < 0) continue;

        /* Find matching supervisor */
        int found = 0;
        for (long j = 0; j < count; j++) {
            if (strcmp(infos[j].name, e->name) == 0) {
                found = 1;
                /* Check if any child is alive */
                int alive = 0;
                for (int k = 0; k < 8; k++) {
                    if (infos[j].child_pids[k] != 0 &&
                        is_pid_alive(infos[j].child_pids[k])) {
                        alive = 1;
                        break;
                    }
                }
                e->state = alive ? SVC_STATE_UP : SVC_STATE_DOWN;
                break;
            }
        }
        if (!found) {
            e->state = SVC_STATE_DOWN;
            e->supervisor_id = -1;
        }
    }
}

/* ---- IPC Handlers ---- */

static void send_ok(const char *reply, int result) {
    svc_response_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.status = SVC_RESP_OK;
    resp.result = result;
    sys_agent_send(reply, &resp, sizeof(resp), 0);
}

static void send_err(const char *reply, int code) {
    svc_response_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.status = SVC_RESP_ERR;
    resp.result = code;
    sys_agent_send(reply, &resp, sizeof(resp), 0);
}

static void handle_start(svc_request_t *req) {
    /* Register in svc_table too */
    unsigned char policy = req->policy;
    svc_register(req->name, req->elf_path, policy, "none");

    svc_entry_t *e = &svc_table[svc_count - 1];
    long sid = svc_start_one(e);
    if (sid >= 0)
        send_ok(req->reply_agent, (int)sid);
    else
        send_err(req->reply_agent, -1);
}

static void handle_stop(svc_request_t *req) {
    int idx = svc_find(req->name);
    if (idx < 0) {
        send_err(req->reply_agent, -1);
        return;
    }

    /* Find supervisor in kernel and stop it */
    super_info_t infos[8];
    long count = sys_super_list(infos, 8);
    for (long i = 0; i < count; i++) {
        if (strcmp(infos[i].name, req->name) == 0) {
            long ret = sys_super_stop(infos[i].id);
            svc_table[idx].state = SVC_STATE_DOWN;
            svc_table[idx].supervisor_id = -1;
            svc_table[idx].used = 0;
            if (ret == 0)
                send_ok(req->reply_agent, 0);
            else
                send_err(req->reply_agent, (int)ret);
            return;
        }
    }
    /* Not found in kernel supervisors — clean up local entry */
    svc_table[idx].used = 0;
    send_ok(req->reply_agent, 0);
}

static void handle_list(svc_request_t *req) {
    /* Run a health check first to get fresh status */
    health_check();

    super_info_t infos[8];
    long count = sys_super_list(infos, 8);

    if (count <= 0) {
        svc_response_t resp;
        memset(&resp, 0, sizeof(resp));
        resp.status = SVC_RESP_LIST;
        resp.count = 0;
        sys_agent_send(req->reply_agent, &resp, sizeof(resp), 0);
        return;
    }

    for (long i = 0; i < count; i++) {
        svc_response_t resp;
        memset(&resp, 0, sizeof(resp));
        resp.status = SVC_RESP_LIST;
        resp.count = (unsigned char)count;
        resp.id = infos[i].id;
        resp.policy = infos[i].policy;
        resp.child_count = infos[i].child_count;
        resp.restart_count = infos[i].restart_count;
        resp.owner_pid = infos[i].owner_pid;

        /* Find health status from svc_table */
        int idx = svc_find(infos[i].name);
        resp.health = (idx >= 0 && svc_table[idx].state == SVC_STATE_UP) ? 1 : 0;

        for (int j = 0; j < 32 && infos[i].name[j]; j++)
            resp.name[j] = infos[i].name[j];
        for (int j = 0; j < 8; j++)
            resp.child_pids[j] = infos[i].child_pids[j];
        sys_agent_send(req->reply_agent, &resp, sizeof(resp), 0);
        sys_yield();
    }
}

static void handle_restart(svc_request_t *req) {
    super_info_t infos[8];
    long count = sys_super_list(infos, 8);

    for (long i = 0; i < count; i++) {
        if (strcmp(infos[i].name, req->name) == 0) {
            int killed = 0;
            for (int j = 0; j < 8; j++) {
                if (infos[i].child_pids[j] != 0) {
                    sys_kill(infos[i].child_pids[j], SIGKILL);
                    killed++;
                }
            }
            send_ok(req->reply_agent, killed);
            return;
        }
    }
    send_err(req->reply_agent, -1);
}

/* ---- Main ---- */

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* Register as the system service daemon */
    long r = sys_agent_register("serviced");
    if (r < 0) {
        printf("[serviced] Failed to register agent\n");
        return 1;
    }

    printf("[serviced] Service daemon started (pid %ld)\n", sys_getpid());

    /* Parse /etc/services and auto-start with dependency ordering */
    parse_config();
    if (svc_count > 0) {
        printf("[serviced] Config: %d service(s) defined\n", svc_count);
        boot_start_services();
    }

    /* Main loop: handle IPC + periodic health checks */
    int loop_count = 0;

    for (;;) {
        char msg_buf[256];
        long sender_pid = 0;
        long token_id = 0;

        long n = sys_agent_recv(msg_buf, sizeof(msg_buf),
                                &sender_pid, &token_id);
        if (n > 0) {
            svc_request_t *req = (svc_request_t *)msg_buf;
            switch (req->cmd) {
            case SVC_CMD_START:   handle_start(req);   break;
            case SVC_CMD_STOP:    handle_stop(req);    break;
            case SVC_CMD_LIST:    handle_list(req);    break;
            case SVC_CMD_RESTART: handle_restart(req); break;
            default: send_err(req->reply_agent, -99);  break;
            }
            loop_count = 0;
        } else {
            sys_yield();
            loop_count++;

            /* Periodic health check every ~500 yields */
            if (loop_count >= 500) {
                health_check();
                loop_count = 0;
            }
        }
    }

    return 0;
}
