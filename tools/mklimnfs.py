#!/usr/bin/env python3
"""
mklimnfs.py — Create a LimnFS disk image with files pre-loaded.

Replicates the kernel's LimnFS format on the host so the kernel can
mount the filesystem directly (no format needed on first boot).

Usage:
  python tools/mklimnfs.py -o build/disk.img -s 1536 /path/to/model.gguf:model.gguf

  -o FILE    Output disk image path
  -s SIZE    Disk size in MB (default: auto-fit)
  FILE:NAME  Host file path : in-filesystem name (in root dir)
"""

import argparse
import os
import struct
import sys

BLOCK_SIZE = 4096
MAX_INODES = 1024
MAX_DIRECT = 10
PTRS_PER_BLOCK = BLOCK_SIZE // 4  # 1024
DENTRY_SIZE = 64
DENTRIES_PER_BLOCK = BLOCK_SIZE // DENTRY_SIZE  # 64
INODE_SIZE = 116  # matches kernel sizeof(limnfs_inode_t) with __packed__
LIMNFS_MAGIC = 0x4C494D46  # "LIMF"

TYPE_FREE = 0
TYPE_FILE = 1
TYPE_DIR = 2


class LimnFSImage:
    """File-backed LimnFS image builder. Uses seek+write to avoid huge in-memory buffer."""

    def __init__(self, path, total_blocks):
        self.path = path
        self.total_blocks = total_blocks
        self.fp = open(path, 'wb+')

        # Preallocate the file with a single seek+write
        self.fp.seek(total_blocks * BLOCK_SIZE - 1)
        self.fp.write(b'\x00')
        self.fp.flush()

        # Layout calculation
        bitmap_bytes = (total_blocks + 7) // 8
        if bitmap_bytes < BLOCK_SIZE:
            bitmap_bytes = BLOCK_SIZE
        bitmap_bytes = (bitmap_bytes + BLOCK_SIZE - 1) & ~(BLOCK_SIZE - 1)
        self.bitmap_blocks = bitmap_bytes // BLOCK_SIZE

        inode_blocks = (MAX_INODES * INODE_SIZE + BLOCK_SIZE - 1) // BLOCK_SIZE
        self.inode_tbl_start = 1 + self.bitmap_blocks + 1
        self.data_start = self.inode_tbl_start + inode_blocks
        self.reserved = self.data_start

        # Block bitmap (in memory — max ~48KB for 1.5GB disk)
        self.block_bitmap = bytearray(bitmap_bytes)
        for b in range(self.reserved):
            self._bitmap_set(b)

        # Inode bitmap
        self.inode_bitmap = bytearray(128)

        self.free_blocks = total_blocks - self.reserved
        self.free_inodes = MAX_INODES
        self.next_data_block = self.data_start  # sequential allocator

        # Cache of inode data (ino -> bytes)
        self.inodes = {}

    def close(self):
        self.fp.close()

    def _bitmap_set(self, bit):
        self.block_bitmap[bit // 8] |= (1 << (bit % 8))

    def alloc_inode(self):
        for i in range(MAX_INODES):
            byte_idx = i // 8
            bit_idx = i % 8
            if not (self.inode_bitmap[byte_idx] & (1 << bit_idx)):
                self.inode_bitmap[byte_idx] |= (1 << bit_idx)
                self.free_inodes -= 1
                return i
        raise RuntimeError("No free inodes")

    def alloc_block(self):
        """Sequential block allocation (fast for bulk writes)."""
        b = self.next_data_block
        if b >= self.total_blocks:
            raise RuntimeError("No free blocks")
        self._bitmap_set(b)
        self.next_data_block += 1
        self.free_blocks -= 1
        return b

    def write_block(self, blk, data):
        self.fp.seek(blk * BLOCK_SIZE)
        if len(data) < BLOCK_SIZE:
            self.fp.write(data + b'\x00' * (BLOCK_SIZE - len(data)))
        else:
            self.fp.write(data[:BLOCK_SIZE])

    def write_inode(self, ino, inode_bytes):
        inodes_per_block = BLOCK_SIZE // INODE_SIZE
        blk = self.inode_tbl_start + ino // inodes_per_block
        off_in_blk = (ino % inodes_per_block) * INODE_SIZE
        self.fp.seek(blk * BLOCK_SIZE + off_in_blk)
        self.fp.write(inode_bytes)
        self.inodes[ino] = inode_bytes

    def read_inode(self, ino):
        return self.inodes.get(ino, b'\x00' * INODE_SIZE)

    def pack_inode(self, itype, mode, size, block_count, direct, indirect, double_ind, triple_ind, parent, uid=0, gid=0):
        buf = struct.pack('<HHI I', itype, mode, size, block_count)
        for i in range(MAX_DIRECT):
            buf += struct.pack('<I', direct[i] if i < len(direct) else 0)
        buf += struct.pack('<III', indirect, double_ind, triple_ind)
        buf += struct.pack('<I', parent)
        buf += struct.pack('<HH', uid, gid)
        buf += b'\x00' * (INODE_SIZE - len(buf))
        return buf

    def pack_dentry(self, inode, name, file_type):
        name_bytes = name.encode('utf-8')[:58]
        buf = struct.pack('<IB B', inode, len(name_bytes), file_type)
        buf += name_bytes
        buf += b'\x00' * (DENTRY_SIZE - len(buf))
        return buf

    def write_file(self, parent_ino, filename, host_path):
        """Write a file to the filesystem from a host path. Streams data."""
        ino = self.alloc_inode()
        file_size = os.path.getsize(host_path)
        n_blocks_needed = (file_size + BLOCK_SIZE - 1) // BLOCK_SIZE

        print(f"  Writing {filename}: {file_size:,} bytes, {n_blocks_needed} blocks, inode {ino}")

        # Allocate all data blocks sequentially
        first_data_block = self.next_data_block
        block_list = []
        for i in range(n_blocks_needed):
            block_list.append(self.alloc_block())

        # Stream file data directly to disk image
        with open(host_path, 'rb') as src:
            for i, blk in enumerate(block_list):
                chunk = src.read(BLOCK_SIZE)
                self.fp.seek(blk * BLOCK_SIZE)
                if len(chunk) < BLOCK_SIZE:
                    self.fp.write(chunk + b'\x00' * (BLOCK_SIZE - len(chunk)))
                else:
                    self.fp.write(chunk)

                if (i + 1) % 50000 == 0:
                    pct = (i + 1) * 100 // n_blocks_needed
                    print(f"    ... {i + 1}/{n_blocks_needed} blocks ({pct}%)")

        # Set up block pointers
        direct = block_list[:MAX_DIRECT]
        indirect_blk = 0
        double_ind_blk = 0
        triple_ind_blk = 0
        idx = min(MAX_DIRECT, n_blocks_needed)

        # Single indirect
        if idx < n_blocks_needed:
            indirect_blk = self.alloc_block()
            ind_data = bytearray(BLOCK_SIZE)
            count = min(PTRS_PER_BLOCK, n_blocks_needed - idx)
            for i in range(count):
                struct.pack_into('<I', ind_data, i * 4, block_list[idx + i])
            self.write_block(indirect_blk, ind_data)
            idx += count

        # Double indirect
        if idx < n_blocks_needed:
            double_ind_blk = self.alloc_block()
            dind_data = bytearray(BLOCK_SIZE)
            l1_idx = 0
            while idx < n_blocks_needed and l1_idx < PTRS_PER_BLOCK:
                l1_blk = self.alloc_block()
                struct.pack_into('<I', dind_data, l1_idx * 4, l1_blk)
                l1_data = bytearray(BLOCK_SIZE)
                count = min(PTRS_PER_BLOCK, n_blocks_needed - idx)
                for i in range(count):
                    struct.pack_into('<I', l1_data, i * 4, block_list[idx + i])
                self.write_block(l1_blk, l1_data)
                idx += count
                l1_idx += 1
            self.write_block(double_ind_blk, dind_data)

        # Triple indirect
        if idx < n_blocks_needed:
            triple_ind_blk = self.alloc_block()
            tind_data = bytearray(BLOCK_SIZE)
            l2_idx = 0
            while idx < n_blocks_needed and l2_idx < PTRS_PER_BLOCK:
                l2_blk = self.alloc_block()
                struct.pack_into('<I', tind_data, l2_idx * 4, l2_blk)
                l2_data = bytearray(BLOCK_SIZE)
                l1_idx = 0
                while idx < n_blocks_needed and l1_idx < PTRS_PER_BLOCK:
                    l1_blk = self.alloc_block()
                    struct.pack_into('<I', l2_data, l1_idx * 4, l1_blk)
                    l1_data = bytearray(BLOCK_SIZE)
                    count = min(PTRS_PER_BLOCK, n_blocks_needed - idx)
                    for i in range(count):
                        struct.pack_into('<I', l1_data, i * 4, block_list[idx + i])
                    self.write_block(l1_blk, l1_data)
                    idx += count
                    l1_idx += 1
                self.write_block(l2_blk, l2_data)
                l2_idx += 1
            self.write_block(triple_ind_blk, tind_data)

        assert idx == n_blocks_needed, f"Block pointer mismatch: {idx} != {n_blocks_needed}"

        # Write inode
        inode_data = self.pack_inode(
            TYPE_FILE, 0x07, file_size, n_blocks_needed,
            direct, indirect_blk, double_ind_blk, triple_ind_blk,
            parent_ino
        )
        self.write_inode(ino, inode_data)

        # Add directory entry to parent
        self.add_dentry(parent_ino, filename, ino, TYPE_FILE)
        return ino

    def add_dentry(self, dir_ino, name, child_ino, file_type):
        inode_raw = self.read_inode(dir_ino)
        itype, mode, size, block_count = struct.unpack_from('<HHI I', inode_raw, 0)
        directs = []
        for i in range(MAX_DIRECT):
            directs.append(struct.unpack_from('<I', inode_raw, 12 + i * 4)[0])

        entry_idx = size // DENTRY_SIZE
        block_idx = entry_idx // DENTRIES_PER_BLOCK
        slot_in_block = entry_idx % DENTRIES_PER_BLOCK

        if block_idx < len(directs) and directs[block_idx] != 0:
            data_blk = directs[block_idx]
        else:
            data_blk = self.alloc_block()
            self.write_block(data_blk, bytearray(BLOCK_SIZE))
            if block_idx < MAX_DIRECT:
                directs[block_idx] = data_blk
            block_count += 1

        dentry = self.pack_dentry(child_ino, name, file_type)
        self.fp.seek(data_blk * BLOCK_SIZE + slot_in_block * DENTRY_SIZE)
        self.fp.write(dentry)

        size += DENTRY_SIZE
        new_inode = self.pack_inode(itype, mode, size, block_count,
                                    directs, 0, 0, 0, dir_ino)
        self.write_inode(dir_ino, new_inode)

    def finalize(self):
        # Superblock (block 0)
        super_data = struct.pack('<IIIIIIII',
                                 LIMNFS_MAGIC,
                                 self.total_blocks,
                                 MAX_INODES,
                                 BLOCK_SIZE,
                                 self.inode_tbl_start,
                                 self.data_start,
                                 self.free_blocks,
                                 self.free_inodes)
        super_data += b'\x00' * (BLOCK_SIZE - len(super_data))
        self.write_block(0, super_data)

        # Block bitmap
        for i in range(self.bitmap_blocks):
            chunk = self.block_bitmap[i * BLOCK_SIZE:(i + 1) * BLOCK_SIZE]
            self.write_block(1 + i, chunk)

        # Inode bitmap
        inode_bmp_blk = 1 + self.bitmap_blocks
        inode_bmp_data = self.inode_bitmap + bytearray(BLOCK_SIZE - len(self.inode_bitmap))
        self.write_block(inode_bmp_blk, inode_bmp_data)

        self.fp.flush()

    def create_root(self):
        ino = self.alloc_inode()
        assert ino == 0
        inode_data = self.pack_inode(TYPE_DIR, 0x07, 0, 0, [], 0, 0, 0, 0)
        self.write_inode(0, inode_data)
        return 0


def main():
    parser = argparse.ArgumentParser(description="Create LimnFS disk image with files")
    parser.add_argument("-o", "--output", required=True, help="Output disk image path")
    parser.add_argument("-s", "--size", type=int, default=0,
                        help="Disk size in MB (0 = auto-fit)")
    parser.add_argument("files", nargs="*",
                        help="host_path:fs_name pairs (e.g. model.gguf:model.gguf)")
    args = parser.parse_args()

    file_pairs = []
    for spec in args.files:
        if ':' in spec:
            host_path, fs_name = spec.split(':', 1)
        else:
            host_path = spec
            fs_name = os.path.basename(spec)
        if not os.path.exists(host_path):
            print(f"Error: {host_path} not found")
            sys.exit(1)
        file_pairs.append((host_path, fs_name))

    total_file_bytes = sum(os.path.getsize(p) for p, _ in file_pairs)
    needed_bytes = int(total_file_bytes * 1.02) + 16 * 1024 * 1024
    needed_mb = (needed_bytes + 1024 * 1024 - 1) // (1024 * 1024)

    if args.size > 0:
        disk_mb = args.size
    else:
        disk_mb = max(needed_mb, 64)

    total_blocks = disk_mb * 256

    print(f"Creating LimnFS image: {disk_mb} MB ({total_blocks} blocks)")
    print(f"  Files: {len(file_pairs)}, total {total_file_bytes:,} bytes")

    fs = LimnFSImage(args.output, total_blocks)
    root_ino = fs.create_root()

    for host_path, fs_name in file_pairs:
        print(f"  Loading: {host_path} -> /{fs_name}")
        fs.write_file(root_ino, fs_name, host_path)

    fs.finalize()
    fs.close()

    print(f"  Free: {fs.free_blocks} blocks, {fs.free_inodes} inodes")
    print(f"Done: {args.output} ({disk_mb} MB)")


if __name__ == "__main__":
    main()
