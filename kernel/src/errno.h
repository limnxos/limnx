/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LIMNX_ERRNO_H
#define LIMNX_ERRNO_H

/*
 * Kernel error codes — single source of truth.
 * Modeled after Linux include/uapi/asm-generic/errno-base.h + errno.h
 *
 * Syscall handlers return negative errno (e.g., -EINVAL).
 * Internal kernel functions follow the same convention.
 * NEVER return raw -1 — always use a named constant.
 */

/* Base errno (Linux errno-base.h equivalent, codes 1-34) */
#define EPERM            1  /* Operation not permitted */
#define ENOENT           2  /* No such file or directory */
#define ESRCH            3  /* No such process */
#define EINTR            4  /* Interrupted system call */
#define EIO              5  /* I/O error */
#define ENXIO            6  /* No such device or address */
#define EBADF            9  /* Bad file descriptor */
#define EAGAIN          11  /* Try again / resource temporarily unavailable */
#define ENOMEM          12  /* Out of memory */
#define EACCES          13  /* Permission denied */
#define EFAULT          14  /* Bad address */
#define EBUSY           16  /* Device or resource busy */
#define EEXIST          17  /* File exists */
#define ENODEV          19  /* No such device */
#define ENOTDIR         20  /* Not a directory */
#define EISDIR          21  /* Is a directory */
#define EINVAL          22  /* Invalid argument */
#define EMFILE          24  /* Too many open files */
#define ENOSPC          28  /* No space left on device */
#define ESPIPE          29  /* Illegal seek */
#define ENOEXEC          8  /* Exec format error */
#define EPIPE           32  /* Broken pipe */

/* Extended errno (Linux errno.h equivalent, codes 35+) */
#define ELOOP           40  /* Too many levels of symbolic links */
#define ENAMETOOLONG    36  /* File name too long */
#define ENOSYS          38  /* Function not implemented */
#define ENOTEMPTY       39  /* Directory not empty */
#define EMSGSIZE        90  /* Message too long */
#define EADDRINUSE      98  /* Address already in use */
#define ENOBUFS        105  /* No buffer space available */
#define ENOTCONN       107  /* Transport endpoint not connected */
#define ETIMEDOUT      110  /* Connection timed out */
#define ECONNREFUSED   111  /* Connection refused */

#endif
