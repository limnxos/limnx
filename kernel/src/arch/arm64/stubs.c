/*
 * ARM64 stub functions for subsystems not yet ported.
 * These provide no-op or error-returning implementations for:
 *   - Network stack (net, tcp)
 *   - Block device / LimnFS filesystem
 *   - Swap / demand paging
 *
 * This allows shared kernel code to link without #ifdef pollution.
 */

#include <stdint.h>
#include "errno.h"

/* --- Network stubs --- */

int net_socket(void) { return -ENOSYS; }
int net_bind(int fd, uint16_t port) { (void)fd; (void)port; return -ENOSYS; }
long net_sendto(int fd, const void *buf, uint64_t len, uint32_t ip, uint16_t port) {
    (void)fd; (void)buf; (void)len; (void)ip; (void)port; return -ENOSYS;
}
long net_recvfrom(int fd, void *buf, uint64_t len, uint32_t *src_ip, uint16_t *src_port) {
    (void)fd; (void)buf; (void)len; (void)src_ip; (void)src_port; return -ENOSYS;
}

/* --- TCP stubs --- */

int tcp_socket(void) { return -ENOSYS; }
int tcp_connect(int idx, uint32_t ip, uint16_t port) { (void)idx; (void)ip; (void)port; return -ENOSYS; }
int tcp_listen(int idx, uint16_t port) { (void)idx; (void)port; return -ENOSYS; }
int tcp_accept(int idx) { (void)idx; return -ENOSYS; }
long tcp_send(int idx, const void *buf, uint64_t len) { (void)idx; (void)buf; (void)len; return -ENOSYS; }
long tcp_recv(int idx, void *buf, uint64_t len) { (void)idx; (void)buf; (void)len; return -ENOSYS; }
int tcp_close(int idx) { (void)idx; return -ENOSYS; }
int tcp_set_nonblock(int idx, int val) { (void)idx; (void)val; return -ENOSYS; }
int tcp_poll(int idx) { (void)idx; return 0; }
void tcp_timer_check(void) { }

/* --- LimnFS stubs --- */

int limnfs_mounted(void) { return 0; }
long limnfs_read_data(uint32_t inode, void *buf, uint64_t off, uint64_t len) {
    (void)inode; (void)buf; (void)off; (void)len; return -ENOSYS;
}
long limnfs_write_data(uint32_t inode, const void *buf, uint64_t off, uint64_t len) {
    (void)inode; (void)buf; (void)off; (void)len; return -ENOSYS;
}
int limnfs_create_file(const char *name, uint32_t parent) { (void)name; (void)parent; return -ENOSYS; }
int limnfs_create_dir(const char *name, uint32_t parent) { (void)name; (void)parent; return -ENOSYS; }
int limnfs_delete(uint32_t inode) { (void)inode; return -ENOSYS; }
int limnfs_truncate(uint32_t inode, uint64_t size) { (void)inode; (void)size; return -ENOSYS; }

typedef struct { uint32_t inode; char name[256]; } limnfs_dirent_t;
int limnfs_dir_iter(uint32_t dir_inode, int index, limnfs_dirent_t *out) {
    (void)dir_inode; (void)index; (void)out; return -1;
}
int limnfs_dir_add(uint32_t dir_inode, const char *name, uint32_t child_inode) {
    (void)dir_inode; (void)name; (void)child_inode; return -ENOSYS;
}
int limnfs_dir_remove(uint32_t dir_inode, const char *name) {
    (void)dir_inode; (void)name; return -ENOSYS;
}

typedef struct {
    uint32_t type;
    uint32_t mode;
    uint64_t size;
    uint32_t uid, gid;
    uint64_t atime, mtime, ctime;
    uint32_t nlink;
    uint32_t blocks;
} limnfs_inode_t;

int limnfs_read_inode(uint32_t inode, limnfs_inode_t *out) { (void)inode; (void)out; return -ENOSYS; }
int limnfs_write_inode(uint32_t inode, const limnfs_inode_t *in) { (void)inode; (void)in; return -ENOSYS; }

typedef struct { uint32_t total_blocks, free_blocks, total_inodes, free_inodes; } limnfs_stats_t;
int limnfs_get_stats(limnfs_stats_t *out) { (void)out; return -ENOSYS; }

/* --- Swap / demand paging stubs --- */

int swap_is_entry(uint64_t pte) { (void)pte; return 0; }
uint64_t swap_get_slot(uint64_t pte) { (void)pte; return 0; }
void swap_free_slot(uint64_t slot) { (void)slot; }
int swap_in(uint64_t slot, uint64_t phys) { (void)slot; (void)phys; return -ENOSYS; }

typedef struct { uint64_t total, used, free; } swap_stat_t;
int swap_stat(swap_stat_t *out) { (void)out; return -ENOSYS; }

int demand_page_fault(uint64_t fault_addr, uint64_t err, void *frame) {
    (void)fault_addr; (void)err; (void)frame; return -1;
}

/* --- Scheduler smoke test stub --- */

void sched_smoke_test(void) { }
