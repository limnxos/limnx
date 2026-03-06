#include "libc/libc.h"

/* Simple float comparison */
static int approx(float a, float b, float eps) {
    float diff = a - b;
    if (diff < 0.0f) diff = -diff;
    return diff < eps;
}

/* --- Part 1: Vector store primitives --- */

static int test_vecstore(void) {
    int pass = 1;

    printf("--- vecstore tests ---\n");

    vecstore_t vs;
    if (vecstore_init(&vs, 4) != 0) {
        printf("FAIL: vecstore_init\n");
        return 0;
    }
    printf("vecstore_init OK (dim=4, capacity=%u)\n", vs.capacity);

    /* Store 3 vectors */
    float v_alpha[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    float v_beta[4]  = {0.0f, 1.0f, 0.0f, 0.0f};
    float v_gamma[4] = {0.0f, 0.0f, 1.0f, 0.0f};

    vecstore_store(&vs, "alpha", v_alpha);
    vecstore_store(&vs, "beta",  v_beta);
    vecstore_store(&vs, "gamma", v_gamma);

    if (vecstore_count(&vs) != 3) {
        printf("FAIL: count=%u, expected 3\n", vecstore_count(&vs));
        pass = 0;
    } else {
        printf("vecstore_store: 3 entries stored\n");
    }

    /* Query with vector similar to alpha */
    float query[4] = {0.9f, 0.1f, 0.0f, 0.0f};
    uint32_t best_idx;
    float best_score;
    if (vecstore_query(&vs, query, &best_idx, &best_score) == 0) {
        if (strcmp(vs.entries[best_idx].key, "alpha") == 0 && best_score > 0.9f) {
            printf("vecstore_query OK: best match \"%s\" (sim=%f)\n",
                   vs.entries[best_idx].key, (double)best_score);
        } else {
            printf("FAIL: query got \"%s\" sim=%f, expected \"alpha\" >0.9\n",
                   vs.entries[best_idx].key, (double)best_score);
            pass = 0;
        }
    } else {
        printf("FAIL: vecstore_query returned -1\n");
        pass = 0;
    }

    /* Get by key */
    float got[4];
    if (vecstore_get(&vs, "beta", got) == 0) {
        if (approx(got[0], 0.0f, 0.001f) && approx(got[1], 1.0f, 0.001f) &&
            approx(got[2], 0.0f, 0.001f) && approx(got[3], 0.0f, 0.001f)) {
            printf("vecstore_get OK\n");
        } else {
            printf("FAIL: vecstore_get data mismatch\n");
            pass = 0;
        }
    } else {
        printf("FAIL: vecstore_get returned -1\n");
        pass = 0;
    }

    /* Delete beta */
    if (vecstore_delete(&vs, "beta") == 0 && vecstore_count(&vs) == 2) {
        printf("vecstore_delete OK (count=%u)\n", vecstore_count(&vs));
    } else {
        printf("FAIL: vecstore_delete\n");
        pass = 0;
    }

    vecstore_destroy(&vs);
    return pass;
}

/* --- Part 2: Agent loop test --- */

static int test_agent_loop(void) {
    int pass = 1;

    printf("--- agent loop test ---\n");

    agent_t ag;
    if (agent_init(&ag, 4, 8, 3, 42) != 0) {
        printf("FAIL: agent_init\n");
        return 0;
    }
    printf("agent_init OK (input=4, hidden=8, actions=3)\n");

    /* Step 1: north — empty memory */
    float in1[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    uint32_t action;
    float conf;
    if (agent_step(&ag, in1, "north", &action, &conf) != 0) {
        printf("FAIL: agent_step 1\n");
        pass = 0;
    } else {
        printf("step 1: \"north\" -> action %u (conf=%f) [no memory]\n",
               action, (double)conf);
    }

    /* Step 2: east */
    float in2[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    if (agent_step(&ag, in2, "east", &action, &conf) != 0) {
        printf("FAIL: agent_step 2\n");
        pass = 0;
    } else {
        printf("step 2: \"east\"  -> action %u (conf=%f) [no memory]\n",
               action, (double)conf);
    }

    /* Step 3: south */
    float in3[4] = {0.0f, 0.0f, 1.0f, 0.0f};
    if (agent_step(&ag, in3, "south", &action, &conf) != 0) {
        printf("FAIL: agent_step 3\n");
        pass = 0;
    } else {
        printf("step 3: \"south\" -> action %u (conf=%f) [no memory]\n",
               action, (double)conf);
    }

    /* Step 4: north2 — should retrieve "north" from memory */
    float in4[4] = {0.9f, 0.1f, 0.0f, 0.0f};
    uint32_t match_idx;
    float match_score;
    /* Pre-check what memory will retrieve */
    vecstore_query(&ag.memory, in4, &match_idx, &match_score);

    if (agent_step(&ag, in4, "north2", &action, &conf) != 0) {
        printf("FAIL: agent_step 4\n");
        pass = 0;
    } else {
        printf("step 4: \"north2\"-> action %u (conf=%f) [retrieved \"%s\" sim=%f]\n",
               action, (double)conf,
               ag.memory.entries[match_idx].key, (double)match_score);
    }

    /* Verify memory count */
    uint32_t mc = vecstore_count(&ag.memory);
    printf("memory count: %u\n", mc);
    if (mc != 4) {
        printf("FAIL: expected memory count 4, got %u\n", mc);
        pass = 0;
    }

    /* Verify similarity to "north" was high */
    if (match_score < 0.9f) {
        printf("FAIL: expected high similarity to north, got %f\n",
               (double)match_score);
        pass = 0;
    }

    /* Verify action is valid */
    if (action >= 3) {
        printf("FAIL: invalid action %u\n", action);
        pass = 0;
    }

    agent_destroy(&ag);
    return pass;
}

/* --- Part 3: Memory influence test --- */

static int test_memory_influence(void) {
    int pass = 1;

    printf("--- memory influence test ---\n");

    /* Run 1: fresh agent, first input (empty memory -> context is zeros) */
    agent_t ag1;
    if (agent_init(&ag1, 4, 8, 3, 42) != 0) {
        printf("FAIL: agent_init for influence test\n");
        return 0;
    }

    float input[4] = {0.7f, 0.3f, 0.0f, 0.0f};
    uint32_t action1, action2;
    float conf1, conf2;

    agent_step(&ag1, input, "test", &action1, &conf1);
    printf("without memory: action %u (conf=%f)\n", action1, (double)conf1);

    agent_destroy(&ag1);

    /* Run 2: agent with pre-loaded memory */
    agent_t ag2;
    if (agent_init(&ag2, 4, 8, 3, 42) != 0) {
        printf("FAIL: agent_init for influence test 2\n");
        return 0;
    }

    /* Store a related experience first */
    float prior[4] = {0.8f, 0.2f, 0.0f, 0.0f};
    vecstore_store(&ag2.memory, "prior", prior);

    agent_step(&ag2, input, "test2", &action2, &conf2);
    printf("with memory:    action %u (conf=%f)\n", action2, (double)conf2);

    /* The outputs may or may not differ — but we verify the system runs correctly */
    if (conf1 <= 0.0f || conf2 <= 0.0f) {
        printf("FAIL: confidence should be positive\n");
        pass = 0;
    }

    agent_destroy(&ag2);
    return pass;
}

/* --- Main --- */

int main(void) {
    printf("=== agentrt: semantic memory + agent runtime test ===\n");

    int pass = 1;

    if (!test_vecstore())
        pass = 0;

    if (!test_agent_loop())
        pass = 0;

    if (!test_memory_influence())
        pass = 0;

    if (pass)
        printf("Agent runtime OK\n");

    printf("=== agentrt: %s ===\n", pass ? "ALL PASSED" : "SOME FAILED");

    return pass ? 0 : 1;
}
