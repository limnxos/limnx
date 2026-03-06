#ifndef LIMNX_PCI_H
#define LIMNX_PCI_H

#include <stdint.h>

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define PCI_MAX_DEVICES 32

typedef struct {
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  irq_line;
    uint32_t bar[6];
} pci_device_t;

void     pci_init(void);
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset,
                     uint32_t value);
void     pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset,
                     uint16_t value);
void     pci_enable_bus_mastering(uint8_t bus, uint8_t dev, uint8_t func);

/* Find device by vendor+device ID. Returns pointer or NULL. */
pci_device_t *pci_find_device(uint16_t vendor_id, uint16_t device_id);

/* Access the scanned device list */
uint32_t      pci_get_device_count(void);
pci_device_t *pci_get_device(uint32_t index);

#endif
