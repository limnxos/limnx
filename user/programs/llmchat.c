/*
 * llmchat.c — Direct chat with llama-server via TCP/HTTP
 *
 * No proxy, no kernel inference service — just raw TCP to llama-server.
 * Usage: llmchat [host_ip] [port]
 *   Default: 10.0.2.2 9200 (QEMU host gateway)
 */

#include "libc/libc.h"

#define MAX_INPUT  256
#define MAX_RESP   8192

static uint32_t server_ip;
static uint16_t server_port;

static uint32_t parse_ip(const char *s) {
    uint32_t a = 0, b = 0, c = 0, d = 0;
    int field = 0;
    uint32_t val = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] == '.') {
            if (field == 0) a = val;
            else if (field == 1) b = val;
            else if (field == 2) c = val;
            val = 0;
            field++;
        } else {
            val = val * 10 + (s[i] - '0');
        }
    }
    d = val;
    return (a << 24) | (b << 16) | (c << 8) | d;
}

/* JSON-escape a string */
static int json_escape(const char *src, int src_len, char *dst, int dst_max) {
    int di = 0;
    for (int i = 0; i < src_len && di < dst_max - 2; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') { dst[di++] = '\\'; dst[di++] = c; }
        else if (c == '\n') { dst[di++] = '\\'; dst[di++] = 'n'; }
        else dst[di++] = c;
    }
    dst[di] = '\0';
    return di;
}

/* Send prompt to llama-server, get response */
static int query_llm(const char *prompt, int prompt_len,
                      char *response, int max_resp) {
    /* Connect */
    long conn = sys_tcp_socket();
    if (conn < 0) {
        printf("[error] TCP socket failed\n");
        return -1;
    }

    printf("[connecting...]\n");
    if (sys_tcp_connect(conn, server_ip, server_port) < 0) {
        printf("[error] TCP connect failed\n");
        sys_tcp_close(conn);
        return -1;
    }
    printf("[connected]\n");

    /* Build JSON body */
    char escaped[MAX_INPUT * 2];
    json_escape(prompt, prompt_len, escaped, sizeof(escaped));

    char body[MAX_INPUT * 2 + 128];
    int blen = 0;
    /* Manual string building (no snprintf) */
    const char *p1 = "{\"prompt\":\"";
    while (*p1) body[blen++] = *p1++;
    for (int i = 0; escaped[i]; i++) body[blen++] = escaped[i];
    const char *p2 = "\",\"n_predict\":128,\"temperature\":0.7,\"stream\":false}";
    while (*p2) body[blen++] = *p2++;
    body[blen] = '\0';

    /* Build HTTP request */
    char http[MAX_INPUT * 2 + 512];
    int hlen = 0;
    const char *h = "POST /completion HTTP/1.0\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: ";
    while (*h) http[hlen++] = *h++;

    /* itoa content-length */
    char cl[16]; int ci = 0;
    int tmp = blen;
    if (tmp == 0) cl[ci++] = '0';
    else { char r[16]; int ri = 0; while (tmp > 0) { r[ri++] = '0' + tmp % 10; tmp /= 10; } while (ri > 0) cl[ci++] = r[--ri]; }
    for (int i = 0; i < ci; i++) http[hlen++] = cl[i];

    const char *h2 = "\r\n\r\n";
    while (*h2) http[hlen++] = *h2++;
    for (int i = 0; i < blen; i++) http[hlen++] = body[i];

    /* Send */
    printf("[sending %d bytes...]\n", hlen);
    sys_tcp_send(conn, http, hlen);

    /* Receive full response (HTTP/1.0 → server closes connection when done) */
    char recv_buf[MAX_RESP];
    int total = 0;
    int empty_count = 0;
    while (total < MAX_RESP - 1 && empty_count < 200) {
        long n = sys_tcp_recv(conn, recv_buf + total, MAX_RESP - 1 - total);
        if (n > 0) {
            total += (int)n;
            empty_count = 0;
        } else {
            empty_count++;
            sys_yield();
        }
    }
    recv_buf[total] = '\0';
    sys_tcp_close(conn);

    printf("[received %d bytes]\n", total);

    if (total == 0) return -1;

    /* Find HTTP body (after \r\n\r\n) */
    char *body_start = recv_buf;
    for (int i = 0; i + 3 < total; i++) {
        if (recv_buf[i] == '\r' && recv_buf[i+1] == '\n' &&
            recv_buf[i+2] == '\r' && recv_buf[i+3] == '\n') {
            body_start = &recv_buf[i + 4];
            break;
        }
    }

    /* Find "content":" or "content": " in JSON */
    const char *content = (const char *)0;
    for (char *p = body_start; *p; p++) {
        if (p[0] == 'c' && p[1] == 'o' && p[2] == 'n' && p[3] == 't' &&
            p[4] == 'e' && p[5] == 'n' && p[6] == 't' && p[7] == '"') {
            p += 8; /* skip content" */
            while (*p == ':' || *p == ' ') p++;
            if (*p == '"') {
                content = p + 1;
                break;
            }
        }
    }

    if (!content) {
        /* Couldn't find content field — show raw body */
        printf("[raw] %.200s\n", body_start);
        return -1;
    }

    /* Extract until closing quote */
    int oi = 0;
    while (*content && *content != '"' && oi < max_resp - 1) {
        if (*content == '\\' && *(content + 1)) {
            content++;
            if (*content == 'n') response[oi++] = '\n';
            else if (*content == 't') response[oi++] = '\t';
            else response[oi++] = *content;
        } else {
            response[oi++] = *content;
        }
        content++;
    }
    response[oi] = '\0';
    return oi;
}

int main(int argc, char **argv) {
    const char *ip_str = (argc >= 2) ? argv[1] : "10.0.2.2";
    const char *port_str = (argc >= 3) ? argv[2] : "9200";

    server_ip = parse_ip(ip_str);
    server_port = (uint16_t)atoi(port_str);

    printf("Limnx LLM Chat (server: %s:%s)\n", ip_str, port_str);
    printf("Type 'quit' to exit.\n\n");

    char line[MAX_INPUT];
    for (;;) {
        printf("you> ");
        int len = 0;
        /* Read line */
        while (len < MAX_INPUT - 1) {
            long n = sys_read(0, &line[len], 1);
            if (n <= 0) goto done;
            if (line[len] == '\n' || line[len] == '\r') break;
            len++;
        }
        line[len] = '\0';
        if (len == 0) continue;
        if (strcmp(line, "quit") == 0) break;

        char response[4096];
        int rlen = query_llm(line, len, response, sizeof(response));
        if (rlen > 0)
            printf("[bot] %s\n\n", response);
        else
            printf("[no response]\n\n");
    }
done:
    return 0;
}
