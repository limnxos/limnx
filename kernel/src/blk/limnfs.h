#ifndef LIMNX_LIMNFS_H
#define LIMNX_LIMNFS_H

#include <stdint.h>

/* LimnFS magic number: "LIMF" */
#define LIMNFS_MAGIC        0x4C494D46

/* Inode types */
#define LIMNFS_TYPE_FREE    0
#define LIMNFS_TYPE_FILE    1
#define LIMNFS_TYPE_DIR     2

/* Layout constants */
#define LIMNFS_BLOCK_SIZE   4096
#define LIMNFS_MAX_INODES   1024
#define LIMNFS_MAX_DIRECT   10
#define LIMNFS_DENTRY_SIZE  64
#define LIMNFS_DENTRIES_PER_BLOCK (LIMNFS_BLOCK_SIZE / LIMNFS_DENTRY_SIZE)  /* 64 */
#define LIMNFS_PTRS_PER_BLOCK     (LIMNFS_BLOCK_SIZE / 4)                   /* 1024 */
#define LIMNFS_BLOCK_BITMAP_BYTES_DEFAULT 32768  /* initial allocation, grows as needed */

/* Superblock (4096 bytes, block 0) */
typedef struct {
    uint32_t magic;
    uint32_t total_blocks;
    uint32_t total_inodes;
    uint32_t block_size;
    uint32_t inode_tbl_start;
    uint32_t data_start;
    uint32_t free_blocks;
    uint32_t free_inodes;
    uint8_t  reserved[4064];
} __attribute__((packed)) limnfs_super_t;

/* Inode (128 bytes) */
typedef struct {
    uint16_t type;
    uint16_t mode;
    uint32_t size;
    uint32_t block_count;
    uint32_t direct[LIMNFS_MAX_DIRECT];
    uint32_t indirect;
    uint32_t double_indirect;
    uint32_t triple_indirect;
    uint32_t parent;
    uint16_t uid;
    uint16_t gid;
    uint8_t  reserved[44];
} __attribute__((packed)) limnfs_inode_t;

/* Directory entry (64 bytes) */
typedef struct {
    uint32_t inode;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[58];
} __attribute__((packed)) limnfs_dentry_t;

/* Format + mount */
int  limnfs_format(uint32_t total_blocks);
int  limnfs_mount(void);
int  limnfs_mounted(void);
void limnfs_sync(void);

/* Inode operations */
int  limnfs_read_inode(uint32_t ino, limnfs_inode_t *out);
int  limnfs_write_inode(uint32_t ino, const limnfs_inode_t *in);
int  limnfs_alloc_inode(void);
void limnfs_free_inode(uint32_t ino);

/* Block operations */
uint32_t limnfs_alloc_block(void);
void     limnfs_free_block(uint32_t blk);

/* File data I/O */
int64_t limnfs_read_data(uint32_t ino, uint64_t offset, uint8_t *buf, uint64_t len);
int64_t limnfs_write_data(uint32_t ino, uint64_t offset, const uint8_t *buf, uint64_t len);

/* Directory operations */
int  limnfs_dir_lookup(uint32_t dir_ino, const char *name);
int  limnfs_dir_add(uint32_t dir_ino, const char *name, uint32_t child_ino, uint8_t type);
int  limnfs_dir_remove(uint32_t dir_ino, const char *name);
int  limnfs_dir_iter(uint32_t dir_ino, uint32_t index, limnfs_dentry_t *out);

/* High-level operations */
int  limnfs_create_file(uint32_t parent_ino, const char *name);
int  limnfs_create_dir(uint32_t parent_ino, const char *name);
int  limnfs_delete(uint32_t parent_ino, const char *name);
int  limnfs_truncate(uint32_t ino, uint32_t new_size);

#endif
