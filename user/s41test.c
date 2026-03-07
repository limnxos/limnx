#include "libc/libc.h"

static int pass_count = 0;
static int fail_count = 0;
static int test_num = 0;

static void PASS(const char *msg) {
    test_num++;
    pass_count++;
    printf("  [%d]  PASS: %s\n", test_num, msg);
}
static void FAIL(const char *msg) {
    test_num++;
    fail_count++;
    printf("  [%d]  FAIL: %s\n", test_num, msg);
}
static void CHECK(int cond, const char *msg) {
    if (cond) PASS(msg); else FAIL(msg);
}

int main(void) {
    printf("=== Stage 41: Capability Tokens + Agent Namespaces ===\n");

    /* --- Capability Token Tests --- */

    /* 1. token_create returns valid ID */
    {
        long id = sys_token_create(0x80 /* CAP_FS_READ */, 0, 0);
        CHECK(id > 0, "token_create returns valid ID");
        if (id > 0) sys_token_revoke(id);
    }

    /* 2. token_create denied without required cap */
    {
        /* Fork a child, drop CAP_FS_READ, try to create token with it */
        long pid = sys_fork();
        if (pid == 0) {
            /* Child: drop CAP_FS_READ (keep others) */
            long caps = sys_getcap();
            sys_setcap(0, caps & ~0x80);
            long id = sys_token_create(0x80, 0, 0);
            sys_exit(id < 0 ? 0 : 1); /* expect failure */
        }
        long st = sys_waitpid(pid);
        CHECK(st == 0, "token_create denied without required cap");
    }

    /* 3. token_create with resource path */
    {
        long id = sys_token_create(0x80, 0, "/data/");
        CHECK(id > 0, "token_create with resource path");
        if (id > 0) sys_token_revoke(id);
    }

    /* 4. token_revoke succeeds for owner */
    {
        long id = sys_token_create(0x80, 0, 0);
        long ret = sys_token_revoke(id);
        CHECK(ret == 0, "token_revoke succeeds for owner");
    }

    /* 5. token_revoke denied for non-owner */
    {
        long id = sys_token_create(0x80, 0, 0);
        long pid = sys_fork();
        if (pid == 0) {
            long ret = sys_token_revoke(id);
            sys_exit(ret < 0 ? 0 : 1); /* expect failure */
        }
        long st = sys_waitpid(pid);
        CHECK(st == 0, "token_revoke denied for non-owner");
        sys_token_revoke(id);
    }

    /* 6. token grants FS_READ to sandboxed process */
    {
        /* Create a test file */
        sys_create("/token_test.txt");
        long fd = sys_open("/token_test.txt", 1); /* O_WRONLY */
        if (fd >= 0) {
            sys_fwrite(fd, "hello", 5);
            sys_close(fd);
        }

        long child_pid = sys_fork();
        if (child_pid == 0) {
            /* We'll get our own pid after fork */
            long my_pid = sys_getpid();
            (void)my_pid;
            /* Drop CAP_FS_READ */
            long caps = sys_getcap();
            sys_setcap(0, caps & ~0x80);
            /* Try to open — should fail */
            long fd2 = sys_open("/token_test.txt", 0);
            sys_exit(fd2 < 0 ? 0 : 1);
        }
        long st = sys_waitpid(child_pid);
        int no_token_blocked = (st == 0);

        /* Now create a token for a new child */
        long tok = sys_token_create(0x80, 0, "/token_test.txt"); /* bearer token */

        long child_pid2 = sys_fork();
        if (child_pid2 == 0) {
            /* Drop CAP_FS_READ */
            long caps = sys_getcap();
            sys_setcap(0, caps & ~0x80);
            /* Try to open — should succeed via token */
            long fd2 = sys_open("/token_test.txt", 0);
            if (fd2 >= 0) {
                char buf[8] = {0};
                sys_read(fd2, buf, 5);
                sys_close(fd2);
                sys_exit(0); /* success */
            }
            sys_exit(1); /* failed */
        }
        long st2 = sys_waitpid(child_pid2);
        CHECK(no_token_blocked && st2 == 0, "token grants FS_READ to sandboxed process");
        sys_token_revoke(tok);
        sys_unlink("/token_test.txt");
    }

    /* 7. token resource scoping */
    {
        long tok = sys_token_create(0x80, 0, "/data/");

        long pid = sys_fork();
        if (pid == 0) {
            long caps = sys_getcap();
            sys_setcap(0, caps & ~0x80);
            /* /etc/test should NOT match /data/ prefix */
            long fd = sys_open("/etc/test", 0);
            sys_exit(fd < 0 ? 0 : 1); /* expect failure */
        }
        long st = sys_waitpid(pid);
        CHECK(st == 0, "token resource scoping (prefix mismatch denied)");
        sys_token_revoke(tok);
    }

    /* 8. bearer token (target_pid=0) */
    {
        long tok = sys_token_create(0x80, 0, 0); /* bearer, any resource */
        CHECK(tok > 0, "bearer token (target_pid=0) created");
        sys_token_revoke(tok);
    }

    /* 9. token auto-cleanup on owner exit */
    {
        long pid = sys_fork();
        if (pid == 0) {
            /* Create a token and exit — it should be cleaned up */
            sys_token_create(0x80, 0, 0);
            sys_exit(0);
        }
        sys_waitpid(pid);
        /* We can't directly verify cleanup, but the slot should be free.
           Create 32 tokens to verify no leaks. */
        long ids[32];
        int created = 0;
        for (int i = 0; i < 32; i++) {
            ids[i] = sys_token_create(0x80, 0, 0);
            if (ids[i] > 0) created++;
        }
        for (int i = 0; i < 32; i++)
            if (ids[i] > 0) sys_token_revoke(ids[i]);
        CHECK(created == 32, "token auto-cleanup on owner exit");
    }

    /* 10. token_list returns owned tokens */
    {
        long t1 = sys_token_create(0x80, 0, "/a");
        long t2 = sys_token_create(0x40, 0, "/b");
        /* token_info_t is 80 bytes: id(4) + pad(4) + target_pid(8) + perms(4) + pad(4) + resource(64) */
        /* Use raw buffer */
        char buf[80 * 4];
        long count = sys_token_list(buf, 4);
        CHECK(count >= 2, "token_list returns owned tokens");
        sys_token_revoke(t1);
        sys_token_revoke(t2);
    }

    /* --- Agent Namespace Tests --- */

    /* 11. ns_create returns valid ID */
    {
        long ns = sys_ns_create("test_ns");
        CHECK(ns > 0, "ns_create returns valid ID");
    }

    /* 12. ns_create denied without CAP_SYS_ADMIN */
    {
        long pid = sys_fork();
        if (pid == 0) {
            long caps = sys_getcap();
            sys_setcap(0, caps & ~0x10); /* drop CAP_SYS_ADMIN */
            sys_setuid(1000); /* become non-root */
            long ns = sys_ns_create("denied_ns");
            sys_exit(ns < 0 ? 0 : 1);
        }
        long st = sys_waitpid(pid);
        CHECK(st == 0, "ns_create denied without CAP_SYS_ADMIN");
    }

    /* 13. ns_join succeeds for owner */
    {
        long ns = sys_ns_create("join_test");
        long ret = sys_ns_join(ns);
        CHECK(ret == 0, "ns_join succeeds for owner");
        /* Join back to global */
        sys_ns_join(0);
    }

    /* 14. agent_register scoped to namespace */
    {
        long ns = sys_ns_create("scope_test");
        sys_ns_join(ns);
        sys_agent_register("scoped_agent");
        /* Should be findable in this namespace */
        long pid_out = 0;
        long ret = sys_agent_lookup("scoped_agent", &pid_out);
        int found_in_ns = (ret == 0);

        /* Switch to global namespace — should NOT find it */
        sys_ns_join(0);
        ret = sys_agent_lookup("scoped_agent", &pid_out);
        int not_found_global = (ret != 0);

        CHECK(found_in_ns && not_found_global, "agent_register scoped to namespace");
    }

    /* 15. agent_lookup across namespaces returns not found */
    {
        /* Register in global namespace */
        sys_ns_join(0);
        sys_agent_register("global_agent");

        /* Create and join a different namespace */
        long ns = sys_ns_create("other_ns");
        sys_ns_join(ns);

        /* Should NOT find global_agent from other_ns */
        long pid_out = 0;
        long ret = sys_agent_lookup("global_agent", &pid_out);
        CHECK(ret != 0, "agent_lookup across namespaces isolated");

        sys_ns_join(0);
    }

    printf("=== Stage 41 Results: %d/%d PASSED ===\n",
           pass_count, pass_count + fail_count);
    return fail_count > 0 ? 1 : 0;
}
