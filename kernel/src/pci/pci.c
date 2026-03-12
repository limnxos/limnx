#define pr_fmt(fmt) "[pci]  " fmt
#include "klog.h"

#include "pci/pci.h"
#include "arch/x86_64/io.h"
#include "arch/serial.h"

static pci_device_t devices[PCI_MAX_DEVICES];
static uint32_t device_count = 0;

static uint32_t pci_config_addr(uint8_t bus, uint8_t dev, uint8_t func,
                                 uint8_t offset) {
    return (1U << 31)
         | ((uint32_t)bus  << 16)
         | ((uint32_t)dev  << 11)
         | ((uint32_t)func <<  8)
         | (offset & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDR, pci_config_addr(bus, dev, func, offset));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t dword = pci_read32(bus, dev, func, offset);
    return (uint16_t)(dword >> ((offset & 2) * 8));
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset,
                 uint32_t value) {
    outl(PCI_CONFIG_ADDR, pci_config_addr(bus, dev, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

void pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset,
                 uint16_t value) {
    uint32_t dword = pci_read32(bus, dev, func, offset);
    int shift = (offset & 2) * 8;
    dword &= ~(0xFFFF << shift);
    dword |= ((uint32_t)value << shift);
    pci_write32(bus, dev, func, offset, dword);
}

void pci_enable_bus_mastering(uint8_t bus, uint8_t dev, uint8_t func) {
    uint16_t cmd = pci_read16(bus, dev, func, 0x04);
    cmd |= (1 << 0) | (1 << 2); /* I/O space + bus master */
    pci_write16(bus, dev, func, 0x04, cmd);
}

pci_device_t *pci_find_device(uint16_t vendor_id, uint16_t device_id) {
    for (uint32_t i = 0; i < device_count; i++) {
        if (devices[i].vendor_id == vendor_id &&
            devices[i].device_id == device_id)
            return &devices[i];
    }
    return 0;
}

uint32_t pci_get_device_count(void) {
    return device_count;
}

pci_device_t *pci_get_device(uint32_t index) {
    if (index < device_count)
        return &devices[index];
    return 0;
}

void pci_init(void) {
    device_count = 0;

    pr_info("Scanning bus 0...\n");

    for (uint8_t dev = 0; dev < 32; dev++) {
        uint32_t id = pci_read32(0, dev, 0, 0x00);
        uint16_t vendor = id & 0xFFFF;
        uint16_t devid  = id >> 16;

        if (vendor == 0xFFFF)
            continue;

        if (device_count >= PCI_MAX_DEVICES)
            break;

        pci_device_t *d = &devices[device_count];
        d->bus       = 0;
        d->dev       = dev;
        d->func      = 0;
        d->vendor_id = vendor;
        d->device_id = devid;

        uint32_t class_reg = pci_read32(0, dev, 0, 0x08);
        d->class_code = (class_reg >> 24) & 0xFF;
        d->subclass   = (class_reg >> 16) & 0xFF;
        d->prog_if    = (class_reg >>  8) & 0xFF;

        uint32_t irq_reg = pci_read32(0, dev, 0, 0x3C);
        d->irq_line = irq_reg & 0xFF;

        for (int bar = 0; bar < 6; bar++)
            d->bar[bar] = pci_read32(0, dev, 0, 0x10 + bar * 4);

        pr_info("%u:%u.%u  %x:%x  class %x:%x  IRQ %u  BAR0=%x\n",
            d->bus, d->dev, d->func,
            d->vendor_id, d->device_id,
            d->class_code, d->subclass,
            d->irq_line, d->bar[0]);

        device_count++;
    }

    pr_info("Found %u devices\n", device_count);
}
