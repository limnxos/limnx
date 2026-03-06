#include "libc/libc.h"

#define MAX_INPUT      256
#define MAX_TOKENS     128
#define DIM            48
#define NUM_TOOLS      6
#define SUMMARY_MAX    128
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

/* --- Helpers (duplicated from toolagent.c per project convention) --- */

static void tf_reset(transformer_t *tf) {
    tf->pos = 0;
    uint32_t kv_count = 2 * tf->cfg.n_layers * tf->cfg.max_seq_len * tf->cfg.dim;
    memset(tf->kv_buf, 0, kv_count * sizeof(float));
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

static void buf_append(char *buf, int max, const char *str) {
    int pos = (int)strlen(buf);
    while (*str && pos < max - 1)
        buf[pos++] = *str++;
    buf[pos] = '\0';
}

static const char *extract_arg(const char *input) {
    while (*input && *input != ' ')
        input++;
    while (*input == ' ')
        input++;
    return input;
}

static const char *split_first(const char *input, char *path_buf, int max) {
    int i = 0;
    while (*input && *input != ' ' && i < max - 1)
        path_buf[i++] = *input++;
    path_buf[i] = '\0';
    while (*input == ' ')
        input++;
    return input;
}

/* --- Tool functions --- */

typedef int (*tool_fn_t)(const char *arg, char *summary, int max);

static int tool_ls(const char *arg, char *summary, int max) {
    (void)arg;
    dirent_t ent;
    int count = 0;
    for (unsigned long i = 0; sys_readdir("/", i, &ent) == 0; i++)
        count++;
    summary[0] = '\0';
    buf_append(summary, max, "listed files");
    return (count > 0) ? 0 : -1;
}

static int tool_cat(const char *arg, char *summary, int max) {
    if (!*arg) return -1;
    long fd = sys_open(arg, 0);
    if (fd < 0) return -1;
    char buf[128];
    sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    summary[0] = '\0';
    buf_append(summary, max, "read ");
    buf_append(summary, max, arg);
    return 0;
}

static int tool_write(const char *arg, char *summary, int max) {
    char path[64];
    const char *content = split_first(arg, path, 64);
    (void)content;
    summary[0] = '\0';
    buf_append(summary, max, "wrote ");
    buf_append(summary, max, path);
    return 0;
}

static int tool_stat(const char *arg, char *summary, int max) {
    if (!*arg) return -1;
    uint8_t st[16];
    if (sys_stat(arg, st) != 0) return -1;
    summary[0] = '\0';
    buf_append(summary, max, "stat ");
    buf_append(summary, max, arg);
    return 0;
}

static int tool_exec(const char *arg, char *summary, int max) {
    (void)arg;
    summary[0] = '\0';
    buf_append(summary, max, "ran program");
    return 0;
}

static int tool_send(const char *arg, char *summary, int max) {
    (void)arg;
    summary[0] = '\0';
    buf_append(summary, max, "sent udp msg");
    return 0;
}

/* --- Tool registry --- */

typedef struct tool_def {
    const char  *name;
    const char  *desc;
    const char  *keywords[5];
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

static float tool_embeddings[NUM_TOOLS][DIM];

static void compute_tool_embeddings(transformer_t *tf, tok_config_t *tok) {
    for (int i = 0; i < NUM_TOOLS; i++)
        get_embedding(tf, tok, tools[i].desc,
                      (int)strlen(tools[i].desc), tool_embeddings[i]);
}

static int select_by_keywords(const char *input) {
    for (int t = 0; t < NUM_TOOLS; t++) {
        for (int k = 0; tools[t].keywords[k]; k++) {
            if (strstr(input, tools[t].keywords[k]))
                return t;
        }
    }
    return -1;
}

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

/* --- Chain planner (duplicated from toolagent.c) --- */

typedef struct chain_step {
    int  tool_idx;
    char arg[MAX_STEP_ARG];
} chain_step_t;

typedef struct chain_plan {
    chain_step_t steps[MAX_CHAIN_STEPS];
    int          n_steps;
} chain_plan_t;

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

    p = strstr(text, ", ");
    if (p && p[2] >= 'a' && p[2] <= 'z') {
        if (!best || p < best) { best = p; best_len = 2; }
    }

    if (best)
        *delim_len = best_len;
    return best;
}

static void extract_clause_arg(const char *clause, int tool_idx, char *arg, int max) {
    arg[0] = '\0';
    while (*clause == ' ') clause++;

    if (tool_idx == 1 || tool_idx == 3) {
        const char *p = clause;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            const char *start = p;
            while (*p && *p != ' ') p++;
            int wlen = (int)(p - start);
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

    const char *a = extract_arg(clause);
    int alen = (int)strlen(a);
    if (alen >= max) alen = max - 1;
    memcpy(arg, a, (size_t)alen);
    arg[alen] = '\0';
}

static void plan_chain(const char *input, transformer_t *tf, tok_config_t *tok,
                        chain_plan_t *plan) {
    plan->n_steps = 0;

    char buf[MAX_INPUT];
    int ilen = (int)strlen(input);
    if (ilen >= MAX_INPUT) ilen = MAX_INPUT - 1;
    memcpy(buf, input, (size_t)ilen);
    buf[ilen] = '\0';

    char *clauses[MAX_CHAIN_STEPS];
    int n_clauses = 0;
    char *cur = buf;

    while (n_clauses < MAX_CHAIN_STEPS && *cur) {
        while (*cur == ' ') cur++;
        if (!*cur) break;

        int dlen = 0;
        const char *delim = find_clause_delim(cur, &dlen);
        if (delim) {
            char *clause_start = cur;
            buf[(int)(delim - buf)] = '\0';
            clauses[n_clauses++] = clause_start;
            cur = (char *)delim + dlen;
        } else {
            clauses[n_clauses++] = cur;
            break;
        }
    }

    for (int i = 0; i < n_clauses; i++) {
        int tidx = select_by_keywords(clauses[i]);
        if (tidx < 0) {
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

static void execute_chain(chain_plan_t *plan, char *combined, int max) {
    combined[0] = '\0';
    char step_summary[SUMMARY_MAX];

    for (int i = 0; i < plan->n_steps; i++) {
        chain_step_t *step = &plan->steps[i];
        step_summary[0] = '\0';
        tools[step->tool_idx].fn(step->arg, step_summary, SUMMARY_MAX);

        if (i > 0 && combined[0])
            buf_append(combined, max, "; ");
        buf_append(combined, max, step_summary);
    }
}

/* --- RAG loss (duplicated from learn.c) --- */

static float compute_loss(transformer_t *tf, const uint32_t *tokens,
                           uint32_t n_tokens) {
    if (n_tokens < 2) return 100.0f;

    tf_reset(tf);
    float total_loss = 0.0f;
    uint32_t count = 0;

    for (uint32_t i = 0; i < n_tokens - 1; i++) {
        float *logits = transformer_forward(tf, tokens[i]);
        if (!logits) continue;

        uint32_t target = tokens[i + 1];
        float max_val = logits[0];
        for (uint32_t j = 1; j < tf->cfg.vocab_size; j++)
            if (logits[j] > max_val)
                max_val = logits[j];

        float sum_exp = 0.0f;
        for (uint32_t j = 0; j < tf->cfg.vocab_size; j++)
            sum_exp += expf(logits[j] - max_val);

        float log_prob = (logits[target] - max_val) - logf(sum_exp);
        total_loss += -log_prob;
        count++;
    }

    return (count > 0) ? total_loss / (float)count : 100.0f;
}

static float compute_loss_with_context(transformer_t *tf, const uint32_t *tokens,
                                        uint32_t n_tokens, uint32_t context_len) {
    if (context_len == 0)
        return compute_loss(tf, tokens, n_tokens);
    if (n_tokens < context_len + 2)
        return 100.0f;

    tf_reset(tf);

    for (uint32_t i = 0; i < context_len; i++)
        transformer_forward(tf, tokens[i]);

    float total_loss = 0.0f;
    uint32_t count = 0;

    for (uint32_t i = context_len; i < n_tokens - 1; i++) {
        float *logits = transformer_forward(tf, tokens[i]);
        if (!logits) continue;

        uint32_t target = tokens[i + 1];
        float max_val = logits[0];
        for (uint32_t j = 1; j < tf->cfg.vocab_size; j++)
            if (logits[j] > max_val)
                max_val = logits[j];

        float sum_exp = 0.0f;
        for (uint32_t j = 0; j < tf->cfg.vocab_size; j++)
            sum_exp += expf(logits[j] - max_val);

        float log_prob = (logits[target] - max_val) - logf(sum_exp);
        total_loss += -log_prob;
        count++;
    }

    return (count > 0) ? total_loss / (float)count : 100.0f;
}

/* === Tests === */

int main(void) {
    printf("=== agenttest2: Agent Intelligence ===\n");
    int passed = 0, failed = 0;

    tok_config_t tok;
    tok_default_config(&tok);

    transformer_t tf;
    if (transformer_init(&tf, &cfg, 42) != 0) {
        printf("FAIL: transformer init\n");
        return 1;
    }

    compute_tool_embeddings(&tf, &tok);

    /* Test 1: Single clause planning */
    printf("  [test 1] single clause plan: ");
    {
        chain_plan_t plan;
        plan_chain("list files", &tf, &tok, &plan);
        if (plan.n_steps == 1 && plan.steps[0].tool_idx == 0) {
            printf("PASS (1 step, ls)\n");
            passed++;
        } else {
            printf("FAIL (n_steps=%d, tool=%d)\n", plan.n_steps,
                   plan.n_steps > 0 ? plan.steps[0].tool_idx : -1);
            failed++;
        }
    }

    /* Test 2: Two-clause "and" */
    printf("  [test 2] two-clause 'and': ");
    {
        chain_plan_t plan;
        plan_chain("list files and read hello.txt", &tf, &tok, &plan);
        if (plan.n_steps == 2 && plan.steps[0].tool_idx == 0 &&
            plan.steps[1].tool_idx == 1) {
            printf("PASS (2 steps: ls, cat)\n");
            passed++;
        } else {
            printf("FAIL (n=%d, t0=%d, t1=%d)\n", plan.n_steps,
                   plan.n_steps > 0 ? plan.steps[0].tool_idx : -1,
                   plan.n_steps > 1 ? plan.steps[1].tool_idx : -1);
            failed++;
        }
    }

    /* Test 3: Three-clause "then" */
    printf("  [test 3] three-clause 'then': ");
    {
        chain_plan_t plan;
        plan_chain("list files then read hello.txt then stat hello.txt",
                    &tf, &tok, &plan);
        if (plan.n_steps == 3 && plan.steps[0].tool_idx == 0 &&
            plan.steps[1].tool_idx == 1 && plan.steps[2].tool_idx == 3) {
            printf("PASS (3 steps: ls, cat, stat)\n");
            passed++;
        } else {
            printf("FAIL (n=%d, t0=%d, t1=%d, t2=%d)\n", plan.n_steps,
                   plan.n_steps > 0 ? plan.steps[0].tool_idx : -1,
                   plan.n_steps > 1 ? plan.steps[1].tool_idx : -1,
                   plan.n_steps > 2 ? plan.steps[2].tool_idx : -1);
            failed++;
        }
    }

    /* Test 4: Comma-separated */
    printf("  [test 4] comma-separated: ");
    {
        chain_plan_t plan;
        plan_chain("list files, read hello.txt", &tf, &tok, &plan);
        if (plan.n_steps == 2 && plan.steps[0].tool_idx == 0 &&
            plan.steps[1].tool_idx == 1) {
            printf("PASS (2 steps: ls, cat)\n");
            passed++;
        } else {
            printf("FAIL (n=%d, t0=%d, t1=%d)\n", plan.n_steps,
                   plan.n_steps > 0 ? plan.steps[0].tool_idx : -1,
                   plan.n_steps > 1 ? plan.steps[1].tool_idx : -1);
            failed++;
        }
    }

    /* Test 5: Chain execution (ls + stat) */
    printf("  [test 5] chain exec (ls+stat): ");
    {
        chain_plan_t plan;
        plan.n_steps = 2;
        plan.steps[0].tool_idx = 0;  /* ls */
        plan.steps[0].arg[0] = '\0';
        plan.steps[1].tool_idx = 3;  /* stat */
        strcpy(plan.steps[1].arg, "hello.elf");

        char combined[SUMMARY_MAX];
        execute_chain(&plan, combined, SUMMARY_MAX);
        if (strlen(combined) > 0 && strstr(combined, "listed") &&
            strstr(combined, "stat")) {
            printf("PASS (%s)\n", combined);
            passed++;
        } else {
            printf("FAIL (summary='%s')\n", combined);
            failed++;
        }
    }

    /* Test 6: Chain single step = same as direct call */
    printf("  [test 6] single step chain: ");
    {
        chain_plan_t plan;
        plan.n_steps = 1;
        plan.steps[0].tool_idx = 0;  /* ls */
        plan.steps[0].arg[0] = '\0';

        char chain_summary[SUMMARY_MAX];
        execute_chain(&plan, chain_summary, SUMMARY_MAX);

        char direct_summary[SUMMARY_MAX];
        direct_summary[0] = '\0';
        tool_ls("", direct_summary, SUMMARY_MAX);

        if (strcmp(chain_summary, direct_summary) == 0) {
            printf("PASS (both='%s')\n", chain_summary);
            passed++;
        } else {
            printf("FAIL (chain='%s', direct='%s')\n", chain_summary,
                   direct_summary);
            failed++;
        }
    }

    /* Test 7: Keyword dispatch on natural phrases */
    printf("  [test 7] keyword dispatch: ");
    {
        int t1 = select_by_keywords("what files are here");
        int t2 = select_by_keywords("show me the contents");
        int t3 = select_by_keywords("send a message");
        /* "files" → ls (0), "show" → cat (1), "send" → send (5) */
        if (t1 == 0 && t2 == 1 && t3 == 5) {
            printf("PASS (files→ls, show→cat, send→send)\n");
            passed++;
        } else {
            printf("FAIL (t1=%d, t2=%d, t3=%d)\n", t1, t2, t3);
            failed++;
        }
    }

    /* Test 8: RAG-conditioned loss differs from plain loss */
    printf("  [test 8] RAG-conditioned loss: ");
    {
        const char *text = "the cat sat on the mat";
        int tlen = (int)strlen(text);
        uint32_t tokens[MAX_TOKENS];
        uint32_t n_tok = tok_encode(&tok, text, (uint32_t)tlen, tokens, MAX_TOKENS);

        float plain_loss = compute_loss(&tf, tokens, n_tok);

        /* Build context + training sequence */
        const char *ctx = "hello world";
        int clen = (int)strlen(ctx);
        uint32_t combined[MAX_TOKENS];
        uint32_t cn = tok_encode(&tok, ctx, (uint32_t)clen, combined, MAX_TOKENS / 2);
        uint32_t ctx_tok_len = cn;
        for (uint32_t t = 0; t < n_tok && cn < MAX_TOKENS; t++)
            combined[cn++] = tokens[t];

        float ctx_loss = compute_loss_with_context(&tf, combined, cn, ctx_tok_len);

        /* Losses should differ because context changes hidden state */
        float diff = plain_loss - ctx_loss;
        if (diff < 0) diff = -diff;
        if (diff > 0.001f) {
            printf("PASS (plain=%.4f, ctx=%.4f, diff=%.4f)\n",
                   (double)plain_loss, (double)ctx_loss, (double)diff);
            passed++;
        } else {
            printf("FAIL (plain=%.4f, ctx=%.4f, diff=%.4f too small)\n",
                   (double)plain_loss, (double)ctx_loss, (double)diff);
            failed++;
        }
    }

    /* Test 9: Argument extraction */
    printf("  [test 9] arg extraction: ");
    {
        chain_plan_t plan;
        plan_chain("read hello.txt and stat hello.txt", &tf, &tok, &plan);
        if (plan.n_steps == 2 &&
            strcmp(plan.steps[0].arg, "hello.txt") == 0 &&
            strcmp(plan.steps[1].arg, "hello.txt") == 0) {
            printf("PASS (arg0='%s', arg1='%s')\n",
                   plan.steps[0].arg, plan.steps[1].arg);
            passed++;
        } else {
            printf("FAIL (n=%d, arg0='%s', arg1='%s')\n", plan.n_steps,
                   plan.n_steps > 0 ? plan.steps[0].arg : "",
                   plan.n_steps > 1 ? plan.steps[1].arg : "");
            failed++;
        }
    }

    /* Test 10: Max steps clamping */
    printf("  [test 10] max steps clamping: ");
    {
        chain_plan_t plan;
        plan_chain("list files and read hello.txt and stat hello.txt and send msg and run prog",
                    &tf, &tok, &plan);
        if (plan.n_steps == MAX_CHAIN_STEPS) {
            printf("PASS (clamped to %d)\n", MAX_CHAIN_STEPS);
            passed++;
        } else {
            printf("FAIL (n_steps=%d, expected %d)\n", plan.n_steps,
                   MAX_CHAIN_STEPS);
            failed++;
        }
    }

    transformer_destroy(&tf);

    printf("=== agenttest2: %d passed, %d failed ===\n", passed, failed);
    if (failed == 0)
        printf("=== agenttest2: ALL PASSED ===\n");
    else
        printf("=== agenttest2: SOME TESTS FAILED ===\n");

    return (failed == 0) ? 0 : 1;
}
