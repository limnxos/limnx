/*
 * infer_test.c — Inference/AI subsystem tests
 * Tests: agent registry, inference service, math primitives
 * Portable — no arch-specific code.
 */
#include "../limntest.h"

static void test_math_primitives(void) {
    /* Float math functions used by inference pipeline */
    float val = sqrtf(4.0f);
    lt_ok(val > 1.99f && val < 2.01f, "sqrtf(4) ≈ 2.0");

    val = expf(0.0f);
    lt_ok(val > 0.99f && val < 1.01f, "expf(0) ≈ 1.0");

    val = logf(1.0f);
    lt_ok(val > -0.01f && val < 0.01f, "logf(1) ≈ 0.0");

    val = fabsf(-3.14f);
    lt_ok(val > 3.13f && val < 3.15f, "fabsf(-3.14) ≈ 3.14");

    val = tanhf(0.0f);
    lt_ok(val > -0.01f && val < 0.01f, "tanhf(0) ≈ 0.0");
}

static void test_agent_registry(void) {
    long ret = sys_agent_register("test_agent");
    lt_ok(ret == 0, "agent register");

    long pid_out = 0;
    ret = sys_agent_lookup("test_agent", &pid_out);
    lt_ok(ret == 0, "agent lookup finds registered agent");
    lt_ok(pid_out == sys_getpid(), "agent lookup returns correct PID");
}

static void test_string_ops(void) {
    /* String functions used by tokenizer/GGUF parser */
    lt_ok(strlen("hello") == 5, "strlen");
    lt_ok(strcmp("abc", "abc") == 0, "strcmp equal");
    lt_ok(strcmp("abc", "abd") < 0, "strcmp less");
    lt_ok(strncmp("abcdef", "abcxyz", 3) == 0, "strncmp prefix match");

    char buf[32];
    strcpy(buf, "hello");
    lt_ok(strcmp(buf, "hello") == 0, "strcpy");

    lt_ok(strstr("hello world", "world") != (void *)0, "strstr found");
    lt_ok(strstr("hello world", "xyz") == (void *)0, "strstr not found");
}

static void test_memops(void) {
    char a[32], b[32];
    memset(a, 0xAA, 32);
    lt_ok((unsigned char)a[0] == 0xAA && (unsigned char)a[31] == 0xAA, "memset");
    memcpy(b, a, 32);
    lt_ok((unsigned char)b[0] == 0xAA && (unsigned char)b[31] == 0xAA, "memcpy");
}

static void test_atoi_strtol(void) {
    lt_ok(atoi("42") == 42, "atoi(42)");
    lt_ok(atoi("-7") == -7, "atoi(-7)");
    lt_ok(atoi("0") == 0, "atoi(0)");
    lt_ok(strtol("255", (void *)0, 10) == 255, "strtol base 10");
    lt_ok(strtol("ff", (void *)0, 16) == 255, "strtol base 16");
}

int main(void) {
    lt_suite("infer");
    test_math_primitives();
    test_agent_registry();
    test_string_ops();
    test_memops();
    test_atoi_strtol();
    return lt_done();
}
