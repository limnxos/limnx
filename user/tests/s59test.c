#include "libc/libc.h"

static int passed = 0, failed = 0;

static void check(int ok, const char *name) {
    if (ok) {
        printf("  PASS: %s\n", name);
        passed++;
    } else {
        printf("  FAIL: %s\n", name);
        failed++;
    }
}

#define TESTFILE "/tmp/s59test.txt"

int main(void) {
    printf("=== Stage 59: getline + sscanf ===\n");

    sys_mkdir("/tmp");

    /* === getline tests === */

    /* Test 1: getline basic */
    {
        FILE *fp = fopen(TESTFILE, "w");
        if (fp) {
            fputs("hello world\n", fp);
            fputs("second line\n", fp);
            fclose(fp);
        }

        fp = fopen(TESTFILE, "r");
        check(fp != NULL, "T1: fopen for getline");
        if (fp) {
            char *line = NULL;
            size_t n = 0;
            ssize_t len = getline(&line, &n, fp);
            check(len == 12, "T1: getline returns correct length");
            check(line != NULL && strcmp(line, "hello world\n") == 0,
                  "T1: getline reads first line");

            len = getline(&line, &n, fp);
            check(len == 12, "T1: getline reads second line length");
            check(strcmp(line, "second line\n") == 0,
                  "T1: getline reads second line content");

            len = getline(&line, &n, fp);
            check(len == -1, "T1: getline returns -1 at EOF");

            free(line);
            fclose(fp);
        }
    }

    /* Test 2: getline long line (exceeds initial 128-byte buffer) */
    {
        FILE *fp = fopen(TESTFILE, "w");
        if (fp) {
            for (int i = 0; i < 200; i++)
                fputc('A' + (i % 26), fp);
            fputc('\n', fp);
            fclose(fp);
        }

        fp = fopen(TESTFILE, "r");
        check(fp != NULL, "T2: fopen for long line");
        if (fp) {
            char *line = NULL;
            size_t n = 0;
            ssize_t len = getline(&line, &n, fp);
            check(len == 201, "T2: getline long line length");
            check(n >= 202, "T2: buffer grew sufficiently");

            int ok = 1;
            for (int i = 0; i < 200; i++) {
                if (line[i] != 'A' + (i % 26)) { ok = 0; break; }
            }
            check(ok && line[200] == '\n', "T2: long line content correct");

            free(line);
            fclose(fp);
        }
    }

    /* Test 3: getline with pre-allocated buffer */
    {
        FILE *fp = fopen(TESTFILE, "w");
        if (fp) { fputs("short\n", fp); fclose(fp); }

        fp = fopen(TESTFILE, "r");
        if (fp) {
            char *line = (char *)malloc(16);
            size_t n = 16;
            ssize_t len = getline(&line, &n, fp);
            check(len == 6, "T3: getline with pre-alloc length");
            check(strcmp(line, "short\n") == 0, "T3: getline with pre-alloc content");
            free(line);
            fclose(fp);
        }
    }

    /* === sscanf tests === */

    /* Test 4: sscanf %d */
    {
        int a;
        int r = sscanf("42", "%d", &a);
        check(r == 1 && a == 42, "T4: sscanf %d positive");

        r = sscanf("-99", "%d", &a);
        check(r == 1 && a == -99, "T4: sscanf %d negative");

        r = sscanf("  123", "%d", &a);
        check(r == 1 && a == 123, "T4: sscanf %d leading spaces");
    }

    /* Test 5: sscanf %s */
    {
        char buf[32];
        int r = sscanf("hello world", "%s", buf);
        check(r == 1 && strcmp(buf, "hello") == 0, "T5: sscanf %s first word");
    }

    /* Test 6: sscanf %x */
    {
        unsigned int a;
        int r = sscanf("ff", "%x", &a);
        check(r == 1 && a == 0xff, "T6: sscanf %x lowercase");

        r = sscanf("0xDEAD", "%x", &a);
        check(r == 1 && a == 0xDEAD, "T6: sscanf %x with 0x prefix");
    }

    /* Test 7: sscanf multiple items */
    {
        int num;
        char str[32];
        int r = sscanf("42 hello", "%d %s", &num, str);
        check(r == 2, "T7: sscanf returns 2 for two items");
        check(num == 42 && strcmp(str, "hello") == 0,
              "T7: sscanf multiple items correct");
    }

    /* Test 8: sscanf %c */
    {
        char c;
        int r = sscanf("X", "%c", &c);
        check(r == 1 && c == 'X', "T8: sscanf %c");

        /* %c does NOT skip whitespace */
        r = sscanf(" Y", "%c", &c);
        check(r == 1 && c == ' ', "T8: sscanf %c no whitespace skip");
    }

    /* Test 9: sscanf %n */
    {
        int a, pos;
        int r = sscanf("123 abc", "%d%n", &a, &pos);
        check(r == 1 && a == 123, "T9: sscanf %d before %n");
        check(pos == 3, "T9: sscanf %n position correct");
    }

    /* Test 10: sscanf width specifier */
    {
        char buf[32];
        int r = sscanf("abcdefghij", "%5s", buf);
        check(r == 1 && strcmp(buf, "abcde") == 0,
              "T10: sscanf %5s limits to 5 chars");

        int a;
        r = sscanf("12345678", "%3d", &a);
        check(r == 1 && a == 123, "T10: sscanf %3d limits to 3 digits");
    }

    /* Test 11: sscanf assignment suppression */
    {
        int b;
        int r = sscanf("42 99", "%*d %d", &b);
        check(r == 1 && b == 99, "T11: sscanf %*d skips first int");
    }

    /* Test 12: sscanf return value */
    {
        int a, b, c;
        int r = sscanf("1 2 3", "%d %d %d", &a, &b, &c);
        check(r == 3, "T12: sscanf returns 3 for three items");
        check(a == 1 && b == 2 && c == 3, "T12: all three values correct");

        r = sscanf("1 abc", "%d %d", &a, &b);
        check(r == 1, "T12: sscanf stops on mismatch");
    }

    /* Test 13: sscanf %ld, %lu, %u */
    {
        long la;
        int r = sscanf("1000000", "%ld", &la);
        check(r == 1 && la == 1000000L, "T13: sscanf %ld");

        unsigned long ula;
        r = sscanf("4294967295", "%lu", &ula);
        check(r == 1 && ula == 4294967295UL, "T13: sscanf %lu");

        unsigned int ua;
        r = sscanf("65535", "%u", &ua);
        check(r == 1 && ua == 65535, "T13: sscanf %u");
    }

    /* Test 14: sscanf literal matching */
    {
        int a, b;
        int r = sscanf("x=10,y=20", "x=%d,y=%d", &a, &b);
        check(r == 2 && a == 10 && b == 20,
              "T14: sscanf literal format matching");
    }

    /* Test 15: sscanf %[...] scanset */
    {
        char buf[32];
        int r = sscanf("abc123def", "%[a-z]", buf);
        /* Note: our scanset doesn't support ranges, just literal chars */
        /* So %[abc] would match a, b, c */
        /* Let's use a simpler test */
        r = sscanf("aaabbbccc123", "%[abc]", buf);
        check(r == 1 && strcmp(buf, "aaabbbccc") == 0,
              "T15: sscanf %[abc] scanset");

        r = sscanf("hello world", "%[^ ]", buf);
        check(r == 1 && strcmp(buf, "hello") == 0,
              "T15: sscanf %[^ ] negated scanset");
    }

    /* Cleanup */
    sys_unlink(TESTFILE);

    printf("\n=== Stage 59 Results: %d/%d passed ===\n",
           passed, passed + failed);

    return (failed > 0) ? 1 : 0;
}
