#ifndef LIMNX_VIRTIO_BLK_H
#define LIMNX_VIRTIO_BLK_H

#include <stdint.h>

/* Virtio PCI vendor/device for legacy block device */
#define VIRTIO_BLK_DEV_ID 0x1001

/* Virtio-blk request types */
#define VIRTIO_BLK_T_IN   0   /* read */
#define VIRTIO_BLK_T_OUT  1   /* write */

/* Virtio-blk status codes (1-byte, device-written) */
#define VIRTIO_BLK_S_OK     0
#define VIRTIO_BLK_S_IOERR  1
#define VIRTIO_BLK_S_UNSUPP 2

/* Virtio-blk request header (16 bytes, readable by device) */
typedef struct {
    uint32_t type;      /* VIRTIO_BLK_T_IN or VIRTIO_BLK_T_OUT */
    uint32_t reserved;
    uint64_t sector;    /* sector number (512 bytes each) */
} __attribute__((packed)) virtio_blk_req_t;

/* Sector size */
#define VIRTIO_BLK_SECTOR_SIZE 512

/* Public API */
int  virtio_blk_init(void);
int  virtio_blk_read(uint64_t sector, void *buf);    /* read 1 sector */
int  virtio_blk_write(uint64_t sector, const void *buf); /* write 1 sector */

#endif
