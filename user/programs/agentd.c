/*
 * agentd.c — Persistent Agent Daemon for Limnx
 *
 * A long-running daemon that accepts client connections, maintains
 * conversation sessions with context + memory, dispatches tools,
 * and routes inference through inferd via sys_infer_request.
 *
 * Runs as a service in /etc/inittab. Listens on /tmp/agentd.sock.
 * Registers as "agentd" in the agent registry for system IPC.
 */

#include "libc/libc.h"

/* --- Configuration --- */

#define AGENTD_SOCK_PATH   "/tmp/agentd.sock"
#define AGENTD_TOOLS_CONF  "/etc/agentd/tools.conf"
#define AGENTD_MEMORY_DIR  "/var/agentd"
#define AGENTD_GLOBAL_MEM  "/var/agentd/global.vec"

#define MAX_SESSIONS       8
#define MAX_TOOLS          16
#define MAX_TOOL_CHAIN     4
#define CTX_BUF_SIZE       2048
#define MAX_REQ            512
#define MAX_RESP           512
#define EMBED_DIM          48

/* --- Message protocol (unix socket framing) --- */

#define MSG_NEW_SESSION    0x01
#define MSG_PROMPT         0x02
#define MSG_CLOSE_SESSION  0x03
#define MSG_LIST_TOOLS     0x04
#define MSG_RESPONSE       0x80
#define MSG_TOOL_OUTPUT    0x81
#define MSG_ERROR          0xFF

typedef struct {
    uint8_t  type;
    uint16_t len;
} __attribute__((packed)) msg_hdr_t;

/* --- Tool registry --- */

#define TOOL_MAX_NAME      32
#define TOOL_MAX_PATH      64
#define TOOL_MAX_DESC      64
#define TOOL_MAX_KEYWORDS  8
#define TOOL_KW_LEN        16

typedef struct {
    int  active;
    char name[TOOL_MAX_NAME];
    char path[TOOL_MAX_PATH];
    char desc[TOOL_MAX_DESC];
    char keywords[TOOL_MAX_KEYWORDS][TOOL_KW_LEN];
    int  n_keywords;
} tool_entry_t;

static tool_entry_t tools[MAX_TOOLS];
static int          n_tools;

/* --- Session state --- */

typedef struct {
    int      active;
    int      client_fd;
    uint32_t session_id;
    char     context[CTX_BUF_SIZE];
    int      ctx_len;
    vecstore_t memory;
    int      memory_initialized;
} session_t;

static session_t sessions[MAX_SESSIONS];
static uint32_t  next_session_id = 1;

/* --- Embedding model (small, for memory/RAG) --- */

static transformer_t embed_tf;
static tf_config_t   embed_cfg = {
    .dim        = EMBED_DIM,
    .hidden_dim = 128,
    .n_heads    = 4,
    .n_layers   = 2,
    .vocab_size = 96,
    .max_seq_len = 64,
};
static tok_config_t  embed_tok;
static int embed_model_ok;

/* --- Global memory --- */

static vecstore_t global_memory;
static int global_memory_ok;

/* --- Helpers --- */

static int str_starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

static void str_trim(char *s) {
    int len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' || s[len-1] == ' '))
        s[--len] = '\0';
}

/* Copy up to max-1 chars, null terminate */
static void str_ncopy(char *dst, const char *src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* --- Tool registry --- */

static void tools_init_defaults(void) {
    /* Built-in tools that are always available */
    struct { const char *name; const char *path; const char *desc; const char *kw; } defaults[] = {
        {"ls",    "/bin/ls",   "list directory contents",  "ls,list,dir,files"},
        {"cat",   "/bin/cat",  "read file contents",       "cat,read,show,view,file"},
        {"echo",  "/bin/echo", "print text",               "echo,print,say"},
        {"ps",    "/bin/ps",   "list processes",           "ps,process,procs"},
        {"wc",    "/bin/wc",   "count lines/words/bytes",  "wc,count,lines,words"},
        {"grep",  "/bin/grep", "search text patterns",     "grep,search,find,pattern"},
        {"head",  "/bin/head", "show first lines of file", "head,top,first,beginning"},
    };
    int ndefaults = sizeof(defaults) / sizeof(defaults[0]);

    for (int i = 0; i < ndefaults && n_tools < MAX_TOOLS; i++) {
        tool_entry_t *t = &tools[n_tools];
        t->active = 1;
        str_ncopy(t->name, defaults[i].name, TOOL_MAX_NAME);
        str_ncopy(t->path, defaults[i].path, TOOL_MAX_PATH);
        str_ncopy(t->desc, defaults[i].desc, TOOL_MAX_DESC);

        /* Parse comma-separated keywords */
        const char *kw = defaults[i].kw;
        t->n_keywords = 0;
        int ki = 0;
        while (*kw && t->n_keywords < TOOL_MAX_KEYWORDS) {
            if (*kw == ',') {
                t->keywords[t->n_keywords][ki] = '\0';
                t->n_keywords++;
                ki = 0;
            } else if (ki < TOOL_KW_LEN - 1) {
                t->keywords[t->n_keywords][ki++] = *kw;
            }
            kw++;
        }
        if (ki > 0) {
            t->keywords[t->n_keywords][ki] = '\0';
            t->n_keywords++;
        }
        n_tools++;
    }
}

static void tools_load_config(void) {
    long fd = sys_open(AGENTD_TOOLS_CONF, 0);
    if (fd < 0) return;

    char buf[1024];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    /* Parse lines: name|path|desc|keywords */
    char *line = buf;
    while (*line && n_tools < MAX_TOOLS) {
        char *eol = line;
        while (*eol && *eol != '\n') eol++;

        char saved = *eol;
        *eol = '\0';

        if (line[0] != '#' && line[0] != '\0') {
            /* Parse fields separated by | */
            char *fields[4] = {0};
            int fi = 0;
            fields[0] = line;
            for (char *p = line; *p && fi < 3; p++) {
                if (*p == '|') {
                    *p = '\0';
                    fields[++fi] = p + 1;
                }
            }

            if (fi >= 3) {
                tool_entry_t *t = &tools[n_tools];
                /* Skip if already registered (from defaults) */
                int dup = 0;
                for (int i = 0; i < n_tools; i++) {
                    if (strcmp(tools[i].name, fields[0]) == 0) { dup = 1; break; }
                }
                if (!dup) {
                    t->active = 1;
                    str_ncopy(t->name, fields[0], TOOL_MAX_NAME);
                    str_ncopy(t->path, fields[1], TOOL_MAX_PATH);
                    str_ncopy(t->desc, fields[2], TOOL_MAX_DESC);
                    /* Parse keywords */
                    const char *kw = fields[3];
                    t->n_keywords = 0;
                    int ki = 0;
                    while (*kw && t->n_keywords < TOOL_MAX_KEYWORDS) {
                        if (*kw == ',') {
                            t->keywords[t->n_keywords][ki] = '\0';
                            t->n_keywords++;
                            ki = 0;
                        } else if (ki < TOOL_KW_LEN - 1) {
                            t->keywords[t->n_keywords][ki++] = *kw;
                        }
                        kw++;
                    }
                    if (ki > 0) {
                        t->keywords[t->n_keywords][ki] = '\0';
                        t->n_keywords++;
                    }
                    n_tools++;
                }
            }
        }

        if (saved == '\0') break;
        line = eol + 1;
    }
}

/* Select tool by keyword matching against prompt */
static int tool_select(const char *prompt) {
    int best_tool = -1;
    int best_score = 0;

    for (int i = 0; i < n_tools; i++) {
        if (!tools[i].active) continue;
        int score = 0;
        for (int k = 0; k < tools[i].n_keywords; k++) {
            if (strstr(prompt, tools[i].keywords[k]))
                score++;
        }
        if (score > best_score) {
            best_score = score;
            best_tool = i;
        }
    }

    return best_tool;
}

/* --- Session management --- */

static session_t *session_create(int client_fd) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].active) {
            session_t *s = &sessions[i];
            memset(s, 0, sizeof(*s));
            s->active = 1;
            s->client_fd = client_fd;
            s->session_id = next_session_id++;
            s->ctx_len = 0;
            s->context[0] = '\0';

            /* Init per-session vecstore */
            if (vecstore_init(&s->memory, EMBED_DIM) == 0)
                s->memory_initialized = 1;

            printf("[agentd] Session %u created (fd %ld)\n",
                   s->session_id, (long)client_fd);
            return s;
        }
    }
    return NULL;
}

static void session_destroy(session_t *s) {
    if (!s || !s->active) return;

    /* Save session memory */
    if (s->memory_initialized && vecstore_count(&s->memory) > 0) {
        char path[64];
        printf("[agentd] Session %u: %u memories (not persisted yet)\n",
               s->session_id, vecstore_count(&s->memory));
        (void)path; /* TODO: vecstore_save when /var/agentd is writable */
    }

    if (s->memory_initialized)
        vecstore_destroy(&s->memory);

    printf("[agentd] Session %u closed\n", s->session_id);
    s->active = 0;
    s->client_fd = -1;
}

static session_t *session_find_by_fd(int fd) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].active && sessions[i].client_fd == fd)
            return &sessions[i];
    }
    return NULL;
}

/* Append text to session context buffer, evicting old content if needed */
static void ctx_append(session_t *s, const char *text, int len) {
    if (len <= 0) return;

    /* If it won't fit, slide window */
    if (s->ctx_len + len + 1 >= CTX_BUF_SIZE) {
        int keep = CTX_BUF_SIZE / 2;
        int discard = s->ctx_len - keep;
        if (discard > 0) {
            memmove(s->context, s->context + discard, keep);
            s->ctx_len = keep;
        } else {
            /* Still won't fit — reset */
            s->ctx_len = 0;
        }
    }

    int copy = len;
    if (s->ctx_len + copy >= CTX_BUF_SIZE)
        copy = CTX_BUF_SIZE - s->ctx_len - 1;
    memcpy(s->context + s->ctx_len, text, copy);
    s->ctx_len += copy;
    s->context[s->ctx_len] = '\0';
}

/* --- Embedding computation --- */

static void compute_embedding(const char *text, int len, float *emb) {
    if (!embed_model_ok) {
        /* Fallback: zero embedding */
        memset(emb, 0, EMBED_DIM * sizeof(float));
        return;
    }

    embed_tf.pos = 0;
    uint32_t kv_count = 2 * embed_tf.cfg.n_layers * embed_tf.cfg.max_seq_len * embed_tf.cfg.dim;
    memset(embed_tf.kv_buf, 0, kv_count * sizeof(float));

    uint32_t tokens[128];
    uint32_t n_tok = tok_encode(&embed_tok, text, (uint32_t)len, tokens, 128);

    for (uint32_t i = 0; i < n_tok; i++)
        transformer_forward(&embed_tf, tokens[i]);

    if (n_tok > 0)
        memcpy(emb, embed_tf.x, EMBED_DIM * sizeof(float));
    else
        memset(emb, 0, EMBED_DIM * sizeof(float));
}

/* --- RAG context assembly --- */

static int assemble_prompt(session_t *s, const char *input, int input_len,
                           char *out, int max_out) {
    int pos = 0;

    /* Retrieve relevant memories via RAG */
    if (s->memory_initialized && vecstore_count(&s->memory) > 0) {
        float emb[EMBED_DIM];
        compute_embedding(input, input_len, emb);

        uint32_t topk_idx[3];
        float topk_score[3];
        int n = vecstore_query_topk(&s->memory, emb, 3, topk_idx, topk_score);

        for (int i = 0; i < n && pos < max_out - 128; i++) {
            if (topk_score[i] < 0.3f) break;
            const char *key = s->memory.entries[topk_idx[i]].key;
            int klen = strlen(key);
            if (pos + klen + 2 < max_out) {
                memcpy(out + pos, key, klen);
                pos += klen;
                out[pos++] = '\n';
            }
        }
    }

    /* Also check global memory */
    if (global_memory_ok && vecstore_count(&global_memory) > 0) {
        float emb[EMBED_DIM];
        compute_embedding(input, input_len, emb);

        uint32_t topk_idx[2];
        float topk_score[2];
        int n = vecstore_query_topk(&global_memory, emb, 2, topk_idx, topk_score);

        for (int i = 0; i < n && pos < max_out - 128; i++) {
            if (topk_score[i] < 0.3f) break;
            const char *key = global_memory.entries[topk_idx[i]].key;
            int klen = strlen(key);
            if (pos + klen + 2 < max_out) {
                memcpy(out + pos, key, klen);
                pos += klen;
                out[pos++] = '\n';
            }
        }
    }

    /* Append conversation context (history) */
    int ctx_budget = max_out - pos - input_len - 2;
    if (ctx_budget > 0 && s->ctx_len > 0) {
        int ctx_copy = s->ctx_len;
        if (ctx_copy > ctx_budget) ctx_copy = ctx_budget;
        /* Take the tail of context (most recent) */
        int offset = s->ctx_len - ctx_copy;
        memcpy(out + pos, s->context + offset, ctx_copy);
        pos += ctx_copy;
        out[pos++] = '\n';
    }

    /* Append current input */
    int copy = input_len;
    if (pos + copy >= max_out) copy = max_out - pos - 1;
    memcpy(out + pos, input, copy);
    pos += copy;
    out[pos] = '\0';

    return pos;
}

/* --- Tool execution --- */

static int execute_tool(int tool_idx, const char *args, char *output, int max_out) {
    if (tool_idx < 0 || tool_idx >= n_tools) return 0;

    tool_entry_t *t = &tools[tool_idx];
    const char *argv[4];
    argv[0] = t->path;
    argv[1] = args;
    argv[2] = NULL;

    tool_result_t result;
    int ret = tool_dispatch(t->path, argv, 0, 0, NULL, 0, &result);
    if (ret < 0) return 0;

    int copy = (int)result.output_len;
    if (copy > max_out - 1) copy = max_out - 1;
    memcpy(output, result.output, copy);
    output[copy] = '\0';
    return copy;
}

/* Extract tool arguments from prompt after the tool keyword */
static const char *extract_args(const char *prompt, int tool_idx) {
    if (tool_idx < 0) return "";
    tool_entry_t *t = &tools[tool_idx];

    /* Find the tool name/keyword in the prompt and return what follows */
    for (int k = 0; k < t->n_keywords; k++) {
        const char *pos = strstr(prompt, t->keywords[k]);
        if (pos) {
            pos += strlen(t->keywords[k]);
            while (*pos == ' ') pos++;
            return pos;
        }
    }

    /* Fallback: skip first word */
    const char *p = prompt;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    return p;
}

/* --- Core request handling --- */

static void handle_prompt(session_t *s, const char *input, int input_len) {
    /* Record user input in context */
    ctx_append(s, "user: ", 6);
    ctx_append(s, input, input_len);
    ctx_append(s, "\n", 1);

    /* Assemble full prompt with RAG + context + input */
    char prompt[MAX_REQ];
    int prompt_len = assemble_prompt(s, input, input_len, prompt, MAX_REQ);

    /* Call inference service */
    char response[MAX_RESP];
    long rlen = sys_infer_request("default", prompt, (unsigned long)prompt_len,
                                  response, sizeof(response) - 1);

    if (rlen > 0) {
        response[rlen] = '\0';
    } else {
        /* Inference failed — check if there's a tool to run directly */
        int tool_idx = tool_select(input);
        if (tool_idx >= 0) {
            const char *args = extract_args(input, tool_idx);
            char tool_out[1024];
            int tlen = execute_tool(tool_idx, args, tool_out, sizeof(tool_out));

            if (tlen > 0) {
                /* Send tool output as response */
                int copy = tlen;
                if (copy > MAX_RESP - 1) copy = MAX_RESP - 1;
                memcpy(response, tool_out, copy);
                response[copy] = '\0';
                rlen = copy;

                /* Record in context */
                ctx_append(s, "tool(", 5);
                ctx_append(s, tools[tool_idx].name, strlen(tools[tool_idx].name));
                ctx_append(s, "): ", 3);
                ctx_append(s, tool_out, tlen > 256 ? 256 : tlen);
                ctx_append(s, "\n", 1);
            } else {
                str_ncopy(response, "(tool failed)", MAX_RESP);
                rlen = strlen(response);
            }
        } else {
            str_ncopy(response, "(no inference service available)", MAX_RESP);
            rlen = strlen(response);
        }
    }

    /* Check if inference response wants a tool call */
    if (rlen > 0) {
        int chain_depth = 0;
        while (chain_depth < MAX_TOOL_CHAIN) {
            int tool_idx = tool_select(response);
            if (tool_idx < 0) break;

            const char *args = extract_args(response, tool_idx);
            char tool_out[1024];
            int tlen = execute_tool(tool_idx, args, tool_out, sizeof(tool_out));
            if (tlen <= 0) break;

            /* Record tool execution in context */
            ctx_append(s, "tool(", 5);
            ctx_append(s, tools[tool_idx].name, strlen(tools[tool_idx].name));
            ctx_append(s, "): ", 3);
            ctx_append(s, tool_out, tlen > 256 ? 256 : tlen);
            ctx_append(s, "\n", 1);

            /* Re-infer with tool output appended */
            int new_prompt_len = prompt_len;
            if (new_prompt_len + tlen + 16 < MAX_REQ) {
                memcpy(prompt + new_prompt_len, "\ntool output: ", 14);
                new_prompt_len += 14;
                int copy = tlen;
                if (new_prompt_len + copy >= MAX_REQ) copy = MAX_REQ - new_prompt_len - 1;
                memcpy(prompt + new_prompt_len, tool_out, copy);
                new_prompt_len += copy;
                prompt[new_prompt_len] = '\0';
            }

            rlen = sys_infer_request("default", prompt, (unsigned long)new_prompt_len,
                                     response, sizeof(response) - 1);
            if (rlen <= 0) break;
            response[rlen] = '\0';

            chain_depth++;
        }
    }

    /* Record response in context */
    ctx_append(s, "agent: ", 7);
    if (rlen > 0) ctx_append(s, response, rlen);
    ctx_append(s, "\n", 1);

    /* Store embedding in session memory */
    if (s->memory_initialized && embed_model_ok) {
        float emb[EMBED_DIM];
        compute_embedding(input, input_len, emb);

        char key[VECSTORE_MAX_KEY + 1];
        int klen = input_len;
        if (klen > VECSTORE_MAX_KEY) klen = VECSTORE_MAX_KEY;
        memcpy(key, input, klen);
        key[klen] = '\0';
        vecstore_store(&s->memory, key, emb);
    }

    /* Send response to client */
    if (rlen > 0) {
        sys_fwrite(s->client_fd, response, rlen);
    }
}

/* --- Message sending helper --- */

static void send_msg(int fd, uint8_t type, const char *data, int len) {
    msg_hdr_t hdr;
    hdr.type = type;
    hdr.len = (uint16_t)len;
    sys_fwrite(fd, &hdr, sizeof(hdr));
    if (len > 0 && data)
        sys_fwrite(fd, data, len);
}

/* --- Tool listing --- */

static void handle_list_tools(int client_fd) {
    char buf[1024];
    int pos = 0;

    for (int i = 0; i < n_tools && pos < 900; i++) {
        if (!tools[i].active) continue;
        int n = snprintf(buf + pos, sizeof(buf) - pos,
                         "%s (%s) - %s\n",
                         tools[i].name, tools[i].path, tools[i].desc);
        if (n > 0) pos += n;
    }

    sys_fwrite(client_fd, buf, pos);
}

/* --- Main server loop --- */

int main(void) {
    printf("[agentd] Starting agent daemon...\n");

    /* Register with agent registry */
    sys_agent_register("agentd");

    /* Initialize tool registry */
    n_tools = 0;
    tools_init_defaults();
    tools_load_config();
    printf("[agentd] %d tools registered\n", n_tools);

    /* Initialize embedding model (for memory/RAG) */
    tok_default_config(&embed_tok);
    if (transformer_load(&embed_tf, &embed_cfg, "/model.bin") == 0) {
        embed_model_ok = 1;
        printf("[agentd] Embedding model loaded (dim=%u)\n", EMBED_DIM);
    } else if (transformer_init(&embed_tf, &embed_cfg, 42) == 0) {
        embed_model_ok = 1;
        printf("[agentd] Embedding model initialized (random weights)\n");
    } else {
        embed_model_ok = 0;
        printf("[agentd] WARNING: No embedding model — RAG disabled\n");
    }

    /* Initialize global memory */
    if (vecstore_init(&global_memory, EMBED_DIM) == 0) {
        global_memory_ok = 1;
        vecstore_load(&global_memory, AGENTD_GLOBAL_MEM);
        printf("[agentd] Global memory: %u entries\n", vecstore_count(&global_memory));
    }

    /* Create and bind unix socket */
    long sock_fd = sys_unix_socket();
    if (sock_fd < 0) {
        printf("[agentd] FATAL: Failed to create socket\n");
        return 1;
    }

    if (sys_unix_bind(sock_fd, AGENTD_SOCK_PATH) < 0) {
        printf("[agentd] FATAL: Failed to bind %s\n", AGENTD_SOCK_PATH);
        sys_close(sock_fd);
        return 1;
    }

    if (sys_unix_listen(sock_fd) < 0) {
        printf("[agentd] FATAL: Failed to listen\n");
        sys_close(sock_fd);
        return 1;
    }

    printf("[agentd] Ready — listening on %s\n", AGENTD_SOCK_PATH);
    printf("[agentd] Sessions: %d max, Tools: %d, Memory: %s\n",
           MAX_SESSIONS, n_tools, embed_model_ok ? "enabled" : "disabled");

    /* Main accept loop */
    for (;;) {
        long client_fd = sys_unix_accept(sock_fd);
        if (client_fd < 0) {
            sys_yield();
            continue;
        }

        /* Read request */
        char req_buf[MAX_REQ];
        long n = sys_read(client_fd, req_buf, sizeof(req_buf) - 1);
        if (n <= 0) {
            sys_close(client_fd);
            continue;
        }
        req_buf[n] = '\0';

        /* Parse message header or treat as raw prompt */
        if (n >= (long)sizeof(msg_hdr_t)) {
            msg_hdr_t *hdr = (msg_hdr_t *)req_buf;

            if (hdr->type == MSG_NEW_SESSION) {
                session_t *s = session_create(client_fd);
                if (s) {
                    char resp[32];
                    int rlen = snprintf(resp, sizeof(resp), "session:%u", s->session_id);
                    sys_fwrite(client_fd, resp, rlen);
                } else {
                    sys_fwrite(client_fd, "error:max sessions", 18);
                }
                sys_close(client_fd);
                continue;
            }

            if (hdr->type == MSG_LIST_TOOLS) {
                handle_list_tools(client_fd);
                sys_close(client_fd);
                continue;
            }

            if (hdr->type == MSG_CLOSE_SESSION) {
                /* Find and close session */
                char *payload = req_buf + sizeof(msg_hdr_t);
                uint32_t sid = 0;
                for (char *p = payload; *p >= '0' && *p <= '9'; p++)
                    sid = sid * 10 + (*p - '0');
                for (int i = 0; i < MAX_SESSIONS; i++) {
                    if (sessions[i].active && sessions[i].session_id == sid)
                        session_destroy(&sessions[i]);
                }
                sys_fwrite(client_fd, "ok", 2);
                sys_close(client_fd);
                continue;
            }
        }

        /* Default: treat as a prompt for the default session */
        /* Find or create a session for this connection */
        session_t *s = NULL;

        /* Reuse first active session (simple single-session mode for now) */
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (sessions[i].active) { s = &sessions[i]; break; }
        }
        if (!s) {
            s = session_create(client_fd);
        }

        if (s) {
            s->client_fd = client_fd;
            handle_prompt(s, req_buf, (int)n);
        } else {
            sys_fwrite(client_fd, "error:no session", 16);
        }

        sys_close(client_fd);
    }

    /* Cleanup (unreachable in daemon mode) */
    if (embed_model_ok) transformer_destroy(&embed_tf);
    if (global_memory_ok) vecstore_destroy(&global_memory);
    sys_close(sock_fd);
    return 0;
}
