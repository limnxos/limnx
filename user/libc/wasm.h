#ifndef LIMNX_WASM_H
#define LIMNX_WASM_H
#include <stdint.h>

#define WASM_MAX_FUNCTIONS 32
#define WASM_MAX_LOCALS 16
#define WASM_MAX_EXPORTS 16
#define WASM_STACK_SIZE 256
#define WASM_MEM_PAGES 1  /* 64KB */

typedef struct wasm_module wasm_module_t;

/* Host function callback: args on stack, return value pushed */
typedef int32_t (*wasm_host_fn)(int32_t *args, int nargs);

/* Parse a WASM binary module */
wasm_module_t *wasm_load(const uint8_t *data, uint32_t size);

/* Free a loaded module */
void wasm_free(wasm_module_t *mod);

/* Register a host function import */
int wasm_register_host(wasm_module_t *mod, const char *name, wasm_host_fn fn);

/* Call an exported function by name */
int32_t wasm_call(wasm_module_t *mod, const char *name, int32_t *args, int nargs);

/* Get export count */
int wasm_export_count(wasm_module_t *mod);

/* Get export name by index */
const char *wasm_export_name(wasm_module_t *mod, int idx);

#endif
