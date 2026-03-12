#ifndef LIMNX_PUBSUB_H
#define LIMNX_PUBSUB_H

#include <stdint.h>

#define MAX_TOPICS         16
#define MAX_SUBSCRIBERS     8   /* per topic */
#define TOPIC_NAME_MAX     32
#define TOPIC_MSG_MAX     256
#define TOPIC_QUEUE_SLOTS   4   /* per-subscriber queue depth */

typedef struct topic_msg {
    uint64_t publisher_pid;
    uint32_t len;
    uint8_t  data[TOPIC_MSG_MAX];
} topic_msg_t;

typedef struct topic_sub {
    uint64_t   pid;
    uint32_t   ns_id;
    topic_msg_t msgs[TOPIC_QUEUE_SLOTS];
    uint32_t   head;
    uint32_t   tail;
    uint32_t   count;
    uint8_t    used;
} topic_sub_t;

typedef struct topic {
    char        name[TOPIC_NAME_MAX];
    uint64_t    owner_pid;
    uint32_t    ns_id;
    topic_sub_t subs[MAX_SUBSCRIBERS];
    uint8_t     used;
} topic_t;

/* Create a named topic. Returns topic_id or -errno. */
int pubsub_topic_create(const char *name, uint32_t ns_id, uint64_t owner_pid);

/* Subscribe to a topic. Returns 0 or -errno. */
int pubsub_subscribe(uint32_t topic_id, uint64_t pid, uint32_t sub_ns_id,
                     uint32_t caller_caps);

/* Publish a message to all subscribers. Returns subscriber count or -errno. */
int pubsub_publish(uint32_t topic_id, uint64_t publisher_pid, uint32_t pub_ns_id,
                   uint32_t caller_caps, const uint8_t *data, uint32_t len);

/* Receive next message. Returns message length or -EAGAIN. */
int pubsub_recv(uint32_t topic_id, uint64_t pid,
                uint64_t *publisher_pid_out, uint8_t *data, uint32_t max_len);

/* Cleanup topics/subscriptions for dying process. */
void pubsub_cleanup_pid(uint64_t pid);

#endif
