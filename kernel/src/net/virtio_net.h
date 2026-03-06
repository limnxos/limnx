#ifndef LIMNX_VIRTIO_NET_H
#define LIMNX_VIRTIO_NET_H

#include <stdint.h>

/* Virtio PCI vendor/device for legacy network */
#define VIRTIO_VENDOR_ID  0x1AF4
#define VIRTIO_NET_DEV_ID 0x1000

/* Virtio device status bits */
#define VIRTIO_STATUS_ACK       1
#define VIRTIO_STATUS_DRIVER    2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FAILED    128

/* Feature bits */
#define VIRTIO_NET_F_MAC (1 << 5)

/* Legacy PCI BAR0 register offsets */
#define VIRTIO_REG_DEVICE_FEATURES   0x00
#define VIRTIO_REG_GUEST_FEATURES    0x04
#define VIRTIO_REG_QUEUE_ADDR        0x08
#define VIRTIO_REG_QUEUE_SIZE        0x0C
#define VIRTIO_REG_QUEUE_SELECT      0x0E
#define VIRTIO_REG_QUEUE_NOTIFY      0x10
#define VIRTIO_REG_DEVICE_STATUS     0x12
#define VIRTIO_REG_ISR_STATUS        0x13
#define VIRTIO_REG_MAC               0x14  /* 6 bytes of MAC */

/* Virtqueue descriptor flags */
#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2

/* Virtqueue structures (packed, hardware layout) */
typedef struct {
    uint64_t addr;   /* physical address */
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) virtq_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed)) virtq_avail_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) virtq_used_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[];
} __attribute__((packed)) virtq_used_t;

/* Virtio-net header (legacy, no mergeable rx buffers) */
typedef struct {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed)) virtio_net_hdr_t;

#define VIRTIO_NET_HDR_SIZE 10

/* Public API */
int  virtio_net_init(void);
int  virtio_net_tx(const void *frame, uint32_t len);
void virtio_net_get_mac(uint8_t mac[6]);

#endif
