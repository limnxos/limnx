/*
 * tool_demo.c — Tool-calling agent demo for Limnx
 *
 * Demonstrates use cases 8-10 from the README:
 *   8. MCP-style tool use (discover + invoke tools via kernel primitives)
 *   9. Multi-tool chains (output of tool A feeds tool B)
 *  10. Skill/plugin system (tools are ELF binaries, sandboxed)
 *
 * Usage: /tool_demo.elf
 */
#include "../libc/libc.h"

/* Tool registry — maps keywords to tool ELF paths */
#define MAX_TOOLS 8

typedef struct {
    const char *name;       /* tool name for display */
    const char *keyword;    /* keyword that triggers this tool */
    const char *elf_path;   /* path to tool binary */
    long        caps;       /* capabilities to grant */
} tool_entry_t;

static tool_entry_t tools[] = {
    {"file_reader",   "read",    "/file_reader.elf",   0},
    {"code_executor", "run",     "/code_executor.elf",  0},
    {"code_executor", "ls",      "/code_executor.elf",  0},
    {"code_executor", "exec",    "/code_executor.elf",  0},
    {NULL, NULL, NULL, 0}
};

static int readline(char *buf, int max) {
    int pos = 0;
    while (pos < max - 1) {
        long ch = sys_getchar();
        if (ch == '\n' || ch == '\r') {
            sys_write("\n", 1);
            break;
        } else if (ch == '\b' || ch == 127) {
            if (pos > 0) { pos--; sys_write("\b \b", 3); }
        } else if (ch >= 32 && ch < 127) {
            buf[pos++] = (char)ch;
            char c = (char)ch;
            sys_write(&c, 1);
        }
    }
    buf[pos] = '\0';
    return pos;
}

/* Find a tool by keyword in the input */
static tool_entry_t *find_tool(const char *input) {
    for (int i = 0; tools[i].name; i++) {
        const char *kw = tools[i].keyword;
        int klen = (int)strlen(kw);
        /* Check if input starts with keyword */
        if (strncmp(input, kw, (size_t)klen) == 0 &&
            (input[klen] == ' ' || input[klen] == '\0'))
            return &tools[i];
    }
    return NULL;
}

/* Extract argument after the keyword */
static const char *extract_arg(const char *input) {
    while (*input && *input != ' ') input++;
    while (*input == ' ') input++;
    return input;
}

/* Call a tool via tool_dispatch (sandboxed fork+pipe) */
static int call_tool(tool_entry_t *tool, const char *arg, char *output, int max_out) {
    printf("  [calling %s", tool->name);
    if (*arg) printf(": %s", arg);
    printf("]\n");

    tool_result_t result;
    const char *argv[] = {tool->name, arg, (void *)0};

    int ret = tool_dispatch(tool->elf_path, argv, tool->caps,
                            1000, arg, (uint32_t)strlen(arg), &result);

    if (ret != 0) {
        printf("  [tool_dispatch failed]\n");
        return -1;
    }

    if (result.exit_status != 0) {
        printf("  [tool exited with status %d]\n", result.exit_status);
        if (result.output_len > 0) {
            printf("  %s\n", result.output);
        }
        return -1;
    }

    /* Copy output */
    int len = (int)result.output_len;
    if (len > max_out - 1) len = max_out - 1;
    for (int i = 0; i < len; i++) output[i] = result.output[i];
    output[len] = '\0';

    return len;
}

/* Check if input contains "and" or "then" for chaining */
static int find_chain_split(const char *input) {
    /* Find " and " or " then " */
    for (int i = 0; input[i]; i++) {
        if (input[i] == ' ') {
            if (strncmp(&input[i+1], "and ", 4) == 0) return i + 1;
            if (strncmp(&input[i+1], "then ", 5) == 0) return i + 1;
        }
    }
    return -1;
}

/* Count words in text */
static int count_words(const char *text) {
    int count = 0;
    int in_word = 0;
    for (int i = 0; text[i]; i++) {
        if (text[i] == ' ' || text[i] == '\n' || text[i] == '\t') {
            in_word = 0;
        } else {
            if (!in_word) count++;
            in_word = 1;
        }
    }
    return count;
}

/* Process a single command (may be part of a chain) */
static int process_command(const char *input, char *output, int max_out,
                           const char *prev_output) {
    /* Check for built-in operations on previous output */
    if (prev_output && prev_output[0]) {
        if (strncmp(input, "count", 5) == 0) {
            int wc = count_words(prev_output);
            int len = 0;
            char tmp[32];
            int tlen = 0;
            if (wc == 0) { tmp[tlen++] = '0'; }
            else {
                int v = wc;
                while (v > 0) { tmp[tlen++] = '0' + (v % 10); v /= 10; }
            }
            for (int j = tlen - 1; j >= 0; j--) output[len++] = tmp[j];
            const char *sfx = " words";
            while (*sfx) output[len++] = *sfx++;
            output[len] = '\0';
            printf("  [counting words: %d]\n", wc);
            return len;
        }
    }

    /* Find matching tool */
    tool_entry_t *tool = find_tool(input);
    if (!tool) {
        printf("  [no tool matches: %s]\n", input);
        /* If it looks like "ls ..." treat it as code_executor */
        if (input[0] == 'l' && input[1] == 's') {
            tool = &tools[2]; /* ls → code_executor */
        } else {
            return -1;
        }
    }

    const char *arg = extract_arg(input);

    /* For "ls" without explicit "run" prefix, pass the whole command */
    char full_cmd[256];
    if (strcmp(tool->keyword, "ls") == 0) {
        int i = 0;
        const char *s = input;
        while (*s && i < 255) full_cmd[i++] = *s++;
        full_cmd[i] = '\0';
        arg = full_cmd;
    }

    return call_tool(tool, arg, output, max_out);
}

int main(void) {
    printf("\n");
    printf("=============================================\n");
    printf("  Limnx Tool-Calling Agent Demo\n");
    printf("=============================================\n");
    printf("  Available tools:\n");
    printf("    read <path>    — read a file\n");
    printf("    run <command>  — execute a command\n");
    printf("    ls <path>      — list directory\n");
    printf("  Chains: \"read /hello.txt and count words\"\n");
    printf("  Type 'quit' to exit.\n");
    printf("=============================================\n\n");

    char line[256];
    for (;;) {
        printf("tool> ");
        int len = readline(line, sizeof(line));
        if (len == 0) continue;
        if (strcmp(line, "quit") == 0) break;

        /* Check for chain (multi-tool) */
        int split = find_chain_split(line);

        if (split < 0) {
            /* Single tool call */
            char output[4096];
            int olen = process_command(line, output, sizeof(output), NULL);
            if (olen > 0) {
                printf("\n  [result] %s\n\n", output);
            }
        } else {
            /* Multi-tool chain */
            char step1[256], step2[256];
            int s1len = 0;
            for (int i = 0; i < split && s1len < 255; i++)
                step1[s1len++] = line[i];
            /* Trim trailing space */
            while (s1len > 0 && step1[s1len-1] == ' ') s1len--;
            step1[s1len] = '\0';

            /* Skip "and " or "then " */
            int s2start = split;
            if (strncmp(&line[s2start], "and ", 4) == 0) s2start += 4;
            else if (strncmp(&line[s2start], "then ", 5) == 0) s2start += 5;
            int s2len = 0;
            while (line[s2start + s2len]) {
                step2[s2len] = line[s2start + s2len];
                s2len++;
            }
            step2[s2len] = '\0';

            printf("  [chain: step 1] %s\n", step1);
            char output1[4096];
            int o1len = process_command(step1, output1, sizeof(output1), NULL);

            if (o1len > 0) {
                printf("  [step 1 output] %s\n", output1);
                printf("  [chain: step 2] %s\n", step2);
                char output2[4096];
                int o2len = process_command(step2, output2, sizeof(output2), output1);
                if (o2len > 0) {
                    printf("\n  [result] %s\n\n", output2);
                }
            } else {
                printf("  [step 1 failed, chain aborted]\n\n");
            }
        }
    }

    printf("Goodbye.\n");
    return 0;
}
