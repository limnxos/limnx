#include "libc.h"

/*
 * Character-level tokenizer.
 * Vocab = printable ASCII (32-126) + newline = 96 tokens.
 */

void tok_default_config(tok_config_t *cfg) {
    /* Build vocab: printable ASCII 32-126 then newline */
    uint32_t idx = 0;

    for (int c = 32; c <= 126; c++) {
        cfg->chars[idx] = (char)c;
        idx++;
    }
    cfg->chars[idx] = '\n';
    idx++;

    cfg->vocab_size = idx;  /* 96 */

    /* Build reverse map */
    for (int i = 0; i < 256; i++)
        cfg->char_to_idx[i] = -1;

    for (uint32_t i = 0; i < cfg->vocab_size; i++)
        cfg->char_to_idx[(unsigned char)cfg->chars[i]] = (int)i;
}

uint32_t tok_encode(const tok_config_t *cfg, const char *text, uint32_t text_len,
                    uint32_t *tokens, uint32_t max_tokens) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < text_len && count < max_tokens; i++) {
        int idx = cfg->char_to_idx[(unsigned char)text[i]];
        if (idx >= 0) {
            tokens[count] = (uint32_t)idx;
            count++;
        }
        /* Skip unknown characters */
    }
    return count;
}

uint32_t tok_decode(const tok_config_t *cfg, const uint32_t *tokens, uint32_t n_tokens,
                    char *out, uint32_t max_out) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < n_tokens && count < max_out - 1; i++) {
        if (tokens[i] < cfg->vocab_size) {
            out[count] = cfg->chars[tokens[i]];
            count++;
        }
    }
    out[count] = '\0';
    return count;
}

/* --- BPE tokenizer --- */

int bpe_init(bpe_tokenizer_t *bpe, uint32_t vocab_size, uint32_t n_merges) {
    bpe->vocab_size = vocab_size;
    bpe->n_merges = n_merges;

    /* Compute total memory needed:
     *   vocab pointers: vocab_size * sizeof(char *)
     *   vocab lengths:  vocab_size * sizeof(uint32_t)
     *   merges:         n_merges * sizeof(bpe_merge_t)
     *   string pool:    vocab_size * 32 bytes (generous average)
     */
    uint64_t ptrs_bytes = (uint64_t)vocab_size * sizeof(char *);
    uint64_t lens_bytes = (uint64_t)vocab_size * sizeof(uint32_t);
    uint64_t merge_bytes = (uint64_t)n_merges * sizeof(bpe_merge_t);
    uint64_t pool_bytes = (uint64_t)vocab_size * 32;
    uint64_t total = ptrs_bytes + lens_bytes + merge_bytes + pool_bytes;

    bpe->mmap_pages = (uint32_t)((total + PAGE_SIZE - 1) / PAGE_SIZE);
    long addr = sys_mmap(bpe->mmap_pages);
    if (addr <= 0) return -1;
    bpe->mmap_addr = (uint64_t)addr;

    uint8_t *p = (uint8_t *)addr;
    bpe->vocab = (char **)p;           p += ptrs_bytes;
    bpe->vocab_len = (uint32_t *)p;    p += lens_bytes;
    bpe->merges = (bpe_merge_t *)p;    p += merge_bytes;
    bpe->pool = (char *)p;
    bpe->pool_size = (uint32_t)pool_bytes;
    bpe->pool_used = 0;

    memset(bpe->vocab, 0, ptrs_bytes);
    memset(bpe->vocab_len, 0, lens_bytes);

    return 0;
}

void bpe_destroy(bpe_tokenizer_t *bpe) {
    if (bpe->mmap_addr) {
        sys_munmap(bpe->mmap_addr);
        bpe->mmap_addr = 0;
    }
}

void bpe_set_vocab(bpe_tokenizer_t *bpe, uint32_t idx, const char *str, uint32_t len) {
    if (idx >= bpe->vocab_size) return;
    if (bpe->pool_used + len + 1 > bpe->pool_size) return;

    char *dst = bpe->pool + bpe->pool_used;
    memcpy(dst, str, len);
    dst[len] = '\0';
    bpe->vocab[idx] = dst;
    bpe->vocab_len[idx] = len;
    bpe->pool_used += len + 1;
}

void bpe_set_merge(bpe_tokenizer_t *bpe, uint32_t idx,
                   uint32_t left, uint32_t right, uint32_t result) {
    if (idx >= bpe->n_merges) return;
    bpe->merges[idx].left = left;
    bpe->merges[idx].right = right;
    bpe->merges[idx].result = result;
}

uint32_t bpe_encode(bpe_tokenizer_t *bpe, const char *text, uint32_t len,
                    uint32_t *tokens, uint32_t max_tokens) {
    /* Step 1: byte-split — each byte becomes a token ID (0-255) */
    uint32_t n = 0;
    for (uint32_t i = 0; i < len && n < max_tokens; i++) {
        tokens[n++] = (uint32_t)(uint8_t)text[i];
    }

    /* Step 2: iterative merge */
    for (uint32_t m = 0; m < bpe->n_merges; m++) {
        uint32_t left = bpe->merges[m].left;
        uint32_t right = bpe->merges[m].right;
        uint32_t result = bpe->merges[m].result;

        uint32_t i = 0;
        while (i + 1 < n) {
            if (tokens[i] == left && tokens[i + 1] == right) {
                tokens[i] = result;
                /* Shift remaining tokens left */
                for (uint32_t j = i + 1; j + 1 < n; j++)
                    tokens[j] = tokens[j + 1];
                n--;
                /* Don't increment i — check for consecutive merges */
            } else {
                i++;
            }
        }
    }

    return n;
}

uint32_t bpe_decode(bpe_tokenizer_t *bpe, const uint32_t *tokens, uint32_t n_tokens,
                    char *out, uint32_t max_out) {
    uint32_t pos = 0;
    for (uint32_t i = 0; i < n_tokens; i++) {
        uint32_t tid = tokens[i];
        if (tid >= bpe->vocab_size) continue;
        uint32_t vlen = bpe->vocab_len[tid];
        if (pos + vlen >= max_out) break;
        memcpy(out + pos, bpe->vocab[tid], vlen);
        pos += vlen;
    }
    out[pos] = '\0';
    return pos;
}
