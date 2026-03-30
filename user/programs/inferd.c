/*
 * inferd.c — Inference daemon for Limnx
 *
 * Loads a GGUF (or model.bin) model at startup, listens on a unix socket,
 * receives prompts, runs the transformer, returns generated text.
 * Registers with the kernel inference service registry for routing.
 *
 * Usage: inferd [model_path] [svc_name] [sock_path] [max_requests]
 *   model_path:   /test.gguf or /model.bin (default: /test.gguf)
 *   svc_name:     service name (default: "default")
 *   sock_path:    unix socket path (default: /tmp/inferd.sock)
 *   max_requests: 0 = serve forever (default: 0)
 */

#include "libc/libc.h"

#define MAX_PROMPT   512
#define MAX_GEN      64
#define TEMPERATURE  0.8f
#define TOP_K        40

static transformer_t tf;
static tf_config_t   cfg;
static bpe_tokenizer_t bpe;
static tok_config_t  char_tok;
static int use_bpe = 0;

/* Try to load GGUF model, fall back to model.bin */
static int load_model(const char *path) {
    /* Check file extension for .gguf */
    int len = strlen(path);
    int is_gguf = (len > 5 &&
                   path[len-5] == '.' && path[len-4] == 'g' &&
                   path[len-3] == 'g' && path[len-2] == 'u' &&
                   path[len-1] == 'f');

    if (is_gguf) {
        printf("[inferd] Loading GGUF model: %s\n", path);
        if (gguf_load(path, &tf, &cfg, &bpe) == 0) {
            use_bpe = (bpe.vocab_size > 0);
            printf("[inferd] GGUF loaded: dim=%u layers=%u vocab=%u heads=%u %s\n",
                   cfg.dim, cfg.n_layers, cfg.vocab_size, cfg.n_heads,
                   use_bpe ? "(BPE)" : "(no BPE)");
            return 0;
        }
        printf("[inferd] GGUF load failed, trying model.bin fallback\n");
    }

    /* Try model.bin format */
    printf("[inferd] Loading model.bin: %s\n", path);
    long fd = sys_open(path, 0);
    if (fd < 0) return -1;

    /* Read header: dim, hidden_dim, n_layers, n_heads, vocab_size, max_seq_len */
    uint32_t header[6];
    long n = sys_read(fd, header, sizeof(header));
    if (n < (long)sizeof(header)) { sys_close(fd); return -1; }

    cfg.dim         = header[0];
    cfg.hidden_dim  = header[1];
    cfg.n_layers    = header[2];
    cfg.n_heads     = header[3];
    cfg.vocab_size  = header[4];
    cfg.max_seq_len = header[5];
    cfg.rope        = 1;
    cfg.swiglu      = 0;
    cfg.n_kv_heads  = 0;
    cfg.qk_norm     = 0;
    cfg.rope_theta  = 10000.0f;
    sys_close(fd);

    if (transformer_load(&tf, &cfg, path) != 0) return -1;

    /* Use character-level tokenizer for model.bin */
    tok_default_config(&char_tok);
    use_bpe = 0;

    printf("[inferd] model.bin loaded: dim=%u layers=%u vocab=%u\n",
           cfg.dim, cfg.n_layers, cfg.vocab_size);
    return 0;
}

/* Encode prompt text to tokens */
static uint32_t encode_prompt(const char *text, uint32_t text_len,
                              uint32_t *tokens, uint32_t max_tokens) {
    if (use_bpe) {
        return bpe_encode(&bpe, text, text_len, tokens, max_tokens);
    } else {
        return tok_encode(&char_tok, text, text_len, tokens, max_tokens);
    }
}

/* Decode tokens to text */
static uint32_t decode_tokens(const uint32_t *tokens, uint32_t n_tokens,
                              char *out, uint32_t max_out) {
    if (use_bpe) {
        return bpe_decode(&bpe, tokens, n_tokens, out, max_out);
    } else {
        return tok_decode(&char_tok, tokens, n_tokens, out, max_out);
    }
}

/* Run inference: encode prompt, feed through transformer, generate response */
static int generate_response(const char *prompt, uint32_t prompt_len,
                             char *response, uint32_t max_response) {
    uint32_t tokens[MAX_PROMPT];
    uint32_t n_tok = encode_prompt(prompt, prompt_len, tokens, MAX_PROMPT);
    if (n_tok == 0) return 0;

    /* Reset transformer state for new sequence */
    tf.pos = 0;

    /* Feed prompt tokens (prefill) */
    float *logits = NULL;
    for (uint32_t i = 0; i < n_tok; i++)
        logits = transformer_forward(&tf, tokens[i]);

    if (!logits) return 0;

    /* Generate continuation tokens */
    uint32_t gen_tokens[MAX_GEN];
    uint32_t gen_count = 0;

    for (uint32_t i = 0; i < MAX_GEN && logits; i++) {
        uint32_t tok = transformer_sample(logits, cfg.vocab_size,
                                          TEMPERATURE, TOP_K);
        gen_tokens[gen_count++] = tok;

        /* Stop on EOS (token 0, 2, or vocab-specific EOS like Qwen's 151645) */
        if (tok == 0 || tok == 2 || tok == 151643 || tok == 151645) break;

        logits = transformer_forward(&tf, tok);
    }

    /* Decode generated tokens to text */
    uint32_t out_len = decode_tokens(gen_tokens, gen_count,
                                     response, max_response);
    return (int)out_len;
}

int main(int argc, char **argv) {
    const char *model_path = "/model.gguf";  /* disk-backed (LimnFS), fallback to initrd */
    const char *svc_name   = "default";
    const char *sock_path  = "/tmp/inferd.sock";
    int max_requests = 0;  /* 0 = serve forever */

    if (argc >= 2) model_path   = argv[1];
    if (argc >= 3) svc_name     = argv[2];
    if (argc >= 4) sock_path    = argv[3];
    if (argc >= 5) max_requests = atoi(argv[4]);

    printf("[inferd] Starting inference daemon '%s'...\n", svc_name);

    /* Seed RNG */
    transformer_seed_rng(12345);

    /* Load model */
    if (load_model(model_path) != 0) {
        /* Fallback: try /test.gguf (initrd) if /model.gguf (disk) fails */
        if (strcmp(model_path, "/model.gguf") == 0) {
            printf("[inferd] /model.gguf not found, trying /test.gguf...\n");
            model_path = "/test.gguf";
            if (load_model(model_path) != 0) {
                printf("[inferd] Failed to load any model\n");
                return 1;
            }
        } else {
            printf("[inferd] Failed to load model: %s\n", model_path);
            return 1;
        }
    }

    /* Create and bind unix socket */
    long sock_fd = sys_unix_socket();
    if (sock_fd < 0) { puts("[inferd] Failed to create socket\n"); return 1; }

    if (sys_unix_bind(sock_fd, sock_path) < 0) {
        puts("[inferd] Failed to bind socket\n");
        sys_close(sock_fd);
        return 1;
    }

    if (sys_unix_listen(sock_fd) < 0) {
        puts("[inferd] Failed to listen\n");
        sys_close(sock_fd);
        return 1;
    }

    /* Register with kernel inference service */
    long ret = sys_infer_register(svc_name, sock_path);
    if (ret < 0) {
        puts("[inferd] Failed to register service\n");
        sys_close(sock_fd);
        return 1;
    }

    printf("[inferd] Ready — model=%s vocab=%u dim=%u layers=%u\n",
           model_path, cfg.vocab_size, cfg.dim, cfg.n_layers);

    sys_infer_health(0);

    /* Serve requests */
    int served = 0;
    for (;;) {
        if (max_requests > 0 && served >= max_requests) break;

        sys_infer_health((long)served);

        long client_fd = sys_unix_accept(sock_fd);
        if (client_fd < 0) {
            sys_yield();
            continue;
        }

        /* Read prompt */
        char req_buf[MAX_PROMPT];
        long n = sys_read(client_fd, req_buf, sizeof(req_buf) - 1);
        if (n > 0) {
            req_buf[n] = '\0';
            printf("[inferd] Request #%d: \"%s\"\n", served + 1, req_buf);

            /* Run inference */
            char response[512];
            int rlen = generate_response(req_buf, (uint32_t)n,
                                         response, sizeof(response));

            if (rlen > 0) {
                sys_fwrite(client_fd, response, rlen);
                printf("[inferd] Response: %d chars\n", rlen);
            } else {
                const char *err = "(no output)";
                sys_fwrite(client_fd, err, 11);
            }
        }

        sys_close(client_fd);
        served++;
    }

    sys_infer_health(9999);
    sys_close(sock_fd);
    printf("[inferd] Exiting after %d requests\n", served);
    return 0;
}
