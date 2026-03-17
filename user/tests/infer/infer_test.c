/*
 * infer_test.c — Inference/AI subsystem tests
 * Tests: math primitives, sampling, transformer, GGUF loader,
 *        inference service pipeline (inferd → kernel registry → client).
 * Portable — no arch-specific code.
 */
#include "../limntest.h"

/* ---- Math primitives ---- */

static void test_math_primitives(void) {
    float val = sqrtf(4.0f);
    lt_ok(val > 1.99f && val < 2.01f, "sqrtf(4) ≈ 2.0");

    val = expf(0.0f);
    lt_ok(val > 0.99f && val < 1.01f, "expf(0) ≈ 1.0");

    val = logf(1.0f);
    lt_ok(val > -0.01f && val < 0.01f, "logf(1) ≈ 0.0");

    val = fabsf(-3.14f);
    lt_ok(val > 3.13f && val < 3.15f, "fabsf(-3.14) ≈ 3.14");

    val = tanhf(0.0f);
    lt_ok(val > -0.01f && val < 0.01f, "tanhf(0) ≈ 0.0");
}

/* ---- String/memory ops ---- */

static void test_string_ops(void) {
    lt_ok(strlen("hello") == 5, "strlen");
    lt_ok(strcmp("abc", "abc") == 0, "strcmp equal");
    lt_ok(strcmp("abc", "abd") < 0, "strcmp less");
    lt_ok(strncmp("abcdef", "abcxyz", 3) == 0, "strncmp prefix match");

    char buf[32];
    strcpy(buf, "hello");
    lt_ok(strcmp(buf, "hello") == 0, "strcpy");

    lt_ok(strstr("hello world", "world") != (void *)0, "strstr found");
    lt_ok(strstr("hello world", "xyz") == (void *)0, "strstr not found");
}

static void test_memops(void) {
    char a[32], b[32];
    memset(a, 0xAA, 32);
    lt_ok((unsigned char)a[0] == 0xAA && (unsigned char)a[31] == 0xAA, "memset");
    memcpy(b, a, 32);
    lt_ok((unsigned char)b[0] == 0xAA && (unsigned char)b[31] == 0xAA, "memcpy");
}

static void test_atoi_strtol(void) {
    lt_ok(atoi("42") == 42, "atoi(42)");
    lt_ok(atoi("-7") == -7, "atoi(-7)");
    lt_ok(atoi("0") == 0, "atoi(0)");
    lt_ok(strtol("255", (void *)0, 10) == 255, "strtol base 10");
    lt_ok(strtol("ff", (void *)0, 16) == 255, "strtol base 16");
}

/* ---- Agent registry ---- */

static void test_agent_registry(void) {
    long ret = sys_agent_register("test_agent");
    lt_ok(ret == 0, "agent register");

    long pid_out = 0;
    ret = sys_agent_lookup("test_agent", &pid_out);
    lt_ok(ret == 0, "agent lookup finds registered agent");
    lt_ok(pid_out == sys_getpid(), "agent lookup returns correct PID");
}

/* ---- Sampling ---- */

static void test_sampling(void) {
    /* Greedy: temperature=0 should pick max */
    float logits1[4] = {1.0f, 5.0f, 2.0f, 3.0f};
    uint32_t tok = transformer_sample(logits1, 4, 0.0f, 1);
    lt_ok(tok == 1, "greedy sample picks argmax");

    /* Greedy with top_k=1 */
    float logits2[4] = {0.0f, 0.0f, 9.0f, 0.0f};
    tok = transformer_sample(logits2, 4, 1.0f, 1);
    lt_ok(tok == 2, "top_k=1 picks argmax");

    /* Sampling: temperature>0 top_k>1 should return a valid index */
    transformer_seed_rng(42);
    float logits3[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    tok = transformer_sample(logits3, 4, 1.0f, 4);
    lt_ok(tok < 4, "sampled token in range");

    /* Multiple samples should produce variation */
    int counts[4] = {0, 0, 0, 0};
    for (int i = 0; i < 100; i++) {
        float l[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        tok = transformer_sample(l, 4, 1.0f, 4);
        if (tok < 4) counts[tok]++;
    }
    int got_diversity = (counts[0] > 0 && counts[1] > 0 &&
                         counts[2] > 0 && counts[3] > 0);
    lt_ok(got_diversity, "sampling with uniform logits covers all tokens");
}

/* ---- Transformer init + forward ---- */

static void test_transformer_local(void) {
    tf_config_t cfg = {
        .dim        = 48,
        .hidden_dim = 128,
        .n_heads    = 4,
        .n_layers   = 2,
        .vocab_size = 96,
        .max_seq_len = 32,
        .rope       = 1,
        .swiglu     = 0,
        .n_kv_heads = 0,
        .qk_norm    = 0,
        .rope_theta = 10000.0f,
    };

    transformer_t tf;
    int ret = transformer_init(&tf, &cfg, 42);
    lt_ok(ret == 0, "transformer init");

    float *logits = transformer_forward(&tf, 1);
    lt_ok(logits != (void *)0, "forward pass returns logits");

    /* Logits should be vocab_size floats */
    int valid = 1;
    for (uint32_t i = 0; i < cfg.vocab_size; i++) {
        if (logits[i] != logits[i]) { valid = 0; break; } /* NaN check */
    }
    lt_ok(valid, "logits are valid (no NaN)");

    /* Generate tokens */
    transformer_seed_rng(123);
    uint32_t tokens[8];
    uint32_t count = transformer_generate_sampled(&tf, 1, tokens, 8, 0.8f, 20);
    lt_ok(count == 8, "generate_sampled produces requested count");

    int all_valid = 1;
    for (uint32_t i = 0; i < count; i++) {
        if (tokens[i] >= cfg.vocab_size) { all_valid = 0; break; }
    }
    lt_ok(all_valid, "all generated tokens in vocab range");

    transformer_destroy(&tf);
}

/* ---- Tokenizer ---- */

static void test_tokenizer(void) {
    tok_config_t tok;
    tok_default_config(&tok);
    lt_ok(tok.vocab_size > 0, "char tokenizer has vocab");

    uint32_t tokens[32];
    uint32_t n = tok_encode(&tok, "Hi", 2, tokens, 32);
    lt_ok(n == 2, "encode 'Hi' = 2 tokens");

    char out[32];
    uint32_t dlen = tok_decode(&tok, tokens, n, out, 32);
    lt_ok(dlen == 2, "decode back to 2 chars");
    lt_ok(out[0] == 'H' && out[1] == 'i', "round-trip Hi");
}

/* ---- GGUF loader ---- */

static void test_gguf_loader(void) {
    transformer_t tf;
    tf_config_t cfg;
    bpe_tokenizer_t bpe;
    memset(&bpe, 0, sizeof(bpe));

    int ret = gguf_load("/test.gguf", &tf, &cfg, &bpe);
    lt_ok(ret == 0, "gguf_load /test.gguf");

    if (ret == 0) {
        lt_ok(cfg.dim == 64, "gguf dim=64");
        lt_ok(cfg.n_layers == 2, "gguf layers=2");
        lt_ok(cfg.vocab_size == 320, "gguf vocab=320");
        lt_ok(cfg.n_heads == 4, "gguf heads=4");

        /* Forward pass should work */
        float *logits = transformer_forward(&tf, 1);
        lt_ok(logits != (void *)0, "gguf forward pass works");

        /* BPE tokenizer should be loaded */
        lt_ok(bpe.vocab_size > 0, "gguf BPE vocab loaded");

        transformer_destroy(&tf);
    } else {
        lt_skip("gguf dim", "gguf_load failed");
        lt_skip("gguf layers", "gguf_load failed");
        lt_skip("gguf vocab", "gguf_load failed");
        lt_skip("gguf heads", "gguf_load failed");
        lt_skip("gguf forward", "gguf_load failed");
        lt_skip("gguf BPE", "gguf_load failed");
    }
}

/* ---- Inference service pipeline ---- */

static void test_infer_pipeline(void) {
    /* Fork inferd as a child process — serve 1 request then exit */
    long pid = sys_fork();
    if (pid == 0) {
        const char *argv[] = {
            "inferd", "/test.gguf", "test_svc", "/tmp/test_inferd.sock", "1", (void *)0
        };
        sys_execve("/inferd.elf", argv);
        sys_exit(99);
    }

    lt_ok(pid > 0, "fork inferd");
    if (pid <= 0) return;

    /* Wait for inferd to start and register */
    for (int i = 0; i < 50; i++) sys_yield();

    /* Flush infer cache so request goes to inferd (not cached) */
    sys_infer_cache_ctrl(2 /* FLUSH */, (void *)0);

    /* Send a request via the kernel inference service */
    char resp[256];
    long ret = sys_infer_request("test_svc", "Hello", 5, resp, sizeof(resp) - 1);

    if (ret > 0) {
        resp[ret] = '\0';
        lt_ok(ret > 0, "infer_request got response");
        lt_ok(ret < 256, "response length reasonable");
        lt_diag(resp);
    } else {
        /* inferd may not have started yet — yield more and retry */
        for (int i = 0; i < 100; i++) sys_yield();
        ret = sys_infer_request("test_svc", "Test", 4, resp, sizeof(resp) - 1);
        if (ret > 0) {
            resp[ret] = '\0';
            lt_ok(1, "infer_request got response (retry)");
            lt_ok(ret < 256, "response length reasonable");
            lt_diag(resp);
        } else {
            lt_ok(0, "infer_request got response");
            lt_ok(0, "response length reasonable");
        }
    }

    /* Wait for inferd to finish (it exits after 1 request) */
    sys_waitpid(pid);
}

/* ---- Async inference (submit/poll/result) ---- */

static void test_infer_async(void) {
    /* Fork inferd for async test — serve 1 request then exit */
    long pid = sys_fork();
    if (pid == 0) {
        const char *argv[] = {
            "inferd", "/test.gguf", "async_svc", "/tmp/test_async.sock", "1", (void *)0
        };
        sys_execve("/inferd.elf", argv);
        sys_exit(99);
    }

    lt_ok(pid > 0, "fork inferd (async)");
    if (pid <= 0) return;

    /* Wait for registration */
    for (int i = 0; i < 80; i++) sys_yield();

    /* Flush cache to ensure request hits inferd */
    sys_infer_cache_ctrl(2, (void *)0);

    /* Submit async request */
    long req_id = sys_infer_submit("async_svc", "World", 5, -1);

    if (req_id >= 0) {
        lt_ok(1, "infer_submit returned request ID");

        /* Poll until complete */
        int done = 0;
        for (int i = 0; i < 200; i++) {
            long status = sys_infer_poll(req_id);
            if (status == 1) { done = 1; break; } /* 1 = complete */
            sys_yield();
        }
        lt_ok(done, "infer_poll shows completion");

        if (done) {
            char resp[256];
            long n = sys_infer_result(req_id, resp, sizeof(resp) - 1);
            lt_ok(n > 0, "infer_result returns data");
            if (n > 0) {
                resp[n] = '\0';
                lt_diag(resp);
            }
        } else {
            lt_skip("infer_result", "poll never completed");
        }
    } else {
        lt_ok(0, "infer_submit returned request ID");
        lt_skip("infer_poll", "submit failed");
        lt_skip("infer_result", "submit failed");
    }

    sys_waitpid(pid);
}

int main(void) {
    lt_suite("infer");

    test_math_primitives();
    test_string_ops();
    test_memops();
    test_atoi_strtol();
    test_agent_registry();
    test_sampling();
    test_tokenizer();
    test_transformer_local();
    test_gguf_loader();
    test_infer_pipeline();
    test_infer_async();

    return lt_done();
}
