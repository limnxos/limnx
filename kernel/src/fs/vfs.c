#define pr_fmt(fmt) "[vfs] " fmt
#include "klog.h"

#include "fs/vfs.h"
#include "mm/kheap.h"
#include "blk/limnfs.h"
#include "serial.h"
#include "errno.h"
#include "kutil.h"

static vfs_node_t nodes[MAX_VFS_NODES];
static int node_count = 0;

/* --- Init --- */

void vfs_init(void) {
    node_count = 0;
    for (int i = 0; i < MAX_VFS_NODES; i++) {
        nodes[i].name[0] = '\0';
        nodes[i].data = NULL;
        nodes[i].size = 0;
        nodes[i].capacity = 0;
        nodes[i].flags = 0;
        nodes[i].parent = -1;
        nodes[i].disk_inode = -1;
    }

    /* Create root directory at node 0 */
    str_copy(nodes[0].name, "/", MAX_PATH);
    nodes[0].type = VFS_DIRECTORY;
    nodes[0].flags = 0;
    nodes[0].mode = 0755;
    nodes[0].uid = 0;
    nodes[0].gid = 0;
    nodes[0].parent = -1;
    nodes[0].size = 0;
    nodes[0].data = NULL;
    nodes[0].disk_inode = -1;
    node_count = 1;

    pr_info("VFS initialized (root at node 0)\n");
}

/* --- Path resolution --- */

void vfs_path_split(const char *path, char *parent_buf, char *base_buf) {
    uint64_t len = str_len(path);

    /* Find last '/' */
    int64_t last_slash = -1;
    for (uint64_t i = 0; i < len; i++) {
        if (path[i] == '/')
            last_slash = (int64_t)i;
    }

    if (last_slash <= 0) {
        /* Root parent: "/" + basename */
        parent_buf[0] = '/';
        parent_buf[1] = '\0';
        if (last_slash == 0) {
            /* path is "/something" */
            str_copy(base_buf, path + 1, MAX_PATH);
        } else {
            /* No slash at all — shouldn't happen with absolute paths */
            str_copy(base_buf, path, MAX_PATH);
        }
    } else {
        /* Copy parent path up to last_slash */
        uint64_t plen = (uint64_t)last_slash;
        if (plen >= MAX_PATH) plen = MAX_PATH - 1;
        for (uint64_t i = 0; i < plen; i++)
            parent_buf[i] = path[i];
        parent_buf[plen] = '\0';
        /* Basename is everything after last slash */
        str_copy(base_buf, path + last_slash + 1, MAX_PATH);
    }
}

int vfs_find_child(int parent_idx, const char *name) {
    for (int i = 0; i < node_count; i++) {
        if (nodes[i].name[0] == '\0') continue;
        if (nodes[i].parent == parent_idx && str_eq(nodes[i].name, name))
            return i;
    }
    return -ENOENT;
}

int vfs_resolve_path(const char *path) {
    if (!path || path[0] != '/')
        return -EINVAL;

    /* Root itself */
    if (path[0] == '/' && path[1] == '\0')
        return 0;

    int current = 0;  /* start at root */
    const char *p = path + 1;  /* skip leading '/' */

    while (*p) {
        /* Extract next component */
        char component[MAX_PATH];
        int ci = 0;
        while (*p && *p != '/' && ci < MAX_PATH - 1) {
            component[ci++] = *p++;
        }
        component[ci] = '\0';

        if (ci == 0) {
            /* Skip consecutive slashes */
            if (*p == '/') p++;
            continue;
        }

        /* Handle . and .. */
        if (component[0] == '.' && component[1] == '\0') {
            /* '.' — stay in current directory */
        } else if (component[0] == '.' && component[1] == '.' && component[2] == '\0') {
            /* '..' — move to parent (at root, stay at root) */
            if (nodes[current].parent >= 0)
                current = nodes[current].parent;
        } else {
            /* Find child with this name under current */
            int child = vfs_find_child(current, component);
            if (child < 0)
                return -ENOENT;
            current = child;
        }

        /* Skip trailing slash */
        if (*p == '/') p++;
    }

    return current;
}

/* --- Node registration --- */

int vfs_register_node(int parent, const char *name, uint8_t type,
                       uint64_t size, uint8_t *data) {
    /* Try to reuse an empty slot first (but not slot 0 = root) */
    for (int i = 1; i < node_count; i++) {
        if (nodes[i].name[0] == '\0') {
            vfs_node_t *n = &nodes[i];
            str_copy(n->name, name, MAX_PATH);
            n->type = type;
            n->size = size;
            n->data = data;
            n->capacity = 0;
            n->flags = 0;
            n->mode = (type == VFS_DIRECTORY) ? 0755 : 0644;
            n->uid = 0;
            n->gid = 0;
            n->parent = (int16_t)parent;
            n->disk_inode = -1;
            return i;
        }
    }

    if (node_count >= MAX_VFS_NODES)
        return -ENOSPC;

    vfs_node_t *n = &nodes[node_count];
    str_copy(n->name, name, MAX_PATH);
    n->type = type;
    n->size = size;
    n->data = data;
    n->capacity = 0;
    n->flags = 0;
    n->mode = (type == VFS_DIRECTORY) ? 0755 : 0644;
    n->uid = 0;
    n->gid = 0;
    n->parent = (int16_t)parent;
    n->disk_inode = -1;
    node_count++;
    return node_count - 1;
}

/* --- Open / Read / Stat --- */

int vfs_open(const char *path) {
    return vfs_resolve_path(path);
}

int64_t vfs_read(int node_idx, uint64_t offset, uint8_t *buf, uint64_t len) {
    if (node_idx < 0 || node_idx >= node_count)
        return -EBADF;

    vfs_node_t *n = &nodes[node_idx];
    if (n->name[0] == '\0')
        return -ENOENT;
    if (n->type != VFS_FILE)
        return -EISDIR;
    if (offset >= n->size)
        return 0;

    /* Disk-backed file: read through LimnFS */
    if (n->disk_inode >= 0)
        return limnfs_read_data((uint32_t)n->disk_inode, offset, buf, len);

    /* RAM-backed file */
    uint64_t available = n->size - offset;
    if (len > available)
        len = available;

    const uint8_t *src = n->data + offset;
    for (uint64_t i = 0; i < len; i++)
        buf[i] = src[i];

    return (int64_t)len;
}

int vfs_stat(const char *path, vfs_stat_t *st) {
    int idx = vfs_resolve_path(path);
    if (idx < 0) return -ENOENT;
    st->size = nodes[idx].size;
    st->type = nodes[idx].type;
    st->pad1 = 0;
    st->mode = nodes[idx].mode;
    st->uid = nodes[idx].uid;
    st->gid = nodes[idx].gid;
    return 0;
}

int vfs_get_node_count(void) {
    return node_count;
}

vfs_node_t *vfs_get_node(int idx) {
    if (idx < 0 || idx >= node_count)
        return NULL;
    if (nodes[idx].name[0] == '\0')
        return NULL;
    return &nodes[idx];
}

/* --- Writable VFS operations --- */

int vfs_create(const char *path) {
    /* Check if file already exists */
    if (vfs_open(path) >= 0)
        return -EEXIST;

    /* Split path into parent + basename */
    char parent_path[MAX_PATH], basename[MAX_PATH];
    vfs_path_split(path, parent_path, basename);

    if (basename[0] == '\0')
        return -EINVAL;

    int parent_idx = vfs_resolve_path(parent_path);
    if (parent_idx < 0)
        return -ENOENT;

    /* Verify parent is a directory */
    if (nodes[parent_idx].type != VFS_DIRECTORY)
        return -ENOTDIR;

    /* If LimnFS is mounted, create on disk */
    if (limnfs_mounted()) {
        int32_t parent_ino = nodes[parent_idx].disk_inode;
        if (parent_ino < 0) parent_ino = 0;  /* root */

        int disk_ino = limnfs_create_file((uint32_t)parent_ino, basename);
        if (disk_ino < 0)
            return -ENOSPC;

        int idx = vfs_register_node(parent_idx, basename, VFS_FILE, 0, NULL);
        if (idx < 0)
            return -ENOSPC;

        nodes[idx].disk_inode = (int32_t)disk_ino;
        nodes[idx].flags = VFS_FLAG_WRITABLE;
        nodes[idx].mode = VFS_PERM_READ | VFS_PERM_WRITE;
        return idx;
    }

    /* RAM-only fallback (before LimnFS is mounted) */
    uint64_t initial_cap = 4096;
    uint8_t *buf = (uint8_t *)kmalloc(initial_cap);
    if (!buf)
        return -ENOMEM;

    int idx = vfs_register_node(parent_idx, basename, VFS_FILE, 0, buf);
    if (idx < 0) {
        kfree(buf);
        return -ENOSPC;
    }

    nodes[idx].capacity = initial_cap;
    nodes[idx].flags = VFS_FLAG_WRITABLE;
    nodes[idx].mode = VFS_PERM_READ | VFS_PERM_WRITE;

    return idx;
}

int64_t vfs_write(int node_idx, uint64_t offset, const uint8_t *buf, uint64_t len) {
    if (node_idx < 0 || node_idx >= node_count)
        return -EBADF;

    vfs_node_t *n = &nodes[node_idx];
    if (n->name[0] == '\0')
        return -ENOENT;
    if (!(n->flags & VFS_FLAG_WRITABLE))
        return -EACCES;

    uint64_t end = offset + len;
    if (end > VFS_MAX_FILE_SIZE)
        return -ENOSPC;

    /* Disk-backed file: write through LimnFS */
    if (n->disk_inode >= 0) {
        int64_t written = limnfs_write_data((uint32_t)n->disk_inode, offset, buf, len);
        if (written > 0) {
            uint64_t new_end = offset + (uint64_t)written;
            if (new_end > n->size)
                n->size = new_end;
        }
        return written;
    }

    /* RAM-backed file */
    /* Grow buffer if needed (double until sufficient) */
    if (end > n->capacity) {
        uint64_t new_cap = n->capacity;
        if (new_cap == 0) new_cap = 4096;
        while (new_cap < end)
            new_cap *= 2;
        if (new_cap > VFS_MAX_FILE_SIZE)
            new_cap = VFS_MAX_FILE_SIZE;

        uint8_t *new_buf = (uint8_t *)krealloc(n->data, new_cap);
        if (!new_buf)
            return -ENOMEM;
        n->data = new_buf;
        n->capacity = new_cap;
    }

    /* Copy data */
    uint8_t *dst = n->data + offset;
    for (uint64_t i = 0; i < len; i++)
        dst[i] = buf[i];

    /* Update size if we wrote past the end */
    if (end > n->size)
        n->size = end;

    return (int64_t)len;
}

int vfs_node_index(vfs_node_t *node) {
    if (!node) return -EINVAL;
    int idx = (int)(node - &nodes[0]);
    if (idx < 0 || idx >= node_count)
        return -EINVAL;
    return idx;
}

int vfs_readdir(const char *dir_path, uint32_t index, vfs_dirent_t *out) {
    int dir_idx = vfs_resolve_path(dir_path);
    if (dir_idx < 0)
        return -ENOENT;

    if (nodes[dir_idx].type != VFS_DIRECTORY)
        return -ENOTDIR;

    /* Count children to find the N-th one */
    uint32_t live = 0;
    for (int i = 0; i < node_count; i++) {
        if (nodes[i].name[0] == '\0') continue;
        if (nodes[i].parent != dir_idx) continue;
        if (live == index) {
            str_copy(out->name, nodes[i].name, 256);
            out->type = nodes[i].type;
            out->size = nodes[i].size;
            return 0;
        }
        live++;
    }
    return -ENOENT;
}

int vfs_delete(const char *path) {
    int idx = vfs_resolve_path(path);
    if (idx < 0) return -ENOENT;
    if (idx == 0) return -EACCES;  /* Cannot delete root */

    vfs_node_t *n = &nodes[idx];

    /* If directory, check it's empty */
    if (n->type == VFS_DIRECTORY) {
        for (int i = 0; i < node_count; i++) {
            if (nodes[i].name[0] != '\0' && nodes[i].parent == idx)
                return -ENOTEMPTY;  /* Directory not empty */
        }
    }

    /* Delete from LimnFS if disk-backed */
    if (n->disk_inode >= 0) {
        int parent_idx = n->parent;
        int32_t parent_ino = (parent_idx >= 0) ? nodes[parent_idx].disk_inode : 0;
        if (parent_ino < 0) parent_ino = 0;
        limnfs_delete((uint32_t)parent_ino, n->name);
    }

    /* Free buffer if writable and heap-allocated */
    if ((n->flags & VFS_FLAG_WRITABLE) && n->data)
        kfree(n->data);

    /* Mark as empty */
    n->name[0] = '\0';
    n->data = NULL;
    n->size = 0;
    n->capacity = 0;
    n->flags = 0;
    n->parent = -1;
    n->disk_inode = -1;
    return 0;
}

/* --- Directory operations --- */

int vfs_mkdir(const char *path) {
    /* Check if already exists */
    if (vfs_resolve_path(path) >= 0)
        return -EEXIST;

    /* Split path into parent + basename */
    char parent_path[MAX_PATH], basename[MAX_PATH];
    vfs_path_split(path, parent_path, basename);

    if (basename[0] == '\0')
        return -EINVAL;

    int parent_idx = vfs_resolve_path(parent_path);
    if (parent_idx < 0)
        return -ENOENT;

    /* Verify parent is a directory */
    if (nodes[parent_idx].type != VFS_DIRECTORY)
        return -ENOTDIR;

    /* Create on disk if LimnFS is mounted */
    if (limnfs_mounted()) {
        int32_t parent_ino = nodes[parent_idx].disk_inode;
        if (parent_ino < 0) parent_ino = 0;

        int disk_ino = limnfs_create_dir((uint32_t)parent_ino, basename);
        if (disk_ino < 0)
            return -ENOSPC;

        int idx = vfs_register_node(parent_idx, basename, VFS_DIRECTORY, 0, NULL);
        if (idx < 0)
            return -ENOSPC;
        nodes[idx].disk_inode = (int32_t)disk_ino;
        return idx;
    }

    return vfs_register_node(parent_idx, basename, VFS_DIRECTORY, 0, NULL);
}

/* --- Truncate --- */

int vfs_truncate_node(int node_idx, uint64_t new_size) {
    if (node_idx < 0 || node_idx >= node_count)
        return -EBADF;

    vfs_node_t *n = &nodes[node_idx];
    if (n->name[0] == '\0')
        return -ENOENT;
    if (n->type != VFS_FILE)
        return -EISDIR;
    if (!(n->flags & VFS_FLAG_WRITABLE))
        return -EACCES;
    if (new_size > VFS_MAX_FILE_SIZE)
        return -ENOSPC;

    /* Disk-backed file */
    if (n->disk_inode >= 0) {
        if (limnfs_truncate((uint32_t)n->disk_inode, (uint32_t)new_size) != 0)
            return -EIO;
        n->size = new_size;
        return 0;
    }

    /* RAM-backed file */
    /* Grow buffer if needed */
    if (new_size > n->capacity) {
        uint64_t new_cap = n->capacity;
        if (new_cap == 0) new_cap = 4096;
        while (new_cap < new_size)
            new_cap *= 2;
        if (new_cap > VFS_MAX_FILE_SIZE)
            new_cap = VFS_MAX_FILE_SIZE;

        uint8_t *new_buf = (uint8_t *)krealloc(n->data, new_cap);
        if (!new_buf)
            return -ENOMEM;
        n->data = new_buf;
        n->capacity = new_cap;
    }

    /* Zero-fill if extending */
    if (new_size > n->size) {
        uint8_t *dst = n->data + n->size;
        for (uint64_t i = 0; i < new_size - n->size; i++)
            dst[i] = 0;
    }

    n->size = new_size;
    return 0;
}

/* --- Rename --- */

int vfs_rename(const char *old_path, const char *new_path) {
    int old_idx = vfs_resolve_path(old_path);
    if (old_idx < 0 || old_idx == 0)
        return -ENOENT;  /* can't rename root */

    /* Destination must not already exist */
    if (vfs_resolve_path(new_path) >= 0)
        return -EEXIST;

    /* Split new path into parent + basename */
    char new_parent_path[MAX_PATH], new_basename[MAX_PATH];
    vfs_path_split(new_path, new_parent_path, new_basename);
    if (new_basename[0] == '\0')
        return -EINVAL;

    int new_parent_idx = vfs_resolve_path(new_parent_path);
    if (new_parent_idx < 0)
        return -ENOENT;
    if (nodes[new_parent_idx].type != VFS_DIRECTORY)
        return -ENOTDIR;

    vfs_node_t *n = &nodes[old_idx];
    char old_name[MAX_PATH];
    str_copy(old_name, n->name, MAX_PATH);
    int old_parent_idx = n->parent;

    /* Update the node's parent and name */
    n->parent = (int16_t)new_parent_idx;
    str_copy(n->name, new_basename, MAX_PATH);

    /* Handle LimnFS rename if disk-backed */
    if (n->disk_inode >= 0) {
        int32_t old_parent_ino = (old_parent_idx >= 0) ? nodes[old_parent_idx].disk_inode : 0;
        int32_t new_parent_ino = nodes[new_parent_idx].disk_inode;
        if (old_parent_ino < 0) old_parent_ino = 0;
        if (new_parent_ino < 0) new_parent_ino = 0;

        uint8_t ftype = (n->type == VFS_DIRECTORY) ? LIMNFS_TYPE_DIR : LIMNFS_TYPE_FILE;

        /* Remove from old parent, add to new parent */
        limnfs_dir_remove((uint32_t)old_parent_ino, old_name);
        limnfs_dir_add((uint32_t)new_parent_ino, new_basename,
                       (uint32_t)n->disk_inode, ftype);

        /* Update inode parent */
        limnfs_inode_t inode;
        if (limnfs_read_inode((uint32_t)n->disk_inode, &inode) == 0) {
            inode.parent = (uint32_t)new_parent_ino;
            limnfs_write_inode((uint32_t)n->disk_inode, &inode);
        }
    }

    return 0;
}

/* --- LimnFS mount: load disk tree into VFS --- */

static void load_limnfs_dir(uint32_t dir_ino, int vfs_parent_idx) {
    limnfs_dentry_t ent;

    for (uint32_t i = 0; limnfs_dir_iter(dir_ino, i, &ent) == 0; i++) {
        /* Check if VFS already has this node (from initrd) */
        int existing = vfs_find_child(vfs_parent_idx, ent.name);

        if (ent.file_type == LIMNFS_TYPE_DIR) {
            int dir_idx;
            if (existing >= 0) {
                dir_idx = existing;
                nodes[dir_idx].disk_inode = (int32_t)ent.inode;
            } else {
                dir_idx = vfs_register_node(vfs_parent_idx, ent.name,
                                             VFS_DIRECTORY, 0, NULL);
                if (dir_idx < 0) continue;
                nodes[dir_idx].disk_inode = (int32_t)ent.inode;
            }
            /* Recurse into subdirectory */
            load_limnfs_dir(ent.inode, dir_idx);
        } else {
            /* File */
            limnfs_inode_t inode;
            if (limnfs_read_inode(ent.inode, &inode) != 0)
                continue;

            if (existing >= 0) {
                /* Already in VFS (initrd) — link to disk, free RAM buffer */
                if (nodes[existing].data) {
                    kfree(nodes[existing].data);
                    nodes[existing].data = NULL;
                    nodes[existing].capacity = 0;
                }
                nodes[existing].disk_inode = (int32_t)ent.inode;
                nodes[existing].size = inode.size;
                nodes[existing].flags |= VFS_FLAG_WRITABLE;
                nodes[existing].mode = inode.mode;
                nodes[existing].uid = inode.uid;
                nodes[existing].gid = inode.gid;
            } else {
                int idx = vfs_register_node(vfs_parent_idx, ent.name,
                                             VFS_FILE, inode.size, NULL);
                if (idx < 0) continue;
                nodes[idx].disk_inode = (int32_t)ent.inode;
                nodes[idx].flags = VFS_FLAG_WRITABLE;
                nodes[idx].mode = inode.mode;
                nodes[idx].uid = inode.uid;
                nodes[idx].gid = inode.gid;
            }
        }
    }
}

int vfs_chmod(const char *path, uint16_t mode) {
    int idx = vfs_resolve_path(path);
    if (idx < 0) return -ENOENT;
    /* Backward compat: if mode fits in 3 bits, expand to all triplets */
    if (mode <= 7)
        mode = (mode << 6) | (mode << 3) | mode;
    nodes[idx].mode = mode & 0x1FF;  /* 9-bit rwx triplets */
    /* Sync VFS_FLAG_WRITABLE with owner write bit */
    if (mode & 0x080)  /* owner write = bit 7 */
        nodes[idx].flags |= VFS_FLAG_WRITABLE;
    else
        nodes[idx].flags &= ~VFS_FLAG_WRITABLE;
    /* Persist to disk if backed by LimnFS */
    if (nodes[idx].disk_inode >= 0) {
        limnfs_inode_t inode;
        limnfs_read_inode((uint32_t)nodes[idx].disk_inode, &inode);
        inode.mode = mode & 0x1FF;
        limnfs_write_inode((uint32_t)nodes[idx].disk_inode, &inode);
    }
    return 0;
}

int vfs_mount_limnfs(void) {
    if (!limnfs_mounted())
        return -ENODEV;

    /* Set root disk_inode */
    nodes[0].disk_inode = 0;

    /* Load entire disk tree into VFS */
    load_limnfs_dir(0, 0);

    pr_info("LimnFS mounted at /\n");
    return 0;
}
