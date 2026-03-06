#include "libc.h"

int agent_init(agent_t *ag, uint32_t input_dim, uint32_t hidden_dim,
               uint32_t num_actions, uint32_t seed) {
    ag->input_dim = input_dim;
    ag->hidden_dim = hidden_dim;
    ag->num_actions = num_actions;
    ag->step_count = 0;
    ag->prng_state = seed;

    /* Create MLP weight tensors: input is [input | context] = input_dim * 2 */
    ag->w1 = tensor_create(input_dim * 2, hidden_dim);
    ag->b1 = tensor_create(1, hidden_dim);
    ag->w2 = tensor_create(hidden_dim, num_actions);
    ag->b2 = tensor_create(1, num_actions);

    if (!ag->w1.data || !ag->b1.data || !ag->w2.data || !ag->b2.data) {
        agent_destroy(ag);
        return -1;
    }

    /* Initialize weights with random values */
    tensor_fill_random(&ag->w1, &ag->prng_state);
    tensor_fill_random(&ag->b1, &ag->prng_state);
    tensor_fill_random(&ag->w2, &ag->prng_state);
    tensor_fill_random(&ag->b2, &ag->prng_state);

    /* Initialize memory */
    if (vecstore_init(&ag->memory, input_dim) != 0) {
        agent_destroy(ag);
        return -1;
    }

    return 0;
}

int agent_step(agent_t *ag, const float *input, const char *label,
               uint32_t *out_action, float *out_confidence) {
    uint32_t idim = ag->input_dim;

    /* 1. Retrieve: query memory with input vector */
    uint32_t match_idx = 0;
    float match_score = 0.0f;
    int has_match = (vecstore_query(&ag->memory, input, &match_idx, &match_score) == 0
                     && match_score > 0.5f);

    /* 2. Build combined input: [input | context] */
    tensor_t combined = tensor_create(1, idim * 2);
    if (!combined.data)
        return -1;

    /* Copy input into first half */
    for (uint32_t i = 0; i < idim; i++)
        combined.data[i] = input[i];

    /* Copy context into second half (retrieved vector or zeros) */
    if (has_match) {
        memcpy(&combined.data[idim],
               &ag->memory.vectors[match_idx * idim],
               idim * sizeof(float));
    } else {
        for (uint32_t i = 0; i < idim; i++)
            combined.data[idim + i] = 0.0f;
    }

    /* 3. Forward pass: combined(1 x idim*2) * W1(idim*2 x hidden) + b1 -> ReLU */
    tensor_t hidden = tensor_create(1, ag->hidden_dim);
    if (!hidden.data) {
        tensor_destroy(&combined);
        return -1;
    }
    tensor_matmul(&hidden, &combined, &ag->w1);
    tensor_add_bias(&hidden, &hidden, &ag->b1);
    tensor_relu(&hidden);

    /* hidden(1 x hidden) * W2(hidden x actions) + b2 -> softmax */
    tensor_t output = tensor_create(1, ag->num_actions);
    if (!output.data) {
        tensor_destroy(&hidden);
        tensor_destroy(&combined);
        return -1;
    }
    tensor_matmul(&output, &hidden, &ag->w2);
    tensor_add_bias(&output, &output, &ag->b2);
    tensor_softmax(&output);

    /* Action = argmax, confidence = max probability */
    uint32_t action = tensor_argmax(&output);
    *out_action = action;
    *out_confidence = output.data[action];

    /* 4. Store current input into memory */
    vecstore_store(&ag->memory, label, input);
    ag->step_count++;

    /* 5. Cleanup temporaries */
    tensor_destroy(&output);
    tensor_destroy(&hidden);
    tensor_destroy(&combined);

    return 0;
}

void agent_destroy(agent_t *ag) {
    tensor_destroy(&ag->w1);
    tensor_destroy(&ag->b1);
    tensor_destroy(&ag->w2);
    tensor_destroy(&ag->b2);
    vecstore_destroy(&ag->memory);
}
