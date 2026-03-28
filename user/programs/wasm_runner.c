/*
 * wasm_runner.c — CLI program to load and run WebAssembly modules on Limnx
 *
 * Usage: wasm_runner <file.wasm> [function] [args...]
 */

#include "libc/libc.h"
#include "libc/wasm.h"
#include "limnx/stat.h"

/* Host function: print(i32) — prints the value and returns it */
static int32_t host_print(int32_t *args, int nargs) {
    if (nargs > 0) {
        printf("%d\n", args[0]);
        return args[0];
    }
    printf("(no args)\n");
    return 0;
}

/* Host function: putchar(i32) — prints a character */
static int32_t host_putchar(int32_t *args, int nargs) {
    if (nargs > 0) {
        char c = (char)args[0];
        sys_write(&c, 1);
        return args[0];
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: wasm_runner <file.wasm> [function] [args...]\n");
        return 1;
    }

    const char *path = argv[1];

    /* Open and read the WASM file */
    long fd = sys_open(path, O_RDONLY);
    if (fd < 0) {
        printf("wasm_runner: cannot open '%s'\n", path);
        return 1;
    }

    /* Get file size via stat */
    struct linux_stat st;
    if (sys_fstat(fd, &st) < 0) {
        printf("wasm_runner: cannot stat '%s'\n", path);
        sys_close(fd);
        return 1;
    }

    uint32_t file_size = (uint32_t)st.st_size;
    if (file_size == 0 || file_size > 1024 * 1024) {
        printf("wasm_runner: invalid file size %u\n", file_size);
        sys_close(fd);
        return 1;
    }

    /* Map file into memory */
    long map_addr = sys_fmmap(fd);
    if (map_addr <= 0) {
        printf("wasm_runner: cannot mmap '%s'\n", path);
        sys_close(fd);
        return 1;
    }

    const uint8_t *data = (const uint8_t *)map_addr;

    /* Load the WASM module */
    wasm_module_t *mod = wasm_load(data, file_size);
    if (!mod) {
        printf("wasm_runner: failed to parse '%s'\n", path);
        sys_close(fd);
        return 1;
    }

    /* Register host functions */
    wasm_register_host(mod, "print", host_print);
    wasm_register_host(mod, "putchar", host_putchar);

    if (argc >= 3) {
        /* Call specific function with optional args */
        const char *func_name = argv[2];
        int32_t call_args[8];
        int nargs = 0;
        for (int i = 3; i < argc && nargs < 8; i++) {
            call_args[nargs++] = (int32_t)atoi(argv[i]);
        }

        int32_t result = wasm_call(mod, func_name, call_args, nargs);
        printf("result: %d\n", result);
    } else {
        /* List exports and call each with no args */
        int count = wasm_export_count(mod);
        printf("exports (%d):\n", count);
        for (int i = 0; i < count; i++) {
            const char *name = wasm_export_name(mod, i);
            printf("  [%d] %s\n", i, name);
        }

        for (int i = 0; i < count; i++) {
            const char *name = wasm_export_name(mod, i);
            printf("\ncalling %s():\n", name);
            int32_t result = wasm_call(mod, name, NULL, 0);
            printf("  => %d\n", result);
        }
    }

    wasm_free(mod);
    sys_close(fd);
    return 0;
}
