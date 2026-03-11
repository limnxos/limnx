#ifndef LIMNX_HTTP_H
#define LIMNX_HTTP_H

#include "libc.h"  /* for uint32_t, int, basic types */

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

#endif
