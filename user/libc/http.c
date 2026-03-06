#include "libc.h"

/* Simple string-to-int */
static int http_atoi(const char *s) {
    int n = 0;
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        s++;
    }
    return n;
}

/* Copy up to max-1 chars until delim or end */
static int copy_until(const char *src, int len, char delim, char *dst, int max) {
    int i = 0;
    while (i < len && i < max - 1 && src[i] != delim) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return i;
}

/* Find \r\n in buffer */
static int find_crlf(const char *buf, int len) {
    for (int i = 0; i < len - 1; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n')
            return i;
    }
    return -1;
}

int http_parse_request(const char *buf, uint32_t len, http_request_t *req) {
    if (!buf || !req || len < 10)
        return -1;

    memset(req, 0, sizeof(*req));

    int pos = 0;

    /* Parse method */
    int n = copy_until(buf + pos, len - pos, ' ', req->method, sizeof(req->method));
    if (n == 0) return -1;
    pos += n + 1;  /* skip space */

    /* Parse path */
    n = copy_until(buf + pos, len - pos, ' ', req->path, sizeof(req->path));
    if (n == 0) return -1;
    pos += n + 1;  /* skip space */

    /* Skip HTTP/x.x\r\n */
    int crlf = find_crlf(buf + pos, len - pos);
    if (crlf < 0) return -1;
    pos += crlf + 2;

    /* Parse headers */
    while (pos < (int)len - 1) {
        /* Empty line = end of headers */
        if (buf[pos] == '\r' && buf[pos + 1] == '\n') {
            pos += 2;
            break;
        }

        crlf = find_crlf(buf + pos, len - pos);
        if (crlf < 0) break;

        /* Check known headers */
        if (strncmp(buf + pos, "Content-Length: ", 16) == 0) {
            req->content_length = (uint32_t)http_atoi(buf + pos + 16);
        } else if (strncmp(buf + pos, "Host: ", 6) == 0) {
            copy_until(buf + pos + 6, crlf - 6, '\r', req->host, sizeof(req->host));
        } else if (strncmp(buf + pos, "Content-Type: ", 14) == 0) {
            copy_until(buf + pos + 14, crlf - 14, '\r', req->content_type, sizeof(req->content_type));
        }

        pos += crlf + 2;
    }

    /* Copy body */
    if (pos < (int)len) {
        uint32_t blen = (uint32_t)(len - pos);
        if (blen > sizeof(req->body) - 1)
            blen = sizeof(req->body) - 1;
        memcpy(req->body, buf + pos, blen);
        req->body[blen] = '\0';
        req->body_len = blen;
    }

    return 0;
}

/* Simple int to string */
static int http_itoa(int n, char *buf) {
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    char tmp[12];
    int i = 0;
    int neg = 0;
    if (n < 0) { neg = 1; n = -n; }
    while (n > 0) {
        tmp[i++] = '0' + (n % 10);
        n /= 10;
    }
    int pos = 0;
    if (neg) buf[pos++] = '-';
    for (int j = i - 1; j >= 0; j--)
        buf[pos++] = tmp[j];
    buf[pos] = '\0';
    return pos;
}

int http_format_response(const http_response_t *resp, char *buf, uint32_t max) {
    if (!resp || !buf || max < 64)
        return -1;

    int pos = 0;

    /* Status line */
    const char *prefix = "HTTP/1.0 ";
    int plen = strlen(prefix);
    memcpy(buf + pos, prefix, plen);
    pos += plen;

    char num[12];
    int nlen = http_itoa(resp->status, num);
    memcpy(buf + pos, num, nlen);
    pos += nlen;

    buf[pos++] = ' ';

    int stlen = strlen(resp->status_text);
    if (stlen > 0) {
        memcpy(buf + pos, resp->status_text, stlen);
        pos += stlen;
    }
    buf[pos++] = '\r';
    buf[pos++] = '\n';

    /* Content-Type header */
    if (resp->content_type[0]) {
        const char *ct = "Content-Type: ";
        int ctlen = strlen(ct);
        memcpy(buf + pos, ct, ctlen);
        pos += ctlen;
        int tlen = strlen(resp->content_type);
        memcpy(buf + pos, resp->content_type, tlen);
        pos += tlen;
        buf[pos++] = '\r';
        buf[pos++] = '\n';
    }

    /* Content-Length header */
    {
        const char *cl = "Content-Length: ";
        int cllen = strlen(cl);
        memcpy(buf + pos, cl, cllen);
        pos += cllen;
        nlen = http_itoa((int)resp->body_len, num);
        memcpy(buf + pos, num, nlen);
        pos += nlen;
        buf[pos++] = '\r';
        buf[pos++] = '\n';
    }

    /* End of headers */
    buf[pos++] = '\r';
    buf[pos++] = '\n';

    /* Body */
    if (resp->body_len > 0 && (uint32_t)pos + resp->body_len < max) {
        memcpy(buf + pos, resp->body, resp->body_len);
        pos += resp->body_len;
    }

    return pos;
}

int http_serve(int port, http_handler_t handler) {
    long conn = sys_tcp_socket();
    if (conn < 0) return -1;

    long ret = sys_tcp_listen(conn, port);
    if (ret < 0) {
        sys_tcp_close(conn);
        return -1;
    }

    /* Accept one connection (for testing) */
    long client = sys_tcp_accept(conn);
    if (client < 0) {
        sys_tcp_close(conn);
        return -1;
    }

    /* Receive request */
    char recv_buf[4096];
    long n = sys_tcp_recv(client, recv_buf, sizeof(recv_buf) - 1);
    if (n <= 0) {
        sys_tcp_close(client);
        sys_tcp_close(conn);
        return -1;
    }
    recv_buf[n] = '\0';

    /* Parse and handle */
    http_request_t req;
    http_response_t resp;
    memset(&resp, 0, sizeof(resp));

    if (http_parse_request(recv_buf, (uint32_t)n, &req) == 0 && handler) {
        handler(&req, &resp);
    } else {
        resp.status = 400;
        strcpy(resp.status_text, "Bad Request");
    }

    /* Format and send response */
    char send_buf[4096];
    int slen = http_format_response(&resp, send_buf, sizeof(send_buf));
    if (slen > 0) {
        sys_tcp_send(client, send_buf, slen);
    }

    sys_tcp_close(client);
    sys_tcp_close(conn);
    return 0;
}
