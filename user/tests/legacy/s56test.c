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

/* Test file path */
#define TESTFILE "/tmp/s56test.txt"
#define TESTFILE2 "/tmp/s56test2.txt"

int main(void) {
    printf("=== Stage 56: Buffered stdio ===\n");

    /* Ensure /tmp exists */
    sys_mkdir("/tmp");

    /* Test 1: fopen write + fclose */
    {
        FILE *fp = fopen(TESTFILE, "w");
        check(fp != NULL, "T1: fopen(w) succeeds");
        if (fp) {
            fputs("Hello stdio!\n", fp);
            fclose(fp);
        }
    }

    /* Test 2: fopen read + fgets */
    {
        FILE *fp = fopen(TESTFILE, "r");
        check(fp != NULL, "T2: fopen(r) succeeds");
        if (fp) {
            char buf[64];
            char *r = fgets(buf, sizeof(buf), fp);
            check(r != NULL, "T2: fgets returns non-NULL");
            check(strcmp(buf, "Hello stdio!\n") == 0, "T2: fgets reads correct line");
            fclose(fp);
        }
    }

    /* Test 3: fwrite + fread */
    {
        FILE *fp = fopen(TESTFILE, "w");
        check(fp != NULL, "T3: fopen(w) for fwrite");
        if (fp) {
            const char data[] = "ABCDEFGHIJ";
            fwrite(data, 1, 10, fp);
            fclose(fp);
        }

        fp = fopen(TESTFILE, "r");
        if (fp) {
            char buf[16] = {0};
            size_t n = fread(buf, 1, 10, fp);
            check(n == 10, "T3: fread returns 10");
            check(strncmp(buf, "ABCDEFGHIJ", 10) == 0, "T3: fread data matches");
            fclose(fp);
        }
    }

    /* Test 4: fputc + fgetc */
    {
        FILE *fp = fopen(TESTFILE, "w");
        if (fp) {
            for (int i = 0; i < 5; i++)
                fputc('0' + i, fp);
            fclose(fp);
        }

        fp = fopen(TESTFILE, "r");
        check(fp != NULL, "T4: fopen for fgetc");
        if (fp) {
            int ok = 1;
            for (int i = 0; i < 5; i++) {
                int c = fgetc(fp);
                if (c != '0' + i) ok = 0;
            }
            check(ok, "T4: fgetc reads correct chars");
            int eof_c = fgetc(fp);
            check(eof_c == -1, "T4: fgetc returns -1 at EOF");
            fclose(fp);
        }
    }

    /* Test 5: fprintf to file */
    {
        FILE *fp = fopen(TESTFILE, "w");
        check(fp != NULL, "T5: fopen for fprintf");
        if (fp) {
            fprintf(fp, "num=%d str=%s\n", 42, "test");
            fclose(fp);
        }

        fp = fopen(TESTFILE, "r");
        if (fp) {
            char buf[64];
            fgets(buf, sizeof(buf), fp);
            check(strcmp(buf, "num=42 str=test\n") == 0, "T5: fprintf output correct");
            fclose(fp);
        }
    }

    /* Test 6: feof detection */
    {
        FILE *fp = fopen(TESTFILE, "w");
        if (fp) { fputs("XY", fp); fclose(fp); }

        fp = fopen(TESTFILE, "r");
        check(fp != NULL, "T6: fopen for feof test");
        if (fp) {
            check(feof(fp) == 0, "T6: not EOF initially");
            fgetc(fp);
            fgetc(fp);
            fgetc(fp);  /* triggers EOF */
            check(feof(fp) != 0, "T6: EOF after reading past end");
            fclose(fp);
        }
    }

    /* Test 7: fseek */
    {
        FILE *fp = fopen(TESTFILE, "w");
        if (fp) { fputs("ABCDEFGHIJ", fp); fclose(fp); }

        fp = fopen(TESTFILE, "r");
        check(fp != NULL, "T7: fopen for fseek");
        if (fp) {
            fseek(fp, 5, SEEK_SET);
            int c = fgetc(fp);
            check(c == 'F', "T7: fseek+fgetc reads correct char");
            fclose(fp);
        }
    }

    /* Test 8: Multiple files open simultaneously */
    {
        FILE *f1 = fopen(TESTFILE, "w");
        FILE *f2 = fopen(TESTFILE2, "w");
        check(f1 != NULL && f2 != NULL, "T8: two files open");
        if (f1 && f2) {
            fputs("file1", f1);
            fputs("file2", f2);
            fclose(f1);
            fclose(f2);
        }

        f1 = fopen(TESTFILE, "r");
        f2 = fopen(TESTFILE2, "r");
        if (f1 && f2) {
            char b1[16] = {0}, b2[16] = {0};
            fgets(b1, sizeof(b1), f1);
            fgets(b2, sizeof(b2), f2);
            check(strcmp(b1, "file1") == 0, "T8: file1 content correct");
            check(strcmp(b2, "file2") == 0, "T8: file2 content correct");
            fclose(f1);
            fclose(f2);
        }
    }

    /* Test 9: Large write (bigger than STDIO_BUFSZ) */
    {
        FILE *fp = fopen(TESTFILE, "w");
        check(fp != NULL, "T9: fopen for large write");
        if (fp) {
            /* Write 512 bytes — exceeds 256-byte buffer */
            for (int i = 0; i < 512; i++)
                fputc('A' + (i % 26), fp);
            fclose(fp);
        }

        fp = fopen(TESTFILE, "r");
        if (fp) {
            int ok = 1;
            for (int i = 0; i < 512; i++) {
                int c = fgetc(fp);
                if (c != 'A' + (i % 26)) { ok = 0; break; }
            }
            check(ok, "T9: large write/read matches");
            fclose(fp);
        }
    }

    /* Test 10: fileno */
    {
        FILE *fp = fopen(TESTFILE, "r");
        check(fp != NULL, "T10: fopen for fileno");
        if (fp) {
            int fd = fileno(fp);
            check(fd >= 0, "T10: fileno returns valid fd");
            fclose(fp);
        }
    }

    /* Test 11: fopen nonexistent file in read mode fails */
    {
        FILE *fp = fopen("/nonexistent_file_xyz", "r");
        check(fp == NULL, "T11: fopen nonexistent returns NULL");
    }

    /* Cleanup */
    sys_unlink(TESTFILE);
    sys_unlink(TESTFILE2);

    printf("\n=== Stage 56 Results: %d/%d passed ===\n",
           passed, passed + failed);

    return (failed > 0) ? 1 : 0;
}
