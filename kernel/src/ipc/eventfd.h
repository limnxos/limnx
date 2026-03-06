#ifndef LIMNX_EVENTFD_H
#define LIMNX_EVENTFD_H

#include <stdint.h>

#define MAX_EVENTFDS  16

/* Flags */
#define EFD_NONBLOCK   (1 << 0)
#define EFD_SEMAPHORE  (1 << 1)

typedef struct eventfd {
    uint64_t counter;
    uint32_t refs;
    uint32_t flags;
    uint8_t  used;
} eventfd_t;

/* Allocate a new eventfd, returns index or -1 */
int  eventfd_alloc(uint32_t flags);

/* Read from eventfd: returns counter value (8 bytes), resets to 0 */
int  eventfd_read(int idx, uint64_t *value, int nonblock);

/* Write to eventfd: adds value to counter */
int  eventfd_write(int idx, uint64_t value);

/* Close and decrement refs */
void eventfd_close(int idx);

/* Increment refs */
void eventfd_ref(int idx);

/* Get eventfd by index */
eventfd_t *eventfd_get(int idx);

/* Check if readable (counter > 0) */
int  eventfd_readable(int idx);

#endif
