#ifndef LIMNX_TENSOR_H
#define LIMNX_TENSOR_H

#include "libc.h"  /* for uint32_t, float, tensor needs basic types */

/* --- Tensor types and operations --- */

typedef struct tensor {
    float   *data;
    uint32_t rows;        /* 1 for 1D vectors */
    uint32_t cols;
    uint32_t size;        /* rows * cols */
    uint64_t mmap_addr;   /* for sys_munmap */
    uint32_t mmap_pages;
} tensor_t;

/* PRNG */
uint32_t prng_next(uint32_t *state);
float    prng_float(uint32_t *state);  /* returns [-1.0, 1.0] */

/* Tensor ops */
tensor_t tensor_create(uint32_t rows, uint32_t cols);
void     tensor_destroy(tensor_t *t);
void     tensor_fill(tensor_t *t, float value);
void     tensor_fill_random(tensor_t *t, uint32_t *seed);
void     tensor_add(tensor_t *dst, const tensor_t *a, const tensor_t *b);
void     tensor_mul(tensor_t *dst, const tensor_t *a, const tensor_t *b);
void     tensor_scale(tensor_t *dst, const tensor_t *src, float scalar);
void     tensor_add_bias(tensor_t *dst, const tensor_t *src, const tensor_t *bias);
void     tensor_matmul(tensor_t *dst, const tensor_t *a, const tensor_t *b);
void     tensor_relu(tensor_t *t);
void     tensor_softmax(tensor_t *t);
uint32_t tensor_argmax(const tensor_t *t);

/* --- Vector store (semantic memory) --- */

#define VECSTORE_MAX_KEY     31
#define VECSTORE_MAX_ENTRIES 64

typedef struct vecstore_entry {
    uint8_t used;
    char    key[VECSTORE_MAX_KEY + 1];
} vecstore_entry_t;

typedef struct vecstore {
    uint32_t dim;           /* vector dimensionality */
    uint32_t capacity;
    uint32_t count;
    vecstore_entry_t entries[VECSTORE_MAX_ENTRIES];
    float   *vectors;       /* mmap'd: capacity * dim floats */
    uint64_t mmap_addr;
    uint32_t mmap_pages;
} vecstore_t;

/* Vector math helpers */
float vec_dot(const float *a, const float *b, uint32_t dim);
float vec_norm(const float *a, uint32_t dim);
float vec_cosine_sim(const float *a, const float *b, uint32_t dim);

/* Vector store operations */
int      vecstore_init(vecstore_t *vs, uint32_t dim);
void     vecstore_destroy(vecstore_t *vs);
int      vecstore_store(vecstore_t *vs, const char *key, const float *vec);
int      vecstore_query(vecstore_t *vs, const float *vec, uint32_t *out_idx, float *out_score);
int      vecstore_query_topk(vecstore_t *vs, const float *vec, uint32_t k,
                             uint32_t *out_indices, float *out_scores);
int      vecstore_get(vecstore_t *vs, const char *key, float *out_vec);
int      vecstore_delete(vecstore_t *vs, const char *key);
uint32_t vecstore_count(const vecstore_t *vs);
int      vecstore_save(vecstore_t *vs, const char *path);
int      vecstore_load(vecstore_t *vs, const char *path);

/* --- Agent runtime --- */

typedef struct agent {
    /* MLP: (input_dim * 2) -> hidden_dim -> num_actions */
    tensor_t w1;            /* (input_dim*2) x hidden_dim */
    tensor_t b1;            /* 1 x hidden_dim */
    tensor_t w2;            /* hidden_dim x num_actions */
    tensor_t b2;            /* 1 x num_actions */

    vecstore_t memory;
    uint32_t input_dim;
    uint32_t hidden_dim;
    uint32_t num_actions;
    uint32_t step_count;
    uint32_t prng_state;
} agent_t;

int  agent_init(agent_t *ag, uint32_t input_dim, uint32_t hidden_dim,
                uint32_t num_actions, uint32_t seed);
int  agent_step(agent_t *ag, const float *input, const char *label,
                uint32_t *out_action, float *out_confidence);
void agent_destroy(agent_t *ag);

/* --- Agent IPC Message Protocol --- */

#define AMSG_REQUEST    1
#define AMSG_RESPONSE   2
#define AMSG_EVENT      3
#define AMSG_HEARTBEAT  4
#define AMSG_MAX_PAYLOAD 1024

typedef struct agent_msg {
    uint32_t type;                    /* AMSG_REQUEST, AMSG_RESPONSE, etc. */
    uint32_t len;                     /* payload length */
    char     payload[AMSG_MAX_PAYLOAD];
} agent_msg_t;

int  agent_msg_send(int fd, const agent_msg_t *msg);
int  agent_msg_recv(int fd, agent_msg_t *msg);

#endif
