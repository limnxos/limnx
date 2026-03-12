#define pr_fmt(fmt) "[bcache] " fmt
#include "klog.h"
#include "blk/bcache.h"
#include "blk/virtio_blk.h"
#include "sched/sched.h"
#include "sched/thread.h"
#include "sync/spinlock.h"
#include "arch/serial.h"

#define SECTORS_PER_BLOCK (BLOCK_SIZE / VIRTIO_BLK_SECTOR_SIZE)  /* 8 */
#define HASH_BUCKETS 256

typedef struct {
    uint8_t  data[BLOCK_SIZE];
    uint32_t tag;
    uint8_t  valid;
    uint8_t  dirty;      /* 1 = modified, needs write-back before eviction */
    /* DLL for LRU ordering */
    int16_t  lru_prev;   /* -1 = none (head) */
    int16_t  lru_next;   /* -1 = none (tail) */
    /* Hash chain */
    int16_t  hash_next;  /* -1 = end of chain */
} bcache_entry_t;

static bcache_entry_t cache[BCACHE_ENTRIES];
static int16_t lru_head = -1;   /* most recently used */
static int16_t lru_tail = -1;   /* least recently used (eviction candidate) */
static int16_t hash_table[HASH_BUCKETS];  /* head of each hash chain, -1 = empty */
static uint64_t cache_hits = 0;
static uint64_t cache_misses = 0;
/*
 * Lock ordering: bcache_lock is at level 4 (subsystem).
 * Must NOT hold sched_lock, pmm_lock, or kheap_lock when acquiring.
 * Does NOT call kmalloc or pmm_alloc while held.
 * May do virtio-blk I/O while held (write-back on eviction).
 */
static spinlock_t bcache_lock = SPINLOCK_INIT;

/* --- DLL helpers --- */

static void dll_unlink(int16_t idx) {
    bcache_entry_t *e = &cache[idx];
    if (e->lru_prev >= 0)
        cache[e->lru_prev].lru_next = e->lru_next;
    else
        lru_head = e->lru_next;
    if (e->lru_next >= 0)
        cache[e->lru_next].lru_prev = e->lru_prev;
    else
        lru_tail = e->lru_prev;
    e->lru_prev = -1;
    e->lru_next = -1;
}

static void dll_push_head(int16_t idx) {
    bcache_entry_t *e = &cache[idx];
    e->lru_prev = -1;
    e->lru_next = lru_head;
    if (lru_head >= 0)
        cache[lru_head].lru_prev = idx;
    lru_head = idx;
    if (lru_tail < 0)
        lru_tail = idx;
}

/* --- Hash helpers --- */

static uint32_t hash_fn(uint32_t block_no) {
    return block_no % HASH_BUCKETS;
}

static int16_t hash_find(uint32_t block_no) {
    uint32_t bucket = hash_fn(block_no);
    int16_t idx = hash_table[bucket];
    while (idx >= 0) {
        if (cache[idx].valid && cache[idx].tag == block_no)
            return idx;
        idx = cache[idx].hash_next;
    }
    return -1;
}

static void hash_insert(int16_t idx) {
    uint32_t bucket = hash_fn(cache[idx].tag);
    cache[idx].hash_next = hash_table[bucket];
    hash_table[bucket] = idx;
}

static void hash_remove(int16_t idx) {
    uint32_t bucket = hash_fn(cache[idx].tag);
    if (hash_table[bucket] == idx) {
        hash_table[bucket] = cache[idx].hash_next;
    } else {
        int16_t prev = hash_table[bucket];
        while (prev >= 0 && cache[prev].hash_next != idx)
            prev = cache[prev].hash_next;
        if (prev >= 0)
            cache[prev].hash_next = cache[idx].hash_next;
    }
    cache[idx].hash_next = -1;
}

/* Write a dirty entry back to disk */
static int writeback_entry(int16_t idx) {
    bcache_entry_t *e = &cache[idx];
    if (!e->valid || !e->dirty)
        return 0;
    uint64_t sector = (uint64_t)e->tag * SECTORS_PER_BLOCK;
    for (int s = 0; s < SECTORS_PER_BLOCK; s++) {
        if (virtio_blk_write(sector + s, e->data + s * VIRTIO_BLK_SECTOR_SIZE) != 0)
            return -1;
    }
    e->dirty = 0;
    return 0;
}

/* --- Public API --- */

void bcache_init(void) {
    for (int i = 0; i < BCACHE_ENTRIES; i++) {
        cache[i].tag = 0;
        cache[i].valid = 0;
        cache[i].dirty = 0;
        cache[i].lru_prev = -1;
        cache[i].lru_next = -1;
        cache[i].hash_next = -1;
    }
    for (int i = 0; i < HASH_BUCKETS; i++)
        hash_table[i] = -1;
    lru_head = -1;
    lru_tail = -1;

    /* Link all entries into LRU list (all invalid, available for eviction) */
    for (int i = 0; i < BCACHE_ENTRIES; i++)
        dll_push_head((int16_t)i);

    cache_hits = 0;
    cache_misses = 0;
    pr_info("Block cache initialized (256 entries, 4KB each, DLL-LRU)\n");
}

int bcache_read(uint32_t block_no, void *buf) {
    spin_lock(&bcache_lock);

    /* Hash lookup for cache hit */
    int16_t idx = hash_find(block_no);
    if (idx >= 0) {
        /* Hit: promote to MRU */
        cache_hits++;
        dll_unlink(idx);
        dll_push_head(idx);
        uint8_t *src = cache[idx].data;
        uint8_t *dst = (uint8_t *)buf;
        for (int j = 0; j < BLOCK_SIZE; j++)
            dst[j] = src[j];
        spin_unlock(&bcache_lock);
        return 0;
    }

    /* Miss: evict LRU tail */
    cache_misses++;
    int16_t victim = lru_tail;
    if (victim < 0) {
        spin_unlock(&bcache_lock);
        return -1;  /* should never happen */
    }

    /* Write back dirty victim before eviction */
    if (cache[victim].valid && cache[victim].dirty)
        writeback_entry(victim);

    /* Remove old entry from hash if valid */
    if (cache[victim].valid)
        hash_remove(victim);

    /* Read from disk into victim slot */
    uint64_t sector = (uint64_t)block_no * SECTORS_PER_BLOCK;
    uint8_t *dst = cache[victim].data;
    for (int s = 0; s < SECTORS_PER_BLOCK; s++) {
        if (virtio_blk_read(sector + s, dst + s * VIRTIO_BLK_SECTOR_SIZE) != 0) {
            spin_unlock(&bcache_lock);
            return -1;
        }
    }
    cache[victim].tag = block_no;
    cache[victim].valid = 1;
    cache[victim].dirty = 0;

    /* Insert into hash and promote to MRU */
    hash_insert(victim);
    dll_unlink(victim);
    dll_push_head(victim);

    /* Copy to caller buffer */
    uint8_t *out = (uint8_t *)buf;
    for (int j = 0; j < BLOCK_SIZE; j++)
        out[j] = dst[j];

    spin_unlock(&bcache_lock);
    return 0;
}

int bcache_write(uint32_t block_no, const void *buf) {
    const uint8_t *src = (const uint8_t *)buf;
    spin_lock(&bcache_lock);

    /* Update cache — write-back: mark dirty, don't write to disk */
    int16_t idx = hash_find(block_no);
    if (idx >= 0) {
        /* Hit: update data, mark dirty, promote to MRU */
        uint8_t *cdst = cache[idx].data;
        for (int i = 0; i < BLOCK_SIZE; i++)
            cdst[i] = src[i];
        cache[idx].dirty = 1;
        dll_unlink(idx);
        dll_push_head(idx);
    } else {
        /* Miss: evict LRU tail */
        int16_t victim = lru_tail;
        if (victim < 0) {
            /* No cache slots — must write directly to disk */
            uint64_t sector = (uint64_t)block_no * SECTORS_PER_BLOCK;
            for (int s = 0; s < SECTORS_PER_BLOCK; s++) {
                if (virtio_blk_write(sector + s,
                        src + s * VIRTIO_BLK_SECTOR_SIZE) != 0) {
                    spin_unlock(&bcache_lock);
                    return -1;
                }
            }
            spin_unlock(&bcache_lock);
            return 0;
        }

        /* Write back dirty victim before eviction */
        if (cache[victim].valid && cache[victim].dirty)
            writeback_entry(victim);

        if (cache[victim].valid)
            hash_remove(victim);

        uint8_t *cdst = cache[victim].data;
        for (int i = 0; i < BLOCK_SIZE; i++)
            cdst[i] = src[i];
        cache[victim].tag = block_no;
        cache[victim].valid = 1;
        cache[victim].dirty = 1;

        hash_insert(victim);
        dll_unlink(victim);
        dll_push_head(victim);
    }

    spin_unlock(&bcache_lock);
    return 0;
}

void bcache_invalidate(uint32_t block_no) {
    spin_lock(&bcache_lock);

    int16_t idx = hash_find(block_no);
    if (idx >= 0) {
        /* Write back if dirty before invalidating */
        if (cache[idx].dirty)
            writeback_entry(idx);
        hash_remove(idx);
        cache[idx].valid = 0;
        cache[idx].dirty = 0;
        /* Move to tail (available for eviction) */
        dll_unlink(idx);
        /* Push to tail instead of head */
        bcache_entry_t *e = &cache[idx];
        e->lru_next = -1;
        e->lru_prev = lru_tail;
        if (lru_tail >= 0)
            cache[lru_tail].lru_next = idx;
        lru_tail = idx;
        if (lru_head < 0)
            lru_head = idx;
    }

    spin_unlock(&bcache_lock);
}

void bcache_flush(void) {
    /* Flush one entry at a time, releasing the lock between entries
     * to avoid blocking concurrent I/O for extended periods. */
    for (int i = 0; i < BCACHE_ENTRIES; i++) {
        spin_lock(&bcache_lock);
        if (cache[i].valid && cache[i].dirty)
            writeback_entry((int16_t)i);
        spin_unlock(&bcache_lock);
    }
}

uint32_t bcache_dirty_count(void) {
    spin_lock(&bcache_lock);
    uint32_t count = 0;
    for (int i = 0; i < BCACHE_ENTRIES; i++) {
        if (cache[i].valid && cache[i].dirty)
            count++;
    }
    spin_unlock(&bcache_lock);
    return count;
}

void bcache_stats(uint64_t *hits, uint64_t *misses) {
    spin_lock(&bcache_lock);
    *hits = cache_hits;
    *misses = cache_misses;
    spin_unlock(&bcache_lock);
}

/* --- Kernel flusher thread --- */

static void bcache_flusher_fn(void) {
    pr_info("Flusher thread started\n");
    for (;;) {
        /* Yield ~500 times (~5 seconds at ~10ms LAPIC timer) */
        for (int i = 0; i < 500; i++)
            sched_yield();
        bcache_flush();
    }
}

void bcache_start_flusher(void) {
    thread_t *t = thread_create(bcache_flusher_fn, 0);
    if (t) {
        sched_add(t);
        pr_info("Flusher thread launched\n");
    } else {
        pr_warn("failed to create flusher thread\n");
    }
}
