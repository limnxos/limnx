#ifndef LIMNX_NET_H
#define LIMNX_NET_H

#include <stdint.h>

/* --- Byte-order helpers (x86 is little-endian, network is big-endian) --- */

static inline uint16_t htons(uint16_t h) {
    return (h >> 8) | (h << 8);
}

static inline uint16_t ntohs(uint16_t n) {
    return htons(n);
}

static inline uint32_t htonl(uint32_t h) {
    return ((h >> 24) & 0xFF)
         | ((h >>  8) & 0xFF00)
         | ((h <<  8) & 0xFF0000)
         | ((h << 24) & 0xFF000000);
}

static inline uint32_t ntohl(uint32_t n) {
    return htonl(n);
}

/* --- Ethernet --- */

#define ETH_ALEN    6
#define ETH_HLEN    14
#define ETH_TYPE_ARP  0x0806
#define ETH_TYPE_IPV4 0x0800

typedef struct {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t ethertype;
} __attribute__((packed)) eth_hdr_t;

/* --- ARP --- */

#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

typedef struct {
    uint16_t hw_type;
    uint16_t proto_type;
    uint8_t  hw_len;
    uint8_t  proto_len;
    uint16_t opcode;
    uint8_t  sender_mac[ETH_ALEN];
    uint32_t sender_ip;
    uint8_t  target_mac[ETH_ALEN];
    uint32_t target_ip;
} __attribute__((packed)) arp_pkt_t;

/* --- IPv4 --- */

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

typedef struct {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} __attribute__((packed)) ipv4_hdr_t;

/* --- ICMP --- */

#define ICMP_TYPE_ECHO_REPLY   0
#define ICMP_TYPE_ECHO_REQUEST 8

typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed)) icmp_hdr_t;

/* --- UDP --- */

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed)) udp_hdr_t;

/* --- TCP --- */

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  data_offset;   /* upper 4 bits = header len in 32-bit words */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
} __attribute__((packed)) tcp_hdr_t;

#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

/* --- Socket API --- */

#define MAX_SOCKETS 8
#define SOCKET_BUF_SIZE 1500

typedef struct {
    int      in_use;
    uint16_t local_port;
    /* Single-packet receive buffer */
    uint8_t  rx_buf[SOCKET_BUF_SIZE];
    uint32_t rx_len;
    uint32_t rx_src_ip;
    uint16_t rx_src_port;
    volatile int rx_ready;
} socket_t;

/* Loopback */
#define LOOPBACK_IP 0x7F000001U  /* 127.0.0.1 */

/* Public API */
void net_init(void);
void net_rx(const void *frame, uint32_t len);
uint32_t net_get_our_ip(void);

/* Socket operations (called from syscall handlers) */
int     net_socket(void);
int     net_bind(int sockfd, uint16_t port);
int     net_sendto(int sockfd, const void *buf, uint32_t len,
                   uint32_t dst_ip, uint16_t dst_port);
int     net_recvfrom(int sockfd, void *buf, uint32_t len,
                     uint32_t *src_ip, uint16_t *src_port);

/* IP-level transmit (used by TCP) */
int     net_tx_ipv4(uint32_t dst_ip, uint8_t protocol,
                     const void *payload, uint32_t payload_len);
uint16_t ip_checksum(const void *data, uint32_t len);

/* Send an ICMP echo request (for smoke test) */
int     net_send_ping(uint32_t dst_ip);

/* Check if we got a ping reply (non-blocking) */
int     net_got_ping_reply(void);

#endif
