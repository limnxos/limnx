#ifndef LIMNX_LIBC_H
#define LIMNX_LIBC_H

/* --- Basic types --- */

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long      uint64_t;
typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long        int64_t;
typedef uint64_t           size_t;
typedef int64_t            ssize_t;

#define NULL ((void *)0)

/* --- Syscall wrappers (defined in syscalls.asm) --- */

long sys_write(const void *buf, unsigned long len);
long sys_yield(void);
void sys_exit(long status) __attribute__((noreturn));
long sys_open(const char *path, unsigned long flags);
long sys_read(long fd, void *buf, unsigned long len);
long sys_close(long fd);
long sys_stat(const char *path, void *stat_buf);
long sys_exec(const char *path, const char **argv);
long sys_socket(void);
long sys_bind(long sockfd, unsigned long port);
long sys_sendto(long sockfd, const void *buf, unsigned long len,
                unsigned long dst_ip, unsigned long dst_port);
long sys_recvfrom(long sockfd, void *buf, unsigned long len,
                  void *src_ip_ptr, void *src_port_ptr);
long sys_fwrite(long fd, const void *buf, unsigned long len);
long sys_create(const char *path);
long sys_unlink(const char *path);
long sys_mmap(unsigned long num_pages);
long sys_munmap(unsigned long virt_addr);
long sys_getchar(void);
long sys_waitpid(long pid);
long sys_pipe(long *rfd_ptr, long *wfd_ptr);
long sys_getpid(void);
long sys_fmmap(long fd);
long sys_readdir(const char *dir_path, unsigned long index, void *dirent_ptr);
long sys_mkdir(const char *path);
long sys_seek(long fd, long offset, int whence);
long sys_truncate(const char *path, unsigned long new_size);
long sys_chdir(const char *path);
long sys_getcwd(char *buf, unsigned long size);
long sys_fstat(long fd, void *stat_buf);
long sys_rename(const char *old_path, const char *new_path);
long sys_dup(long fd);
long sys_dup2(long oldfd, long newfd);
long sys_kill(long pid, long signal);
long sys_fcntl(long fd, long cmd, long arg);
long sys_setpgid(long pid, long pgid);
long sys_getpgid(long pid);
long sys_chmod(const char *path, long mode);
long sys_shmget(long key, long num_pages);
long sys_shmat(long shmid);
long sys_shmdt(long addr);
long sys_fork(void);
long sys_sigaction(int signum, void (*handler)(int));
long sys_sigreturn(void);
long sys_openpty(long *master_fd, long *slave_fd);
long sys_tcp_socket(void);
long sys_tcp_connect(long conn, unsigned int ip, int port);
long sys_tcp_listen(long conn, int port);
long sys_tcp_accept(long listen_conn);
long sys_tcp_send(long conn, const void *buf, long len);
long sys_tcp_recv(long conn, void *buf, long len);
long sys_tcp_close(long conn);
long sys_ioctl(long fd, long cmd, long arg);
long sys_clock_gettime(long clockid, void *timespec_ptr);
long sys_nanosleep(const void *timespec_ptr);
long sys_getenv(const char *key, char *val_buf, long val_buf_size);
long sys_setenv(const char *key, const char *value);
long sys_poll(void *fds, long nfds, long timeout_ms);
long sys_waitpid_flags(long pid, long flags);
long sys_getuid(void);
long sys_setuid(long uid);
long sys_getgid(void);
long sys_setgid(long gid);
long sys_getcap(void);
long sys_setcap(long pid, long caps);
long sys_getrlimit(long resource, void *rlimit_ptr);
long sys_setrlimit(long resource, const void *rlimit_ptr);
long sys_seccomp(unsigned long mask, long strict);
long sys_setaudit(long pid, long flags);
long sys_unix_socket(void);
long sys_unix_bind(long fd, const char *path);
long sys_unix_listen(long fd);
long sys_unix_accept(long fd);
long sys_unix_connect(const char *path);
long sys_agent_register(const char *name);
long sys_agent_lookup(const char *name, long *pid_out);
long sys_eventfd(long flags);

/* termios structures (matching kernel termios.h) */
typedef struct {
    unsigned int c_iflag;
    unsigned int c_oflag;
    unsigned int c_cflag;
    unsigned int c_lflag;
} termios_t;

typedef struct {
    unsigned short ws_row;
    unsigned short ws_col;
} winsize_t;

/* termios c_lflag bits */
#define TERMIOS_ECHO   (1 << 0)
#define TERMIOS_ICANON (1 << 1)

/* ioctl commands */
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCSPGRP  0x5410
#define TIOCGPGRP  0x5411

/* Signal numbers */
#define SIGINT   2
#define SIGKILL  9
#define SIGTERM  15
#define SIGSTOP  19
#define SIGCONT  18

/* Signal handler constants */
#define SIG_DFL  ((void (*)(int))0)
#define SIG_IGN  ((void (*)(int))1)

/* Open flags */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0x100
#define O_TRUNC   0x200
#define O_APPEND  0x400
#define O_NONBLOCK 0x800

/* fcntl commands */
#define F_GETFD  1
#define F_SETFD  2
#define F_GETFL  3
#define F_SETFL  4

/* fd flags */
#define FD_CLOEXEC  0x01

/* waitpid flags */
#define WNOHANG  1

/* errno codes */
#define EPERM    1
#define ENOENT   2
#define ESRCH    3
#define EINTR    4
#define EIO      5
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EINVAL  22
#define EMFILE  24
#define ENOSYS  38
#define EAGAIN  11
#define EADDRINUSE  98
#define ENOTCONN   107
#define ECONNREFUSED 111

extern int errno;

/* Capability bits */
#define CAP_NET_BIND  (1 << 0)
#define CAP_NET_RAW   (1 << 1)
#define CAP_KILL      (1 << 2)
#define CAP_SETUID    (1 << 3)
#define CAP_SYS_ADMIN (1 << 4)
#define CAP_EXEC      (1 << 5)
#define CAP_FS_WRITE  (1 << 6)
#define CAP_FS_READ   (1 << 7)
#define CAP_ALL       0xFF

/* Resource limit IDs */
#define RLIMIT_MEM   0
#define RLIMIT_CPU   1
#define RLIMIT_NFDS  2

/* Resource limit structure */
typedef struct {
    uint64_t current;
    uint64_t max;
} rlimit_t;

/* Audit flags */
#define AUDIT_SYSCALL  (1 << 0)
#define AUDIT_SECURITY (1 << 1)
#define AUDIT_EXEC     (1 << 2)
#define AUDIT_FILE     (1 << 3)

/* Clock IDs */
#define CLOCK_MONOTONIC 1

/* Time structures */
typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} timespec_t;

/* Poll structures */
#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010

typedef struct {
    int32_t fd;
    int16_t events;
    int16_t revents;
} pollfd_t;

/* Seek whence */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

/* --- Directory entry (matches kernel vfs_dirent_t) --- */

typedef struct dirent {
    char     name[256];
    uint8_t  type;
    uint8_t  pad[7];
    uint64_t size;
} dirent_t;

/* --- String functions --- */

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strstr(const char *haystack, const char *needle);

/* --- Stdio functions --- */

int puts(const char *s);
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* --- Math functions (float, compiled with -msse2) --- */

float fabsf(float x);
float sqrtf(float x);
float expf(float x);
float logf(float x);
float tanhf(float x);
float floorf(float x);
float ceilf(float x);
float fmaxf(float a, float b);
float fminf(float a, float b);
float sinf(float x);
float cosf(float x);
float sigmoidf(float x);

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

/* Signal numbers for user-space */
#define SIGCHLD 20

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

typedef struct transformer {
    tf_config_t cfg;

    /* Weights: 1 mmap buffer + pointer views */
    float   *weights_buf;
    uint64_t weights_mmap_addr;
    uint32_t weights_mmap_pages;

    float  *token_emb;          /* vocab_size × dim */
    float **rms_att_w;          /* [n_layers] → dim floats */
    float **wq;                 /* [n_layers] → dim×dim */
    float **wk, **wv, **wo;
    float **rms_ffn_w;          /* [n_layers] → dim */
    float **w1;                 /* [n_layers] → dim×hidden_dim */
    float **w2;                 /* [n_layers] → hidden_dim×dim */
    float **w3;                 /* [n_layers] → dim×hidden_dim (SwiGLU gate, NULL when swiglu=0) */
    float **wq_norm;            /* [n_layers] → head_dim (NULL when qk_norm=0) */
    float **wk_norm;            /* [n_layers] → head_dim (NULL when qk_norm=0) */
    float  *rms_final_w;        /* dim */
    float  *wcls;               /* dim × vocab_size */

    /* Per-layer pointer arrays: 1 mmap buffer */
    uint64_t ptrs_mmap_addr;
    uint32_t ptrs_mmap_pages;

    /* KV cache: 1 mmap buffer */
    float   *kv_buf;
    uint64_t kv_mmap_addr;
    uint32_t kv_mmap_pages;
    float   *key_cache;          /* n_layers × max_seq_len × dim */
    float   *value_cache;        /* n_layers × max_seq_len × dim */

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
void     transformer_destroy(transformer_t *tf);
float   *transformer_forward(transformer_t *tf, uint32_t token);
uint32_t transformer_generate(transformer_t *tf, uint32_t start_token,
                               uint32_t *out_tokens, uint32_t max_tokens);

/* --- Character-level tokenizer --- */

typedef struct tok_config {
    uint32_t vocab_size;
    char     chars[256];          /* vocab index → character */
    int      char_to_idx[256];    /* ASCII code → vocab index (-1 = unknown) */
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
    char **vocab;             /* vocab[i] → string */
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

/* --- Eventfd flags --- */

#define EFD_NONBLOCK   (1 << 0)
#define EFD_SEMAPHORE  (1 << 1)

/* --- Stage 37: epoll types --- */

#define EPOLL_CTL_ADD  1
#define EPOLL_CTL_MOD  2
#define EPOLL_CTL_DEL  3
#define EPOLLIN   0x001
#define EPOLLOUT  0x004
#define EPOLLERR  0x008
#define EPOLLHUP  0x010

typedef struct epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed)) epoll_event_t;

/* --- Stage 37: io_uring types --- */

#define IORING_OP_NOP       0
#define IORING_OP_READ      1
#define IORING_OP_WRITE     2
#define IORING_OP_POLL_ADD  3

typedef struct uring_sqe {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t reserved;
    int32_t  fd;
    uint64_t off;
    uint64_t addr;
    uint32_t len;
    uint32_t user_data;
} __attribute__((packed)) uring_sqe_t;

typedef struct uring_cqe {
    uint32_t user_data;
    int32_t  res;
    uint32_t flags;
    uint32_t reserved;
} __attribute__((packed)) uring_cqe_t;

/* --- Stage 37: mmap2 flags --- */

#define MMAP_DEMAND  1

/* --- Stage 37 syscalls --- */

long sys_epoll_create(long flags);
long sys_epoll_ctl(long epfd, long op, long fd, epoll_event_t *event);
long sys_epoll_wait(long epfd, epoll_event_t *events, long max_events, long timeout_ms);
long sys_swap_stat(uint32_t *stat);
long sys_infer_register(const char *name, const char *sock_path);
long sys_infer_request(const char *name, const void *req_buf, long req_len,
                       void *resp_buf, long resp_len);
long sys_uring_setup(long entries, void *params);
long sys_uring_enter(long uring_fd, void *sqe_ptr, long count, void *cqe_ptr);
long sys_mmap2(long num_pages, long flags);
long sys_token_create(long perms, long target_pid, const char *resource);
long sys_token_revoke(long token_id);
long sys_token_list(void *buf, long max_count);
long sys_ns_create(const char *name);
long sys_ns_join(long ns_id);

/* Process info (matches kernel layout: 56 bytes) */
typedef struct proc_info {
    long     pid;
    long     parent_pid;
    int      state;       /* 0=running, 1=stopped */
    unsigned short uid;
    unsigned short gid;
    char     name[32];
} proc_info_t;

long sys_procinfo(long index, proc_info_t *info);

/* Filesystem stats (matches kernel layout: 24 bytes) */
typedef struct fs_stat {
    unsigned int total_blocks;
    unsigned int free_blocks;
    unsigned int total_inodes;
    unsigned int free_inodes;
    unsigned int block_size;
    unsigned int mounted;
} fs_stat_t;

long sys_fsstat(fs_stat_t *st);

/* --- HTTP types and functions --- */

typedef struct http_request {
    char method[16];
    char path[256];
    char host[128];
    char content_type[128];
    uint32_t content_length;
    char body[2048];
    uint32_t body_len;
} http_request_t;

typedef struct http_response {
    int  status;
    char status_text[32];
    char content_type[128];
    char body[2048];
    uint32_t body_len;
} http_response_t;

typedef void (*http_handler_t)(const http_request_t *req, http_response_t *resp);

int  http_parse_request(const char *buf, uint32_t len, http_request_t *req);
int  http_format_response(const http_response_t *resp, char *buf, uint32_t max);
int  http_serve(int port, http_handler_t handler);

/* --- Tool dispatch (sandboxed execution) --- */

typedef struct tool_result {
    int      exit_status;
    uint32_t output_len;
    char     output[4096];
} tool_result_t;

int tool_dispatch(const char *path, const char **argv, long caps,
                  long cpu_ticks, const char *input, uint32_t input_len,
                  tool_result_t *result);

/* --- Dequantization (GGML quantized → F32) --- */

int dequant(const void *src, float *dst, uint64_t n, uint32_t type);
int dequant_block_info(uint32_t type, uint32_t *block_size, uint32_t *block_bytes);

/* --- GGUF model loader --- */

int gguf_load(const char *path, transformer_t *tf, tf_config_t *cfg,
              bpe_tokenizer_t *bpe);

#endif
