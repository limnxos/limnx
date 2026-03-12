/*
 * s48test.c — Stage 48 Tests: Bcache Thread Safety + Agent-to-Agent Delegation
 *
 * Phase 1: Bcache locking (concurrent file I/O under SMP)
 * Phase 2: Agent delegation (kernel-routed messages with token passing)
 */

#include "libc/libc.h"

static int test_num = 0;
static int pass_count = 0;

static void check(int ok, const char *desc) {
    test_num++;
    if (ok) {
        printf("  [%d] PASS: %s\n", test_num, desc);
        pass_count++;
    } else {
        printf("  [%d] FAIL: %s\n", test_num, desc);
    }
}

/* ============================================================
 * Phase 1: Bcache Thread Safety
 * ============================================================ */

static void test_concurrent_file_io(void) {
    /* Parent and child both write to different files concurrently.
     * This exercises bcache_write from two CPUs under SMP. */
    const char *file1 = "/tmp/bctest1.txt";
    const char *file2 = "/tmp/bctest2.txt";

    /* Create directories */
    sys_mkdir("/tmp");

    /* Create files */
    sys_create(file1);
    sys_create(file2);

    long child_pid = sys_fork();
    if (child_pid == 0) {
        /* Child: write to file2 */
        long fd = sys_open(file2, O_WRONLY);
        if (fd >= 0) {
            const char *data = "child-data-written-to-file2";
            sys_fwrite(fd, data, 27);
            sys_close(fd);
        }
        sys_exit(0);
    }

    /* Parent: write to file1 */
    long fd1 = sys_open(file1, O_WRONLY);
    if (fd1 >= 0) {
        const char *data = "parent-data-written-to-file1";
        sys_fwrite(fd1, data, 28);
        sys_close(fd1);
    }

    sys_waitpid(child_pid);

    /* Test 1: Read back file1 */
    char buf1[64];
    memset(buf1, 0, sizeof(buf1));
    long fd = sys_open(file1, O_RDONLY);
    long n = 0;
    if (fd >= 0) {
        n = sys_read(fd, buf1, sizeof(buf1) - 1);
        sys_close(fd);
    }
    check(n == 28 && strncmp(buf1, "parent-data-written-to-file1", 28) == 0,
          "concurrent write: parent file intact");

    /* Test 2: Read back file2 */
    char buf2[64];
    memset(buf2, 0, sizeof(buf2));
    fd = sys_open(file2, O_RDONLY);
    n = 0;
    if (fd >= 0) {
        n = sys_read(fd, buf2, sizeof(buf2) - 1);
        sys_close(fd);
    }
    check(n == 27 && strncmp(buf2, "child-data-written-to-file2", 27) == 0,
          "concurrent write: child file intact");

    /* Cleanup */
    sys_unlink(file1);
    sys_unlink(file2);
}

static void test_concurrent_read(void) {
    /* Both parent and child read the same file simultaneously */
    const char *file = "/tmp/bcread.txt";
    sys_create(file);
    long fd = sys_open(file, O_WRONLY);
    if (fd >= 0) {
        const char *data = "shared-read-data-for-testing";
        sys_fwrite(fd, data, 28);
        sys_close(fd);
    }

    long child_pid = sys_fork();
    if (child_pid == 0) {
        /* Child: read the file */
        char buf[64];
        long rfd = sys_open(file, O_RDONLY);
        if (rfd >= 0) {
            long n = sys_read(rfd, buf, 28);
            sys_close(rfd);
            /* Exit with 0 if data matches, 1 if not */
            if (n == 28 && strncmp(buf, "shared-read-data-for-testing", 28) == 0)
                sys_exit(0);
        }
        sys_exit(1);
    }

    /* Parent: also read the file */
    char buf[64];
    fd = sys_open(file, O_RDONLY);
    long n = 0;
    if (fd >= 0) {
        n = sys_read(fd, buf, 28);
        sys_close(fd);
    }

    long child_status = sys_waitpid(child_pid);

    /* Test 3: Both reads got correct data */
    check(n == 28 && strncmp(buf, "shared-read-data-for-testing", 28) == 0 &&
          child_status == 0,
          "concurrent read: both parent and child read correct data");

    sys_unlink(file);
}

static void test_bcache_flush_concurrent(void) {
    /* Write a file, then fork. Child does more writes while parent triggers
     * a read (which may cause bcache eviction). Tests locking during LRU eviction. */
    sys_create("/tmp/bcflush.txt");
    long fd = sys_open("/tmp/bcflush.txt", O_WRONLY);
    if (fd >= 0) {
        const char *data = "flush-test-initial-data";
        sys_fwrite(fd, data, 23);
        sys_close(fd);
    }

    long child_pid = sys_fork();
    if (child_pid == 0) {
        /* Child: overwrite the file */
        long wfd = sys_open("/tmp/bcflush.txt", O_WRONLY);
        if (wfd >= 0) {
            const char *data = "flush-test-updated-data";
            sys_fwrite(wfd, data, 23);
            sys_close(wfd);
        }
        sys_exit(0);
    }

    /* Parent: yield to let child run, then read */
    for (int i = 0; i < 10; i++)
        sys_yield();

    sys_waitpid(child_pid);

    char buf[64];
    memset(buf, 0, sizeof(buf));
    fd = sys_open("/tmp/bcflush.txt", O_RDONLY);
    long n = 0;
    if (fd >= 0) {
        n = sys_read(fd, buf, sizeof(buf) - 1);
        sys_close(fd);
    }

    /* Test 4: Data is consistent (either initial or updated, not corrupted) */
    int is_initial = (n == 23 && strncmp(buf, "flush-test-initial-data", 23) == 0);
    int is_updated = (n == 23 && strncmp(buf, "flush-test-updated-data", 23) == 0);
    check(is_initial || is_updated,
          "bcache flush: data consistent after concurrent write");

    sys_unlink("/tmp/bcflush.txt");
}

/* ============================================================
 * Phase 2: Agent-to-Agent Delegation
 * ============================================================ */

static void agent_b_process(void) {
    /* Register as "agent-b", receive messages */
    sys_agent_register("agent-b");

    /* Wait for messages (yield-based polling) */
    char buf[256];
    long sender_pid = 0;
    long token_id = 0;
    int got_msg = 0;

    for (int attempt = 0; attempt < 100; attempt++) {
        long r = sys_agent_recv(buf, sizeof(buf), &sender_pid, &token_id);
        if (r > 0) {
            got_msg = 1;
            break;
        }
        sys_yield();
    }

    /* Exit with encoded result:
     * 0 = got message with correct content
     * 1 = no message received
     * 2 = wrong content */
    if (!got_msg) sys_exit(1);
    if (strncmp(buf, "hello-from-a", 12) != 0) sys_exit(2);
    sys_exit(0);
}

static void test_basic_send_recv(void) {
    /* Fork agent B */
    long b_pid = sys_fork();
    if (b_pid == 0) {
        agent_b_process();
        sys_exit(99); /* unreachable */
    }

    /* Register as agent A */
    sys_agent_register("agent-a");

    /* Give agent B time to register */
    for (int i = 0; i < 20; i++)
        sys_yield();

    /* Test 5: Send message to agent-b */
    long r = sys_agent_send("agent-b", "hello-from-a", 12, 0);
    check(r == 0, "agent A sends message to agent B");

    /* Wait for agent B to process and exit */
    long status = sys_waitpid(b_pid);

    /* Test 6: Agent B received correct message */
    check(status == 0, "agent B received message with correct content");
}

static void test_send_recv_with_sender_pid(void) {
    /* Fork agent that checks sender_pid */
    long b_pid = sys_fork();
    if (b_pid == 0) {
        sys_agent_register("checker-b");
        char buf[256];
        long sender_pid = 0;
        long token_id = 0;
        for (int attempt = 0; attempt < 100; attempt++) {
            long r = sys_agent_recv(buf, sizeof(buf), &sender_pid, &token_id);
            if (r > 0) {
                /* Exit 0 if sender_pid is non-zero (valid parent pid) */
                sys_exit(sender_pid > 0 ? 0 : 1);
            }
            sys_yield();
        }
        sys_exit(2); /* timeout */
    }

    sys_agent_register("checker-a");
    for (int i = 0; i < 20; i++) sys_yield();

    sys_agent_send("checker-b", "check-sender", 12, 0);
    long status = sys_waitpid(b_pid);

    /* Test 7: Sender PID correctly delivered */
    check(status == 0, "received message contains correct sender PID");
}

static void test_token_delegation(void) {
    /* Create a capability token, send it to agent B via delegation */
    long b_pid = sys_fork();
    if (b_pid == 0) {
        sys_agent_register("token-recv");
        char buf[256];
        long sender_pid = 0;
        long token_id = 0;
        for (int attempt = 0; attempt < 100; attempt++) {
            long r = sys_agent_recv(buf, sizeof(buf), &sender_pid, &token_id);
            if (r > 0) {
                /* Exit with token_id > 0 means we got a delegated token */
                sys_exit(token_id > 0 ? 0 : 1);
            }
            sys_yield();
        }
        sys_exit(2);
    }

    sys_agent_register("token-send");

    /* Create a parent token with FS_READ capability */
    long tok = sys_token_create(0x80, 0, "/data/");  /* CAP_FS_READ */
    check(tok > 0, "create token for delegation");

    for (int i = 0; i < 20; i++) sys_yield();

    /* Test 8 (above): token created */

    /* Send message with token delegation */
    long r = sys_agent_send("token-recv", "do-work", 7, (long)tok);
    check(r == 0, "agent sends message with token delegation");

    long status = sys_waitpid(b_pid);

    /* Test 10: Recipient got delegated token */
    check(status == 0, "recipient received delegated token ID");

    /* Cleanup */
    sys_token_revoke(tok);
}

static void test_mailbox_full(void) {
    /* Register an agent, send 4 messages (fills mailbox), 5th should fail */
    long b_pid = sys_fork();
    if (b_pid == 0) {
        sys_agent_register("full-test");
        /* Don't receive — let mailbox fill up */
        for (int i = 0; i < 100; i++) sys_yield();
        sys_exit(0);
    }

    sys_agent_register("full-sender");
    for (int i = 0; i < 20; i++) sys_yield();

    /* Send 4 messages (should all succeed) */
    int ok_count = 0;
    for (int i = 0; i < 4; i++) {
        long r = sys_agent_send("full-test", "msg", 3, 0);
        if (r == 0) ok_count++;
    }

    /* Test 11: First 4 messages accepted */
    check(ok_count == 4, "mailbox accepts 4 messages");

    /* 5th message should fail */
    long r = sys_agent_send("full-test", "overflow", 8, 0);

    /* Test 12: 5th message rejected (mailbox full) */
    check(r != 0, "mailbox rejects 5th message when full");

    sys_kill(b_pid, 9);
    sys_waitpid(b_pid);
}

static void test_nonexistent_agent(void) {
    /* Test 13: Send to nonexistent agent fails */
    long r = sys_agent_send("no-such-agent", "hello", 5, 0);
    check(r != 0, "send to nonexistent agent returns error");
}

static void test_recv_empty(void) {
    /* Test 14: Recv from empty mailbox returns EAGAIN */
    sys_agent_register("empty-recv");
    long sender = 0, token = 0;
    char buf[64];
    long r = sys_agent_recv(buf, sizeof(buf), &sender, &token);
    check(r < 0, "recv from empty mailbox returns error");
}

int main(void) {
    printf("=== Stage 48 Tests: Bcache Thread Safety + Agent Delegation ===\n\n");

    printf("--- Phase 1: Bcache Thread Safety ---\n");
    test_concurrent_file_io();
    test_concurrent_read();
    test_bcache_flush_concurrent();

    printf("\n--- Phase 2: Agent-to-Agent Delegation ---\n");
    test_basic_send_recv();
    test_send_recv_with_sender_pid();
    test_token_delegation();
    test_mailbox_full();
    test_nonexistent_agent();
    test_recv_empty();

    printf("\n=== Stage 48 Results: %d/%d passed ===\n", pass_count, test_num);
    return 0;
}
