/*
 * inferd.c — Inference daemon for Limnx
 *
 * Listens on a unix socket, receives prompts, returns generated text.
 * Registers with the inference service registry so other processes
 * can route requests via sys_infer_request.
 * Reports health heartbeats for load balancing.
 */

#include "libc/libc.h"

int main(int argc, char **argv) {
    /* Determine service name and socket path from argv, or use defaults */
    const char *svc_name = "default";
    const char *sock_path = "/tmp/inferd.sock";
    int max_requests = 4;  /* default: serve 4 then exit */
    int crash_mode = 0;    /* if 1, exit with status 1 (simulates crash) */

    if (argc >= 2) svc_name = argv[1];
    if (argc >= 3) sock_path = argv[2];
    if (argc >= 4) {
        /* Parse max_requests from argv[3] */
        max_requests = 0;
        for (int i = 0; argv[3][i]; i++)
            max_requests = max_requests * 10 + (argv[3][i] - '0');
    }
    if (argc >= 5 && argv[4][0] == 'c') {
        crash_mode = 1;  /* argv[4] == "crash" → exit with 1 */
    }

    printf("[inferd] Starting inference daemon '%s'...\n", svc_name);

    /* Create and bind unix socket */
    long sock_fd = sys_unix_socket();
    if (sock_fd < 0) {
        puts("[inferd] Failed to create unix socket\n");
        return 1;
    }

    long ret = sys_unix_bind(sock_fd, sock_path);
    if (ret < 0) {
        puts("[inferd] Failed to bind socket\n");
        sys_close(sock_fd);
        return 1;
    }

    ret = sys_unix_listen(sock_fd);
    if (ret < 0) {
        puts("[inferd] Failed to listen\n");
        sys_close(sock_fd);
        return 1;
    }

    /* Register with inference service registry */
    ret = sys_infer_register(svc_name, sock_path);
    if (ret < 0) {
        puts("[inferd] Failed to register service\n");
        sys_close(sock_fd);
        return 1;
    }

    printf("[inferd] Registered as '%s', listening on %s\n", svc_name, sock_path);

    /* Report initial health (load=0) */
    sys_infer_health(0);

    /* Serve requests */
    for (int i = 0; i < max_requests; i++) {
        /* Report health before each accept (load = pending request count) */
        sys_infer_health((long)i);

        long client_fd = sys_unix_accept(sock_fd);
        if (client_fd < 0) {
            puts("[inferd] Accept failed\n");
            break;
        }

        /* Read request */
        char req_buf[256];
        long n = sys_read(client_fd, req_buf, sizeof(req_buf) - 1);
        if (n > 0) {
            req_buf[n] = '\0';
            printf("[inferd] Got request: %s\n", req_buf);

            /* Generate a simple response */
            const char *response = "Hello from inferd! This is a generated response.";
            int rlen = 0;
            while (response[rlen]) rlen++;
            sys_fwrite(client_fd, response, rlen);
        }

        sys_close(client_fd);
    }

    /* Report final health (high load = shutting down) */
    sys_infer_health(9999);

    sys_close(sock_fd);
    if (crash_mode) {
        printf("[inferd] Daemon '%s' crashing (simulated)\n", svc_name);
        return 1;  /* non-zero exit triggers supervisor restart */
    }
    printf("[inferd] Daemon '%s' exiting cleanly\n", svc_name);
    return 0;
}
