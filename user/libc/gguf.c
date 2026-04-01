#include "libc.h"
#include "limnx/stat.h"

/*
 * GGUF v3 parser and model loader.
 *
 * Supports all GGML quantization types via dequantization to F32.
 * Architecture-agnostic: matches metadata keys by suffix (e.g. ".embedding_length").
 * Supports GQA, QK-norm, configurable RoPE theta.
 *
 * GGUF spec: https://github.com/ggerganov/ggml/blob/master/docs/gguf.md
 */

/* GGUF constants */
#define GGUF_MAGIC       0x46554747  /* "GGUF" */
#define GGUF_VERSION     3

/* Metadata value types */
#define GGUF_TYPE_UINT32   4
#define GGUF_TYPE_INT32    5
#define GGUF_TYPE_FLOAT32  6
#define GGUF_TYPE_STRING   8
#define GGUF_TYPE_ARRAY    9
#define GGUF_TYPE_UINT64  10

/* Tensor types */
#define GGML_TYPE_F32     0

/* --- Helpers for reading from memory-mapped file --- */

typedef struct {
    const uint8_t *base;
    uint64_t size;
    uint64_t pos;
} gguf_reader_t;

static int read_bytes(gguf_reader_t *r, void *dst, uint64_t n) {
    if (r->pos + n > r->size) return -1;
    memcpy(dst, r->base + r->pos, n);
    r->pos += n;
    return 0;
}

static uint32_t read_u32(gguf_reader_t *r) {
    uint32_t v;
    read_bytes(r, &v, 4);
    return v;
}

static uint64_t read_u64(gguf_reader_t *r) {
    uint64_t v;
    read_bytes(r, &v, 8);
    return v;
}

static float read_f32(gguf_reader_t *r) {
    float v;
    read_bytes(r, &v, 4);
    return v;
}

/* Read a GGUF string (uint64_t length + chars, not null-terminated in file) */
static int read_string(gguf_reader_t *r, char *buf, uint32_t buf_size, uint64_t *out_len) {
    uint64_t len = read_u64(r);
    if (out_len) *out_len = len;
    if (len >= buf_size) {
        /* Too long — skip over and truncate */
        if (r->pos + len > r->size) return -1;
        memcpy(buf, r->base + r->pos, buf_size - 1);
        buf[buf_size - 1] = '\0';
        r->pos += len;
        return 0;
    }
    if (read_bytes(r, buf, len) != 0) return -1;
    buf[len] = '\0';
    return 0;
}

/* Skip a string without storing */
static int skip_string(gguf_reader_t *r) {
    uint64_t len = read_u64(r);
    if (r->pos + len > r->size) return -1;
    r->pos += len;
    return 0;
}

/* Skip a single metadata value */
static int skip_value(gguf_reader_t *r, uint32_t type);

static int skip_value(gguf_reader_t *r, uint32_t type) {
    switch (type) {
    case 0: r->pos += 1; break;  /* UINT8 */
    case 1: r->pos += 1; break;  /* INT8 */
    case 2: r->pos += 2; break;  /* UINT16 */
    case 3: r->pos += 2; break;  /* INT16 */
    case GGUF_TYPE_UINT32: r->pos += 4; break;
    case GGUF_TYPE_INT32:  r->pos += 4; break;
    case GGUF_TYPE_FLOAT32: r->pos += 4; break;
    case 7: r->pos += 1; break;  /* BOOL */
    case GGUF_TYPE_STRING: return skip_string(r);
    case GGUF_TYPE_ARRAY: {
        uint32_t elem_type = read_u32(r);
        uint64_t count = read_u64(r);
        for (uint64_t i = 0; i < count; i++) {
            if (skip_value(r, elem_type) != 0) return -1;
        }
        break;
    }
    case GGUF_TYPE_UINT64: r->pos += 8; break;
    case 11: r->pos += 8; break;  /* INT64 */
    case 12: r->pos += 8; break;  /* FLOAT64 */
    default: return -1;
    }
    return 0;
}

/* Transpose a 2D float matrix from [rows, cols] to [cols, rows] */
static void transpose_f32(float *dst, const float *src, uint32_t rows, uint32_t cols) {
    for (uint32_t r = 0; r < rows; r++)
        for (uint32_t c = 0; c < cols; c++)
            dst[c * rows + r] = src[r * cols + c];
}

/* Find first '.' in key and return pointer to it, or key itself if no dot */
static const char *key_suffix(const char *key, uint64_t key_len) {
    for (uint64_t i = 0; i < key_len; i++) {
        if (key[i] == '.') return &key[i];
    }
    return key;
}

/* --- Tensor info --- */

#define MAX_TENSORS 512

typedef struct {
    char name[128];
    uint32_t n_dims;
    uint64_t shape[4];
    uint32_t type;
    uint64_t offset;
} gguf_tensor_info_t;

/* --- Main loader --- */

int gguf_load(const char *path, transformer_t *tf, tf_config_t *cfg,
              bpe_tokenizer_t *bpe) {
    /* Open and mmap the file */
    long fd = sys_open(path, O_RDONLY);
    if (fd < 0) {
        printf("[gguf] open(%s) failed: %ld\n", path, fd);
        return -1;
    }

    /* Get file size first */
    struct linux_stat st;
    if (sys_stat(path, &st) != 0) {
        printf("[gguf] stat(%s) failed\n", path);
        sys_close(fd);
        return -1;
    }
    printf("[gguf] %s: size=%lu bytes\n", path, (unsigned long)st.st_size);

    long map_addr = sys_fmmap(fd);
    sys_close(fd);
    if (map_addr <= 0) {
        printf("[gguf] fmmap failed: %ld\n", map_addr);
        return -1;
    }
    printf("[gguf] mmap at 0x%lx\n", (unsigned long)map_addr);

    gguf_reader_t reader = { .base = (const uint8_t *)map_addr, .size = (uint64_t)st.st_size, .pos = 0 };
    gguf_reader_t *r = &reader;

    /* Parse header */
    uint32_t magic = read_u32(r);
    if (magic != GGUF_MAGIC) {
        sys_munmap((uint64_t)map_addr);
        return -1;
    }

    uint32_t version = read_u32(r);
    if (version != GGUF_VERSION) {
        sys_munmap((uint64_t)map_addr);
        return -1;
    }

    uint64_t n_tensors = read_u64(r);
    uint64_t n_kv = read_u64(r);

    /* Initialize config defaults */
    memset(cfg, 0, sizeof(*cfg));
    cfg->rope = 1;
    cfg->swiglu = 1;

    uint32_t vocab_size = 0;
    int bpe_initialized = 0;

    /* Parse metadata KV pairs — match by suffix for architecture-agnostic support */
    for (uint64_t kv = 0; kv < n_kv; kv++) {
        char key[256];
        uint64_t key_len;
        if (read_string(r, key, sizeof(key), &key_len) != 0) goto fail;

        uint32_t val_type = read_u32(r);

        /* Get suffix after first dot for architecture-agnostic matching */
        const char *suffix = key_suffix(key, key_len);

        if (strcmp(suffix, ".embedding_length") == 0 && val_type == GGUF_TYPE_UINT32) {
            cfg->dim = read_u32(r);
        } else if (strcmp(suffix, ".block_count") == 0 && val_type == GGUF_TYPE_UINT32) {
            cfg->n_layers = read_u32(r);
        } else if (strcmp(suffix, ".attention.head_count") == 0 && val_type == GGUF_TYPE_UINT32) {
            cfg->n_heads = read_u32(r);
        } else if (strcmp(suffix, ".attention.head_count_kv") == 0 && val_type == GGUF_TYPE_UINT32) {
            cfg->n_kv_heads = read_u32(r);
        } else if (strcmp(suffix, ".feed_forward_length") == 0 && val_type == GGUF_TYPE_UINT32) {
            cfg->hidden_dim = read_u32(r);
        } else if (strcmp(suffix, ".context_length") == 0 && val_type == GGUF_TYPE_UINT32) {
            cfg->max_seq_len = read_u32(r);
        } else if (strcmp(suffix, ".rope.freq_base") == 0 && val_type == GGUF_TYPE_FLOAT32) {
            cfg->rope_theta = read_f32(r);
        } else if (strcmp(key, "tokenizer.ggml.tokens") == 0 && val_type == GGUF_TYPE_ARRAY) {
            /* Array of strings — extract BPE vocab */
            uint32_t elem_type = read_u32(r);
            uint64_t count = read_u64(r);
            vocab_size = (uint32_t)count;
            cfg->vocab_size = vocab_size;

            if (elem_type != GGUF_TYPE_STRING) {
                /* Skip the array */
                for (uint64_t i = 0; i < count; i++)
                    if (skip_value(r, elem_type) != 0) goto fail;
            } else {
                /* Initialize BPE with vocab_size and a reasonable max merges.
                 * We'll set actual n_merges when we encounter the merges key. */
                if (bpe_init(bpe, vocab_size, vocab_size) != 0) goto fail;
                bpe_initialized = 1;

                for (uint64_t i = 0; i < count; i++) {
                    char tok_str[256];
                    uint64_t tok_len;
                    if (read_string(r, tok_str, sizeof(tok_str), &tok_len) != 0) goto fail;
                    bpe_set_vocab(bpe, (uint32_t)i, tok_str, (uint32_t)tok_len);
                }
            }
        } else if (strcmp(key, "tokenizer.ggml.merges") == 0 && val_type == GGUF_TYPE_ARRAY) {
            /* Array of strings: "left right" */
            uint32_t elem_type = read_u32(r);
            uint64_t count = read_u64(r);

            if (!bpe_initialized) {
                for (uint64_t i = 0; i < count; i++)
                    if (skip_value(r, elem_type) != 0) goto fail;
                continue;
            }

            bpe->n_merges = (uint32_t)count;

            if (elem_type == GGUF_TYPE_STRING) {
                /* Build a hash table for O(1) vocab string → token ID lookup.
                 * Without this, merge parsing is O(merges × vocab) = ~23 billion ops
                 * for Qwen3's 151K vocab. */
                #define VOCAB_HT_SIZE 262144  /* power of 2, ~1.7x vocab_size */
                uint32_t ht_pages = (VOCAB_HT_SIZE * sizeof(uint32_t) + PAGE_SIZE - 1) / PAGE_SIZE;
                long ht_addr = sys_mmap(ht_pages);
                uint32_t *vocab_ht = (ht_addr > 0) ? (uint32_t *)ht_addr : NULL;

                if (vocab_ht) {
                    /* Initialize hash table: 0xFFFFFFFF = empty */
                    for (uint32_t h = 0; h < VOCAB_HT_SIZE; h++)
                        vocab_ht[h] = 0xFFFFFFFF;

                    /* FNV-1a hash for strings */
                    #define FNV_OFFSET 2166136261u
                    #define FNV_PRIME  16777619u

                    /* Insert all vocab entries */
                    for (uint32_t v = 0; v < bpe->vocab_size; v++) {
                        if (!bpe->vocab[v]) continue;
                        uint32_t h = FNV_OFFSET;
                        for (uint32_t j = 0; j < bpe->vocab_len[v]; j++)
                            h = (h ^ (uint8_t)bpe->vocab[v][j]) * FNV_PRIME;
                        h &= (VOCAB_HT_SIZE - 1);
                        /* Linear probe */
                        while (vocab_ht[h] != 0xFFFFFFFF)
                            h = (h + 1) & (VOCAB_HT_SIZE - 1);
                        vocab_ht[h] = v;
                    }

                    /* Lookup helper: find token ID for a string */
                    #define VOCAB_LOOKUP(str, slen, out_id, out_found) do { \
                        uint32_t _h = FNV_OFFSET; \
                        for (uint32_t _j = 0; _j < (slen); _j++) \
                            _h = (_h ^ (uint8_t)(str)[_j]) * FNV_PRIME; \
                        _h &= (VOCAB_HT_SIZE - 1); \
                        (out_found) = 0; \
                        while (vocab_ht[_h] != 0xFFFFFFFF) { \
                            uint32_t _v = vocab_ht[_h]; \
                            if (bpe->vocab_len[_v] == (slen) && \
                                strncmp(bpe->vocab[_v], (str), (slen)) == 0) { \
                                (out_id) = _v; (out_found) = 1; break; \
                            } \
                            _h = (_h + 1) & (VOCAB_HT_SIZE - 1); \
                        } \
                    } while(0)

                    for (uint64_t i = 0; i < count; i++) {
                        char merge_str[512];
                        uint64_t merge_len;
                        if (read_string(r, merge_str, sizeof(merge_str), &merge_len) != 0) {
                            sys_munmap((uint64_t)ht_addr);
                            goto fail;
                        }

                        /* Parse "left right" */
                        char *space = (char *)0;
                        for (uint32_t s = 0; s < (uint32_t)merge_len; s++) {
                            if (merge_str[s] == ' ') { space = &merge_str[s]; break; }
                        }
                        if (!space) continue;
                        *space = '\0';
                        char *left_str = merge_str;
                        char *right_str = space + 1;
                        uint32_t left_len = (uint32_t)(space - merge_str);
                        uint32_t right_len = (uint32_t)(merge_len - left_len - 1);

                        uint32_t left_id = 0, right_id = 0, result_id = 0;
                        int found_left = 0, found_right = 0;

                        VOCAB_LOOKUP(left_str, left_len, left_id, found_left);
                        VOCAB_LOOKUP(right_str, right_len, right_id, found_right);

                        /* Result = concatenation of left + right */
                        char concat[512];
                        uint32_t concat_len = left_len + right_len;
                        memcpy(concat, left_str, left_len);
                        memcpy(concat + left_len, right_str, right_len);
                        int found_result = 0;
                        VOCAB_LOOKUP(concat, concat_len, result_id, found_result);

                        if (found_left && found_right)
                            bpe_set_merge(bpe, (uint32_t)i, left_id, right_id, result_id);
                    }

                    #undef VOCAB_LOOKUP
                    #undef FNV_OFFSET
                    #undef FNV_PRIME
                    #undef VOCAB_HT_SIZE
                    sys_munmap((uint64_t)ht_addr);
                } else {
                    /* Fallback: skip merges if hash table allocation fails */
                    for (uint64_t i = 0; i < count; i++)
                        if (skip_value(r, elem_type) != 0) goto fail;
                    bpe->n_merges = 0;
                }
            } else {
                for (uint64_t i = 0; i < count; i++)
                    if (skip_value(r, elem_type) != 0) goto fail;
            }
        } else {
            /* Skip unknown KV */
            if (skip_value(r, val_type) != 0) goto fail;
        }
    }

    /* Validate config */
    if (cfg->dim == 0 || cfg->hidden_dim == 0 || cfg->n_heads == 0 ||
        cfg->n_layers == 0 || cfg->vocab_size == 0 || cfg->max_seq_len == 0) {
        goto fail;
    }

    /* Parse tensor info */
    gguf_tensor_info_t *tensors = (gguf_tensor_info_t *)0;
    uint64_t tensors_bytes = n_tensors * sizeof(gguf_tensor_info_t);
    uint32_t tensors_pages = (uint32_t)((tensors_bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    long tensors_addr = sys_mmap(tensors_pages);
    if (tensors_addr <= 0) goto fail;
    tensors = (gguf_tensor_info_t *)tensors_addr;

    int has_qk_norm = 0;

    for (uint64_t t = 0; t < n_tensors; t++) {
        char name[128];
        uint64_t name_len;
        if (read_string(r, name, sizeof(name), &name_len) != 0) {
            sys_munmap((uint64_t)tensors_addr);
            goto fail;
        }
        memcpy(tensors[t].name, name, 128);

        /* Detect QK-norm from tensor names */
        if (strstr(name, "attn_q_norm.weight"))
            has_qk_norm = 1;

        tensors[t].n_dims = read_u32(r);
        for (uint32_t d = 0; d < tensors[t].n_dims; d++)
            tensors[t].shape[d] = read_u64(r);
        for (uint32_t d = tensors[t].n_dims; d < 4; d++)
            tensors[t].shape[d] = 1;

        tensors[t].type = read_u32(r);
        tensors[t].offset = read_u64(r);
    }

    if (has_qk_norm)
        cfg->qk_norm = 1;

    /* Tensor data starts at alignment boundary after metadata+tensor info.
     * GGUF v3 uses 32-byte alignment for tensor data. */
    uint64_t data_start = (r->pos + 31) & ~(uint64_t)31;

    /* Compute GQA dimensions */
    uint32_t head_dim = cfg->dim / cfg->n_heads;
    uint32_t kv_heads = (cfg->n_kv_heads > 0) ? cfg->n_kv_heads : cfg->n_heads;
    uint32_t kv_dim = kv_heads * head_dim;

    /* Detect if model has quantized 2D tensors */
    int has_quantized = 0;
    for (uint64_t t = 0; t < n_tensors; t++) {
        if (tensors[t].n_dims >= 2 && tensors[t].type != GGML_TYPE_F32) {
            has_quantized = 1;
            break;
        }
    }

    if (has_quantized) {
        /* === QUANTIZED PATH: zero-copy, keep GGUF mmap alive === */

        /* Cap max_seq_len for memory savings (KV cache) */
        if (cfg->max_seq_len > 512)
            cfg->max_seq_len = 512;

        if (transformer_init_quantized(tf, cfg) != 0) {
            sys_munmap((uint64_t)tensors_addr);
            goto fail;
        }

        /* Store GGUF mmap in transformer for lifetime management */
        tf->gguf_mmap_addr = (uint64_t)map_addr;
        /* (pages computed from file size) */
        tf->gguf_mmap_pages = (uint32_t)(((uint64_t)st.st_size + PAGE_SIZE - 1) / PAGE_SIZE);

        /* Small temp buffer for dequanting 1D norm weights */
        uint64_t max_norm_size = cfg->dim;
        if (head_dim > max_norm_size) max_norm_size = head_dim;
        uint32_t norm_temp_pages = (uint32_t)((max_norm_size * 4 + PAGE_SIZE - 1) / PAGE_SIZE);
        long norm_temp_addr = sys_mmap(norm_temp_pages);
        float *norm_dq = (norm_temp_addr > 0) ? (float *)norm_temp_addr : NULL;

        int found_output = 0;

        for (uint64_t t = 0; t < n_tensors; t++) {
            const uint8_t *raw = r->base + data_start + tensors[t].offset;
            const char *name = tensors[t].name;
            uint32_t ttype = tensors[t].type;
            uint32_t ndims = tensors[t].n_dims;

            /* For 2D weight tensors: store qweight_t pointer.
             * GGUF shape is [inner_dim, outer_dim] (column-first).
             * Our qweight_t uses rows=outer (output), cols=inner (input).
             * E.g. token_embd shape=[2048,151936] → rows=151936, cols=2048 */
            if (ndims >= 2) {
                qweight_t qw;
                qw.data = raw;
                qw.qtype = ttype;
                qw.rows = (uint32_t)tensors[t].shape[1];  /* outer dim = output */
                qw.cols = (uint32_t)tensors[t].shape[0];  /* inner dim = input  */

                if (strcmp(name, "token_embd.weight") == 0) {
                    tf->q_token_emb = qw;
                } else if (strcmp(name, "output.weight") == 0) {
                    tf->q_wcls = qw;
                    found_output = 1;
                } else if (strncmp(name, "blk.", 4) == 0) {
                    uint32_t layer = 0;
                    uint32_t ni = 4;
                    while (name[ni] >= '0' && name[ni] <= '9') {
                        layer = layer * 10 + (name[ni] - '0');
                        ni++;
                    }
                    if (name[ni] != '.' || layer >= cfg->n_layers) continue;
                    ni++;
                    const char *tsuffix = &name[ni];

                    if (strcmp(tsuffix, "attn_q.weight") == 0)
                        tf->qqwq[layer] = qw;
                    else if (strcmp(tsuffix, "attn_k.weight") == 0)
                        tf->qqwk[layer] = qw;
                    else if (strcmp(tsuffix, "attn_v.weight") == 0)
                        tf->qqwv[layer] = qw;
                    else if (strcmp(tsuffix, "attn_output.weight") == 0)
                        tf->qqwo[layer] = qw;
                    else if (strcmp(tsuffix, "ffn_gate.weight") == 0)
                        tf->qqw1[layer] = qw;
                    else if (strcmp(tsuffix, "ffn_down.weight") == 0)
                        tf->qqw2[layer] = qw;
                    else if (strcmp(tsuffix, "ffn_up.weight") == 0 && tf->qqw3)
                        tf->qqw3[layer] = qw;
                }
                continue;
            }

            /* For 1D norm tensors: dequantize to F32 */
            uint64_t n_elements = 1;
            for (uint32_t d = 0; d < ndims; d++)
                n_elements *= tensors[t].shape[d];

            const float *src_f32;
            if (ttype == GGML_TYPE_F32) {
                src_f32 = (const float *)raw;
            } else if (norm_dq) {
                if (dequant(raw, norm_dq, n_elements, ttype) != 0)
                    continue;
                src_f32 = norm_dq;
            } else {
                continue;
            }

            if (strcmp(name, "output_norm.weight") == 0) {
                memcpy(tf->rms_final_w, src_f32, (uint64_t)cfg->dim * 4);
            } else if (strncmp(name, "blk.", 4) == 0) {
                uint32_t layer = 0;
                uint32_t ni = 4;
                while (name[ni] >= '0' && name[ni] <= '9') {
                    layer = layer * 10 + (name[ni] - '0');
                    ni++;
                }
                if (name[ni] != '.' || layer >= cfg->n_layers) continue;
                ni++;
                const char *tsuffix = &name[ni];

                if (strcmp(tsuffix, "attn_norm.weight") == 0)
                    memcpy(tf->rms_att_w[layer], src_f32, (uint64_t)cfg->dim * 4);
                else if (strcmp(tsuffix, "ffn_norm.weight") == 0)
                    memcpy(tf->rms_ffn_w[layer], src_f32, (uint64_t)cfg->dim * 4);
                else if (strcmp(tsuffix, "attn_q_norm.weight") == 0 && tf->wq_norm)
                    memcpy(tf->wq_norm[layer], src_f32, (uint64_t)head_dim * 4);
                else if (strcmp(tsuffix, "attn_k_norm.weight") == 0 && tf->wk_norm)
                    memcpy(tf->wk_norm[layer], src_f32, (uint64_t)head_dim * 4);
            }
        }

        /* Weight tying: if output.weight missing, share with token_embd */
        if (!found_output) {
            tf->q_wcls = tf->q_token_emb;
            printf("[gguf] weight tying: output = token_embd\n");
        }

        printf("[gguf] quantized: emb=%ux%u wq=%ux%u wk=%ux%u w1=%ux%u wcls=%ux%u\n",
               tf->q_token_emb.rows, tf->q_token_emb.cols,
               tf->qqwq[0].rows, tf->qqwq[0].cols,
               tf->qqwk[0].rows, tf->qqwk[0].cols,
               tf->qqw1[0].rows, tf->qqw1[0].cols,
               tf->q_wcls.rows, tf->q_wcls.cols);

        if (norm_temp_addr > 0)
            sys_munmap((uint64_t)norm_temp_addr);
        sys_munmap((uint64_t)tensors_addr);
        /* DO NOT munmap map_addr — quantized data points into it */
        return 0;

    } else {
        /* === F32 PATH: dequantize everything (existing behavior) === */

        /* Initialize transformer */
        if (transformer_init(tf, cfg, 0) != 0) {
            sys_munmap((uint64_t)tensors_addr);
            goto fail;
        }

        /* Allocate a temporary buffer for dequantization + transpose
         * (max of all weight sizes in F32) */
        uint64_t max_weight_size = (uint64_t)cfg->dim * cfg->vocab_size;
        uint64_t tmp = (uint64_t)cfg->dim * cfg->hidden_dim;
        if (tmp > max_weight_size) max_weight_size = tmp;
        tmp = (uint64_t)cfg->dim * cfg->dim;
        if (tmp > max_weight_size) max_weight_size = tmp;
        /* Need 2x for dequant buffer + transpose buffer */
        uint32_t temp_pages = (uint32_t)((max_weight_size * 4 * 2 + PAGE_SIZE - 1) / PAGE_SIZE);
        long temp_addr = sys_mmap(temp_pages);
        if (temp_addr <= 0) {
            transformer_destroy(tf);
            sys_munmap((uint64_t)tensors_addr);
            goto fail;
        }
        float *dequant_buf = (float *)temp_addr;

        /* Copy tensor data into transformer weight buffers */
        for (uint64_t t = 0; t < n_tensors; t++) {
            const uint8_t *raw = r->base + data_start + tensors[t].offset;
            const char *name = tensors[t].name;
            uint32_t ttype = tensors[t].type;

            /* Compute element count */
            uint64_t n_elements = 1;
            for (uint32_t d = 0; d < tensors[t].n_dims; d++)
                n_elements *= tensors[t].shape[d];

            /* Dequantize to F32 if needed */
            const float *src_f32;
            if (ttype == GGML_TYPE_F32) {
                src_f32 = (const float *)raw;
            } else {
                if (dequant(raw, dequant_buf, n_elements, ttype) != 0)
                    continue;  /* skip unsupported types */
                src_f32 = dequant_buf;
            }

            /* token_embd.weight → token_emb [vocab_size, dim] — NO transpose */
            if (strcmp(name, "token_embd.weight") == 0) {
                memcpy(tf->token_emb, src_f32, (uint64_t)cfg->vocab_size * cfg->dim * 4);
                continue;
            }

            /* output.weight → wcls [vocab_size, dim] → transpose to [dim, vocab_size] */
            if (strcmp(name, "output.weight") == 0) {
                transpose_f32(tf->wcls, src_f32, cfg->vocab_size, cfg->dim);
                continue;
            }

            /* output_norm.weight → rms_final_w [dim] */
            if (strcmp(name, "output_norm.weight") == 0) {
                memcpy(tf->rms_final_w, src_f32, (uint64_t)cfg->dim * 4);
                continue;
            }

            /* Per-layer tensors: blk.N.xxx */
            if (strncmp(name, "blk.", 4) != 0) continue;

            /* Parse layer number */
            uint32_t layer = 0;
            uint32_t ni = 4;
            while (name[ni] >= '0' && name[ni] <= '9') {
                layer = layer * 10 + (name[ni] - '0');
                ni++;
            }
            if (name[ni] != '.') continue;
            ni++; /* skip '.' */
            const char *tsuffix = &name[ni];

            if (layer >= cfg->n_layers) continue;

            /* attn_norm.weight → rms_att_w[layer] [dim] */
            if (strcmp(tsuffix, "attn_norm.weight") == 0) {
                memcpy(tf->rms_att_w[layer], src_f32, (uint64_t)cfg->dim * 4);
            }
            /* ffn_norm.weight → rms_ffn_w[layer] [dim] */
            else if (strcmp(tsuffix, "ffn_norm.weight") == 0) {
                memcpy(tf->rms_ffn_w[layer], src_f32, (uint64_t)cfg->dim * 4);
            }
            /* attn_q.weight → wq[layer] [dim, dim] → transpose */
            else if (strcmp(tsuffix, "attn_q.weight") == 0) {
                transpose_f32(tf->wq[layer], src_f32, cfg->dim, cfg->dim);
            }
            /* attn_k.weight → wk[layer] [kv_dim, dim] → transpose to [dim, kv_dim] */
            else if (strcmp(tsuffix, "attn_k.weight") == 0) {
                transpose_f32(tf->wk[layer], src_f32, kv_dim, cfg->dim);
            }
            /* attn_v.weight → wv[layer] [kv_dim, dim] → transpose to [dim, kv_dim] */
            else if (strcmp(tsuffix, "attn_v.weight") == 0) {
                transpose_f32(tf->wv[layer], src_f32, kv_dim, cfg->dim);
            }
            /* attn_output.weight → wo[layer] [dim, dim] → transpose */
            else if (strcmp(tsuffix, "attn_output.weight") == 0) {
                transpose_f32(tf->wo[layer], src_f32, cfg->dim, cfg->dim);
            }
            /* ffn_gate.weight → w1[layer] [hidden_dim, dim] → transpose to [dim, hidden_dim] */
            else if (strcmp(tsuffix, "ffn_gate.weight") == 0) {
                transpose_f32(tf->w1[layer], src_f32, cfg->hidden_dim, cfg->dim);
            }
            /* ffn_down.weight → w2[layer] [dim, hidden_dim] → transpose to [hidden_dim, dim] */
            else if (strcmp(tsuffix, "ffn_down.weight") == 0) {
                transpose_f32(tf->w2[layer], src_f32, cfg->dim, cfg->hidden_dim);
            }
            /* ffn_up.weight → w3[layer] [hidden_dim, dim] → transpose to [dim, hidden_dim] */
            else if (strcmp(tsuffix, "ffn_up.weight") == 0 && tf->w3) {
                transpose_f32(tf->w3[layer], src_f32, cfg->hidden_dim, cfg->dim);
            }
            /* attn_q_norm.weight → wq_norm[layer] [head_dim] */
            else if (strcmp(tsuffix, "attn_q_norm.weight") == 0 && tf->wq_norm) {
                memcpy(tf->wq_norm[layer], src_f32, (uint64_t)head_dim * 4);
            }
            /* attn_k_norm.weight → wk_norm[layer] [head_dim] */
            else if (strcmp(tsuffix, "attn_k_norm.weight") == 0 && tf->wk_norm) {
                memcpy(tf->wk_norm[layer], src_f32, (uint64_t)head_dim * 4);
            }
        }

        /* Cleanup */
        sys_munmap((uint64_t)temp_addr);
        sys_munmap((uint64_t)tensors_addr);
        sys_munmap((uint64_t)map_addr);
        return 0;
    }

fail:
    sys_munmap((uint64_t)map_addr);
    return -1;
}
