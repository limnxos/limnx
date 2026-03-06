#ifndef LIMNX_NETSTOR_H
#define LIMNX_NETSTOR_H

#include <stdint.h>

/*
 * Network Storage — UDP key-value client
 *
 * Protocol (binary, over UDP):
 *   Request:  [cmd:1] [key_len:1] [val_len:2 BE] [key] [value]
 *   Response: [status:1] [reserved:1] [data_len:2 BE] [data]
 *
 * Server runs on host at 10.0.2.2:9999 (QEMU gateway)
 */

/* Commands */
#define NETSTOR_CMD_PUT   1
#define NETSTOR_CMD_GET   2
#define NETSTOR_CMD_DEL   3
#define NETSTOR_CMD_LIST  4

/* Response status */
#define NETSTOR_OK         0
#define NETSTOR_NOT_FOUND  1
#define NETSTOR_ERROR      2

/* Limits */
#define NETSTOR_MAX_KEY    63
#define NETSTOR_MAX_VALUE  1400

/* Server address */
#define NETSTOR_SERVER_IP   0x0A000202U  /* 10.0.2.2 */
#define NETSTOR_SERVER_PORT 9999

/* Timeout: max yields before giving up */
#define NETSTOR_TIMEOUT     5000

/* Public API */
int  netstor_init(void);
int  netstor_put(const char *key, const void *value, uint16_t val_len);
int  netstor_get(const char *key, void *buf, uint16_t buf_size,
                 uint16_t *out_len);
int  netstor_del(const char *key);
int  netstor_list(void *buf, uint16_t buf_size, uint16_t *out_len);

#endif
