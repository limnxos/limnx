#include "libc/libc.h"

#define INPUT_DIM   8
#define HIDDEN_DIM  16
#define NUM_ACTIONS 4
#define MAX_INPUT   128

static const char *action_names[NUM_ACTIONS] = {
    "explore", "gather", "rest", "talk"
};

/* Hash text into a fixed-dimension normalized vector */
static void text_to_vec(const char *text, float *vec, uint32_t dim) {
    memset(vec, 0, dim * sizeof(float));

    for (int i = 0; text[i]; i++) {
        /* Knuth multiplicative hash per character */
        uint32_t h = (uint32_t)(unsigned char)text[i];
        h = h * 2654435761u;
        vec[h % dim] += 1.0f;

        /* Also hash 2-char bigrams for richer features */
        if (text[i + 1]) {
            uint32_t h2 = ((uint32_t)(unsigned char)text[i] << 8)
                         | (uint32_t)(unsigned char)text[i + 1];
            h2 = h2 * 2246822519u;
            vec[h2 % dim] += 0.5f;
        }
    }

    /* L2 normalize */
    float norm = 0.0f;
    for (uint32_t i = 0; i < dim; i++)
        norm += vec[i] * vec[i];
    norm = sqrtf(norm);
    if (norm > 1e-8f) {
        for (uint32_t i = 0; i < dim; i++)
            vec[i] /= norm;
    }
}

/* Read a line with echo */
static int readline(char *buf, int max) {
    int pos = 0;
    while (pos < max - 1) {
        long ch = sys_getchar();
        if (ch == '\n' || ch == '\r') {
            char nl = '\n';
            sys_write(&nl, 1);
            break;
        } else if (ch == '\b' || ch == 127) {
            if (pos > 0) {
                pos--;
                sys_write("\b \b", 3);
            }
        } else if (ch >= 32 && ch < 127) {
            buf[pos++] = (char)ch;
            char c = (char)ch;
            sys_write(&c, 1);
        }
    }
    buf[pos] = '\0';
    return pos;
}

int main(void) {
    printf("\n");
    printf("========================================\n");
    printf("  Limnx Agent\n");
    printf("========================================\n");
    printf("Describe a situation. The agent decides.\n");
    printf("Actions: explore, gather, rest, talk\n");
    printf("Type 'quit' to exit.\n\n");

    agent_t ag;
    if (agent_init(&ag, INPUT_DIM, HIDDEN_DIM, NUM_ACTIONS, 42) != 0) {
        printf("FAIL: agent_init\n");
        return 1;
    }

    printf("Agent initialized (input=%u, hidden=%u, actions=%u)\n\n",
           ag.input_dim, ag.hidden_dim, ag.num_actions);

    char line[MAX_INPUT];
    float vec[INPUT_DIM];
    uint32_t last_action = NUM_ACTIONS; /* sentinel: no action yet */

    for (;;) {
        /* Print current state */
        printf("[state] step=%u, memories=%u",
               ag.step_count, vecstore_count(&ag.memory));
        if (last_action < NUM_ACTIONS)
            printf(", last=%s", action_names[last_action]);
        printf("\n");

        printf("situation> ");
        int len = readline(line, MAX_INPUT);
        if (len == 0)
            continue;
        if (strcmp(line, "quit") == 0)
            break;

        /* Convert text to vector */
        text_to_vec(line, vec, INPUT_DIM);

        /* Check memory for recall before stepping */
        uint32_t recall_idx;
        float recall_score;
        int has_recall = (vecstore_query(&ag.memory, vec, &recall_idx, &recall_score) == 0
                          && recall_score > 0.5f);

        /* Agent step */
        uint32_t action;
        float confidence;
        char label[VECSTORE_MAX_KEY + 1];
        int klen = len;
        if (klen > VECSTORE_MAX_KEY)
            klen = VECSTORE_MAX_KEY;
        memcpy(label, line, (size_t)klen);
        label[klen] = '\0';

        if (agent_step(&ag, vec, label, &action, &confidence) != 0) {
            printf("FAIL: agent_step\n");
            continue;
        }

        /* Print decision */
        printf("  -> action: %s (confidence=%.2f)\n",
               action_names[action], (double)confidence);

        /* Print recall if found */
        if (has_recall) {
            printf("  -> recalled: \"%s\" (similarity=%.2f)\n",
                   ag.memory.entries[recall_idx].key, (double)recall_score);
        }

        last_action = action;
        printf("\n");
    }

    agent_destroy(&ag);
    printf("Goodbye.\n");
    return 0;
}
