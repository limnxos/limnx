/*
 * inferd_proxy.c — Network inference proxy for Limnx
 *
 * Registers as an inference service, accepts requests via unix socket,
 * forwards them over TCP to a remote GPU/cloud inference server,
 * and returns the response.
 *
 * Usage: inferd_proxy <host_ip> <port> [svc_name] [sock_path]
 *
 * The remote server protocol:
 *   Send: [uint32_t prompt_len][prompt bytes]
 *   Recv: [uint32_t resp_len][response bytes]
 *
 * Host-side: run tools/gpu_inference_server.py on a machine with a GPU.
 */

#include "libc/libc.h"

#define MAX_PROMPT  2048
#define MAX_RESP    4096

static uint32_t remote_ip;
static uint16_t remote_port;

/* Parse "A.B.C.D" → uint32_t IP */
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

/* Forward a prompt to the remote server via TCP */
static int forward_request(const char *prompt, uint32_t prompt_len,
                           char *response, uint32_t max_resp) {
    /* Connect to remote server */
    long conn = sys_tcp_socket();
    if (conn < 0) {
        printf("[proxy] Failed to create TCP socket\n");
        return -1;
    }

    if (sys_tcp_connect(conn, remote_ip, remote_port) < 0) {
        printf("[proxy] Failed to connect to %lx:%u\n",
               (unsigned long)remote_ip, remote_port);
        sys_tcp_close(conn);
        return -1;
    }

    /* Send: [uint32_t len][prompt bytes] */
    uint32_t net_len = prompt_len;
    sys_tcp_send(conn, &net_len, 4);
    sys_tcp_send(conn, prompt, prompt_len);

    /* Recv: [uint32_t len][response bytes] */
    uint32_t resp_len = 0;
    long n = sys_tcp_recv(conn, &resp_len, 4);
    if (n < 4 || resp_len == 0) {
        sys_tcp_close(conn);
        return -1;
    }

    if (resp_len > max_resp)
        resp_len = max_resp;

    uint32_t total = 0;
    while (total < resp_len) {
        n = sys_tcp_recv(conn, response + total, resp_len - total);
        if (n <= 0) break;
        total += (uint32_t)n;
    }

    sys_tcp_close(conn);
    return (int)total;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: inferd_proxy <host_ip> <port> [svc_name] [sock_path]\n");
        return 1;
    }

    remote_ip   = parse_ip(argv[1]);
    remote_port = (uint16_t)atoi(argv[2]);
    const char *svc_name  = (argc >= 4) ? argv[3] : "default";
    const char *sock_path = (argc >= 5) ? argv[4] : "/tmp/inferd_proxy.sock";

    printf("[proxy] Starting: remote=%s:%s svc=%s sock=%s\n",
           argv[1], argv[2], svc_name, sock_path);

    /* Create and bind unix socket */
    long sock_fd = sys_unix_socket();
    if (sock_fd < 0) { printf("[proxy] Failed to create socket\n"); return 1; }

    if (sys_unix_bind(sock_fd, sock_path) < 0) {
        printf("[proxy] Failed to bind %s\n", sock_path);
        sys_close(sock_fd);
        return 1;
    }

    if (sys_unix_listen(sock_fd) < 0) {
        printf("[proxy] Failed to listen\n");
        sys_close(sock_fd);
        return 1;
    }

    /* Register with kernel inference service */
    long ret = sys_infer_register(svc_name, sock_path);
    if (ret < 0) {
        printf("[proxy] Failed to register service '%s'\n", svc_name);
        sys_close(sock_fd);
        return 1;
    }

    printf("[proxy] Ready — forwarding '%s' to %s:%u\n",
           svc_name, argv[1], remote_port);

    sys_infer_health(0);

    /* Serve requests */
    int served = 0;
    for (;;) {
        sys_infer_health((long)served);

        long client_fd = sys_unix_accept(sock_fd);
        if (client_fd < 0) {
            sys_yield();
            continue;
        }

        /* Read prompt from local client */
        char req_buf[MAX_PROMPT];
        long n = sys_read(client_fd, req_buf, sizeof(req_buf) - 1);
        if (n > 0) {
            req_buf[n] = '\0';
            printf("[proxy] Request #%d: %ld bytes\n", served + 1, n);

            /* Forward to remote GPU server */
            char response[MAX_RESP];
            int rlen = forward_request(req_buf, (uint32_t)n,
                                       response, sizeof(response) - 1);

            if (rlen > 0) {
                response[rlen] = '\0';
                sys_fwrite(client_fd, response, rlen);
                printf("[proxy] Response: %d bytes\n", rlen);
            } else {
                const char *err = "(remote server error)";
                sys_fwrite(client_fd, err, strlen(err));
                printf("[proxy] Remote server failed\n");
            }
        }

        sys_close(client_fd);
        served++;
    }

    sys_close(sock_fd);
    return 0;
}
