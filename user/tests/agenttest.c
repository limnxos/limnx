#include "libc/libc.h"

/* Simple float comparison */
static int approx(float a, float b, float eps) {
    float diff = a - b;
    if (diff < 0.0f) diff = -diff;
    return diff < eps;
}

static int test_primitives(void) {
    int pass = 1;

    printf("--- tensor primitive tests ---\n");

    /* Create */
    tensor_t a = tensor_create(2, 3);
    if (!a.data) { printf("FAIL: tensor_create\n"); return 0; }
    printf("tensor_create(2,3) OK: %u elements\n", a.size);

    /* Fill */
    tensor_fill(&a, 1.5f);
    int fill_ok = 1;
    for (uint32_t i = 0; i < a.size; i++)
        if (!approx(a.data[i], 1.5f, 0.001f)) { fill_ok = 0; break; }
    if (fill_ok) printf("tensor_fill OK\n");
    else { printf("FAIL: tensor_fill\n"); pass = 0; }

    /* Add: a(2x3) all 1.5 + b(2x3) all 2.0 = c(2x3) all 3.5 */
    tensor_t b = tensor_create(2, 3);
    tensor_t c = tensor_create(2, 3);
    tensor_fill(&b, 2.0f);
    tensor_add(&c, &a, &b);
    int add_ok = 1;
    for (uint32_t i = 0; i < c.size; i++)
        if (!approx(c.data[i], 3.5f, 0.001f)) { add_ok = 0; break; }
    if (add_ok) printf("tensor_add OK\n");
    else { printf("FAIL: tensor_add\n"); pass = 0; }

    tensor_destroy(&a);
    tensor_destroy(&b);
    tensor_destroy(&c);

    /* Matmul: [1,2; 3,4](2x2) * [5,6; 7,8](2x2) = [19,22; 43,50] */
    tensor_t m1 = tensor_create(2, 2);
    tensor_t m2 = tensor_create(2, 2);
    tensor_t m3 = tensor_create(2, 2);
    m1.data[0] = 1.0f; m1.data[1] = 2.0f;
    m1.data[2] = 3.0f; m1.data[3] = 4.0f;
    m2.data[0] = 5.0f; m2.data[1] = 6.0f;
    m2.data[2] = 7.0f; m2.data[3] = 8.0f;
    tensor_matmul(&m3, &m1, &m2);
    if (approx(m3.data[0], 19.0f, 0.01f) &&
        approx(m3.data[1], 22.0f, 0.01f) &&
        approx(m3.data[2], 43.0f, 0.01f) &&
        approx(m3.data[3], 50.0f, 0.01f))
        printf("tensor_matmul OK\n");
    else { printf("FAIL: tensor_matmul\n"); pass = 0; }

    tensor_destroy(&m1);
    tensor_destroy(&m2);
    tensor_destroy(&m3);

    /* ReLU */
    tensor_t r = tensor_create(1, 4);
    r.data[0] = -2.0f; r.data[1] = 0.0f;
    r.data[2] = 3.0f;  r.data[3] = -0.5f;
    tensor_relu(&r);
    if (approx(r.data[0], 0.0f, 0.001f) &&
        approx(r.data[1], 0.0f, 0.001f) &&
        approx(r.data[2], 3.0f, 0.001f) &&
        approx(r.data[3], 0.0f, 0.001f))
        printf("tensor_relu OK\n");
    else { printf("FAIL: tensor_relu\n"); pass = 0; }
    tensor_destroy(&r);

    /* Softmax */
    tensor_t s = tensor_create(1, 3);
    s.data[0] = 1.0f; s.data[1] = 2.0f; s.data[2] = 3.0f;
    tensor_softmax(&s);
    float sum = s.data[0] + s.data[1] + s.data[2];
    if (approx(sum, 1.0f, 0.001f) &&
        s.data[0] < s.data[1] && s.data[1] < s.data[2])
        printf("tensor_softmax OK (sum=%f)\n", (double)sum);
    else { printf("FAIL: tensor_softmax\n"); pass = 0; }
    tensor_destroy(&s);

    return pass;
}

static int test_agent_forward(void) {
    printf("--- agent forward pass ---\n");

    uint32_t seed = 42;

    /* Layer 1: 4 -> 8 */
    tensor_t W1 = tensor_create(4, 8);
    tensor_t b1 = tensor_create(1, 8);
    tensor_fill_random(&W1, &seed);
    tensor_fill_random(&b1, &seed);

    /* Layer 2: 8 -> 3 */
    tensor_t W2 = tensor_create(8, 3);
    tensor_t b2 = tensor_create(1, 3);
    tensor_fill_random(&W2, &seed);
    tensor_fill_random(&b2, &seed);

    printf("Weights initialized (seed=42)\n");

    /* Input vector (1x4) */
    tensor_t input = tensor_create(1, 4);
    input.data[0] = 0.5f;
    input.data[1] = -0.3f;
    input.data[2] = 0.8f;
    input.data[3] = 0.1f;

    printf("Input:  [%f, %f, %f, %f]\n",
           (double)input.data[0], (double)input.data[1],
           (double)input.data[2], (double)input.data[3]);

    /* Forward pass: input(1x4) * W1(4x8) + b1(1x8) -> ReLU */
    tensor_t h1 = tensor_create(1, 8);
    tensor_matmul(&h1, &input, &W1);
    tensor_add_bias(&h1, &h1, &b1);
    tensor_relu(&h1);

    /* h1(1x8) * W2(8x3) + b2(1x3) -> softmax */
    tensor_t output = tensor_create(1, 3);
    tensor_matmul(&output, &h1, &W2);
    tensor_add_bias(&output, &output, &b2);
    tensor_softmax(&output);

    printf("Output: [%f, %f, %f]\n",
           (double)output.data[0], (double)output.data[1],
           (double)output.data[2]);

    /* Validate */
    int pass = 1;

    /* All outputs non-negative */
    for (uint32_t i = 0; i < 3; i++) {
        if (output.data[i] < 0.0f) {
            printf("FAIL: output[%u] = %f < 0\n", i, (double)output.data[i]);
            pass = 0;
        }
    }

    /* Softmax sums to 1.0 */
    float sum = output.data[0] + output.data[1] + output.data[2];
    if (!approx(sum, 1.0f, 0.01f)) {
        printf("FAIL: softmax sum = %f\n", (double)sum);
        pass = 0;
    }

    /* Decision */
    uint32_t decision = tensor_argmax(&output);
    printf("Decision: action %u (confidence=%f)\n",
           decision, (double)output.data[decision]);

    if (pass)
        printf("Agent forward pass OK\n");

    /* Cleanup */
    tensor_destroy(&output);
    tensor_destroy(&h1);
    tensor_destroy(&input);
    tensor_destroy(&b2);
    tensor_destroy(&W2);
    tensor_destroy(&b1);
    tensor_destroy(&W1);

    return pass;
}

int main(void) {
    printf("=== agenttest: tensor + agent runtime test ===\n");

    int pass = 1;

    if (!test_primitives())
        pass = 0;

    if (!test_agent_forward())
        pass = 0;

    printf("=== agenttest: %s ===\n", pass ? "ALL PASSED" : "SOME FAILED");

    return pass ? 0 : 1;
}
