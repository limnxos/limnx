#define pr_fmt(fmt) "[limnfs] " fmt
#include "klog.h"
#include "blk/limnfs.h"
#include "blk/bcache.h"
#include "mm/kheap.h"
#include "arch/serial.h"

/* --- In-memory state --- */

typedef struct {
    int mounted;
    limnfs_super_t super;
    uint8_t *block_bitmap;        /* dynamically allocated from kheap */
    uint32_t block_bitmap_bytes;  /* actual size = total_blocks / 8 */
    uint8_t inode_bitmap[128];    /* 1024 inodes max */
} limnfs_state_t;

static limnfs_state_t lfs;

/* Static scratch buffers to avoid 4KB stack allocations.
 * Safe because LimnFS operations are not reentrant. */
static uint8_t scratch_block[LIMNFS_BLOCK_SIZE] __attribute__((aligned(16)));
static uint8_t scratch_block2[LIMNFS_BLOCK_SIZE] __attribute__((aligned(16)));
static uint8_t scratch_block3[LIMNFS_BLOCK_SIZE] __attribute__((aligned(16)));

/* --- String helpers --- */

static int lfs_str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void lfs_str_copy(char *dst, const char *src, uint32_t max) {
    uint32_t i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static uint32_t lfs_str_len(const char *s) {
    uint32_t len = 0;
    while (s[len]) len++;
    return len;
}

/* --- Bitmap helpers --- */

static int bitmap_get(const uint8_t *bitmap, uint32_t bit) {
    return (bitmap[bit / 8] >> (bit % 8)) & 1;
}

static void bitmap_set(uint8_t *bitmap, uint32_t bit) {
    bitmap[bit / 8] |= (1 << (bit % 8));
}

static void bitmap_clear(uint8_t *bitmap, uint32_t bit) {
    bitmap[bit / 8] &= ~(1 << (bit % 8));
}

/* --- Flush bitmaps to disk --- */

static int flush_block_bitmap(void) {
    /* Block bitmap may span multiple disk blocks */
    uint32_t bitmap_blocks = lfs.block_bitmap_bytes / LIMNFS_BLOCK_SIZE;
    if (bitmap_blocks == 0) bitmap_blocks = 1;
    for (uint32_t b = 0; b < bitmap_blocks; b++) {
        for (int i = 0; i < LIMNFS_BLOCK_SIZE; i++)
            scratch_block[i] = 0;
        uint32_t offset = b * LIMNFS_BLOCK_SIZE;
        uint32_t remain = lfs.block_bitmap_bytes - offset;
        if (remain > LIMNFS_BLOCK_SIZE) remain = LIMNFS_BLOCK_SIZE;
        for (uint32_t i = 0; i < remain; i++)
            scratch_block[i] = lfs.block_bitmap[offset + i];
        if (bcache_write(1 + b, scratch_block) != 0)
            return -1;
    }
    return 0;
}

static int flush_inode_bitmap(void) {
    /* Inode bitmap always 1 block (follows block bitmap) */
    uint32_t bitmap_blocks = lfs.block_bitmap_bytes / LIMNFS_BLOCK_SIZE;
    if (bitmap_blocks == 0) bitmap_blocks = 1;
    uint32_t inode_bm_block = 1 + bitmap_blocks;
    for (int i = 0; i < LIMNFS_BLOCK_SIZE; i++)
        scratch_block[i] = 0;
    for (int i = 0; i < 128; i++)
        scratch_block[i] = lfs.inode_bitmap[i];
    return bcache_write(inode_bm_block, scratch_block);
}

static int flush_superblock(void) {
    return bcache_write(0, &lfs.super);
}

/* --- Block pointer resolution --- */

static uint32_t get_file_block(limnfs_inode_t *inode, uint32_t logical_block) {
    if (logical_block < LIMNFS_MAX_DIRECT)
        return inode->direct[logical_block];

    uint32_t idx = logical_block - LIMNFS_MAX_DIRECT;
    if (idx < LIMNFS_PTRS_PER_BLOCK && inode->indirect) {
        uint32_t *indirect_ptrs = (uint32_t *)scratch_block2;
        if (bcache_read(inode->indirect, indirect_ptrs) != 0)
            return 0;
        return indirect_ptrs[idx];
    }

    /* Double indirect */
    if (idx >= LIMNFS_PTRS_PER_BLOCK && inode->double_indirect) {
        uint32_t dbl_offset = idx - LIMNFS_PTRS_PER_BLOCK;
        if (dbl_offset < LIMNFS_PTRS_PER_BLOCK * LIMNFS_PTRS_PER_BLOCK) {
            uint32_t l1_idx = dbl_offset / LIMNFS_PTRS_PER_BLOCK;
            uint32_t l2_idx = dbl_offset % LIMNFS_PTRS_PER_BLOCK;

            uint32_t *dbl_ptrs = (uint32_t *)scratch_block2;
            if (bcache_read(inode->double_indirect, dbl_ptrs) != 0)
                return 0;
            uint32_t mid_blk = dbl_ptrs[l1_idx];
            if (mid_blk == 0)
                return 0;

            uint32_t *mid_ptrs = (uint32_t *)scratch_block2;
            if (bcache_read(mid_blk, mid_ptrs) != 0)
                return 0;
            return mid_ptrs[l2_idx];
        }
    }

    /* Triple indirect */
    {
        uint32_t triple_base = LIMNFS_MAX_DIRECT + LIMNFS_PTRS_PER_BLOCK +
                               LIMNFS_PTRS_PER_BLOCK * LIMNFS_PTRS_PER_BLOCK;
        if (logical_block >= triple_base && inode->triple_indirect) {
            uint32_t tri_offset = logical_block - triple_base;
            uint32_t l1 = tri_offset / (LIMNFS_PTRS_PER_BLOCK * LIMNFS_PTRS_PER_BLOCK);
            uint32_t rem = tri_offset % (LIMNFS_PTRS_PER_BLOCK * LIMNFS_PTRS_PER_BLOCK);
            uint32_t l2 = rem / LIMNFS_PTRS_PER_BLOCK;
            uint32_t l3 = rem % LIMNFS_PTRS_PER_BLOCK;

            uint32_t *tri_ptrs = (uint32_t *)scratch_block3;
            if (bcache_read(inode->triple_indirect, tri_ptrs) != 0) return 0;
            uint32_t dbl_blk = tri_ptrs[l1];
            if (dbl_blk == 0) return 0;

            uint32_t *dbl_ptrs = (uint32_t *)scratch_block3;
            if (bcache_read(dbl_blk, dbl_ptrs) != 0) return 0;
            uint32_t mid_blk = dbl_ptrs[l2];
            if (mid_blk == 0) return 0;

            uint32_t *mid_ptrs = (uint32_t *)scratch_block3;
            if (bcache_read(mid_blk, mid_ptrs) != 0) return 0;
            return mid_ptrs[l3];
        }
    }

    return 0;
}

/* Allocate a block for a specific logical position in a file.
 * Updates inode in-memory (caller must write inode back). */
static uint32_t alloc_file_block(limnfs_inode_t *inode, uint32_t logical_block) {
    uint32_t blk = limnfs_alloc_block();
    if (blk == 0) return 0;

    /* Zero the new block */
    for (int i = 0; i < LIMNFS_BLOCK_SIZE; i++)
        scratch_block2[i] = 0;
    bcache_write(blk, scratch_block2);

    if (logical_block < LIMNFS_MAX_DIRECT) {
        inode->direct[logical_block] = blk;
    } else {
        uint32_t idx = logical_block - LIMNFS_MAX_DIRECT;

        if (idx < LIMNFS_PTRS_PER_BLOCK) {
            /* Single indirect */
            if (inode->indirect == 0) {
                uint32_t ind_blk = limnfs_alloc_block();
                if (ind_blk == 0) {
                    limnfs_free_block(blk);
                    return 0;
                }
                for (int i = 0; i < LIMNFS_BLOCK_SIZE; i++)
                    scratch_block2[i] = 0;
                bcache_write(ind_blk, scratch_block2);
                inode->indirect = ind_blk;
            }

            uint32_t *indirect_ptrs = (uint32_t *)scratch_block2;
            bcache_read(inode->indirect, indirect_ptrs);
            indirect_ptrs[idx] = blk;
            bcache_write(inode->indirect, indirect_ptrs);
        } else {
            uint32_t dbl_offset = idx - LIMNFS_PTRS_PER_BLOCK;

            if (dbl_offset < LIMNFS_PTRS_PER_BLOCK * LIMNFS_PTRS_PER_BLOCK) {
                /* Double indirect */
                uint32_t l1_idx = dbl_offset / LIMNFS_PTRS_PER_BLOCK;
                uint32_t l2_idx = dbl_offset % LIMNFS_PTRS_PER_BLOCK;

                /* Allocate double indirect block if needed */
                if (inode->double_indirect == 0) {
                    uint32_t dbl_blk = limnfs_alloc_block();
                    if (dbl_blk == 0) {
                        limnfs_free_block(blk);
                        return 0;
                    }
                    for (int i = 0; i < LIMNFS_BLOCK_SIZE; i++)
                        scratch_block2[i] = 0;
                    bcache_write(dbl_blk, scratch_block2);
                    inode->double_indirect = dbl_blk;
                }

                /* Read double indirect block */
                uint32_t *dbl_ptrs = (uint32_t *)scratch_block2;
                bcache_read(inode->double_indirect, dbl_ptrs);
                uint32_t mid_blk = dbl_ptrs[l1_idx];

                /* Allocate intermediate indirect block if needed */
                if (mid_blk == 0) {
                    mid_blk = limnfs_alloc_block();
                    if (mid_blk == 0) {
                        limnfs_free_block(blk);
                        return 0;
                    }
                    for (int i = 0; i < LIMNFS_BLOCK_SIZE; i++)
                        scratch_block2[i] = 0;
                    bcache_write(mid_blk, scratch_block2);

                    /* Write mid_blk pointer into double indirect block */
                    bcache_read(inode->double_indirect, dbl_ptrs);
                    dbl_ptrs[l1_idx] = mid_blk;
                    bcache_write(inode->double_indirect, dbl_ptrs);
                }

                /* Write data block pointer into intermediate indirect block */
                uint32_t *mid_ptrs = (uint32_t *)scratch_block2;
                bcache_read(mid_blk, mid_ptrs);
                mid_ptrs[l2_idx] = blk;
                bcache_write(mid_blk, mid_ptrs);
            } else {
                /* Triple indirect */
                uint32_t triple_base = LIMNFS_PTRS_PER_BLOCK + LIMNFS_PTRS_PER_BLOCK * LIMNFS_PTRS_PER_BLOCK;
                uint32_t tri_offset = idx - LIMNFS_PTRS_PER_BLOCK - triple_base + LIMNFS_PTRS_PER_BLOCK;
                /* Recalculate: tri_offset = logical_block - (MAX_DIRECT + PTRS + PTRS^2) */
                tri_offset = dbl_offset - LIMNFS_PTRS_PER_BLOCK * LIMNFS_PTRS_PER_BLOCK;
                uint32_t l1 = tri_offset / (LIMNFS_PTRS_PER_BLOCK * LIMNFS_PTRS_PER_BLOCK);
                uint32_t rem = tri_offset % (LIMNFS_PTRS_PER_BLOCK * LIMNFS_PTRS_PER_BLOCK);
                uint32_t l2 = rem / LIMNFS_PTRS_PER_BLOCK;
                uint32_t l3 = rem % LIMNFS_PTRS_PER_BLOCK;

                /* Allocate triple indirect block if needed */
                if (inode->triple_indirect == 0) {
                    uint32_t tri_blk = limnfs_alloc_block();
                    if (tri_blk == 0) { limnfs_free_block(blk); return 0; }
                    for (int i = 0; i < LIMNFS_BLOCK_SIZE; i++) scratch_block3[i] = 0;
                    bcache_write(tri_blk, scratch_block3);
                    inode->triple_indirect = tri_blk;
                }

                /* Read triple indirect block */
                uint32_t *tri_ptrs = (uint32_t *)scratch_block3;
                bcache_read(inode->triple_indirect, tri_ptrs);
                uint32_t dbl_blk = tri_ptrs[l1];

                /* Allocate double-level block if needed */
                if (dbl_blk == 0) {
                    dbl_blk = limnfs_alloc_block();
                    if (dbl_blk == 0) { limnfs_free_block(blk); return 0; }
                    for (int i = 0; i < LIMNFS_BLOCK_SIZE; i++) scratch_block3[i] = 0;
                    bcache_write(dbl_blk, scratch_block3);
                    bcache_read(inode->triple_indirect, tri_ptrs);
                    tri_ptrs[l1] = dbl_blk;
                    bcache_write(inode->triple_indirect, tri_ptrs);
                }

                /* Read double-level block */
                uint32_t *dbl_ptrs = (uint32_t *)scratch_block3;
                bcache_read(dbl_blk, dbl_ptrs);
                uint32_t mid_blk = dbl_ptrs[l2];

                /* Allocate single-level block if needed */
                if (mid_blk == 0) {
                    mid_blk = limnfs_alloc_block();
                    if (mid_blk == 0) { limnfs_free_block(blk); return 0; }
                    for (int i = 0; i < LIMNFS_BLOCK_SIZE; i++) scratch_block3[i] = 0;
                    bcache_write(mid_blk, scratch_block3);
                    bcache_read(dbl_blk, dbl_ptrs);
                    dbl_ptrs[l2] = mid_blk;
                    bcache_write(dbl_blk, dbl_ptrs);
                }

                /* Write data block pointer */
                uint32_t *mid_ptrs = (uint32_t *)scratch_block3;
                bcache_read(mid_blk, mid_ptrs);
                mid_ptrs[l3] = blk;
                bcache_write(mid_blk, mid_ptrs);
            }
        }
    }

    inode->block_count++;
    return blk;
}

/* --- Public API: Format + Mount --- */

int limnfs_format(uint32_t total_blocks) {
    pr_info("Formatting disk (%u blocks, %u MB)...\n",
            total_blocks, total_blocks / 256);

    /* Allocate block bitmap dynamically */
    uint32_t bitmap_bytes = (total_blocks + 7) / 8;
    if (bitmap_bytes < 4096) bitmap_bytes = 4096;  /* minimum 1 block */
    /* Round up to block boundary */
    bitmap_bytes = (bitmap_bytes + LIMNFS_BLOCK_SIZE - 1) & ~(LIMNFS_BLOCK_SIZE - 1);

    if (lfs.block_bitmap) kfree(lfs.block_bitmap);
    lfs.block_bitmap = (uint8_t *)kmalloc(bitmap_bytes);
    if (!lfs.block_bitmap) {
        pr_err("Failed to allocate block bitmap (%u bytes)\n", bitmap_bytes);
        return -1;
    }
    lfs.block_bitmap_bytes = bitmap_bytes;

    /* Calculate layout */
    uint32_t bitmap_blocks = bitmap_bytes / LIMNFS_BLOCK_SIZE;
    uint32_t inode_blocks = (LIMNFS_MAX_INODES * 128 + LIMNFS_BLOCK_SIZE - 1) / LIMNFS_BLOCK_SIZE;
    /* Layout: super(1) + block_bitmap(N) + inode_bitmap(1) + inode_table(32) + data */
    uint32_t inode_tbl_start = 1 + bitmap_blocks + 1;
    uint32_t data_start = inode_tbl_start + inode_blocks;
    uint32_t reserved = data_start;

    pr_info("Layout: bitmap=%u blocks, inodes at %u, data at %u\n",
            bitmap_blocks, inode_tbl_start, data_start);

    /* Prepare superblock */
    for (uint32_t i = 0; i < sizeof(limnfs_super_t); i++)
        ((uint8_t *)&lfs.super)[i] = 0;

    lfs.super.magic = LIMNFS_MAGIC;
    lfs.super.total_blocks = total_blocks;
    lfs.super.total_inodes = LIMNFS_MAX_INODES;
    lfs.super.block_size = LIMNFS_BLOCK_SIZE;
    lfs.super.inode_tbl_start = inode_tbl_start;
    lfs.super.data_start = data_start;
    lfs.super.free_blocks = total_blocks - reserved;
    lfs.super.free_inodes = LIMNFS_MAX_INODES - 1;  /* inode 0 = root */

    if (flush_superblock() != 0) {
        pr_err("Failed to write superblock\n");
        return -1;
    }

    /* Prepare block bitmap: mark blocks 0..data_start-1 as used */
    for (uint32_t i = 0; i < lfs.block_bitmap_bytes; i++)
        lfs.block_bitmap[i] = 0;
    for (uint32_t b = 0; b < reserved; b++)
        bitmap_set(lfs.block_bitmap, b);

    if (flush_block_bitmap() != 0) {
        pr_err("Failed to write block bitmap\n");
        return -1;
    }

    /* Prepare inode bitmap: mark inode 0 as used */
    for (int i = 0; i < 128; i++)
        lfs.inode_bitmap[i] = 0;
    bitmap_set(lfs.inode_bitmap, 0);

    if (flush_inode_bitmap() != 0) {
        pr_err("Failed to write inode bitmap\n");
        return -1;
    }

    /* Zero out inode table */
    for (int i = 0; i < LIMNFS_BLOCK_SIZE; i++)
        scratch_block[i] = 0;

    for (uint32_t b = 0; b < inode_blocks; b++) {
        if (bcache_write(inode_tbl_start + b, scratch_block) != 0) {
            pr_err("Failed to write inode table\n");
            return -1;
        }
    }

    /* Write root inode (inode 0) */
    limnfs_inode_t root;
    for (uint32_t i = 0; i < sizeof(limnfs_inode_t); i++)
        ((uint8_t *)&root)[i] = 0;
    root.type = LIMNFS_TYPE_DIR;
    root.mode = 0x07;  /* rwx */
    root.parent = 0;   /* root's parent is itself */

    if (limnfs_write_inode(0, &root) != 0) {
        pr_err("Failed to write root inode\n");
        return -1;
    }

    lfs.mounted = 1;
    pr_info("Format complete (data starts at block %u)\n", data_start);
    return 0;
}

int limnfs_mount(void) {
    pr_info("Mounting filesystem...\n");

    /* Read superblock */
    if (bcache_read(0, scratch_block) != 0) {
        pr_err("Failed to read superblock\n");
        return -1;
    }

    limnfs_super_t *sb = (limnfs_super_t *)scratch_block;
    if (sb->magic != LIMNFS_MAGIC) {
        pr_err("Bad magic: %x (expected %x)\n",
               sb->magic, LIMNFS_MAGIC);
        return -1;
    }

    /* Copy superblock */
    const uint8_t *src = (const uint8_t *)sb;
    uint8_t *dst = (uint8_t *)&lfs.super;
    for (uint32_t i = 0; i < sizeof(limnfs_super_t); i++)
        dst[i] = src[i];

    pr_info("Superblock: %u blocks, %u inodes, data@%u\n",
            lfs.super.total_blocks, lfs.super.total_inodes,
            lfs.super.data_start);

    /* Allocate block bitmap based on superblock total_blocks */
    uint32_t bitmap_bytes = (lfs.super.total_blocks + 7) / 8;
    if (bitmap_bytes < 4096) bitmap_bytes = 4096;
    bitmap_bytes = (bitmap_bytes + LIMNFS_BLOCK_SIZE - 1) & ~(LIMNFS_BLOCK_SIZE - 1);

    if (lfs.block_bitmap) kfree(lfs.block_bitmap);
    lfs.block_bitmap = (uint8_t *)kmalloc(bitmap_bytes);
    if (!lfs.block_bitmap) {
        pr_err("Failed to allocate block bitmap (%u bytes)\n", bitmap_bytes);
        return -1;
    }
    lfs.block_bitmap_bytes = bitmap_bytes;

    /* Read block bitmap (may span multiple blocks) */
    uint32_t bitmap_blocks = bitmap_bytes / LIMNFS_BLOCK_SIZE;
    if (bitmap_blocks == 0) bitmap_blocks = 1;
    for (uint32_t b = 0; b < bitmap_blocks; b++) {
        if (bcache_read(1 + b, scratch_block) != 0) {
            pr_err("Failed to read block bitmap (block %u)\n", 1 + b);
            return -1;
        }
        uint32_t offset = b * LIMNFS_BLOCK_SIZE;
        uint32_t remain = bitmap_bytes - offset;
        if (remain > LIMNFS_BLOCK_SIZE) remain = LIMNFS_BLOCK_SIZE;
        for (uint32_t i = 0; i < remain; i++)
            lfs.block_bitmap[offset + i] = scratch_block[i];
    }

    /* Read inode bitmap (follows block bitmap) */
    uint32_t inode_bm_block = 1 + bitmap_blocks;
    if (bcache_read(inode_bm_block, scratch_block) != 0) {
        pr_err("Failed to read inode bitmap\n");
        return -1;
    }
    for (int i = 0; i < 128; i++)
        lfs.inode_bitmap[i] = scratch_block[i];

    lfs.mounted = 1;
    pr_info("Mounted (%u free blocks, %u free inodes)\n",
            lfs.super.free_blocks, lfs.super.free_inodes);
    return 0;
}

int limnfs_mounted(void) {
    return lfs.mounted;
}

int limnfs_get_stats(uint32_t *total, uint32_t *free_blk,
                      uint32_t *total_ino, uint32_t *free_ino) {
    if (!lfs.mounted) return -1;
    *total = lfs.super.total_blocks;
    *free_blk = lfs.super.free_blocks;
    *total_ino = lfs.super.total_inodes;
    *free_ino = lfs.super.free_inodes;
    return 0;
}

/* --- Inode operations --- */

int limnfs_read_inode(uint32_t ino, limnfs_inode_t *out) {
    if (ino >= LIMNFS_MAX_INODES) return -1;

    /* Each block holds 32 inodes (4096 / 128) */
    uint32_t inodes_per_block = LIMNFS_BLOCK_SIZE / sizeof(limnfs_inode_t);
    uint32_t block = lfs.super.inode_tbl_start + ino / inodes_per_block;
    uint32_t offset = (ino % inodes_per_block) * sizeof(limnfs_inode_t);

    if (bcache_read(block, scratch_block) != 0)
        return -1;

    const uint8_t *s = scratch_block + offset;
    uint8_t *d = (uint8_t *)out;
    for (uint32_t i = 0; i < sizeof(limnfs_inode_t); i++)
        d[i] = s[i];

    return 0;
}

int limnfs_write_inode(uint32_t ino, const limnfs_inode_t *in) {
    if (ino >= LIMNFS_MAX_INODES) return -1;

    uint32_t inodes_per_block = LIMNFS_BLOCK_SIZE / sizeof(limnfs_inode_t);
    uint32_t block = lfs.super.inode_tbl_start + ino / inodes_per_block;
    uint32_t offset = (ino % inodes_per_block) * sizeof(limnfs_inode_t);

    if (bcache_read(block, scratch_block) != 0)
        return -1;

    const uint8_t *s = (const uint8_t *)in;
    for (uint32_t i = 0; i < sizeof(limnfs_inode_t); i++)
        scratch_block[offset + i] = s[i];

    return bcache_write(block, scratch_block);
}

int limnfs_alloc_inode(void) {
    for (uint32_t i = 1; i < LIMNFS_MAX_INODES; i++) {
        if (!bitmap_get(lfs.inode_bitmap, i)) {
            bitmap_set(lfs.inode_bitmap, i);
            lfs.super.free_inodes--;
            flush_inode_bitmap();
            flush_superblock();
            return (int)i;
        }
    }
    return -1;
}

void limnfs_free_inode(uint32_t ino) {
    if (ino == 0 || ino >= LIMNFS_MAX_INODES) return;

    /* Free all data blocks of this inode */
    limnfs_inode_t inode;
    if (limnfs_read_inode(ino, &inode) != 0) return;

    /* Free direct blocks */
    for (int i = 0; i < LIMNFS_MAX_DIRECT; i++) {
        if (inode.direct[i])
            limnfs_free_block(inode.direct[i]);
    }

    /* Free indirect block and its entries */
    if (inode.indirect) {
        uint32_t *indirect_ptrs = (uint32_t *)scratch_block2;
        if (bcache_read(inode.indirect, indirect_ptrs) == 0) {
            for (int i = 0; i < LIMNFS_PTRS_PER_BLOCK; i++) {
                if (indirect_ptrs[i])
                    limnfs_free_block(indirect_ptrs[i]);
            }
        }
        limnfs_free_block(inode.indirect);
    }

    /* Free double indirect block and all its entries */
    if (inode.double_indirect) {
        uint32_t *dbl_ptrs = (uint32_t *)scratch_block2;
        if (bcache_read(inode.double_indirect, dbl_ptrs) == 0) {
            for (int i = 0; i < LIMNFS_PTRS_PER_BLOCK; i++) {
                if (dbl_ptrs[i] == 0) continue;
                uint32_t mid_blk = dbl_ptrs[i];
                uint32_t *mid_ptrs = (uint32_t *)scratch_block2;
                if (bcache_read(mid_blk, mid_ptrs) == 0) {
                    for (int j = 0; j < LIMNFS_PTRS_PER_BLOCK; j++) {
                        if (mid_ptrs[j])
                            limnfs_free_block(mid_ptrs[j]);
                    }
                }
                limnfs_free_block(mid_blk);
                /* Re-read double indirect since scratch_block2 was overwritten */
                bcache_read(inode.double_indirect, dbl_ptrs);
            }
        }
        limnfs_free_block(inode.double_indirect);
    }

    /* Free triple indirect block and all its entries */
    if (inode.triple_indirect) {
        uint32_t *tri_ptrs = (uint32_t *)scratch_block3;
        if (bcache_read(inode.triple_indirect, tri_ptrs) == 0) {
            for (int i = 0; i < LIMNFS_PTRS_PER_BLOCK; i++) {
                if (tri_ptrs[i] == 0) continue;
                uint32_t dbl_blk = tri_ptrs[i];
                uint32_t *dbl_ptrs = (uint32_t *)scratch_block3;
                if (bcache_read(dbl_blk, dbl_ptrs) == 0) {
                    for (int j = 0; j < LIMNFS_PTRS_PER_BLOCK; j++) {
                        if (dbl_ptrs[j] == 0) continue;
                        uint32_t mid_blk = dbl_ptrs[j];
                        uint32_t *mid_ptrs = (uint32_t *)scratch_block3;
                        if (bcache_read(mid_blk, mid_ptrs) == 0) {
                            for (int k = 0; k < LIMNFS_PTRS_PER_BLOCK; k++) {
                                if (mid_ptrs[k])
                                    limnfs_free_block(mid_ptrs[k]);
                            }
                        }
                        limnfs_free_block(mid_blk);
                        /* Re-read dbl_blk since scratch_block3 was overwritten */
                        bcache_read(dbl_blk, dbl_ptrs);
                    }
                }
                limnfs_free_block(dbl_blk);
                /* Re-read triple indirect since scratch_block3 was overwritten */
                bcache_read(inode.triple_indirect, tri_ptrs);
            }
        }
        limnfs_free_block(inode.triple_indirect);
    }

    /* Zero inode */
    for (uint32_t i = 0; i < sizeof(limnfs_inode_t); i++)
        ((uint8_t *)&inode)[i] = 0;
    limnfs_write_inode(ino, &inode);

    /* Mark free */
    bitmap_clear(lfs.inode_bitmap, ino);
    lfs.super.free_inodes++;
    flush_inode_bitmap();
    flush_superblock();
}

/* --- Block operations --- */

uint32_t limnfs_alloc_block(void) {
    for (uint32_t i = lfs.super.data_start; i < lfs.super.total_blocks; i++) {
        if (!bitmap_get(lfs.block_bitmap, i)) {
            bitmap_set(lfs.block_bitmap, i);
            lfs.super.free_blocks--;
            flush_block_bitmap();
            flush_superblock();
            return i;
        }
    }
    return 0;
}

void limnfs_free_block(uint32_t blk) {
    if (blk < lfs.super.data_start || blk >= lfs.super.total_blocks) return;
    bitmap_clear(lfs.block_bitmap, blk);
    lfs.super.free_blocks++;
    flush_block_bitmap();
    flush_superblock();
}

/* --- File data I/O --- */

int64_t limnfs_read_data(uint32_t ino, uint64_t offset, uint8_t *buf, uint64_t len) {
    limnfs_inode_t inode;
    if (limnfs_read_inode(ino, &inode) != 0)
        return -1;

    if (offset >= inode.size)
        return 0;

    uint64_t available = inode.size - offset;
    if (len > available)
        len = available;

    uint64_t bytes_read = 0;
    while (bytes_read < len) {
        uint32_t logical_block = (uint32_t)((offset + bytes_read) / LIMNFS_BLOCK_SIZE);
        uint32_t block_offset = (uint32_t)((offset + bytes_read) % LIMNFS_BLOCK_SIZE);
        uint32_t disk_block = get_file_block(&inode, logical_block);
        if (disk_block == 0)
            break;

        if (bcache_read(disk_block, scratch_block) != 0)
            break;

        uint64_t chunk = LIMNFS_BLOCK_SIZE - block_offset;
        if (chunk > len - bytes_read)
            chunk = len - bytes_read;

        for (uint64_t i = 0; i < chunk; i++)
            buf[bytes_read + i] = scratch_block[block_offset + i];

        bytes_read += chunk;
    }

    return (int64_t)bytes_read;
}

int64_t limnfs_write_data(uint32_t ino, uint64_t offset, const uint8_t *buf, uint64_t len) {
    limnfs_inode_t inode;
    if (limnfs_read_inode(ino, &inode) != 0)
        return -1;

    uint64_t bytes_written = 0;
    while (bytes_written < len) {
        uint32_t logical_block = (uint32_t)((offset + bytes_written) / LIMNFS_BLOCK_SIZE);
        uint32_t block_offset = (uint32_t)((offset + bytes_written) % LIMNFS_BLOCK_SIZE);

        uint32_t disk_block = get_file_block(&inode, logical_block);
        if (disk_block == 0) {
            /* Need to allocate a new block */
            disk_block = alloc_file_block(&inode, logical_block);
            if (disk_block == 0)
                break;
        }

        if (bcache_read(disk_block, scratch_block) != 0)
            break;

        uint64_t chunk = LIMNFS_BLOCK_SIZE - block_offset;
        if (chunk > len - bytes_written)
            chunk = len - bytes_written;

        for (uint64_t i = 0; i < chunk; i++)
            scratch_block[block_offset + i] = buf[bytes_written + i];

        if (bcache_write(disk_block, scratch_block) != 0)
            break;

        bytes_written += chunk;
    }

    /* Update inode size */
    uint64_t end = offset + bytes_written;
    if (end > inode.size)
        inode.size = (uint32_t)end;

    limnfs_write_inode(ino, &inode);
    return (int64_t)bytes_written;
}

/* --- Directory operations --- */

int limnfs_dir_lookup(uint32_t dir_ino, const char *name) {
    limnfs_inode_t dir;
    if (limnfs_read_inode(dir_ino, &dir) != 0)
        return -1;
    if (dir.type != LIMNFS_TYPE_DIR)
        return -1;

    uint32_t total_entries = dir.size / LIMNFS_DENTRY_SIZE;
    uint32_t entries_read = 0;

    for (uint32_t lb = 0; entries_read < total_entries; lb++) {
        uint32_t disk_block = get_file_block(&dir, lb);
        if (disk_block == 0) break;

        limnfs_dentry_t *entries = (limnfs_dentry_t *)scratch_block;
        if (bcache_read(disk_block, entries) != 0)
            break;

        for (int i = 0; i < LIMNFS_DENTRIES_PER_BLOCK && entries_read < total_entries; i++, entries_read++) {
            if (entries[i].inode != 0 && lfs_str_eq(entries[i].name, name))
                return (int)entries[i].inode;
        }
    }

    return -1;
}

int limnfs_dir_add(uint32_t dir_ino, const char *name, uint32_t child_ino, uint8_t type) {
    limnfs_inode_t dir;
    if (limnfs_read_inode(dir_ino, &dir) != 0)
        return -1;
    if (dir.type != LIMNFS_TYPE_DIR)
        return -1;

    uint32_t name_len = lfs_str_len(name);
    if (name_len == 0 || name_len > 57)
        return -1;

    /* Try to find a free slot (inode == 0) in existing directory blocks */
    uint32_t total_entries = dir.size / LIMNFS_DENTRY_SIZE;

    for (uint32_t lb = 0; lb * LIMNFS_DENTRIES_PER_BLOCK < total_entries; lb++) {
        uint32_t disk_block = get_file_block(&dir, lb);
        if (disk_block == 0) break;

        limnfs_dentry_t *entries = (limnfs_dentry_t *)scratch_block;
        if (bcache_read(disk_block, entries) != 0)
            break;

        uint32_t end = total_entries - lb * LIMNFS_DENTRIES_PER_BLOCK;
        if (end > LIMNFS_DENTRIES_PER_BLOCK) end = LIMNFS_DENTRIES_PER_BLOCK;

        for (uint32_t i = 0; i < end; i++) {
            if (entries[i].inode == 0) {
                /* Reuse deleted slot */
                entries[i].inode = child_ino;
                entries[i].name_len = (uint8_t)name_len;
                entries[i].file_type = type;
                lfs_str_copy(entries[i].name, name, 58);
                bcache_write(disk_block, entries);
                return 0;
            }
        }
    }

    /* No free slot — append new entry */
    uint32_t new_entry_idx = total_entries;
    uint32_t lb = new_entry_idx / LIMNFS_DENTRIES_PER_BLOCK;
    uint32_t within = new_entry_idx % LIMNFS_DENTRIES_PER_BLOCK;

    uint32_t disk_block = get_file_block(&dir, lb);
    if (disk_block == 0) {
        /* Need a new block for directory data */
        disk_block = alloc_file_block(&dir, lb);
        if (disk_block == 0)
            return -1;
    }

    limnfs_dentry_t *entries = (limnfs_dentry_t *)scratch_block;
    if (bcache_read(disk_block, entries) != 0)
        return -1;

    entries[within].inode = child_ino;
    entries[within].name_len = (uint8_t)name_len;
    entries[within].file_type = type;
    lfs_str_copy(entries[within].name, name, 58);

    if (bcache_write(disk_block, entries) != 0)
        return -1;

    /* Update directory size */
    dir.size = (new_entry_idx + 1) * LIMNFS_DENTRY_SIZE;
    limnfs_write_inode(dir_ino, &dir);

    return 0;
}

int limnfs_dir_remove(uint32_t dir_ino, const char *name) {
    limnfs_inode_t dir;
    if (limnfs_read_inode(dir_ino, &dir) != 0)
        return -1;
    if (dir.type != LIMNFS_TYPE_DIR)
        return -1;

    uint32_t total_entries = dir.size / LIMNFS_DENTRY_SIZE;
    uint32_t entries_read = 0;

    for (uint32_t lb = 0; entries_read < total_entries; lb++) {
        uint32_t disk_block = get_file_block(&dir, lb);
        if (disk_block == 0) break;

        limnfs_dentry_t *entries = (limnfs_dentry_t *)scratch_block;
        if (bcache_read(disk_block, entries) != 0)
            break;

        for (int i = 0; i < LIMNFS_DENTRIES_PER_BLOCK && entries_read < total_entries; i++, entries_read++) {
            if (entries[i].inode != 0 && lfs_str_eq(entries[i].name, name)) {
                entries[i].inode = 0;
                entries[i].name[0] = '\0';
                bcache_write(disk_block, entries);
                return 0;
            }
        }
    }

    return -1;
}

int limnfs_dir_iter(uint32_t dir_ino, uint32_t index, limnfs_dentry_t *out) {
    limnfs_inode_t dir;
    if (limnfs_read_inode(dir_ino, &dir) != 0)
        return -1;
    if (dir.type != LIMNFS_TYPE_DIR)
        return -1;

    uint32_t total_entries = dir.size / LIMNFS_DENTRY_SIZE;
    uint32_t entries_read = 0;
    uint32_t live_seen = 0;

    for (uint32_t lb = 0; entries_read < total_entries; lb++) {
        uint32_t disk_block = get_file_block(&dir, lb);
        if (disk_block == 0) break;

        limnfs_dentry_t *entries = (limnfs_dentry_t *)scratch_block;
        if (bcache_read(disk_block, entries) != 0)
            break;

        for (int i = 0; i < LIMNFS_DENTRIES_PER_BLOCK && entries_read < total_entries; i++, entries_read++) {
            if (entries[i].inode == 0) continue;
            if (live_seen == index) {
                uint8_t *s = (uint8_t *)&entries[i];
                uint8_t *d = (uint8_t *)out;
                for (uint32_t j = 0; j < sizeof(limnfs_dentry_t); j++)
                    d[j] = s[j];
                return 0;
            }
            live_seen++;
        }
    }

    return -1;
}

/* --- High-level operations --- */

int limnfs_create_file(uint32_t parent_ino, const char *name) {
    /* Allocate inode */
    int ino = limnfs_alloc_inode();
    if (ino < 0) return -1;

    /* Initialize inode */
    limnfs_inode_t inode;
    for (uint32_t i = 0; i < sizeof(limnfs_inode_t); i++)
        ((uint8_t *)&inode)[i] = 0;
    inode.type = LIMNFS_TYPE_FILE;
    inode.mode = 0x06;  /* rw- */
    inode.parent = parent_ino;

    if (limnfs_write_inode((uint32_t)ino, &inode) != 0) {
        limnfs_free_inode((uint32_t)ino);
        return -1;
    }

    /* Add directory entry */
    if (limnfs_dir_add(parent_ino, name, (uint32_t)ino, LIMNFS_TYPE_FILE) != 0) {
        limnfs_free_inode((uint32_t)ino);
        return -1;
    }

    return ino;
}

int limnfs_create_dir(uint32_t parent_ino, const char *name) {
    int ino = limnfs_alloc_inode();
    if (ino < 0) return -1;

    limnfs_inode_t inode;
    for (uint32_t i = 0; i < sizeof(limnfs_inode_t); i++)
        ((uint8_t *)&inode)[i] = 0;
    inode.type = LIMNFS_TYPE_DIR;
    inode.mode = 0x07;  /* rwx */
    inode.parent = parent_ino;

    if (limnfs_write_inode((uint32_t)ino, &inode) != 0) {
        limnfs_free_inode((uint32_t)ino);
        return -1;
    }

    if (limnfs_dir_add(parent_ino, name, (uint32_t)ino, LIMNFS_TYPE_DIR) != 0) {
        limnfs_free_inode((uint32_t)ino);
        return -1;
    }

    return ino;
}

int limnfs_delete(uint32_t parent_ino, const char *name) {
    int child_ino = limnfs_dir_lookup(parent_ino, name);
    if (child_ino < 0) return -1;

    limnfs_inode_t child;
    if (limnfs_read_inode((uint32_t)child_ino, &child) != 0)
        return -1;

    /* If directory, check it's empty */
    if (child.type == LIMNFS_TYPE_DIR) {
        limnfs_dentry_t ent;
        if (limnfs_dir_iter((uint32_t)child_ino, 0, &ent) == 0)
            return -1;  /* Not empty */
    }

    /* Remove directory entry */
    if (limnfs_dir_remove(parent_ino, name) != 0)
        return -1;

    /* Free inode and its blocks */
    limnfs_free_inode((uint32_t)child_ino);

    return 0;
}

int limnfs_truncate(uint32_t ino, uint32_t new_size) {
    limnfs_inode_t inode;
    if (limnfs_read_inode(ino, &inode) != 0)
        return -1;

    if (new_size >= inode.size) {
        /* Extending — just update size */
        inode.size = new_size;
        return limnfs_write_inode(ino, &inode);
    }

    /* Shrinking — free blocks beyond new size */
    uint32_t new_blocks = (new_size + LIMNFS_BLOCK_SIZE - 1) / LIMNFS_BLOCK_SIZE;
    if (new_size == 0) new_blocks = 0;

    /* Free direct blocks beyond new_blocks */
    for (uint32_t i = new_blocks; i < LIMNFS_MAX_DIRECT; i++) {
        if (inode.direct[i]) {
            limnfs_free_block(inode.direct[i]);
            inode.direct[i] = 0;
        }
    }

    /* Free indirect block entries beyond new_blocks */
    if (inode.indirect) {
        uint32_t *indirect_ptrs = (uint32_t *)scratch_block2;
        if (bcache_read(inode.indirect, indirect_ptrs) == 0) {
            uint32_t indirect_start = (new_blocks > LIMNFS_MAX_DIRECT) ?
                                       new_blocks - LIMNFS_MAX_DIRECT : 0;
            int any_used = 0;
            for (uint32_t i = 0; i < LIMNFS_PTRS_PER_BLOCK; i++) {
                if (i >= indirect_start && indirect_ptrs[i]) {
                    limnfs_free_block(indirect_ptrs[i]);
                    indirect_ptrs[i] = 0;
                }
                if (indirect_ptrs[i]) any_used = 1;
            }
            if (!any_used) {
                limnfs_free_block(inode.indirect);
                inode.indirect = 0;
            } else {
                bcache_write(inode.indirect, indirect_ptrs);
            }
        }
    }

    /* Free double indirect block entries beyond new_blocks */
    if (inode.double_indirect) {
        /* dbl_start_idx: first double-indirect data block index to free */
        uint32_t dbl_base = LIMNFS_MAX_DIRECT + LIMNFS_PTRS_PER_BLOCK;
        uint32_t dbl_start_idx = (new_blocks > dbl_base) ? new_blocks - dbl_base : 0;

        uint32_t *dbl_ptrs = (uint32_t *)scratch_block2;
        if (bcache_read(inode.double_indirect, dbl_ptrs) == 0) {
            int any_l1_used = 0;
            for (uint32_t i = 0; i < LIMNFS_PTRS_PER_BLOCK; i++) {
                if (dbl_ptrs[i] == 0) continue;
                uint32_t mid_blk = dbl_ptrs[i];

                /* Determine which entries in this intermediate block to free */
                uint32_t mid_start = (i * LIMNFS_PTRS_PER_BLOCK < dbl_start_idx) ?
                                      dbl_start_idx - i * LIMNFS_PTRS_PER_BLOCK : 0;

                if (mid_start < LIMNFS_PTRS_PER_BLOCK) {
                    uint32_t *mid_ptrs = (uint32_t *)scratch_block2;
                    if (bcache_read(mid_blk, mid_ptrs) == 0) {
                        int any_mid_used = 0;
                        for (uint32_t j = 0; j < LIMNFS_PTRS_PER_BLOCK; j++) {
                            if (j >= mid_start && mid_ptrs[j]) {
                                limnfs_free_block(mid_ptrs[j]);
                                mid_ptrs[j] = 0;
                            }
                            if (mid_ptrs[j]) any_mid_used = 1;
                        }
                        if (!any_mid_used) {
                            limnfs_free_block(mid_blk);
                            /* Re-read double indirect and clear entry */
                            bcache_read(inode.double_indirect, dbl_ptrs);
                            dbl_ptrs[i] = 0;
                            bcache_write(inode.double_indirect, dbl_ptrs);
                        } else {
                            bcache_write(mid_blk, mid_ptrs);
                            any_l1_used = 1;
                            /* Re-read double indirect for next iteration */
                            bcache_read(inode.double_indirect, dbl_ptrs);
                        }
                    }
                } else {
                    any_l1_used = 1;
                }
            }
            if (!any_l1_used) {
                limnfs_free_block(inode.double_indirect);
                inode.double_indirect = 0;
            }
        }
    }

    /* Free triple indirect block entries beyond new_blocks */
    if (inode.triple_indirect) {
        uint32_t tri_base = LIMNFS_MAX_DIRECT + LIMNFS_PTRS_PER_BLOCK +
                            LIMNFS_PTRS_PER_BLOCK * LIMNFS_PTRS_PER_BLOCK;
        uint32_t tri_start_idx = (new_blocks > tri_base) ? new_blocks - tri_base : 0;

        uint32_t *tri_ptrs = (uint32_t *)scratch_block3;
        if (bcache_read(inode.triple_indirect, tri_ptrs) == 0) {
            int any_tri_used = 0;
            for (uint32_t i = 0; i < LIMNFS_PTRS_PER_BLOCK; i++) {
                if (tri_ptrs[i] == 0) continue;
                uint32_t dbl_blk = tri_ptrs[i];
                uint32_t i_base = i * LIMNFS_PTRS_PER_BLOCK * LIMNFS_PTRS_PER_BLOCK;

                uint32_t *dbl_ptrs = (uint32_t *)scratch_block3;
                if (bcache_read(dbl_blk, dbl_ptrs) == 0) {
                    int any_dbl_used = 0;
                    for (uint32_t j = 0; j < LIMNFS_PTRS_PER_BLOCK; j++) {
                        if (dbl_ptrs[j] == 0) continue;
                        uint32_t mid_blk = dbl_ptrs[j];
                        uint32_t j_base = i_base + j * LIMNFS_PTRS_PER_BLOCK;

                        uint32_t mid_start = (j_base < tri_start_idx) ?
                                              tri_start_idx - j_base : 0;

                        if (mid_start < LIMNFS_PTRS_PER_BLOCK) {
                            uint32_t *mid_ptrs = (uint32_t *)scratch_block3;
                            if (bcache_read(mid_blk, mid_ptrs) == 0) {
                                int any_mid_used = 0;
                                for (uint32_t k = 0; k < LIMNFS_PTRS_PER_BLOCK; k++) {
                                    if (k >= mid_start && mid_ptrs[k]) {
                                        limnfs_free_block(mid_ptrs[k]);
                                        mid_ptrs[k] = 0;
                                    }
                                    if (mid_ptrs[k]) any_mid_used = 1;
                                }
                                if (!any_mid_used) {
                                    limnfs_free_block(mid_blk);
                                    bcache_read(dbl_blk, dbl_ptrs);
                                    dbl_ptrs[j] = 0;
                                    bcache_write(dbl_blk, dbl_ptrs);
                                } else {
                                    bcache_write(mid_blk, mid_ptrs);
                                    any_dbl_used = 1;
                                    bcache_read(dbl_blk, dbl_ptrs);
                                }
                            }
                        } else {
                            any_dbl_used = 1;
                        }
                    }
                    if (!any_dbl_used) {
                        limnfs_free_block(dbl_blk);
                        bcache_read(inode.triple_indirect, tri_ptrs);
                        tri_ptrs[i] = 0;
                        bcache_write(inode.triple_indirect, tri_ptrs);
                    } else {
                        any_tri_used = 1;
                        bcache_read(inode.triple_indirect, tri_ptrs);
                    }
                }
            }
            if (!any_tri_used) {
                limnfs_free_block(inode.triple_indirect);
                inode.triple_indirect = 0;
            }
        }
    }

    inode.size = new_size;
    uint32_t meta_blocks = (inode.indirect ? 1 : 0) + (inode.double_indirect ? 1 : 0) +
                           (inode.triple_indirect ? 1 : 0);
    inode.block_count = new_blocks + meta_blocks;
    return limnfs_write_inode(ino, &inode);
}
