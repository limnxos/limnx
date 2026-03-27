/*
 * wasm.c — Minimal WebAssembly bytecode interpreter for Limnx
 *
 * Supports MVP i32-only subset: arithmetic, comparison, control flow,
 * local variables, function calls, imports, and exports.
 */

#include "libc.h"

/* ---- Constants (matching wasm.h) ---- */

#define WASM_MAX_FUNCTIONS 32
#define WASM_MAX_LOCALS    16
#define WASM_MAX_EXPORTS   16
#define WASM_STACK_SIZE    256
#define WASM_MEM_PAGES     1   /* 64KB */

#define WASM_MAX_IMPORTS   16
#define WASM_MAX_PARAMS    8
#define WASM_MAX_CALL_DEPTH 16
#define WASM_MAX_BLOCK_DEPTH 32

/* Host function callback */
typedef int32_t (*wasm_host_fn)(int32_t *args, int nargs);

/* ---- WASM section IDs ---- */

#define SEC_TYPE     1
#define SEC_IMPORT   2
#define SEC_FUNCTION 3
#define SEC_EXPORT   7
#define SEC_CODE    10

/* ---- WASM opcodes ---- */

#define OP_UNREACHABLE  0x00
#define OP_NOP          0x01
#define OP_BLOCK        0x02
#define OP_LOOP         0x03
#define OP_IF           0x04
#define OP_ELSE         0x05
#define OP_END          0x0b
#define OP_BR           0x0c
#define OP_BR_IF        0x0d
#define OP_RETURN       0x0f
#define OP_CALL         0x10
#define OP_DROP         0x1a
#define OP_LOCAL_GET    0x20
#define OP_LOCAL_SET    0x21
#define OP_LOCAL_TEE    0x22
#define OP_I32_CONST    0x41
#define OP_I32_EQZ      0x45
#define OP_I32_EQ       0x46
#define OP_I32_NE       0x47
#define OP_I32_LT_S     0x48
#define OP_I32_GT_S     0x4a
#define OP_I32_LE_S     0x4c
#define OP_I32_GE_S     0x4e
#define OP_I32_ADD      0x6a
#define OP_I32_SUB      0x6b
#define OP_I32_MUL      0x6c
#define OP_I32_DIV_S    0x6d
#define OP_I32_REM_S    0x6f
#define OP_I32_AND      0x71
#define OP_I32_OR       0x72

/* WASM value types */
#define VALTYPE_I32  0x7f
#define VALTYPE_VOID 0x40

/* ---- Internal structures ---- */

typedef struct {
    uint8_t  param_count;
    uint8_t  result_count;   /* 0 or 1 */
    uint8_t  params[WASM_MAX_PARAMS];
} wasm_functype_t;

typedef struct {
    const uint8_t *code;     /* pointer into module binary */
    uint32_t       code_len;
    uint32_t       type_idx;
    uint32_t       local_count; /* params + declared locals */
} wasm_func_t;

typedef struct {
    char          name[64];
    uint32_t      func_idx;
} wasm_export_t;

typedef struct {
    char          module_name[64];
    char          field_name[64];
    uint32_t      type_idx;
    wasm_host_fn  host_fn;
} wasm_import_t;

/* Block/label entry for control flow */
typedef struct {
    uint8_t        kind;        /* 0=block, 1=loop, 2=if */
    const uint8_t *continuation; /* PC after end */
    const uint8_t *loop_start;   /* PC at loop header (for loop kind) */
    int            stack_height; /* stack height at block entry */
} wasm_block_t;

/* Call frame */
typedef struct {
    const uint8_t *ret_pc;
    int32_t        locals[WASM_MAX_LOCALS];
    int            local_count;
    int            stack_base;
    wasm_block_t   blocks[WASM_MAX_BLOCK_DEPTH];
    int            block_depth;
} wasm_frame_t;

struct wasm_module {
    /* Raw binary */
    const uint8_t *data;
    uint32_t       data_size;

    /* Types */
    wasm_functype_t types[WASM_MAX_FUNCTIONS];
    int             type_count;

    /* Imports */
    wasm_import_t   imports[WASM_MAX_IMPORTS];
    int             import_count;

    /* Functions (index = import_count + local index) */
    wasm_func_t     funcs[WASM_MAX_FUNCTIONS];
    int             func_count;   /* number of module-defined functions */

    /* Exports */
    wasm_export_t   exports[WASM_MAX_EXPORTS];
    int             export_count;

    /* Operand stack */
    int32_t         stack[WASM_STACK_SIZE];
    int             sp;

    /* Call stack */
    wasm_frame_t    frames[WASM_MAX_CALL_DEPTH];
    int             fp;

    /* Linear memory (64KB * WASM_MEM_PAGES) */
    uint8_t        *memory;
};

/* ---- LEB128 decoding ---- */

static uint32_t read_u32_leb128(const uint8_t **pp) {
    uint32_t result = 0;
    uint32_t shift = 0;
    const uint8_t *p = *pp;
    for (;;) {
        uint8_t byte = *p++;
        result |= (uint32_t)(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) break;
        shift += 7;
        if (shift >= 35) break; /* overflow guard */
    }
    *pp = p;
    return result;
}

static int32_t read_s32_leb128(const uint8_t **pp) {
    int32_t result = 0;
    uint32_t shift = 0;
    const uint8_t *p = *pp;
    uint8_t byte;
    do {
        byte = *p++;
        result |= (int32_t)(byte & 0x7f) << shift;
        shift += 7;
    } while (byte & 0x80);
    /* Sign extend */
    if ((shift < 32) && (byte & 0x40))
        result |= -(1 << shift);
    *pp = p;
    return result;
}

/* ---- Section parsing ---- */

static int parse_type_section(wasm_module_t *mod, const uint8_t *p, uint32_t len) {
    const uint8_t *end = p + len;
    uint32_t count = read_u32_leb128(&p);
    if (count > WASM_MAX_FUNCTIONS) count = WASM_MAX_FUNCTIONS;
    mod->type_count = (int)count;

    for (uint32_t i = 0; i < count && p < end; i++) {
        if (*p++ != 0x60) return -1; /* functype marker */
        wasm_functype_t *ft = &mod->types[i];
        uint32_t nparams = read_u32_leb128(&p);
        ft->param_count = (uint8_t)(nparams > WASM_MAX_PARAMS ? WASM_MAX_PARAMS : nparams);
        for (uint32_t j = 0; j < nparams; j++) {
            uint8_t t = *p++;
            if (j < WASM_MAX_PARAMS) ft->params[j] = t;
        }
        uint32_t nresults = read_u32_leb128(&p);
        ft->result_count = (uint8_t)(nresults > 1 ? 1 : nresults);
        for (uint32_t j = 0; j < nresults; j++) p++; /* skip result types (all i32) */
    }
    return 0;
}

static int parse_import_section(wasm_module_t *mod, const uint8_t *p, uint32_t len) {
    const uint8_t *end = p + len;
    uint32_t count = read_u32_leb128(&p);
    if (count > WASM_MAX_IMPORTS) count = WASM_MAX_IMPORTS;

    for (uint32_t i = 0; i < count && p < end; i++) {
        wasm_import_t *imp = &mod->imports[mod->import_count];

        /* module name */
        uint32_t mlen = read_u32_leb128(&p);
        uint32_t copy_len = mlen < 63 ? mlen : 63;
        memcpy(imp->module_name, p, copy_len);
        imp->module_name[copy_len] = '\0';
        p += mlen;

        /* field name */
        uint32_t flen = read_u32_leb128(&p);
        copy_len = flen < 63 ? flen : 63;
        memcpy(imp->field_name, p, copy_len);
        imp->field_name[copy_len] = '\0';
        p += flen;

        uint8_t kind = *p++;
        if (kind == 0x00) {
            /* function import */
            imp->type_idx = read_u32_leb128(&p);
            imp->host_fn = NULL;
            mod->import_count++;
        } else {
            /* skip non-function imports (table, memory, global) */
            /* just skip the remaining bytes for this import */
            if (kind == 0x01) { p += 2; }       /* table: elemtype + limits */
            else if (kind == 0x02) {             /* memory: limits */
                uint8_t flags = *p++;
                read_u32_leb128(&p);
                if (flags & 1) read_u32_leb128(&p);
            }
            else if (kind == 0x03) { p += 2; }  /* global: valtype + mut */
        }
    }
    return 0;
}

static int parse_function_section(wasm_module_t *mod, const uint8_t *p, uint32_t len) {
    const uint8_t *end = p + len;
    uint32_t count = read_u32_leb128(&p);
    if (count > WASM_MAX_FUNCTIONS) count = WASM_MAX_FUNCTIONS;
    mod->func_count = (int)count;

    for (uint32_t i = 0; i < count && p < end; i++) {
        mod->funcs[i].type_idx = read_u32_leb128(&p);
    }
    return 0;
}

static int parse_export_section(wasm_module_t *mod, const uint8_t *p, uint32_t len) {
    const uint8_t *end = p + len;
    uint32_t count = read_u32_leb128(&p);
    if (count > WASM_MAX_EXPORTS) count = WASM_MAX_EXPORTS;

    for (uint32_t i = 0; i < count && p < end; i++) {
        uint32_t nlen = read_u32_leb128(&p);
        uint32_t copy_len = nlen < 63 ? nlen : 63;

        wasm_export_t *exp = &mod->exports[mod->export_count];
        memcpy(exp->name, p, copy_len);
        exp->name[copy_len] = '\0';
        p += nlen;

        uint8_t kind = *p++;
        uint32_t idx = read_u32_leb128(&p);

        if (kind == 0x00) {
            /* function export */
            exp->func_idx = idx;
            mod->export_count++;
        }
        /* skip non-function exports */
    }
    return 0;
}

static int parse_code_section(wasm_module_t *mod, const uint8_t *p, uint32_t len) {
    const uint8_t *end = p + len;
    uint32_t count = read_u32_leb128(&p);
    if ((int)count > mod->func_count) count = (uint32_t)mod->func_count;

    for (uint32_t i = 0; i < count && p < end; i++) {
        uint32_t body_size = read_u32_leb128(&p);
        const uint8_t *body_start = p;
        const uint8_t *body_end = p + body_size;

        /* Count locals */
        uint32_t local_decl_count = read_u32_leb128(&p);
        uint32_t total_locals = 0;
        /* Get param count from the function's type */
        uint32_t tidx = mod->funcs[i].type_idx;
        if ((int)tidx < mod->type_count)
            total_locals = mod->types[tidx].param_count;

        for (uint32_t j = 0; j < local_decl_count; j++) {
            uint32_t n = read_u32_leb128(&p);
            p++; /* skip valtype */
            total_locals += n;
        }
        if (total_locals > WASM_MAX_LOCALS)
            total_locals = WASM_MAX_LOCALS;

        mod->funcs[i].local_count = total_locals;
        mod->funcs[i].code = p;
        mod->funcs[i].code_len = (uint32_t)(body_end - p);

        p = body_end;
    }
    return 0;
}

/* ---- Skip to end of block (for if/else skipping) ---- */

static const uint8_t *skip_to_end_or_else(const uint8_t *pc, const uint8_t *end, int *found_else) {
    int depth = 1;
    *found_else = 0;
    while (pc < end && depth > 0) {
        uint8_t op = *pc++;
        switch (op) {
        case OP_BLOCK: case OP_LOOP: case OP_IF:
            pc++; /* block type */
            depth++;
            break;
        case OP_ELSE:
            if (depth == 1) { *found_else = 1; return pc; }
            break;
        case OP_END:
            depth--;
            if (depth == 0) return pc;
            break;
        case OP_BR: case OP_BR_IF:
            read_u32_leb128(&pc);
            break;
        case OP_CALL:
            read_u32_leb128(&pc);
            break;
        case OP_LOCAL_GET: case OP_LOCAL_SET: case OP_LOCAL_TEE:
            read_u32_leb128(&pc);
            break;
        case OP_I32_CONST:
            read_s32_leb128(&pc);
            break;
        default:
            break;
        }
    }
    return pc;
}

/* Skip to the matching end (used for br targets) */
static const uint8_t *skip_block_to_end(const uint8_t *pc, const uint8_t *end) {
    int depth = 1;
    while (pc < end && depth > 0) {
        uint8_t op = *pc++;
        switch (op) {
        case OP_BLOCK: case OP_LOOP: case OP_IF:
            pc++; /* block type */
            depth++;
            break;
        case OP_END:
            depth--;
            break;
        case OP_BR: case OP_BR_IF:
            read_u32_leb128(&pc);
            break;
        case OP_CALL:
            read_u32_leb128(&pc);
            break;
        case OP_LOCAL_GET: case OP_LOCAL_SET: case OP_LOCAL_TEE:
            read_u32_leb128(&pc);
            break;
        case OP_I32_CONST:
            read_s32_leb128(&pc);
            break;
        default:
            break;
        }
    }
    return pc;
}

/* ---- Interpreter ---- */

/* Forward declaration */
static int32_t exec_function(wasm_module_t *mod, uint32_t func_idx,
                             int32_t *args, int nargs);

static int32_t exec_code(wasm_module_t *mod, wasm_frame_t *frame,
                         const uint8_t *code, uint32_t code_len) {
    const uint8_t *pc = code;
    const uint8_t *end = code + code_len;
    int32_t *stack = mod->stack;
    int sp = mod->sp;

    while (pc < end) {
        uint8_t op = *pc++;

        switch (op) {
        case OP_UNREACHABLE:
            printf("wasm: unreachable executed\n");
            mod->sp = sp;
            return 0;

        case OP_NOP:
            break;

        case OP_BLOCK: {
            uint8_t bt = *pc++; /* block type */
            (void)bt;
            if (frame->block_depth >= WASM_MAX_BLOCK_DEPTH) {
                printf("wasm: block depth overflow\n");
                mod->sp = sp;
                return 0;
            }
            /* Find the end of this block */
            const uint8_t *block_end = skip_block_to_end(pc, end);
            wasm_block_t *blk = &frame->blocks[frame->block_depth++];
            blk->kind = 0; /* block */
            blk->continuation = block_end;
            blk->loop_start = NULL;
            blk->stack_height = sp;
            break;
        }

        case OP_LOOP: {
            uint8_t bt = *pc++; /* block type */
            (void)bt;
            if (frame->block_depth >= WASM_MAX_BLOCK_DEPTH) {
                printf("wasm: block depth overflow\n");
                mod->sp = sp;
                return 0;
            }
            const uint8_t *loop_header = pc;
            const uint8_t *block_end = skip_block_to_end(pc, end);
            wasm_block_t *blk = &frame->blocks[frame->block_depth++];
            blk->kind = 1; /* loop */
            blk->continuation = block_end;
            blk->loop_start = loop_header;
            blk->stack_height = sp;
            break;
        }

        case OP_IF: {
            uint8_t bt = *pc++; /* block type */
            (void)bt;
            if (sp <= 0) { mod->sp = sp; return 0; }
            int32_t cond = stack[--sp];

            if (cond) {
                /* Enter the if body */
                if (frame->block_depth >= WASM_MAX_BLOCK_DEPTH) {
                    mod->sp = sp;
                    return 0;
                }
                /* We need to find the end for the block entry */
                const uint8_t *save_pc = pc;
                const uint8_t *block_end = skip_block_to_end(pc, end);
                wasm_block_t *blk = &frame->blocks[frame->block_depth++];
                blk->kind = 2; /* if */
                blk->continuation = block_end;
                blk->loop_start = NULL;
                blk->stack_height = sp;
                /* pc stays at start of if body — but we re-scanned, so reset */
                pc = save_pc;
            } else {
                /* Skip to else or end */
                int found_else = 0;
                const uint8_t *target = skip_to_end_or_else(pc, end, &found_else);
                if (found_else) {
                    /* Enter else body, push block for it */
                    if (frame->block_depth >= WASM_MAX_BLOCK_DEPTH) {
                        mod->sp = sp;
                        return 0;
                    }
                    const uint8_t *block_end = skip_block_to_end(target, end);
                    wasm_block_t *blk = &frame->blocks[frame->block_depth++];
                    blk->kind = 2; /* if (else branch) */
                    blk->continuation = block_end;
                    blk->loop_start = NULL;
                    blk->stack_height = sp;
                    pc = target; /* right after else */
                } else {
                    pc = target; /* right after end */
                }
            }
            break;
        }

        case OP_ELSE: {
            /* We are executing the if-true branch and hit else: skip to end */
            if (frame->block_depth > 0) {
                wasm_block_t *blk = &frame->blocks[frame->block_depth - 1];
                pc = blk->continuation;
                frame->block_depth--;
            }
            break;
        }

        case OP_END:
            if (frame->block_depth > 0) {
                frame->block_depth--;
            } else {
                /* End of function */
                mod->sp = sp;
                if (sp > 0) return stack[sp - 1];
                return 0;
            }
            break;

        case OP_BR: {
            uint32_t depth = read_u32_leb128(&pc);
            if ((int)depth >= frame->block_depth) {
                /* Branch past all blocks = return */
                mod->sp = sp;
                if (sp > 0) return stack[sp - 1];
                return 0;
            }
            int target_idx = frame->block_depth - 1 - (int)depth;
            wasm_block_t *target_blk = &frame->blocks[target_idx];
            if (target_blk->kind == 1) {
                /* Loop: branch to loop header */
                pc = target_blk->loop_start;
            } else {
                /* Block/if: branch to continuation (past end) */
                pc = target_blk->continuation;
            }
            sp = target_blk->stack_height;
            frame->block_depth = target_idx + (target_blk->kind == 1 ? 1 : 0);
            break;
        }

        case OP_BR_IF: {
            uint32_t depth = read_u32_leb128(&pc);
            if (sp <= 0) { mod->sp = sp; return 0; }
            int32_t cond = stack[--sp];
            if (cond) {
                if ((int)depth >= frame->block_depth) {
                    mod->sp = sp;
                    if (sp > 0) return stack[sp - 1];
                    return 0;
                }
                int target_idx = frame->block_depth - 1 - (int)depth;
                wasm_block_t *target_blk = &frame->blocks[target_idx];
                if (target_blk->kind == 1) {
                    pc = target_blk->loop_start;
                } else {
                    pc = target_blk->continuation;
                }
                sp = target_blk->stack_height;
                frame->block_depth = target_idx + (target_blk->kind == 1 ? 1 : 0);
            }
            break;
        }

        case OP_RETURN:
            mod->sp = sp;
            if (sp > 0) return stack[sp - 1];
            return 0;

        case OP_CALL: {
            uint32_t func_idx = read_u32_leb128(&pc);
            /* Determine param count */
            uint32_t tidx;
            int nparams;
            if ((int)func_idx < mod->import_count) {
                tidx = mod->imports[func_idx].type_idx;
            } else {
                int local_idx = (int)func_idx - mod->import_count;
                if (local_idx < 0 || local_idx >= mod->func_count) {
                    printf("wasm: call to invalid func %d\n", func_idx);
                    mod->sp = sp;
                    return 0;
                }
                tidx = mod->funcs[local_idx].type_idx;
            }
            nparams = (int)tidx < mod->type_count ? mod->types[tidx].param_count : 0;
            int has_result = (int)tidx < mod->type_count ? mod->types[tidx].result_count : 0;

            /* Pop args from stack */
            int32_t call_args[WASM_MAX_PARAMS];
            if (sp < nparams) { mod->sp = sp; return 0; }
            sp -= nparams;
            for (int j = 0; j < nparams; j++)
                call_args[j] = stack[sp + j];

            /* Save interpreter state, do the call */
            mod->sp = sp;
            int32_t result = exec_function(mod, func_idx, call_args, nparams);
            sp = mod->sp;

            if (has_result && sp < WASM_STACK_SIZE)
                stack[sp++] = result;
            break;
        }

        case OP_DROP:
            if (sp > 0) sp--;
            break;

        case OP_LOCAL_GET: {
            uint32_t idx = read_u32_leb128(&pc);
            if ((int)idx < frame->local_count && sp < WASM_STACK_SIZE)
                stack[sp++] = frame->locals[idx];
            break;
        }

        case OP_LOCAL_SET: {
            uint32_t idx = read_u32_leb128(&pc);
            if (sp > 0 && (int)idx < frame->local_count)
                frame->locals[idx] = stack[--sp];
            break;
        }

        case OP_LOCAL_TEE: {
            uint32_t idx = read_u32_leb128(&pc);
            if (sp > 0 && (int)idx < frame->local_count)
                frame->locals[idx] = stack[sp - 1];
            break;
        }

        case OP_I32_CONST: {
            int32_t val = read_s32_leb128(&pc);
            if (sp < WASM_STACK_SIZE)
                stack[sp++] = val;
            break;
        }

        /* Comparison ops */
        case OP_I32_EQZ:
            if (sp > 0) stack[sp - 1] = (stack[sp - 1] == 0) ? 1 : 0;
            break;
        case OP_I32_EQ:
            if (sp >= 2) { sp--; stack[sp - 1] = (stack[sp - 1] == stack[sp]) ? 1 : 0; }
            break;
        case OP_I32_NE:
            if (sp >= 2) { sp--; stack[sp - 1] = (stack[sp - 1] != stack[sp]) ? 1 : 0; }
            break;
        case OP_I32_LT_S:
            if (sp >= 2) { sp--; stack[sp - 1] = (stack[sp - 1] < stack[sp]) ? 1 : 0; }
            break;
        case OP_I32_GT_S:
            if (sp >= 2) { sp--; stack[sp - 1] = (stack[sp - 1] > stack[sp]) ? 1 : 0; }
            break;
        case OP_I32_LE_S:
            if (sp >= 2) { sp--; stack[sp - 1] = (stack[sp - 1] <= stack[sp]) ? 1 : 0; }
            break;
        case OP_I32_GE_S:
            if (sp >= 2) { sp--; stack[sp - 1] = (stack[sp - 1] >= stack[sp]) ? 1 : 0; }
            break;

        /* Arithmetic ops */
        case OP_I32_ADD:
            if (sp >= 2) { sp--; stack[sp - 1] += stack[sp]; }
            break;
        case OP_I32_SUB:
            if (sp >= 2) { sp--; stack[sp - 1] -= stack[sp]; }
            break;
        case OP_I32_MUL:
            if (sp >= 2) { sp--; stack[sp - 1] *= stack[sp]; }
            break;
        case OP_I32_DIV_S:
            if (sp >= 2) {
                sp--;
                if (stack[sp] == 0) {
                    printf("wasm: division by zero\n");
                    stack[sp - 1] = 0;
                } else {
                    stack[sp - 1] /= stack[sp];
                }
            }
            break;
        case OP_I32_REM_S:
            if (sp >= 2) {
                sp--;
                if (stack[sp] == 0) {
                    printf("wasm: division by zero\n");
                    stack[sp - 1] = 0;
                } else {
                    stack[sp - 1] %= stack[sp];
                }
            }
            break;
        case OP_I32_AND:
            if (sp >= 2) { sp--; stack[sp - 1] &= stack[sp]; }
            break;
        case OP_I32_OR:
            if (sp >= 2) { sp--; stack[sp - 1] |= stack[sp]; }
            break;

        default:
            printf("wasm: unknown opcode 0x%02x at offset %d\n",
                   op, (int)(pc - 1 - code));
            mod->sp = sp;
            return 0;
        }
    }

    mod->sp = sp;
    if (sp > 0) return stack[sp - 1];
    return 0;
}

static int32_t exec_function(wasm_module_t *mod, uint32_t func_idx,
                             int32_t *args, int nargs) {
    /* Handle imported (host) functions */
    if ((int)func_idx < mod->import_count) {
        wasm_import_t *imp = &mod->imports[func_idx];
        if (imp->host_fn) {
            return imp->host_fn(args, nargs);
        }
        printf("wasm: unresolved import '%s.%s'\n",
               imp->module_name, imp->field_name);
        return 0;
    }

    int local_idx = (int)func_idx - mod->import_count;
    if (local_idx < 0 || local_idx >= mod->func_count) {
        printf("wasm: invalid function index %d\n", func_idx);
        return 0;
    }

    if (mod->fp >= WASM_MAX_CALL_DEPTH) {
        printf("wasm: call stack overflow\n");
        return 0;
    }

    wasm_func_t *func = &mod->funcs[local_idx];
    wasm_frame_t *frame = &mod->frames[mod->fp++];

    /* Initialize frame */
    memset(frame->locals, 0, sizeof(frame->locals));
    frame->local_count = (int)func->local_count;
    frame->stack_base = mod->sp;
    frame->block_depth = 0;

    /* Copy args into locals */
    for (int i = 0; i < nargs && i < WASM_MAX_LOCALS; i++)
        frame->locals[i] = args[i];

    int32_t result = exec_code(mod, frame, func->code, func->code_len);

    mod->fp--;

    /* Restore stack to frame base (function consumed its args already) */
    /* Keep only the result */
    mod->sp = frame->stack_base;

    return result;
}

/* ---- Public API ---- */

wasm_module_t *wasm_load(const uint8_t *data, uint32_t size) {
    if (size < 8) {
        printf("wasm: file too small\n");
        return NULL;
    }

    /* Check magic: \0asm */
    if (data[0] != 0x00 || data[1] != 0x61 ||
        data[2] != 0x73 || data[3] != 0x6d) {
        printf("wasm: invalid magic\n");
        return NULL;
    }

    /* Check version 1 */
    if (data[4] != 0x01 || data[5] != 0x00 ||
        data[6] != 0x00 || data[7] != 0x00) {
        printf("wasm: unsupported version\n");
        return NULL;
    }

    /* Allocate module via mmap (one page is enough for the struct) */
    long pages_needed = (sizeof(wasm_module_t) + PAGE_SIZE - 1) / PAGE_SIZE;
    long addr = sys_mmap(pages_needed);
    if (addr <= 0) {
        printf("wasm: mmap failed for module\n");
        return NULL;
    }
    wasm_module_t *mod = (wasm_module_t *)addr;
    memset(mod, 0, sizeof(wasm_module_t));

    mod->data = data;
    mod->data_size = size;

    /* Allocate linear memory (64KB per page) */
    long mem_pages = (WASM_MEM_PAGES * 65536 + PAGE_SIZE - 1) / PAGE_SIZE;
    long mem_addr = sys_mmap(mem_pages);
    if (mem_addr > 0) {
        mod->memory = (uint8_t *)mem_addr;
        memset(mod->memory, 0, WASM_MEM_PAGES * 65536);
    }

    /* Parse sections */
    const uint8_t *p = data + 8;
    const uint8_t *end = data + size;

    while (p < end) {
        uint8_t sec_id = *p++;
        uint32_t sec_size = read_u32_leb128(&p);
        const uint8_t *sec_data = p;

        if (p + sec_size > end) break;

        switch (sec_id) {
        case SEC_TYPE:
            parse_type_section(mod, sec_data, sec_size);
            break;
        case SEC_IMPORT:
            parse_import_section(mod, sec_data, sec_size);
            break;
        case SEC_FUNCTION:
            parse_function_section(mod, sec_data, sec_size);
            break;
        case SEC_EXPORT:
            parse_export_section(mod, sec_data, sec_size);
            break;
        case SEC_CODE:
            parse_code_section(mod, sec_data, sec_size);
            break;
        default:
            /* Skip unknown sections */
            break;
        }

        p = sec_data + sec_size;
    }

    return mod;
}

void wasm_free(wasm_module_t *mod) {
    if (!mod) return;
    if (mod->memory) {
        long mem_pages = (WASM_MEM_PAGES * 65536 + PAGE_SIZE - 1) / PAGE_SIZE;
        (void)mem_pages;
        sys_munmap((unsigned long)mod->memory);
    }
    sys_munmap((unsigned long)mod);
}

int wasm_register_host(wasm_module_t *mod, const char *name, wasm_host_fn fn) {
    if (!mod || !name || !fn) return -1;

    for (int i = 0; i < mod->import_count; i++) {
        if (strcmp(mod->imports[i].field_name, name) == 0) {
            mod->imports[i].host_fn = fn;
            return 0;
        }
    }
    return -1; /* import not found */
}

int32_t wasm_call(wasm_module_t *mod, const char *name, int32_t *args, int nargs) {
    if (!mod || !name) return 0;

    for (int i = 0; i < mod->export_count; i++) {
        if (strcmp(mod->exports[i].name, name) == 0) {
            mod->sp = 0;
            mod->fp = 0;
            return exec_function(mod, mod->exports[i].func_idx, args, nargs);
        }
    }
    printf("wasm: export '%s' not found\n", name);
    return 0;
}

int wasm_export_count(wasm_module_t *mod) {
    return mod ? mod->export_count : 0;
}

const char *wasm_export_name(wasm_module_t *mod, int idx) {
    if (!mod || idx < 0 || idx >= mod->export_count) return NULL;
    return mod->exports[idx].name;
}
