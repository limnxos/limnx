#define pr_fmt(fmt) "[dtb]  " fmt
#include "klog.h"

#include "dtb/dtb.h"

/* FDT header constants */
#define FDT_MAGIC       0xd00dfeed
#define FDT_BEGIN_NODE  0x00000001
#define FDT_END_NODE    0x00000002
#define FDT_PROP        0x00000003
#define FDT_NOP         0x00000004
#define FDT_END         0x00000009

/* FDT header structure (big-endian) */
typedef struct {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
} fdt_header_t;

/* Big-endian to little-endian conversion */
static inline uint32_t fdt32(uint32_t be) {
    return ((be >> 24) & 0xFF) |
           ((be >> 8)  & 0xFF00) |
           ((be << 8)  & 0xFF0000) |
           ((be << 24) & 0xFF000000);
}

static inline uint64_t fdt64(const uint8_t *p) {
    return ((uint64_t)fdt32(*(uint32_t *)p) << 32) |
           (uint64_t)fdt32(*(uint32_t *)(p + 4));
}

/* Simple string comparison */
static int str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* Check if string starts with prefix */
static int str_startswith(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s != *prefix) return 0;
        s++; prefix++;
    }
    return 1;
}

/* String length */
static int str_length(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* Align up to 4-byte boundary */
static inline uint32_t align4(uint32_t v) {
    return (v + 3) & ~3;
}

/* Cached platform info */
static dtb_platform_info_t platform;

/* DTB pointers (set during init) */
static const uint8_t *dt_struct;
static const char    *dt_strings;
static uint32_t       dt_struct_size;

/*
 * Walk the FDT structure block.
 * For each node, calls back with node name and its properties.
 * We use a simple flat walk to find nodes by name prefix matching.
 */

/* Find a property value within a node starting at offset.
 * Returns pointer to property data and sets *out_len, or NULL if not found.
 * Stops at FDT_END_NODE for the current node. */
static const uint8_t *find_prop_in_node(uint32_t node_start,
                                         const char *prop_name,
                                         uint32_t *out_len) {
    uint32_t off = node_start;
    int depth = 1;  /* already inside target node (consumed by find_node) */

    while (off < dt_struct_size) {
        uint32_t token = fdt32(*(uint32_t *)(dt_struct + off));
        off += 4;

        switch (token) {
        case FDT_BEGIN_NODE:
            if (depth > 0) {
                /* Skip nested node name */
                int nlen = str_length((const char *)(dt_struct + off));
                off += align4(nlen + 1);
                depth++;
            } else {
                /* We're already inside the target node, skip name */
                depth++;
            }
            break;

        case FDT_END_NODE:
            depth--;
            if (depth <= 0)
                return NULL;  /* End of our node, property not found */
            break;

        case FDT_PROP: {
            uint32_t len = fdt32(*(uint32_t *)(dt_struct + off));
            off += 4;
            uint32_t nameoff = fdt32(*(uint32_t *)(dt_struct + off));
            off += 4;
            const char *name = dt_strings + nameoff;
            const uint8_t *data = dt_struct + off;
            off += align4(len);

            if (depth == 1 && str_eq(name, prop_name)) {
                *out_len = len;
                return data;
            }
            break;
        }

        case FDT_NOP:
            break;

        case FDT_END:
            return NULL;

        default:
            return NULL;
        }
    }
    return NULL;
}

/*
 * Find a node by name prefix in the FDT structure block.
 * Returns the offset AFTER the node name (start of properties/children).
 * Returns 0 on failure.
 */
static uint32_t find_node(const char *name_prefix) {
    uint32_t off = 0;
    int depth = 0;

    while (off < dt_struct_size) {
        uint32_t token = fdt32(*(uint32_t *)(dt_struct + off));
        off += 4;

        switch (token) {
        case FDT_BEGIN_NODE: {
            const char *nodename = (const char *)(dt_struct + off);
            int nlen = str_length(nodename);
            off += align4(nlen + 1);
            depth++;

            if (depth == 2 && str_startswith(nodename, name_prefix))
                return off;  /* Found — return offset after name */
            break;
        }

        case FDT_END_NODE:
            depth--;
            break;

        case FDT_PROP: {
            uint32_t len = fdt32(*(uint32_t *)(dt_struct + off));
            off += 4 + 4;  /* skip len + nameoff */
            off += align4(len);
            break;
        }

        case FDT_NOP:
            break;

        case FDT_END:
            return 0;

        default:
            return 0;
        }
    }
    return 0;
}

/*
 * Count nodes matching a name prefix at depth 2.
 * Also records the lowest base address from the node name (e.g., "virtio_mmio@a000000").
 */
static uint32_t count_nodes(const char *name_prefix, uint64_t *lowest_base) {
    uint32_t off = 0;
    int depth = 0;
    uint32_t count = 0;
    *lowest_base = ~0ULL;

    while (off < dt_struct_size) {
        uint32_t token = fdt32(*(uint32_t *)(dt_struct + off));
        off += 4;

        switch (token) {
        case FDT_BEGIN_NODE: {
            const char *nodename = (const char *)(dt_struct + off);
            int nlen = str_length(nodename);
            off += align4(nlen + 1);
            depth++;

            if (depth == 2 && str_startswith(nodename, name_prefix)) {
                count++;
                /* Parse hex address from "name@HEXADDR" */
                const char *at = nodename;
                while (*at && *at != '@') at++;
                if (*at == '@') {
                    at++;
                    uint64_t addr = 0;
                    while (*at) {
                        if (*at >= '0' && *at <= '9')
                            addr = (addr << 4) | (*at - '0');
                        else if (*at >= 'a' && *at <= 'f')
                            addr = (addr << 4) | (*at - 'a' + 10);
                        else
                            break;
                        at++;
                    }
                    if (addr < *lowest_base)
                        *lowest_base = addr;
                }
            }
            break;
        }

        case FDT_END_NODE:
            depth--;
            break;

        case FDT_PROP: {
            uint32_t len = fdt32(*(uint32_t *)(dt_struct + off));
            off += 4 + 4;
            off += align4(len);
            break;
        }

        case FDT_NOP:
            break;

        case FDT_END:
            return count;

        default:
            return count;
        }
    }
    return count;
}

int dtb_init(void *dtb_ptr) {
    if (!dtb_ptr) {
        pr_err("NULL DTB pointer\n");
        return -1;
    }

    const fdt_header_t *hdr = (const fdt_header_t *)dtb_ptr;
    uint32_t magic = fdt32(hdr->magic);
    if (magic != FDT_MAGIC) {
        pr_err("bad magic: %lx (expected %lx)\n",
               (unsigned long)magic, (unsigned long)FDT_MAGIC);
        return -1;
    }

    uint32_t version = fdt32(hdr->version);
    uint32_t totalsize = fdt32(hdr->totalsize);
    pr_info("DTB at %p, version %u, size %u bytes\n",
            dtb_ptr, version, totalsize);

    /* Set up structure and strings block pointers */
    dt_struct = (const uint8_t *)dtb_ptr + fdt32(hdr->off_dt_struct);
    dt_strings = (const char *)dtb_ptr + fdt32(hdr->off_dt_strings);
    dt_struct_size = fdt32(hdr->size_dt_struct);

    /* Initialize platform with defaults (fallback values) */
    platform.ram_base = 0x40000000ULL;
    platform.ram_size = 256ULL * 1024 * 1024;
    platform.gic_dist_base = 0x08000000ULL;
    platform.gic_cpu_base = 0x08010000ULL;
    platform.uart_base = 0x09000000ULL;
    platform.virtio_mmio_base = 0x0A000000ULL;
    platform.virtio_mmio_num_slots = 32;
    platform.virtio_mmio_slot_size = 0x200;
    platform.valid = 0;

    /* Parse /memory node */
    {
        uint32_t node_off = find_node("memory");
        if (node_off) {
            uint32_t len = 0;
            const uint8_t *reg = find_prop_in_node(node_off, "reg", &len);
            if (reg && len >= 16) {
                platform.ram_base = fdt64(reg);
                platform.ram_size = fdt64(reg + 8);
                pr_info("memory: base=%lx size=%luMB\n",
                        (unsigned long)platform.ram_base,
                        (unsigned long)(platform.ram_size / (1024 * 1024)));
            }
        }
    }

    /* Parse GIC (interrupt controller) node */
    {
        uint32_t node_off = find_node("intc");
        if (node_off) {
            uint32_t len = 0;
            const uint8_t *reg = find_prop_in_node(node_off, "reg", &len);
            if (reg && len >= 32) {
                /* GIC reg: <dist_base dist_size cpu_base cpu_size> */
                platform.gic_dist_base = fdt64(reg);
                platform.gic_cpu_base = fdt64(reg + 16);
                pr_info("GIC: dist=%lx cpu=%lx\n",
                        (unsigned long)platform.gic_dist_base,
                        (unsigned long)platform.gic_cpu_base);
            }
        }
    }

    /* Parse PL011 UART node */
    {
        uint32_t node_off = find_node("pl011");
        if (!node_off)
            node_off = find_node("uart");
        if (node_off) {
            uint32_t len = 0;
            const uint8_t *reg = find_prop_in_node(node_off, "reg", &len);
            if (reg && len >= 16) {
                platform.uart_base = fdt64(reg);
                pr_info("UART: base=%lx\n",
                        (unsigned long)platform.uart_base);
            }
        }
    }

    /* Count virtio-mmio nodes and find lowest base address */
    {
        uint64_t lowest = 0;
        uint32_t count = count_nodes("virtio_mmio", &lowest);
        if (count > 0) {
            platform.virtio_mmio_base = lowest;
            platform.virtio_mmio_num_slots = count;
            pr_info("virtio-mmio: %u slots, base=%lx\n",
                    count, (unsigned long)lowest);
        }
    }

    platform.valid = 1;
    pr_info("platform info initialized\n");
    return 0;
}

const dtb_platform_info_t *dtb_get_platform(void) {
    return &platform;
}
