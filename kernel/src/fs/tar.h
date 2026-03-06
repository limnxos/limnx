#ifndef LIMNX_TAR_H
#define LIMNX_TAR_H

#include <stdint.h>

typedef struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];          /* octal ASCII */
    char mtime[12];
    char checksum[8];
    char typeflag;          /* '0' or '\0' = regular file, '5' = directory */
    char linkname[100];
    char magic[6];          /* "ustar" */
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
} __attribute__((packed)) tar_header_t;

/* Parse a tar archive and register all files with the VFS.
 * Returns the number of files found, or -1 on error. */
int tar_init(const void *archive, uint64_t size);

#endif
