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

static int int_cmp(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}

int main(void) {
    printf("=== Stage 57: String utilities ===\n");

    /* Test 1: strncpy */
    {
        char buf[16];
        memset(buf, 'X', sizeof(buf));
        strncpy(buf, "hello", 10);
        check(strcmp(buf, "hello") == 0, "T1: strncpy copies string");
        check(buf[5] == '\0' && buf[9] == '\0', "T1: strncpy pads with NUL");
    }

    /* Test 2: strcat, strncat */
    {
        char buf[32] = "hello";
        strcat(buf, " world");
        check(strcmp(buf, "hello world") == 0, "T2: strcat");

        char buf2[32] = "foo";
        strncat(buf2, "barbaz", 3);
        check(strcmp(buf2, "foobar") == 0, "T2: strncat limits");
    }

    /* Test 3: strchr, strrchr */
    {
        const char *s = "hello world";
        check(strchr(s, 'o') == s + 4, "T3: strchr finds first");
        check(strrchr(s, 'o') == s + 7, "T3: strrchr finds last");
        check(strchr(s, 'z') == NULL, "T3: strchr not found");
        check(strchr(s, '\0') == s + 11, "T3: strchr finds NUL");
    }

    /* Test 4: memcmp, memmove */
    {
        check(memcmp("abc", "abc", 3) == 0, "T4: memcmp equal");
        check(memcmp("abc", "abd", 3) < 0, "T4: memcmp less");
        check(memcmp("abd", "abc", 3) > 0, "T4: memcmp greater");

        char buf[16] = "abcdefgh";
        memmove(buf + 2, buf, 6); /* overlapping forward */
        check(memcmp(buf, "ababcdef", 8) == 0, "T4: memmove overlap forward");

        char buf2[16] = "abcdefgh";
        memmove(buf2, buf2 + 2, 6); /* overlapping backward */
        check(memcmp(buf2, "cdefghgh", 8) == 0, "T4: memmove overlap backward");
    }

    /* Test 5: strtok */
    {
        char str[] = "hello,world,,foo";
        char *t = strtok(str, ",");
        check(t != NULL && strcmp(t, "hello") == 0, "T5: strtok first token");
        t = strtok(NULL, ",");
        check(t != NULL && strcmp(t, "world") == 0, "T5: strtok second token");
        t = strtok(NULL, ",");
        check(t != NULL && strcmp(t, "foo") == 0, "T5: strtok skips empty");
        t = strtok(NULL, ",");
        check(t == NULL, "T5: strtok returns NULL at end");
    }

    /* Test 6: atoi, atol */
    {
        check(atoi("42") == 42, "T6: atoi positive");
        check(atoi("-99") == -99, "T6: atoi negative");
        check(atoi("0") == 0, "T6: atoi zero");
        check(atoi("  123") == 123, "T6: atoi leading spaces");
        check(atol("1000000") == 1000000L, "T6: atol large");
    }

    /* Test 7: strtol */
    {
        char *end;
        check(strtol("0xFF", &end, 0) == 255, "T7: strtol hex auto-detect");
        check(strtol("077", &end, 0) == 63, "T7: strtol octal auto-detect");
        check(strtol("42abc", &end, 10) == 42, "T7: strtol stops at non-digit");
        check(*end == 'a', "T7: strtol endptr correct");
        check(strtol("-100", NULL, 10) == -100, "T7: strtol negative");
    }

    /* Test 8: strtoul */
    {
        char *end;
        check(strtoul("255", &end, 10) == 255, "T8: strtoul decimal");
        check(strtoul("0xDEAD", &end, 0) == 0xDEAD, "T8: strtoul hex");
        check(strtoul("FF", &end, 16) == 255, "T8: strtoul hex no prefix");
    }

    /* Test 9: character classification */
    {
        check(isdigit('5') && !isdigit('a'), "T9: isdigit");
        check(isalpha('a') && isalpha('Z') && !isalpha('5'), "T9: isalpha");
        check(isalnum('a') && isalnum('5') && !isalnum('!'), "T9: isalnum");
        check(isspace(' ') && isspace('\t') && !isspace('a'), "T9: isspace");
        check(isupper('A') && !isupper('a'), "T9: isupper");
        check(islower('a') && !islower('A'), "T9: islower");
        check(toupper('a') == 'A' && toupper('A') == 'A', "T9: toupper");
        check(tolower('A') == 'a' && tolower('a') == 'a', "T9: tolower");
    }

    /* Test 10: abs, labs */
    {
        check(abs(-5) == 5, "T10: abs negative");
        check(abs(5) == 5, "T10: abs positive");
        check(labs(-1000000L) == 1000000L, "T10: labs");
    }

    /* Test 11: qsort */
    {
        int arr[] = {5, 3, 8, 1, 9, 2, 7, 4, 6, 0};
        qsort(arr, 10, sizeof(int), int_cmp);
        int ok = 1;
        for (int i = 0; i < 10; i++) {
            if (arr[i] != i) { ok = 0; break; }
        }
        check(ok, "T11: qsort sorts correctly");
    }

    /* Test 12: bsearch */
    {
        int arr[] = {1, 3, 5, 7, 9, 11, 13};
        int key = 7;
        int *found = (int *)bsearch(&key, arr, 7, sizeof(int), int_cmp);
        check(found != NULL && *found == 7, "T12: bsearch finds element");

        key = 6;
        found = (int *)bsearch(&key, arr, 7, sizeof(int), int_cmp);
        check(found == NULL, "T12: bsearch returns NULL for missing");
    }

    /* Test 13: snprintf */
    {
        char buf[16];
        int n = snprintf(buf, sizeof(buf), "hello %s", "world!");
        check(strcmp(buf, "hello world!") == 0, "T13: snprintf formats correctly");
        check(n == 12, "T13: snprintf returns total length");

        /* Truncation */
        char small[8];
        n = snprintf(small, sizeof(small), "hello %s", "world!");
        check(small[7] == '\0', "T13: snprintf null-terminates on truncation");
        check(strncmp(small, "hello w", 7) == 0, "T13: snprintf truncates correctly");
        check(n == 12, "T13: snprintf returns full length even when truncated");
    }

    /* Test 14: sprintf */
    {
        char buf[64];
        int n = sprintf(buf, "num=%d hex=%x", 42, 255);
        check(strcmp(buf, "num=42 hex=ff") == 0, "T14: sprintf formats correctly");
        check(n == 13, "T14: sprintf returns length");
    }

    /* Test 15: strdup */
    {
        char *d = strdup("test string");
        check(d != NULL, "T15: strdup non-NULL");
        if (d) {
            check(strcmp(d, "test string") == 0, "T15: strdup content matches");
            free(d);
        }
    }

    printf("\n=== Stage 57 Results: %d/%d passed ===\n",
           passed, passed + failed);

    return (failed > 0) ? 1 : 0;
}
