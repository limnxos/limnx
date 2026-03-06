#include "libc.h"

/* --- Vector math helpers --- */

float vec_dot(const float *a, const float *b, uint32_t dim) {
    float sum = 0.0f;
    for (uint32_t i = 0; i < dim; i++)
        sum += a[i] * b[i];
    return sum;
}

float vec_norm(const float *a, uint32_t dim) {
    return sqrtf(vec_dot(a, a, dim));
}

float vec_cosine_sim(const float *a, const float *b, uint32_t dim) {
    float na = vec_norm(a, dim);
    float nb = vec_norm(b, dim);
    if (na < 1e-8f || nb < 1e-8f)
        return 0.0f;
    return vec_dot(a, b, dim) / (na * nb);
}

/* --- Vector store operations --- */

int vecstore_init(vecstore_t *vs, uint32_t dim) {
    vs->dim = dim;
    vs->capacity = VECSTORE_MAX_ENTRIES;
    vs->count = 0;
    vs->mmap_pages = (vs->capacity * dim * sizeof(float) + 4095) / 4096;

    long addr = sys_mmap(vs->mmap_pages);
    if (addr <= 0) {
        vs->vectors = NULL;
        vs->mmap_addr = 0;
        vs->mmap_pages = 0;
        return -1;
    }

    vs->mmap_addr = (uint64_t)addr;
    vs->vectors = (float *)addr;
    memset(vs->vectors, 0, vs->capacity * dim * sizeof(float));
    memset(vs->entries, 0, sizeof(vs->entries));
    return 0;
}

void vecstore_destroy(vecstore_t *vs) {
    if (vs->mmap_addr) {
        sys_munmap(vs->mmap_addr);
    }
    memset(vs, 0, sizeof(*vs));
}

int vecstore_store(vecstore_t *vs, const char *key, const float *vec) {
    /* Check if key already exists (update) */
    for (uint32_t i = 0; i < vs->capacity; i++) {
        if (vs->entries[i].used && strcmp(vs->entries[i].key, key) == 0) {
            memcpy(&vs->vectors[i * vs->dim], vec, vs->dim * sizeof(float));
            return 0;
        }
    }

    /* Find first free slot (insert) */
    for (uint32_t i = 0; i < vs->capacity; i++) {
        if (!vs->entries[i].used) {
            vs->entries[i].used = 1;
            /* Copy key (truncate if needed) */
            uint32_t klen = 0;
            while (key[klen] && klen < VECSTORE_MAX_KEY) {
                vs->entries[i].key[klen] = key[klen];
                klen++;
            }
            vs->entries[i].key[klen] = '\0';
            memcpy(&vs->vectors[i * vs->dim], vec, vs->dim * sizeof(float));
            vs->count++;
            return 0;
        }
    }

    return -1; /* full */
}

int vecstore_query(vecstore_t *vs, const float *vec, uint32_t *out_idx, float *out_score) {
    float best_score = -2.0f;
    uint32_t best_idx = 0;
    int found = 0;

    for (uint32_t i = 0; i < vs->capacity; i++) {
        if (!vs->entries[i].used)
            continue;
        float sim = vec_cosine_sim(vec, &vs->vectors[i * vs->dim], vs->dim);
        if (sim > best_score) {
            best_score = sim;
            best_idx = i;
            found = 1;
        }
    }

    if (!found)
        return -1;

    *out_idx = best_idx;
    *out_score = best_score;
    return 0;
}

int vecstore_query_topk(vecstore_t *vs, const float *vec, uint32_t k,
                        uint32_t *out_indices, float *out_scores) {
    uint32_t found = 0;

    for (uint32_t i = 0; i < vs->capacity; i++) {
        if (!vs->entries[i].used)
            continue;
        float sim = vec_cosine_sim(vec, &vs->vectors[i * vs->dim], vs->dim);

        /* Insertion sort into top-K (descending by score) */
        if (found < k || sim > out_scores[found - 1]) {
            uint32_t pos = found < k ? found : found - 1;
            /* Find insertion position */
            while (pos > 0 && sim > out_scores[pos - 1]) {
                if (pos < k) {
                    out_scores[pos] = out_scores[pos - 1];
                    out_indices[pos] = out_indices[pos - 1];
                }
                pos--;
            }
            out_scores[pos] = sim;
            out_indices[pos] = i;
            if (found < k)
                found++;
        }
    }

    return (int)found;
}

int vecstore_get(vecstore_t *vs, const char *key, float *out_vec) {
    for (uint32_t i = 0; i < vs->capacity; i++) {
        if (vs->entries[i].used && strcmp(vs->entries[i].key, key) == 0) {
            memcpy(out_vec, &vs->vectors[i * vs->dim], vs->dim * sizeof(float));
            return 0;
        }
    }
    return -1;
}

int vecstore_delete(vecstore_t *vs, const char *key) {
    for (uint32_t i = 0; i < vs->capacity; i++) {
        if (vs->entries[i].used && strcmp(vs->entries[i].key, key) == 0) {
            vs->entries[i].used = 0;
            vs->entries[i].key[0] = '\0';
            memset(&vs->vectors[i * vs->dim], 0, vs->dim * sizeof(float));
            vs->count--;
            return 0;
        }
    }
    return -1;
}

uint32_t vecstore_count(const vecstore_t *vs) {
    return vs->count;
}

/* --- Save/Load --- */

#define VECSTORE_MAGIC 0x56454353  /* "VECS" */

int vecstore_save(vecstore_t *vs, const char *path) {
    long fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;

    /* Write 12-byte header: magic + dim + count */
    uint32_t header[3];
    header[0] = VECSTORE_MAGIC;
    header[1] = vs->dim;
    header[2] = vs->count;

    long n = sys_fwrite(fd, header, sizeof(header));
    if (n != (long)sizeof(header)) {
        sys_close(fd);
        return -1;
    }

    /* Write all entry slots (including unused) */
    uint32_t entries_bytes = sizeof(vs->entries);
    n = sys_fwrite(fd, vs->entries, entries_bytes);
    if (n != (long)entries_bytes) {
        sys_close(fd);
        return -1;
    }

    /* Write all vector slots */
    uint32_t vec_bytes = vs->capacity * vs->dim * sizeof(float);
    n = sys_fwrite(fd, vs->vectors, vec_bytes);
    sys_close(fd);

    return (n == (long)vec_bytes) ? 0 : -1;
}

int vecstore_load(vecstore_t *vs, const char *path) {
    long fd = sys_open(path, 0);
    if (fd < 0) return -1;

    /* Read header */
    uint32_t header[3];
    long n = sys_read(fd, header, sizeof(header));
    if (n != (long)sizeof(header)) {
        sys_close(fd);
        return -1;
    }

    /* Validate magic and dim match */
    if (header[0] != VECSTORE_MAGIC || header[1] != vs->dim) {
        sys_close(fd);
        return -1;
    }

    /* Read entry slots */
    uint32_t entries_bytes = sizeof(vs->entries);
    n = sys_read(fd, vs->entries, entries_bytes);
    if (n != (long)entries_bytes) {
        sys_close(fd);
        return -1;
    }

    /* Read vector slots */
    uint32_t vec_bytes = vs->capacity * vs->dim * sizeof(float);
    n = sys_read(fd, vs->vectors, vec_bytes);
    sys_close(fd);

    if (n != (long)vec_bytes)
        return -1;

    vs->count = header[2];
    return 0;
}
