#ifndef LIMNX_TSS_H
#define LIMNX_TSS_H

#include <stdint.h>

#define GDT_TSS_SELECTOR 0x28

typedef struct tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed)) tss_t;

void tss_init(void);
void tss_set_rsp0(uint64_t rsp0);

#endif
