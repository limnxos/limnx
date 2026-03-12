#define pr_fmt(fmt) "[tar] " fmt
#include "klog.h"

#include "fs/tar.h"
#include "fs/vfs.h"
#include "arch/serial.h"

/* Parse an octal ASCII string (no libc strtol) */
static uint64_t oct_to_u64(const char *s, int len) {
    uint64_t val = 0;
    for (int i = 0; i < len && s[i] >= '0' && s[i] <= '7'; i++)
        val = (val << 3) | (uint64_t)(s[i] - '0');
    return val;
}

/* Check if a 512-byte block is all zeros (end-of-archive marker) */
static int block_is_zero(const uint8_t *block) {
    for (int i = 0; i < 512; i++) {
        if (block[i] != 0)
            return 0;
    }
    return 1;
}

static uint64_t str_len(const char *s) {
    uint64_t len = 0;
    while (s[len]) len++;
    return len;
}

/* Extract basename from a path (e.g., "./hello.txt" -> "hello.txt") */
static const char *extract_basename(const char *name) {
    /* Strip leading "./" if present */
    uint64_t nlen = str_len(name);
    if (nlen >= 2 && name[0] == '.' && name[1] == '/')
        name += 2;

    /* Find last '/' */
    const char *base = name;
    for (const char *p = name; *p; p++) {
        if (*p == '/' && *(p + 1) != '\0')
            base = p + 1;
    }
    return base;
}

int tar_init(const void *archive, uint64_t size) {
    const uint8_t *ptr = (const uint8_t *)archive;
    const uint8_t *end = ptr + size;
    int file_count = 0;

    while (ptr + 512 <= end) {
        if (block_is_zero(ptr))
            break;

        const tar_header_t *hdr = (const tar_header_t *)ptr;

        /* Parse file size from octal */
        uint64_t file_size = oct_to_u64(hdr->size, 12);

        /* Skip directories and other non-regular entries */
        if (hdr->typeflag == '0' || hdr->typeflag == '\0') {
            const char *basename = extract_basename(hdr->name);

            const uint8_t *data = ptr + 512;
            if (data + file_size > end)
                break;

            pr_info("Found file: %s (%lu bytes)\n",
                          basename, file_size);
            /* Register under root (node 0) with basename only */
            int idx = vfs_register_node(0, basename, VFS_FILE, file_size,
                              (uint8_t *)data);
            /* Apply file mode from TAR header (octal, lower 9 bits) */
            if (idx >= 0) {
                uint64_t tar_mode = oct_to_u64(hdr->mode, 8);
                uint16_t unix_mode = (uint16_t)(tar_mode & 0x1FF);
                if (unix_mode == 0) unix_mode = 0755;  /* fallback */
                vfs_node_t *n = vfs_get_node(idx);
                if (n) n->mode = unix_mode;
            }
            file_count++;
        }

        /* Advance past header + data blocks (padded to 512) */
        uint64_t data_blocks = (file_size + 511) / 512;
        ptr += 512 + data_blocks * 512;
    }

    pr_info("Parsed %d files from initrd\n", file_count);
    return file_count;
}
