#include "libc/libc.h"

#define DIM 4

static int failures = 0;

static void check(const char *name, int cond) {
    if (cond) {
        printf("  [memtest] %s: OK\n", name);
    } else {
        printf("  [memtest] %s: FAIL\n", name);
        failures++;
    }
}

int main(void) {
    printf("=== memtest: persistent vecstore test ===\n");

    /* Phase 1: Init and store 3 orthogonal vectors */
    vecstore_t vs;
    if (vecstore_init(&vs, DIM) != 0) {
        printf("FAIL: vecstore_init\n");
        return 1;
    }

    /* Orthogonal unit vectors */
    float v_alpha[DIM] = { 1.0f, 0.0f, 0.0f, 0.0f };
    float v_beta[DIM]  = { 0.0f, 1.0f, 0.0f, 0.0f };
    float v_gamma[DIM] = { 0.0f, 0.0f, 1.0f, 0.0f };

    vecstore_store(&vs, "alpha", v_alpha);
    vecstore_store(&vs, "beta", v_beta);
    vecstore_store(&vs, "gamma", v_gamma);

    check("store count=3", vecstore_count(&vs) == 3);

    /* Phase 2: Save to file */
    int save_rc = vecstore_save(&vs, "/vecmem.dat");
    check("save", save_rc == 0);

    /* Phase 3: Destroy (wipes all state) */
    vecstore_destroy(&vs);

    /* Phase 4: Re-init fresh vecstore */
    if (vecstore_init(&vs, DIM) != 0) {
        printf("FAIL: vecstore_init (second)\n");
        return 1;
    }
    check("fresh count=0", vecstore_count(&vs) == 0);

    /* Phase 5: Load from file */
    int load_rc = vecstore_load(&vs, "/vecmem.dat");
    check("load", load_rc == 0);
    check("loaded count=3", vecstore_count(&vs) == 3);

    /* Phase 6: Query — "alpha" vector should match "alpha" entry */
    uint32_t match_idx;
    float match_score;
    int q_rc = vecstore_query(&vs, v_alpha, &match_idx, &match_score);
    check("query alpha", q_rc == 0 && match_score > 0.99f);
    check("query alpha key",
          q_rc == 0 && strcmp(vs.entries[match_idx].key, "alpha") == 0);

    /* Phase 7: Get — retrieve beta vector and verify */
    float out[DIM];
    int g_rc = vecstore_get(&vs, "beta", out);
    check("get beta", g_rc == 0);
    check("beta data correct",
          g_rc == 0 && out[0] < 0.01f && out[1] > 0.99f &&
          out[2] < 0.01f && out[3] < 0.01f);

    /* Phase 8: Save again (overwrite test) */
    vecstore_store(&vs, "delta", (float[]){ 0.0f, 0.0f, 0.0f, 1.0f });
    save_rc = vecstore_save(&vs, "/vecmem.dat");
    check("overwrite save", save_rc == 0);

    /* Reload and verify count grew */
    vecstore_destroy(&vs);
    vecstore_init(&vs, DIM);
    load_rc = vecstore_load(&vs, "/vecmem.dat");
    check("reload count=4", load_rc == 0 && vecstore_count(&vs) == 4);

    /* Phase 9: Dim mismatch should fail */
    vecstore_t vs2;
    vecstore_init(&vs2, 8);  /* different dim */
    int bad_rc = vecstore_load(&vs2, "/vecmem.dat");
    check("dim mismatch rejected", bad_rc != 0);
    vecstore_destroy(&vs2);

    /* Cleanup */
    sys_unlink("/vecmem.dat");
    vecstore_destroy(&vs);

    if (failures == 0)
        printf("=== memtest: ALL PASSED ===\n");
    else
        printf("=== memtest: %d FAILED ===\n", failures);

    return failures;
}
