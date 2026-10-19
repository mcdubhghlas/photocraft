#include "recent.h"
#include "base/mem.h"
#include <stdlib.h>
#include <string.h>
void pc_ui_recent_clear(pc_ui_state *state)
{
    for (size_t i = 0; i < state->n_recent_files; i++) {
        free(state->recent_files[i]);
    }
    free(state->recent_files);
    state->recent_files = NULL;
    state->n_recent_files = 0;
}
pc_status pc_ui_recent_push(pc_ui_state *state, const char *path, size_t limit, pc_error *err)
{
    if (!state || !path || !*path || strlen(path) > 65536 || limit > 100) {
        return pc_fail(err, PC_ERR_INVALID, "invalid recent file");
    }
    if (!limit) {
        pc_ui_recent_clear(state);
        return PC_OK;
    }
    char **next = pc_calloc_array(limit, sizeof *next);
    char *copy = pc_strndup(path, strlen(path));
    if (!next || !copy) {
        free(next);
        free(copy);
        return pc_fail(err, PC_ERR_NOMEM, "recent files");
    }
    size_t n = 1;
    next[0] = copy;
    for (size_t i = 0; i < state->n_recent_files; i++) {
        if (n < limit && strcmp(state->recent_files[i], path)) {
            next[n++] = state->recent_files[i];
        } else {
            free(state->recent_files[i]);
        }
    }
    free(state->recent_files);
    state->recent_files = next;
    state->n_recent_files = n;
    return PC_OK;
}
