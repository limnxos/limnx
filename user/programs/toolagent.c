#include "libc/libc.h"

#define MAX_INPUT    256
#define MAX_TOKENS   128
#define RESP_TOKENS  24
#define DIM          48
#define NUM_TOOLS    6
#define RECALL_THRESH 0.3f
#define SUMMARY_MAX  128
#define MAX_CHAIN_STEPS 4
#define MAX_STEP_ARG   64

static tf_config_t cfg = {
    .dim        = DIM,
    .hidden_dim = 128,
    .n_heads    = 4,
    .n_layers   = 2,
    .vocab_size = 96,
    .max_seq_len = 64,
};

/* --- Helpers --- */

static void tf_reset(transformer_t *tf) {
    tf->pos = 0;
    uint32_t kv_count = 2 * tf->cfg.n_layers * tf->cfg.max_seq_len * tf->cfg.dim;
    memset(tf->kv_buf, 0, kv_count * sizeof(float));
}

static int readline(char *buf, int max) {
    int pos = 0;
    while (pos < max - 1) {
        long ch = sys_getchar();
        if (ch == '\n' || ch == '\r') {
            char nl = '\n';
            sys_write(&nl, 1);
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

static void get_embedding(transformer_t *tf, tok_config_t *tok,
                           const char *text, int len, float *emb) {
    tf_reset(tf);
    uint32_t tokens[MAX_TOKENS];
    uint32_t n_tok = tok_encode(tok, text, (uint32_t)len, tokens, MAX_TOKENS);
    if (n_tok == 0) {
        memset(emb, 0, DIM * sizeof(float));
        return;
    }
    for (uint32_t i = 0; i < n_tok; i++)
        transformer_forward(tf, tokens[i]);
    memcpy(emb, tf->x, DIM * sizeof(float));
}

/* Append string to buffer with bounds checking */
static void buf_append(char *buf, int max, const char *str) {
    int pos = (int)strlen(buf);
    while (*str && pos < max - 1)
        buf[pos++] = *str++;
    buf[pos] = '\0';
}

/* Return pointer to text after first whitespace-delimited word */
static const char *extract_arg(const char *input) {
    while (*input && *input != ' ')
        input++;
    while (*input == ' ')
        input++;
    return input;
}

/* Split "path content..." into path_buf and return pointer to content */
static const char *split_first(const char *input, char *path_buf, int max) {
    int i = 0;
    while (*input && *input != ' ' && i < max - 1)
        path_buf[i++] = *input++;
    path_buf[i] = '\0';
    while (*input == ' ')
        input++;
    return input;
}

/* --- Tools --- */

/* Tool function signature: returns 0 on success, writes summary to buf */
typedef int (*tool_fn_t)(const char *arg, char *summary, int max);

static int tool_ls(const char *arg, char *summary, int max) {
    (void)arg;
    dirent_t ent;
    int count = 0;
    printf("  Files:\n");
    for (unsigned long i = 0; sys_readdir("/", i, &ent) == 0; i++) {
        printf("    %-24s %lu bytes\n", ent.name, ent.size);
        count++;
    }
    printf("  (%d files total)\n", count);

    summary[0] = '\0';
    buf_append(summary, max, "listed files");
    return 0;
}

static int tool_cat(const char *arg, char *summary, int max) {
    if (!*arg) {
        printf("  [cat] no file specified\n");
        return -1;
    }

    long fd = sys_open(arg, 0);
    if (fd < 0) {
        printf("  [cat] cannot open '%s'\n", arg);
        return -1;
    }

    printf("  Contents of %s:\n", arg);
    char buf[512];
    long n;
    while ((n = sys_read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }
    printf("\n");
    sys_close(fd);

    summary[0] = '\0';
    buf_append(summary, max, "read ");
    buf_append(summary, max, arg);
    return 0;
}

static int tool_write(const char *arg, char *summary, int max) {
    char path[64];
    const char *content = split_first(arg, path, 64);
    if (!path[0]) {
        printf("  [write] usage: write <path> <content>\n");
        return -1;
    }

    long fd = sys_create(path);
    if (fd < 0) {
        /* File may exist already, try opening */
        fd = sys_open(path, 0);
        if (fd < 0) {
            printf("  [write] cannot create '%s'\n", path);
            return -1;
        }
    }

    unsigned long len = strlen(content);
    if (len > 0)
        sys_fwrite(fd, content, len);
    sys_close(fd);

    printf("  Wrote %lu bytes to %s\n", len, path);

    summary[0] = '\0';
    buf_append(summary, max, "wrote ");
    buf_append(summary, max, path);
    return 0;
}

static int tool_stat(const char *arg, char *summary, int max) {
    if (!*arg) {
        printf("  [stat] no file specified\n");
        return -1;
    }

    uint8_t st[16];
    if (sys_stat(arg, st) != 0) {
        printf("  [stat] '%s' not found\n", arg);
        return -1;
    }

    uint64_t size = *(uint64_t *)st;
    uint8_t type = st[8];
    printf("  %s: %lu bytes, type=%s\n", arg, size,
           type == 0 ? "file" : "dir");

    summary[0] = '\0';
    buf_append(summary, max, "stat ");
    buf_append(summary, max, arg);
    return 0;
}

static int tool_exec(const char *arg, char *summary, int max) {
    if (!*arg) {
        printf("  [exec] no program specified\n");
        return -1;
    }

    long child_pid = sys_exec(arg, NULL);
    if (child_pid <= 0) {
        printf("  [exec] failed to run '%s'\n", arg);
        return -1;
    }

    printf("  Running %s (pid %ld)...\n", arg, child_pid);
    long status = sys_waitpid(child_pid);
    printf("  %s exited with status %ld\n", arg, status);

    summary[0] = '\0';
    buf_append(summary, max, "ran ");
    buf_append(summary, max, arg);
    return 0;
}

static int tool_send(const char *arg, char *summary, int max) {
    if (!*arg) {
        printf("  [send] no message specified\n");
        return -1;
    }

    long sock = sys_socket();
    if (sock < 0) {
        printf("  [send] socket failed\n");
        return -1;
    }

    unsigned long len = strlen(arg);
    /* Send to gateway 10.0.2.2:5555 */
    uint32_t dst_ip = (10 << 24) | (0 << 16) | (2 << 8) | 2;
    long ret = sys_sendto(sock, arg, len, dst_ip, 5555);
    if (ret < 0) {
        printf("  [send] sendto failed\n");
        return -1;
    }

    printf("  Sent %lu bytes to 10.0.2.2:5555\n", len);

    summary[0] = '\0';
    buf_append(summary, max, "sent udp msg");
    return 0;
}

/* --- Tool registry --- */

typedef struct tool_def {
    const char  *name;
    const char  *desc;
    const char  *keywords[5];  /* NULL-terminated */
    tool_fn_t    fn;
} tool_def_t;

static tool_def_t tools[NUM_TOOLS] = {
    { "ls",    "list files in the filesystem",
      { "list", "files", "dir", "ls", NULL },  tool_ls },
    { "cat",   "read and show file contents",
      { "read", "cat", "show", "view", NULL },  tool_cat },
    { "write", "create or write to a file",
      { "write", "create", "save", "note", NULL }, tool_write },
    { "stat",  "show file size and metadata",
      { "stat", "size", "info", NULL },         tool_stat },
    { "exec",  "run an ELF program",
      { "run", "exec", "launch", NULL },        tool_exec },
    { "send",  "send a UDP network message",
      { "send", "udp", "network", NULL },       tool_send },
};

/* Pre-computed tool description embeddings (filled at init) */
static float tool_embeddings[NUM_TOOLS][DIM];

static void compute_tool_embeddings(transformer_t *tf, tok_config_t *tok) {
    for (int i = 0; i < NUM_TOOLS; i++)
        get_embedding(tf, tok, tools[i].desc,
                      (int)strlen(tools[i].desc), tool_embeddings[i]);
}

/* Select tool by keyword scan; return index or -1 */
static int select_by_keywords(const char *input) {
    for (int t = 0; t < NUM_TOOLS; t++) {
        for (int k = 0; tools[t].keywords[k]; k++) {
            if (strstr(input, tools[t].keywords[k]))
                return t;
        }
    }
    return -1;
}

/* Select tool by embedding cosine similarity */
static int select_by_embedding(const float *input_emb) {
    int best = 0;
    float best_sim = vec_cosine_sim(input_emb, tool_embeddings[0], DIM);
    for (int i = 1; i < NUM_TOOLS; i++) {
        float sim = vec_cosine_sim(input_emb, tool_embeddings[i], DIM);
        if (sim > best_sim) {
            best_sim = sim;
            best = i;
        }
    }
    return best;
}

/* --- Multi-tool chain planner + executor --- */

typedef struct chain_step {
    int  tool_idx;
    char arg[MAX_STEP_ARG];
} chain_step_t;

typedef struct chain_plan {
    chain_step_t steps[MAX_CHAIN_STEPS];
    int          n_steps;
} chain_plan_t;

/* Scan for clause delimiters: " and ", " then ", " also ", ", "
 * Returns pointer to start of delimiter or NULL. Sets *delim_len. */
static const char *find_clause_delim(const char *text, int *delim_len) {
    const char *best = NULL;
    int best_len = 0;

    const char *p;

    p = strstr(text, " and ");
    if (p && (!best || p < best)) { best = p; best_len = 5; }

    p = strstr(text, " then ");
    if (p && (!best || p < best)) { best = p; best_len = 6; }

    p = strstr(text, " also ");
    if (p && (!best || p < best)) { best = p; best_len = 6; }

    /* ", " — but only if followed by a letter (avoid splitting paths) */
    p = strstr(text, ", ");
    if (p && p[2] >= 'a' && p[2] <= 'z') {
        if (!best || p < best) { best = p; best_len = 2; }
    }

    if (best)
        *delim_len = best_len;
    return best;
}

/* Extract argument from a clause: skip keyword tokens, return rest.
 * For file-related tools, looks for filename patterns. */
static void extract_clause_arg(const char *clause, int tool_idx, char *arg, int max) {
    arg[0] = '\0';
    /* Skip leading spaces */
    while (*clause == ' ') clause++;

    /* For tools that need file arguments, scan for filename-like tokens */
    if (tool_idx == 1 || tool_idx == 3) {  /* cat, stat */
        /* Look for a word containing '.' or '/' (likely a filename) */
        const char *p = clause;
        while (*p) {
            /* Skip spaces */
            while (*p == ' ') p++;
            if (!*p) break;
            /* Find end of word */
            const char *start = p;
            while (*p && *p != ' ') p++;
            int wlen = (int)(p - start);
            /* Check if this word looks like a filename */
            int has_dot = 0, has_slash = 0;
            for (int i = 0; i < wlen; i++) {
                if (start[i] == '.') has_dot = 1;
                if (start[i] == '/') has_slash = 1;
            }
            if (has_dot || has_slash) {
                if (wlen >= max) wlen = max - 1;
                memcpy(arg, start, (size_t)wlen);
                arg[wlen] = '\0';
                return;
            }
        }
    }

    /* Default: use extract_arg logic (text after first word) */
    const char *a = extract_arg(clause);
    int alen = (int)strlen(a);
    if (alen >= max) alen = max - 1;
    memcpy(arg, a, (size_t)alen);
    arg[alen] = '\0';
}

/* Plan a chain of tools from input text.
 * Splits on clause delimiters, selects tool per clause. */
static void plan_chain(const char *input, transformer_t *tf, tok_config_t *tok,
                        chain_plan_t *plan) {
    plan->n_steps = 0;

    /* Work on a copy since we'll null-terminate clauses */
    char buf[MAX_INPUT];
    int ilen = (int)strlen(input);
    if (ilen >= MAX_INPUT) ilen = MAX_INPUT - 1;
    memcpy(buf, input, (size_t)ilen);
    buf[ilen] = '\0';

    /* Split into clauses */
    char *clauses[MAX_CHAIN_STEPS];
    int n_clauses = 0;
    char *cur = buf;

    while (n_clauses < MAX_CHAIN_STEPS && *cur) {
        /* Skip leading spaces */
        while (*cur == ' ') cur++;
        if (!*cur) break;

        int dlen = 0;
        const char *delim = find_clause_delim(cur, &dlen);
        if (delim) {
            /* Null-terminate this clause */
            char *clause_start = cur;
            buf[(int)(delim - buf)] = '\0';
            clauses[n_clauses++] = clause_start;
            cur = (char *)delim + dlen;
        } else {
            /* Last clause */
            clauses[n_clauses++] = cur;
            break;
        }
    }

    /* For each clause, select tool and extract argument */
    for (int i = 0; i < n_clauses; i++) {
        int tidx = select_by_keywords(clauses[i]);
        if (tidx < 0) {
            /* Embedding fallback */
            float emb[DIM];
            get_embedding(tf, tok, clauses[i], (int)strlen(clauses[i]), emb);
            tidx = select_by_embedding(emb);
        }
        plan->steps[plan->n_steps].tool_idx = tidx;
        extract_clause_arg(clauses[i], tidx, plan->steps[plan->n_steps].arg,
                            MAX_STEP_ARG);
        plan->n_steps++;
    }
}

/* Execute a chain plan sequentially, building combined summary */
static void execute_chain(chain_plan_t *plan, char *combined, int max) {
    combined[0] = '\0';
    char step_summary[SUMMARY_MAX];

    for (int i = 0; i < plan->n_steps; i++) {
        chain_step_t *step = &plan->steps[i];
        printf("  [step %d/%d: %s]\n", i + 1, plan->n_steps,
               tools[step->tool_idx].name);

        step_summary[0] = '\0';
        tools[step->tool_idx].fn(step->arg, step_summary, SUMMARY_MAX);

        if (i > 0 && combined[0])
            buf_append(combined, max, "; ");
        buf_append(combined, max, step_summary);
    }
}

/* --- Self-test (automated, no user input) --- */

static int self_test(void) {
    printf("=== toolagent: self-test ===\n");

    /* Test 1: readdir finds files */
    printf("[test] readdir: ");
    dirent_t ent;
    int count = 0;
    for (unsigned long i = 0; sys_readdir("/", i, &ent) == 0; i++)
        count++;
    if (count > 0) {
        printf("found %d files OK\n", count);
    } else {
        printf("FAIL: no files found\n");
        return 1;
    }

    /* Test 2: keyword-based tool selection */
    printf("[test] tool select: ");
    int t1 = select_by_keywords("list files");
    int t2 = select_by_keywords("read hello.txt");
    int t3 = select_by_keywords("run /hello.elf");
    if (t1 == 0 && t2 == 1 && t3 == 4) {
        printf("ls=%d cat=%d exec=%d OK\n", t1, t2, t3);
    } else {
        printf("FAIL: ls=%d cat=%d exec=%d\n", t1, t2, t3);
        return 1;
    }

    /* Test 3: tool_ls executes */
    printf("[test] tool_ls: ");
    char summary[SUMMARY_MAX];
    summary[0] = '\0';
    if (tool_ls("", summary, SUMMARY_MAX) == 0) {
        printf("OK (summary: %s)\n", summary);
    } else {
        printf("FAIL\n");
        return 1;
    }

    /* Test 4: transformer + memory */
    printf("[test] transformer+memory: ");
    tok_config_t tok;
    tok_default_config(&tok);

    transformer_t tf;
    if (transformer_load(&tf, &cfg, "/model.bin") != 0) {
        if (transformer_init(&tf, &cfg, 42) != 0) {
            printf("FAIL: transformer init\n");
            return 1;
        }
    }

    vecstore_t memory;
    if (vecstore_init(&memory, DIM) != 0) {
        printf("FAIL: vecstore init\n");
        transformer_destroy(&tf);
        return 1;
    }

    float emb[DIM];
    get_embedding(&tf, &tok, "hello world", 11, emb);
    vecstore_store(&memory, "hello world", emb);

    uint32_t match_idx;
    float match_score;
    if (vecstore_query(&memory, emb, &match_idx, &match_score) == 0 &&
        match_score > 0.9f) {
        printf("store+recall OK (score=%f)\n", (double)match_score);
    } else {
        printf("FAIL\n");
        vecstore_destroy(&memory);
        transformer_destroy(&tf);
        return 1;
    }

    /* Test 5: chain planning — multi-clause */
    printf("[test] chain plan: ");
    compute_tool_embeddings(&tf, &tok);
    {
        chain_plan_t plan;
        plan_chain("list files and read hello.txt", &tf, &tok, &plan);
        if (plan.n_steps == 2 && plan.steps[0].tool_idx == 0 &&
            plan.steps[1].tool_idx == 1) {
            printf("2 steps (ls, cat) OK\n");
        } else {
            printf("FAIL: n_steps=%d t0=%d t1=%d\n",
                   plan.n_steps,
                   plan.n_steps > 0 ? plan.steps[0].tool_idx : -1,
                   plan.n_steps > 1 ? plan.steps[1].tool_idx : -1);
            vecstore_destroy(&memory);
            transformer_destroy(&tf);
            return 1;
        }
    }

    /* Test 6: chain execution */
    printf("[test] chain exec: ");
    {
        chain_plan_t plan;
        plan.n_steps = 2;
        plan.steps[0].tool_idx = 0;  /* ls */
        plan.steps[0].arg[0] = '\0';
        plan.steps[1].tool_idx = 3;  /* stat */
        strcpy(plan.steps[1].arg, "hello.elf");

        char combined[SUMMARY_MAX];
        execute_chain(&plan, combined, SUMMARY_MAX);
        if (strlen(combined) > 0) {
            printf("OK (summary: %s)\n", combined);
        } else {
            printf("FAIL: empty summary\n");
            vecstore_destroy(&memory);
            transformer_destroy(&tf);
            return 1;
        }
    }

    vecstore_destroy(&memory);
    transformer_destroy(&tf);

    printf("=== toolagent: self-test PASSED ===\n");
    return 0;
}

/* --- Interactive agent loop --- */

static void interactive(void) {
    tok_config_t tok;
    tok_default_config(&tok);

    transformer_t tf;
    if (transformer_load(&tf, &cfg, "/model.bin") != 0) {
        if (transformer_init(&tf, &cfg, 42) != 0) {
            printf("FAIL: transformer_init\n");
            return;
        }
    }

    vecstore_t memory;
    if (vecstore_init(&memory, DIM) != 0) {
        printf("FAIL: vecstore_init\n");
        transformer_destroy(&tf);
        return;
    }

    /* Try to load persistent memories from previous session */
    if (vecstore_load(&memory, "/toolmem.dat") == 0)
        printf("Loaded %u memories from /toolmem.dat\n", vecstore_count(&memory));

    /* Pre-compute tool description embeddings */
    compute_tool_embeddings(&tf, &tok);

    printf("\n");
    printf("========================================\n");
    printf("  Limnx Tool Agent\n");
    printf("========================================\n");
    printf("I can use tools to help you:\n");
    printf("  ls    — list files\n");
    printf("  cat   — read a file\n");
    printf("  write — create/write a file\n");
    printf("  stat  — file info\n");
    printf("  exec  — run a program\n");
    printf("  send  — send UDP message\n");
    printf("Multi-step commands supported (e.g. 'list files and read hello.txt').\n");
    printf("Type 'quit' to exit.\n\n");

    char line[MAX_INPUT];
    float embedding[DIM];
    char summary[SUMMARY_MAX];

    for (;;) {
        printf("agent> ");
        int len = readline(line, MAX_INPUT);
        if (len == 0)
            continue;
        if (strcmp(line, "quit") == 0)
            break;

        /* Compute embedding of user input */
        get_embedding(&tf, &tok, line, len, embedding);

        /* Retrieve top-K memories for RAG */
        uint32_t topk_idx[3];
        float topk_score[3];
        int n_recall = vecstore_query_topk(&memory, embedding, 3,
                                            topk_idx, topk_score);

        /* Build RAG prompt for transformer commentary */
        char rag_prompt[MAX_INPUT + 128];
        int rag_len = 0;

        int budget = (int)cfg.max_seq_len - RESP_TOKENS;
        int input_tokens = len;
        int ctx_budget = budget - input_tokens;

        int injected = 0;
        for (int i = 0; i < n_recall && ctx_budget > 0; i++) {
            if (topk_score[i] < RECALL_THRESH)
                break;
            const char *mkey = memory.entries[topk_idx[i]].key;
            int mlen = (int)strlen(mkey);
            int cost = mlen + 1;
            if (cost > ctx_budget)
                continue;
            memcpy(&rag_prompt[rag_len], mkey, (size_t)mlen);
            rag_len += mlen;
            rag_prompt[rag_len++] = '|';
            ctx_budget -= cost;
            printf("  (recall[%d]: \"%s\", sim=%f)\n",
                   injected, mkey, (double)topk_score[i]);
            injected++;
        }

        if (injected > 0)
            rag_prompt[rag_len - 1] = '\n';

        memcpy(&rag_prompt[rag_len], line, (size_t)len);
        rag_len += len;
        rag_prompt[rag_len] = '\0';

        /* Plan and execute tool chain */
        chain_plan_t plan;
        plan_chain(line, &tf, &tok, &plan);

        summary[0] = '\0';
        execute_chain(&plan, summary, SUMMARY_MAX);

        /* Generate brief AI commentary — try inference service first */
        {
            char ai_resp[256];
            long ai_ret = sys_infer_request("default", rag_prompt,
                                             (unsigned long)rag_len,
                                             ai_resp, sizeof(ai_resp) - 1);
            if (ai_ret > 0) {
                ai_resp[ai_ret] = '\0';
                printf("  [ai] %s\n", ai_resp);
            } else {
                /* Fall back to local transformer */
                tf_reset(&tf);
                uint32_t tokens[MAX_TOKENS];
                uint32_t n_tok = tok_encode(&tok, rag_prompt, (uint32_t)rag_len,
                                             tokens, MAX_TOKENS);
                float *logits = NULL;
                for (uint32_t i = 0; i < n_tok; i++)
                    logits = transformer_forward(&tf, tokens[i]);

                if (logits) {
                    printf("  [ai] ");
                    for (int i = 0; i < RESP_TOKENS && logits; i++) {
                        uint32_t best = transformer_sample(logits,
                            tf.cfg.vocab_size, 0.8f, 20);
                        char c = (best < tok.vocab_size) ? tok.chars[best] : '?';
                        printf("%c", c);
                        logits = transformer_forward(&tf, best);
                    }
                    printf("\n");
                }
            }
        }

        /* Store interaction in memory */
        char key[VECSTORE_MAX_KEY + 1];
        int klen = len;
        if (klen > VECSTORE_MAX_KEY)
            klen = VECSTORE_MAX_KEY;
        memcpy(key, line, (size_t)klen);
        key[klen] = '\0';
        vecstore_store(&memory, key, embedding);

        printf("  [memory: %u entries]\n\n", vecstore_count(&memory));
    }

    /* Save memories for next session */
    if (vecstore_count(&memory) > 0) {
        if (vecstore_save(&memory, "/toolmem.dat") == 0)
            printf("Saved %u memories to /toolmem.dat\n", vecstore_count(&memory));
    }

    vecstore_destroy(&memory);
    transformer_destroy(&tf);
    printf("Goodbye.\n");
}

int main(int argc, char **argv) {
    int test_only = 0;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--test") == 0) test_only = 1;

    int rc = self_test();
    if (test_only)
        return rc;

    interactive();
    return 0;
}
