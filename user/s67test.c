/* Stage 67 tests: shell EINTR hardening — waitpid retry + input resilience */
#include "libc/libc.h"

static int pass = 0, fail = 0;
#define TEST(name, cond) do { \
    if (cond) { printf("  PASS: %s\n", name); pass++; } \
    else      { printf("  FAIL: %s\n", name); fail++; } \
} while(0)

static volatile int got_signal = 0;

static void test_handler(int sig) {
    (void)sig;
    got_signal = 1;
    sys_sigreturn();
}

/* --- Test 1: waitpid returns -EINTR when signaled --- */
static void test_waitpid_eintr(void) {
    sys_sigaction3(SIGINT, test_handler, 0);
    got_signal = 0;

    long ppid = sys_getpid();

    /* Fork a long-running child */
    long child = sys_fork();
    if (child == 0) {
        /* Child: sleep a bit then exit with status 42 */
        for (int i = 0; i < 20; i++) sys_yield();
        sys_exit(42);
    }

    /* Fork a signaler that interrupts us while we wait */
    long signaler = sys_fork();
    if (signaler == 0) {
        for (int i = 0; i < 5; i++) sys_yield();
        sys_kill(ppid, SIGINT);
        sys_exit(0);
    }

    /* waitpid should get -EINTR from the signal */
    long st = sys_waitpid(child);
    TEST("waitpid returns -EINTR on signal", st == -EINTR);
    TEST("waitpid: signal handler was called", got_signal == 1);

    /* Clean up */
    /* Wait for child to actually finish */
    long st2;
    while ((st2 = sys_waitpid(child)) == -EINTR)
        ;
    /* st2 might be 42 or -1 if already reaped */
    sys_waitpid(signaler);
}

/* --- Test 2: waitpid retry gets correct exit status --- */
static void test_waitpid_retry(void) {
    sys_sigaction3(SIGINT, test_handler, 0);
    got_signal = 0;

    long ppid = sys_getpid();

    /* Fork a child that exits with specific status */
    long child = sys_fork();
    if (child == 0) {
        for (int i = 0; i < 20; i++) sys_yield();
        sys_exit(77);
    }

    /* Fork signaler */
    long signaler = sys_fork();
    if (signaler == 0) {
        for (int i = 0; i < 5; i++) sys_yield();
        sys_kill(ppid, SIGINT);
        sys_exit(0);
    }

    /* Retry loop — this is what shell's waitpid_retry does */
    long st;
    while ((st = sys_waitpid(child)) == -EINTR)
        ;
    TEST("waitpid retry gets correct exit status (77)", st == 77);

    sys_waitpid(signaler);
}

/* --- Test 3: waitpid without signal returns normally --- */
static void test_waitpid_normal(void) {
    long child = sys_fork();
    if (child == 0) {
        sys_exit(99);
    }
    long st = sys_waitpid(child);
    TEST("waitpid without signal returns exit status (99)", st == 99);
}

/* --- Test 4: getchar -EINTR value check --- */
static void test_getchar_eintr_value(void) {
    /* Verify EINTR constant is correct */
    TEST("EINTR == 4", EINTR == 4);
    /* -EINTR as long should be negative */
    long v = -EINTR;
    TEST("-EINTR is negative", v < 0);
    /* -EINTR should not match any printable char */
    TEST("-EINTR != newline", v != '\n');
    TEST("-EINTR != carriage return", v != '\r');
    TEST("-EINTR not in printable range", !(v >= 32 && v < 127));
}

/* --- Test 5: SA_RESTART with waitpid --- */
static void test_waitpid_sa_restart(void) {
    sys_sigaction3(SIGINT, test_handler, SA_RESTART);
    got_signal = 0;

    long ppid = sys_getpid();

    long child = sys_fork();
    if (child == 0) {
        for (int i = 0; i < 20; i++) sys_yield();
        sys_exit(55);
    }

    long signaler = sys_fork();
    if (signaler == 0) {
        for (int i = 0; i < 5; i++) sys_yield();
        sys_kill(ppid, SIGINT);
        sys_exit(0);
    }

    /* With SA_RESTART, waitpid should auto-restart and return real status */
    long st = sys_waitpid(child);
    TEST("SA_RESTART waitpid gets exit status (55)", st == 55);
    TEST("SA_RESTART: signal handler was called", got_signal == 1);

    sys_waitpid(signaler);
}

/* --- Test 6: multiple signals during waitpid retry --- */
static void test_waitpid_multi_signal(void) {
    sys_sigaction3(SIGINT, test_handler, 0);
    got_signal = 0;

    long ppid = sys_getpid();

    long child = sys_fork();
    if (child == 0) {
        for (int i = 0; i < 40; i++) sys_yield();
        sys_exit(33);
    }

    /* Two signalers */
    long sig1 = sys_fork();
    if (sig1 == 0) {
        for (int i = 0; i < 5; i++) sys_yield();
        sys_kill(ppid, SIGINT);
        sys_exit(0);
    }

    long sig2 = sys_fork();
    if (sig2 == 0) {
        for (int i = 0; i < 15; i++) sys_yield();
        sys_kill(ppid, SIGINT);
        sys_exit(0);
    }

    /* Retry loop should handle multiple -EINTRs */
    long st;
    while ((st = sys_waitpid(child)) == -EINTR)
        ;
    TEST("multi-signal waitpid retry gets status (33)", st == 33);

    sys_waitpid(sig1);
    sys_waitpid(sig2);
}

int main(void) {
    printf("=== Stage 67: Shell EINTR Hardening ===\n");

    test_waitpid_eintr();
    test_waitpid_retry();
    test_waitpid_normal();
    test_getchar_eintr_value();
    test_waitpid_sa_restart();
    test_waitpid_multi_signal();

    printf("--- Results: %d passed, %d failed ---\n", pass, fail);
    return fail;
}
