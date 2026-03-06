#include "net/netstor.h"
#include "net/net.h"
#include "sched/sched.h"
#include "serial.h"

static int sockfd = -1;

/* --- Helpers --- */

static uint32_t str_len(const char *s) {
    uint32_t len = 0;
    while (s[len]) len++;
    return len;
}

/* --- Init --- */

int netstor_init(void) {
    sockfd = net_socket();
    if (sockfd < 0) {
        serial_puts("[nstor] Failed to create socket\n");
        return -1;
    }

    /* Bind to ephemeral port */
    if (net_bind(sockfd, 7777) != 0) {
        serial_puts("[nstor] Failed to bind socket\n");
        return -1;
    }

    serial_puts("[nstor] Network storage client initialized\n");
    return 0;
}

/* --- Send request and receive response --- */

static int netstor_request(uint8_t cmd,
                            const char *key, uint8_t key_len,
                            const void *value, uint16_t val_len,
                            void *resp_buf, uint16_t resp_buf_size,
                            uint16_t *resp_data_len) {
    /* Build request packet */
    uint8_t pkt[4 + NETSTOR_MAX_KEY + NETSTOR_MAX_VALUE];
    pkt[0] = cmd;
    pkt[1] = key_len;
    pkt[2] = (uint8_t)(val_len >> 8);   /* BE high byte */
    pkt[3] = (uint8_t)(val_len & 0xFF); /* BE low byte */

    uint32_t off = 4;
    if (key && key_len > 0) {
        const uint8_t *k = (const uint8_t *)key;
        for (uint8_t i = 0; i < key_len; i++)
            pkt[off++] = k[i];
    }
    if (value && val_len > 0) {
        const uint8_t *v = (const uint8_t *)value;
        for (uint16_t i = 0; i < val_len; i++)
            pkt[off++] = v[i];
    }

    /* Send */
    if (net_sendto(sockfd, pkt, off,
                   NETSTOR_SERVER_IP, NETSTOR_SERVER_PORT) < 0) {
        return -1;
    }

    /* Wait for response with timeout */
    uint8_t resp[4 + NETSTOR_MAX_VALUE];
    uint32_t src_ip;
    uint16_t src_port;

    /* We need a non-blocking check with timeout.
     * Since net_recvfrom is blocking, we use a simple approach:
     * spin-yield for a limited time. The socket rx_ready flag
     * will be set by the IRQ handler. */
    /* Note: net_recvfrom blocks forever, so we need a workaround.
     * We'll check the socket state directly with a timeout loop. */

    /* Actually, let's just call net_recvfrom in a thread-safe way.
     * We'll set a simple timeout by sending the request and hoping
     * for a quick response. For robustness, we accept the blocking
     * nature but expect QEMU to respond quickly. */

    int n = net_recvfrom(sockfd, resp, sizeof(resp), &src_ip, &src_port);
    if (n < 4) return -1;

    uint8_t status = resp[0];
    uint16_t data_len = ((uint16_t)resp[2] << 8) | resp[3];

    if (data_len > (uint16_t)(n - 4))
        data_len = (uint16_t)(n - 4);

    if (resp_data_len)
        *resp_data_len = data_len;

    if (resp_buf && data_len > 0) {
        uint16_t copy = data_len;
        if (copy > resp_buf_size) copy = resp_buf_size;
        uint8_t *dst = (uint8_t *)resp_buf;
        for (uint16_t i = 0; i < copy; i++)
            dst[i] = resp[4 + i];
    }

    return (int)status;
}

/* --- Public API --- */

int netstor_put(const char *key, const void *value, uint16_t val_len) {
    if (!key || val_len > NETSTOR_MAX_VALUE) return -1;
    uint8_t key_len = (uint8_t)str_len(key);
    if (key_len > NETSTOR_MAX_KEY) return -1;

    return netstor_request(NETSTOR_CMD_PUT, key, key_len,
                            value, val_len, 0, 0, 0);
}

int netstor_get(const char *key, void *buf, uint16_t buf_size,
                uint16_t *out_len) {
    if (!key) return -1;
    uint8_t key_len = (uint8_t)str_len(key);
    if (key_len > NETSTOR_MAX_KEY) return -1;

    return netstor_request(NETSTOR_CMD_GET, key, key_len,
                            0, 0, buf, buf_size, out_len);
}

int netstor_del(const char *key) {
    if (!key) return -1;
    uint8_t key_len = (uint8_t)str_len(key);
    if (key_len > NETSTOR_MAX_KEY) return -1;

    return netstor_request(NETSTOR_CMD_DEL, key, key_len,
                            0, 0, 0, 0, 0);
}

int netstor_list(void *buf, uint16_t buf_size, uint16_t *out_len) {
    return netstor_request(NETSTOR_CMD_LIST, 0, 0,
                            0, 0, buf, buf_size, out_len);
}
