#ifndef LIMNX_VFS_H
#define LIMNX_VFS_H

#include <stdint.h>

#define VFS_FILE      0
#define VFS_DIRECTORY 1
#define VFS_SYMLINK   2

#define MAX_VFS_NODES 1024
#define MAX_SYMLINK_DEPTH 8
#define MAX_FDS       64
#define MAX_PATH      256

/* Node flags */
#define VFS_FLAG_WRITABLE    (1 << 0)

/* Permission bits (UNIX-like) */
#define VFS_PERM_READ   0x04
#define VFS_PERM_WRITE  0x02
#define VFS_PERM_EXEC   0x01

/* Special mode bits (upper nibble of 12-bit mode) */
#define VFS_MODE_SETUID 0x800   /* set-user-ID on exec */
#define VFS_MODE_SETGID 0x400   /* set-group-ID on exec */
#define VFS_MODE_STICKY 0x200   /* sticky bit (restricted delete) */

/* Maximum file size for writable files: 1 GB */
#define VFS_MAX_FILE_SIZE  (1024ULL * 1024 * 1024)

/* Open flags */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0x100
#define O_TRUNC   0x200
#define O_APPEND  0x400
#define O_ACCMODE 3       /* mask for access mode bits */
#define O_NONBLOCK 0x800

/* Per-fd flags (fd_entry_t.fd_flags) */
#define FD_CLOEXEC  0x01

/* Seek whence */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

typedef struct vfs_node {
    char name[MAX_PATH];        /* basename only (e.g., "hello.txt") */
    uint8_t type;               /* VFS_FILE or VFS_DIRECTORY */
    uint8_t flags;              /* VFS_FLAG_WRITABLE etc. */
    uint16_t mode;              /* 9-bit: owner rwx | group rwx | other rwx */
    uint16_t uid;               /* owner user ID */
    uint16_t gid;               /* owner group ID */
    int16_t parent;             /* parent node index, -1 for root */
    uint64_t size;              /* file size in bytes */
    uint64_t capacity;          /* allocated buffer size (writable files) */
    uint8_t *data;              /* pointer to file data (mutable) */
    int32_t disk_inode;         /* LimnFS inode number, -1 = no disk backing */
} vfs_node_t;

typedef struct vfs_stat {
    uint64_t size;
    uint8_t  type;              /* VFS_FILE or VFS_DIRECTORY */
    uint8_t  pad1;
    uint16_t mode;              /* 9-bit permission mode */
    uint16_t uid;
    uint16_t gid;
} vfs_stat_t;

typedef struct fd_entry {
    vfs_node_t *node;           /* NULL = unused slot */
    uint64_t offset;            /* current read/write position */
    void    *pipe;              /* pipe_t* if pipe fd, NULL otherwise */
    uint8_t  pipe_write;        /* 1=write end, 0=read end */
    void    *pty;               /* pty_t* if pty fd, NULL otherwise */
    uint8_t  pty_is_master;     /* 1=master side, 0=slave side */
    void    *unix_sock;         /* unix_sock_t* if unix socket fd */
    void    *eventfd;           /* eventfd_t* if eventfd */
    void    *epoll;             /* epoll_instance_t* if epoll fd */
    void    *uring;             /* uring_instance_t* if uring fd */
    int16_t  tcp_conn_idx;      /* TCP conn index if TCP fd, -1 otherwise */
    uint8_t  open_flags;        /* O_RDONLY/O_WRONLY/O_RDWR/O_APPEND (low bits) */
    uint8_t  fd_flags;          /* FD_CLOEXEC, O_NONBLOCK (bit 1) */
} fd_entry_t;

typedef struct vfs_dirent {
    char     name[256];
    uint8_t  type;
    uint8_t  pad[7];
    uint64_t size;
} vfs_dirent_t;           /* 272 bytes */

void vfs_init(void);
int  vfs_register_node(int parent, const char *name, uint8_t type,
                        uint64_t size, uint8_t *data);
int  vfs_open(const char *path);           /* returns node index, or -1 */
int64_t vfs_read(int node_idx, uint64_t offset, uint8_t *buf, uint64_t len);
int  vfs_stat(const char *path, vfs_stat_t *st);
int  vfs_get_node_count(void);
vfs_node_t *vfs_get_node(int idx);

/* Path resolution */
int  vfs_resolve_path(const char *path);   /* returns node index, or -1 */
int  vfs_find_child(int parent_idx, const char *name);
void vfs_path_split(const char *path, char *parent_buf, char *base_buf);

/* Writable VFS operations */
int  vfs_create(const char *path);         /* create empty file, returns node index */
int64_t vfs_write(int node_idx, uint64_t offset, const uint8_t *buf, uint64_t len);
int  vfs_delete(const char *path);         /* delete file by path */
int  vfs_node_index(vfs_node_t *node);     /* get index of a node pointer */
int  vfs_readdir(const char *dir_path, uint32_t index, vfs_dirent_t *out);

/* Directory operations */
int  vfs_mkdir(const char *path);          /* create directory, returns node index */

/* Truncate / Rename */
int  vfs_truncate_node(int node_idx, uint64_t new_size);
int  vfs_rename(const char *old_path, const char *new_path);

/* Chmod — update mode bits and sync VFS_FLAG_WRITABLE */
int  vfs_chmod(const char *path, uint16_t mode);

/* Chown — update file owner/group */
int  vfs_chown(int node_idx, uint16_t uid, uint16_t gid);

/* Symlink operations */
int  vfs_symlink(const char *path, const char *target);
int  vfs_readlink(const char *path, char *buf, uint64_t bufsize);

/* LimnFS mount — load disk tree into VFS */
int  vfs_mount_limnfs(void);

/* /proc filesystem */
void vfs_procfs_init(void);
void vfs_procfs_register_pid(uint64_t pid);
void vfs_procfs_unregister_pid(uint64_t pid);
void vfs_procfs_refresh(uint64_t pid);

#endif
