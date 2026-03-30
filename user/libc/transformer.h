#ifndef LIMNX_TRANSFORMER_H
#define LIMNX_TRANSFORMER_H

#include "libc.h"  /* for uint32_t, float, basic types */

/* --- Transformer inference runtime --- */

typedef struct tf_config {
    uint32_t dim;
    uint32_t hidden_dim;
    uint32_t n_heads;
    uint32_t n_layers;
    uint32_t vocab_size;
    uint32_t max_seq_len;
    uint32_t rope;      /* 1 = RoPE positional encoding, 0 = none */
    uint32_t swiglu;    /* 1 = SwiGLU FFN, 0 = ReLU FFN */
    uint32_t n_kv_heads;/* 0 = same as n_heads (MHA), >0 = GQA/MQA */
    uint32_t qk_norm;   /* 1 = per-head RMS norm on Q and K */
    float    rope_theta; /* 0.0 = default 10000.0 */
} tf_config_t;

/* Quantized weight descriptor — points into mmap'd GGUF data */
typedef struct qweight {
    const uint8_t *data;    /* raw quantized bytes (in GGUF mmap) */
    uint32_t qtype;         /* GGML type (Q4_K=12, Q6_K=14, etc.) */
    uint32_t rows;          /* GGUF shape[0] (output dim) */
    uint32_t cols;          /* GGUF shape[1] (input dim) */
} qweight_t;

typedef struct transformer {
    tf_config_t cfg;

    /* === F32 weights (used when quantized=0) === */
    float   *weights_buf;
    uint64_t weights_mmap_addr;
    uint32_t weights_mmap_pages;

    float  *token_emb;          /* vocab_size x dim */
    float **rms_att_w;          /* [n_layers] -> dim floats */
    float **wq;                 /* [n_layers] -> dim x dim */
    float **wk, **wv, **wo;
    float **rms_ffn_w;          /* [n_layers] -> dim */
    float **w1;                 /* [n_layers] -> dim x hidden_dim */
    float **w2;                 /* [n_layers] -> hidden_dim x dim */
    float **w3;                 /* [n_layers] -> dim x hidden_dim (SwiGLU gate, NULL when swiglu=0) */
    float **wq_norm;            /* [n_layers] -> head_dim (NULL when qk_norm=0) */
    float **wk_norm;            /* [n_layers] -> head_dim (NULL when qk_norm=0) */
    float  *rms_final_w;        /* dim */
    float  *wcls;               /* dim x vocab_size */

    /* Per-layer pointer arrays: 1 mmap buffer */
    uint64_t ptrs_mmap_addr;
    uint32_t ptrs_mmap_pages;

    /* === Quantized weights (used when quantized=1) === */
    uint32_t quantized;         /* 0 = F32, 1 = quantized (GGUF zero-copy) */
    uint64_t gguf_mmap_addr;    /* GGUF file mmap (kept alive for quantized data) */
    uint32_t gguf_mmap_pages;

    qweight_t  q_token_emb;     /* [vocab_size, dim] quantized */
    qweight_t  q_wcls;          /* [vocab_size, dim] quantized */
    qweight_t *qqwq, *qqwk, *qqwv, *qqwo;   /* per-layer quantized projections */
    qweight_t *qqw1, *qqw2, *qqw3;           /* per-layer quantized FFN */

    /* Norm weights buffer (always F32, small — allocated for quantized mode) */
    float   *norms_buf;
    uint64_t norms_mmap_addr;
    uint32_t norms_mmap_pages;

    /* qweight_t pointer arrays mmap */
    uint64_t qptrs_mmap_addr;
    uint32_t qptrs_mmap_pages;

    /* Dequant scratch: one row at a time */
    float   *dq_row;            /* max(dim, hidden_dim, vocab_size) floats */
    uint64_t dq_mmap_addr;
    uint32_t dq_mmap_pages;

    /* KV cache: 1 mmap buffer */
    float   *kv_buf;
    uint64_t kv_mmap_addr;
    uint32_t kv_mmap_pages;
    float   *key_cache;          /* n_layers x max_seq_len x dim */
    float   *value_cache;        /* n_layers x max_seq_len x dim */

    /* Scratch: 1 mmap buffer */
    float   *scratch_buf;
    uint64_t scratch_mmap_addr;
    uint32_t scratch_mmap_pages;
    float   *x;       /* dim */
    float   *xb;      /* dim */
    float   *xb2;     /* dim */
    float   *q;       /* dim */
    float   *k;       /* dim (K projection scratch for GQA) */
    float   *hb;      /* hidden_dim */
    float   *hb2;     /* hidden_dim (SwiGLU gate scratch, NULL when swiglu=0) */
    float   *att;     /* max_seq_len */
    float   *logits;  /* vocab_size */

    uint32_t pos;
} transformer_t;

int      transformer_init(transformer_t *tf, const tf_config_t *cfg,
                           uint32_t seed);
int      transformer_init_quantized(transformer_t *tf, const tf_config_t *cfg);
void     transformer_destroy(transformer_t *tf);
float   *transformer_forward(transformer_t *tf, uint32_t token);
uint32_t transformer_generate(transformer_t *tf, uint32_t start_token,
                               uint32_t *out_tokens, uint32_t max_tokens);
uint32_t transformer_generate_sampled(transformer_t *tf, uint32_t start_token,
                               uint32_t *out_tokens, uint32_t max_tokens,
                               float temperature, uint32_t top_k);
uint32_t transformer_sample(float *logits, uint32_t vocab_size,
                            float temperature, uint32_t top_k);
void     transformer_seed_rng(uint64_t seed);

/* --- Character-level tokenizer --- */

typedef struct tok_config {
    uint32_t vocab_size;
    char     chars[256];          /* vocab index -> character */
    int      char_to_idx[256];    /* ASCII code -> vocab index (-1 = unknown) */
} tok_config_t;

void     tok_default_config(tok_config_t *cfg);
uint32_t tok_encode(const tok_config_t *cfg, const char *text, uint32_t text_len,
                    uint32_t *tokens, uint32_t max_tokens);
uint32_t tok_decode(const tok_config_t *cfg, const uint32_t *tokens, uint32_t n_tokens,
                    char *out, uint32_t max_out);

/* --- Transformer save/load --- */

int transformer_save(transformer_t *tf, const char *path);
int transformer_load(transformer_t *tf, tf_config_t *cfg, const char *path);

/* --- BPE tokenizer --- */

typedef struct bpe_merge {
    uint32_t left, right, result;
} bpe_merge_t;

typedef struct bpe_tokenizer {
    uint32_t vocab_size;
    uint32_t n_merges;
    char **vocab;             /* vocab[i] -> string */
    uint32_t *vocab_len;      /* length of each vocab entry */
    bpe_merge_t *merges;
    uint64_t mmap_addr;
    uint32_t mmap_pages;
    char *pool;               /* string pool bump allocator */
    uint32_t pool_used;
    uint32_t pool_size;
} bpe_tokenizer_t;

int      bpe_init(bpe_tokenizer_t *bpe, uint32_t vocab_size, uint32_t n_merges);
void     bpe_destroy(bpe_tokenizer_t *bpe);
void     bpe_set_vocab(bpe_tokenizer_t *bpe, uint32_t idx, const char *str, uint32_t len);
void     bpe_set_merge(bpe_tokenizer_t *bpe, uint32_t idx,
                       uint32_t left, uint32_t right, uint32_t result);
uint32_t bpe_encode(bpe_tokenizer_t *bpe, const char *text, uint32_t len,
                    uint32_t *tokens, uint32_t max_tokens);
uint32_t bpe_decode(bpe_tokenizer_t *bpe, const uint32_t *tokens, uint32_t n_tokens,
                    char *out, uint32_t max_out);

/* --- Dequantization (GGML quantized -> F32) --- */

int dequant(const void *src, float *dst, uint64_t n, uint32_t type);
int dequant_block_info(uint32_t type, uint32_t *block_size, uint32_t *block_bytes);

/* --- GGUF model loader --- */

int gguf_load(const char *path, transformer_t *tf, tf_config_t *cfg,
              bpe_tokenizer_t *bpe);

#endif
