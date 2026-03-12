#define pr_fmt(fmt) "[pubsub] " fmt
#include "klog.h"

#include "ipc/pubsub.h"
#include "ipc/cap_token.h"
#include "syscall/syscall.h"
#include "sync/spinlock.h"
#include "errno.h"

static topic_t topics[MAX_TOPICS];

/* Lock order: tier 5 (subsystem). See klog.h for full hierarchy */
static spinlock_t pubsub_lock = SPINLOCK_INIT;

/* Check cross-namespace access. Must be called WITHOUT pubsub_lock held. */
static int check_xns_access(uint32_t topic_ns, uint32_t caller_ns,
                             uint64_t caller_pid, uint32_t caller_caps) {
    if (caller_ns == topic_ns) return 1;  /* same namespace */
    if (topic_ns == 0) return 1;          /* global namespace */
    if (caller_caps & CAP_XNS_PUBSUB) return 1;

    /* Check capability token */
    char res[16];
    res[0] = 'n'; res[1] = 's'; res[2] = '/';
    if (topic_ns < 10) {
        res[3] = '0' + (char)topic_ns;
        res[4] = '\0';
    } else {
        res[3] = '0' + (char)(topic_ns / 10);
        res[4] = '0' + (char)(topic_ns % 10);
        res[5] = '\0';
    }
    return cap_token_check(caller_pid, CAP_XNS_PUBSUB, res);
}

int pubsub_topic_create(const char *name, uint32_t ns_id, uint64_t owner_pid) {
    uint64_t flags;
    spin_lock_irqsave(&pubsub_lock, &flags);

    /* Check for duplicate name in same namespace */
    for (int i = 0; i < MAX_TOPICS; i++) {
        if (!topics[i].used) continue;
        if (topics[i].ns_id != ns_id) continue;
        int match = 1;
        for (int j = 0; j < TOPIC_NAME_MAX; j++) {
            if (topics[i].name[j] != name[j]) { match = 0; break; }
            if (name[j] == '\0') break;
        }
        if (match) {
            spin_unlock_irqrestore(&pubsub_lock, flags);
            return -EEXIST;
        }
    }

    for (int i = 0; i < MAX_TOPICS; i++) {
        if (!topics[i].used) {
            topics[i].used = 1;
            topics[i].owner_pid = owner_pid;
            topics[i].ns_id = ns_id;
            int j = 0;
            for (; j < TOPIC_NAME_MAX - 1 && name[j]; j++)
                topics[i].name[j] = name[j];
            topics[i].name[j] = '\0';
            for (int s = 0; s < MAX_SUBSCRIBERS; s++) {
                topics[i].subs[s].used = 0;
                topics[i].subs[s].head = 0;
                topics[i].subs[s].tail = 0;
                topics[i].subs[s].count = 0;
            }
            spin_unlock_irqrestore(&pubsub_lock, flags);
            pr_info("topic '%s' created (id=%d, ns=%u)\n", name, i, ns_id);
            return i;
        }
    }

    spin_unlock_irqrestore(&pubsub_lock, flags);
    pr_err("topic table full\n");
    return -ENOMEM;
}

int pubsub_subscribe(uint32_t topic_id, uint64_t pid, uint32_t sub_ns_id,
                     uint32_t caller_caps) {
    if (topic_id >= MAX_TOPICS) return -EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&pubsub_lock, &flags);

    if (!topics[topic_id].used) {
        spin_unlock_irqrestore(&pubsub_lock, flags);
        return -ENOENT;
    }

    /* Cross-namespace check — release lock first */
    uint32_t topic_ns = topics[topic_id].ns_id;
    if (sub_ns_id != topic_ns && topic_ns != 0) {
        spin_unlock_irqrestore(&pubsub_lock, flags);
        if (!check_xns_access(topic_ns, sub_ns_id, pid, caller_caps))
            return -EPERM;
        /* Re-acquire and re-validate */
        spin_lock_irqsave(&pubsub_lock, &flags);
        if (!topics[topic_id].used) {
            spin_unlock_irqrestore(&pubsub_lock, flags);
            return -ENOENT;
        }
    }

    /* Check for duplicate subscription */
    topic_t *tp = &topics[topic_id];
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (tp->subs[i].used && tp->subs[i].pid == pid) {
            spin_unlock_irqrestore(&pubsub_lock, flags);
            return 0; /* already subscribed */
        }
    }

    /* Find free slot */
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (!tp->subs[i].used) {
            tp->subs[i].used = 1;
            tp->subs[i].pid = pid;
            tp->subs[i].ns_id = sub_ns_id;
            tp->subs[i].head = 0;
            tp->subs[i].tail = 0;
            tp->subs[i].count = 0;
            spin_unlock_irqrestore(&pubsub_lock, flags);
            return 0;
        }
    }

    spin_unlock_irqrestore(&pubsub_lock, flags);
    return -ENOMEM;
}

int pubsub_publish(uint32_t topic_id, uint64_t publisher_pid, uint32_t pub_ns_id,
                   uint32_t caller_caps, const uint8_t *data, uint32_t len) {
    if (topic_id >= MAX_TOPICS) return -EINVAL;
    if (len > TOPIC_MSG_MAX) return -EMSGSIZE;

    uint64_t flags;
    spin_lock_irqsave(&pubsub_lock, &flags);

    if (!topics[topic_id].used) {
        spin_unlock_irqrestore(&pubsub_lock, flags);
        return -ENOENT;
    }

    /* Cross-namespace check */
    uint32_t topic_ns = topics[topic_id].ns_id;
    if (pub_ns_id != topic_ns && topic_ns != 0) {
        spin_unlock_irqrestore(&pubsub_lock, flags);
        if (!check_xns_access(topic_ns, pub_ns_id, publisher_pid, caller_caps))
            return -EPERM;
        spin_lock_irqsave(&pubsub_lock, &flags);
        if (!topics[topic_id].used) {
            spin_unlock_irqrestore(&pubsub_lock, flags);
            return -ENOENT;
        }
    }

    /* Deliver to all active subscribers */
    topic_t *tp = &topics[topic_id];
    int delivered = 0;
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (!tp->subs[i].used) continue;

        topic_sub_t *sub = &tp->subs[i];
        if (sub->count >= TOPIC_QUEUE_SLOTS) {
            /* Queue full — drop oldest */
            sub->head = (sub->head + 1) % TOPIC_QUEUE_SLOTS;
            sub->count--;
        }

        topic_msg_t *msg = &sub->msgs[sub->tail];
        msg->publisher_pid = publisher_pid;
        msg->len = len;
        for (uint32_t j = 0; j < len; j++)
            msg->data[j] = data[j];

        sub->tail = (sub->tail + 1) % TOPIC_QUEUE_SLOTS;
        sub->count++;
        delivered++;
    }

    spin_unlock_irqrestore(&pubsub_lock, flags);
    return delivered;
}

int pubsub_recv(uint32_t topic_id, uint64_t pid,
                uint64_t *publisher_pid_out, uint8_t *data, uint32_t max_len) {
    if (topic_id >= MAX_TOPICS) return -EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&pubsub_lock, &flags);

    if (!topics[topic_id].used) {
        spin_unlock_irqrestore(&pubsub_lock, flags);
        return -ENOENT;
    }

    /* Find this pid's subscription */
    topic_t *tp = &topics[topic_id];
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (!tp->subs[i].used || tp->subs[i].pid != pid)
            continue;

        topic_sub_t *sub = &tp->subs[i];
        if (sub->count == 0) {
            spin_unlock_irqrestore(&pubsub_lock, flags);
            return -EAGAIN;
        }

        topic_msg_t *msg = &sub->msgs[sub->head];
        uint32_t copy_len = msg->len;
        if (copy_len > max_len) copy_len = max_len;

        /* Copy data out under lock */
        if (publisher_pid_out)
            *publisher_pid_out = msg->publisher_pid;
        for (uint32_t j = 0; j < copy_len; j++)
            data[j] = msg->data[j];

        sub->head = (sub->head + 1) % TOPIC_QUEUE_SLOTS;
        sub->count--;

        spin_unlock_irqrestore(&pubsub_lock, flags);
        return (int)copy_len;
    }

    spin_unlock_irqrestore(&pubsub_lock, flags);
    return -ENOENT; /* not subscribed */
}

void pubsub_cleanup_pid(uint64_t pid) {
    uint64_t flags;
    spin_lock_irqsave(&pubsub_lock, &flags);

    for (int i = 0; i < MAX_TOPICS; i++) {
        if (!topics[i].used) continue;

        /* Remove subscriptions by this pid */
        for (int s = 0; s < MAX_SUBSCRIBERS; s++) {
            if (topics[i].subs[s].used && topics[i].subs[s].pid == pid)
                topics[i].subs[s].used = 0;
        }

        /* Remove topic if owned by this pid */
        if (topics[i].owner_pid == pid)
            topics[i].used = 0;
    }

    spin_unlock_irqrestore(&pubsub_lock, flags);
}
