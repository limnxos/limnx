/*
 * inferd.c — Inference daemon for Limnx
 *
 * Listens on a unix socket, receives prompts, returns generated text.
 * Registers with the inference service registry so other processes
 * can route requests via sys_infer_request.
 */

#include "libc/libc.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    puts("[inferd] Starting inference daemon...\n");

    /* Create and bind unix socket */
    long sock_fd = sys_unix_socket();
    if (sock_fd < 0) {
        puts("[inferd] Failed to create unix socket\n");
        return 1;
    }

    long ret = sys_unix_bind(sock_fd, "/tmp/inferd.sock");
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
    ret = sys_infer_register("default", "/tmp/inferd.sock");
    if (ret < 0) {
        puts("[inferd] Failed to register service\n");
        sys_close(sock_fd);
        return 1;
    }

    puts("[inferd] Registered as 'default', listening on /tmp/inferd.sock\n");

    /* Serve up to 4 requests then exit (for testing) */
    for (int i = 0; i < 4; i++) {
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

    sys_close(sock_fd);
    puts("[inferd] Daemon exiting\n");
    return 0;
}
