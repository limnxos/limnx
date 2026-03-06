#include "libc/libc.h"

#define MAX_INPUT    128
#define MAX_TOKENS   128
#define RESP_TOKENS  32
#define DIM          48
#define RECALL_THRESH 0.3f

static tf_config_t cfg = {
    .dim        = DIM,
    .hidden_dim = 128,
    .n_heads    = 4,
    .n_layers   = 2,
    .vocab_size = 96,
    .max_seq_len = 64,
};

/* Reset transformer for a new turn */
static void tf_reset(transformer_t *tf) {
    tf->pos = 0;
    uint32_t kv_count = 2 * tf->cfg.n_layers * tf->cfg.max_seq_len * tf->cfg.dim;
    memset(tf->kv_buf, 0, kv_count * sizeof(float));
}

/* Read a line with echo */
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

/* Get embedding from transformer: feed tokens, return copy of tf.x */
static void get_embedding(transformer_t *tf, tok_config_t *tok,
                           const char *text, int len, float *emb) {
    tf_reset(tf);

    uint32_t tokens[MAX_TOKENS];
    uint32_t n_tok = tok_encode(tok, text, (uint32_t)len, tokens, MAX_TOKENS);

    if (n_tok == 0) {
        /* No tokens — zero embedding */
        memset(emb, 0, DIM * sizeof(float));
        return;
    }

    /* Feed all tokens through transformer */
    for (uint32_t i = 0; i < n_tok; i++)
        transformer_forward(tf, tokens[i]);

    /* Copy hidden state as embedding */
    memcpy(emb, tf->x, DIM * sizeof(float));
}

/* Generate a response: continue from current transformer state */
static void generate_response(transformer_t *tf, tok_config_t *tok,
                                const char *input, int input_len) {
    /* Feed input tokens first */
    uint32_t tokens[MAX_TOKENS];
    uint32_t n_tok = tok_encode(tok, input, (uint32_t)input_len, tokens, MAX_TOKENS);

    tf_reset(tf);
    float *logits = NULL;
    for (uint32_t i = 0; i < n_tok; i++)
        logits = transformer_forward(tf, tokens[i]);

    if (!logits) return;

    /* Generate response tokens */
    printf("[bot] ");
    for (int i = 0; i < RESP_TOKENS && logits; i++) {
        uint32_t best = 0;
        float best_val = logits[0];
        for (uint32_t j = 1; j < tf->cfg.vocab_size; j++) {
            if (logits[j] > best_val) {
                best_val = logits[j];
                best = j;
            }
        }
        char c = (best < tok->vocab_size) ? tok->chars[best] : '?';
        printf("%c", c);
        logits = transformer_forward(tf, best);
    }
    printf("\n");
}

int main(void) {
    printf("\n");
    printf("========================================\n");
    printf("  Limnx Chatbot\n");
    printf("========================================\n");
    printf("Chat with an AI that remembers.\n");
    printf("Type 'quit' to exit.\n\n");

    tok_config_t tok;
    tok_default_config(&tok);

    transformer_t tf;
    if (transformer_load(&tf, &cfg, "/model.bin") == 0) {
        printf("Loaded trained model from /model.bin\n");
    } else if (transformer_init(&tf, &cfg, 42) != 0) {
        printf("FAIL: transformer_init\n");
        return 1;
    }

    vecstore_t memory;
    if (vecstore_init(&memory, DIM) != 0) {
        printf("FAIL: vecstore_init\n");
        transformer_destroy(&tf);
        return 1;
    }

    /* Try to load persistent memories from previous session */
    if (vecstore_load(&memory, "/memory.dat") == 0)
        printf("Loaded %u memories from /memory.dat\n", vecstore_count(&memory));

    printf("Initialized: transformer (dim=%u), memory (dim=%u)\n\n",
           tf.cfg.dim, memory.dim);

    char line[MAX_INPUT];
    float embedding[DIM];
    uint32_t turn = 0;

    for (;;) {
        printf("you> ");
        int len = readline(line, MAX_INPUT);
        if (len == 0)
            continue;
        if (strcmp(line, "quit") == 0)
            break;

        /* Get embedding of user message */
        get_embedding(&tf, &tok, line, len, embedding);

        /* Retrieve top-K memories for RAG */
        uint32_t topk_idx[3];
        float topk_score[3];
        int n_recall = vecstore_query_topk(&memory, embedding, 3,
                                            topk_idx, topk_score);

        /* Build RAG prompt: context memories + user input */
        char rag_prompt[MAX_INPUT + 128];
        int rag_len = 0;

        /* Token budget: max_seq_len - RESP_TOKENS = tokens for context+input */
        int budget = (int)cfg.max_seq_len - RESP_TOKENS;
        int input_tokens = len;  /* char-level: 1 char = 1 token */
        int ctx_budget = budget - input_tokens;

        /* Inject memories (best-first) while budget allows */
        int injected = 0;
        for (int i = 0; i < n_recall && ctx_budget > 0; i++) {
            if (topk_score[i] < RECALL_THRESH)
                break;
            const char *mkey = memory.entries[topk_idx[i]].key;
            int mlen = (int)strlen(mkey);
            int cost = mlen + 1;  /* memory text + separator ('|' or '\n') */
            if (cost > ctx_budget)
                continue;
            /* Append memory text */
            memcpy(&rag_prompt[rag_len], mkey, (size_t)mlen);
            rag_len += mlen;
            rag_prompt[rag_len++] = '|';
            ctx_budget -= cost;
            printf("  (recall[%d]: \"%s\", sim=%f)\n",
                   injected, mkey, (double)topk_score[i]);
            injected++;
        }

        /* Replace trailing '|' with '\n' if we injected anything */
        if (injected > 0)
            rag_prompt[rag_len - 1] = '\n';

        /* Append user input */
        memcpy(&rag_prompt[rag_len], line, (size_t)len);
        rag_len += len;
        rag_prompt[rag_len] = '\0';

        /* Generate response using RAG prompt */
        generate_response(&tf, &tok, rag_prompt, rag_len);

        /* Store this message in memory (truncate key to fit) */
        char key[VECSTORE_MAX_KEY + 1];
        int klen = len;
        if (klen > VECSTORE_MAX_KEY)
            klen = VECSTORE_MAX_KEY;
        memcpy(key, line, (size_t)klen);
        key[klen] = '\0';
        vecstore_store(&memory, key, embedding);
        turn++;

        printf("  [memory: %u entries]\n\n", vecstore_count(&memory));
    }

    /* Save memories for next session */
    if (vecstore_count(&memory) > 0) {
        if (vecstore_save(&memory, "/memory.dat") == 0)
            printf("Saved %u memories to /memory.dat\n", vecstore_count(&memory));
    }

    vecstore_destroy(&memory);
    transformer_destroy(&tf);
    printf("Goodbye.\n");
    return 0;
}
