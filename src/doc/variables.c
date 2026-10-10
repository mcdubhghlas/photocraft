/* Image > Variables and Data Sets (port of crates/doc/src/variables.rs). */
#include "doc/variables.h"

#include <stdlib.h>
#include <string.h>

#include "base/mem.h"

/* ---- variable definitions ---- */

static bool var_kind_eq(const pc_var_kind *a, const pc_var_kind *b)
{
    if (a->type != b->type) {
        return false;
    }
    /* Only Pixel Replacement carries fields. */
    return a->type != PC_VAR_PIXEL_REPLACEMENT ||
           (a->method == b->method && a->align == b->align && a->clip == b->clip);
}

void pc_variable_def_fini(pc_variable_def *d)
{
    if (d == NULL) {
        return;
    }
    pc_str_fini(&d->name);
    memset(d, 0, sizeof *d);
}

pc_status pc_variable_def_clone(const pc_variable_def *d, pc_variable_def *out, pc_error *err)
{
    pc_variable_def t = *d;
    pc_status st = pc_str_clone(&d->name, &t.name, err);
    if (st != PC_OK) {
        return st;
    }
    *out = t;
    return PC_OK;
}

bool pc_variable_def_eq(const pc_variable_def *a, const pc_variable_def *b)
{
    return pc_str_eq(&a->name, &b->name) && a->layer == b->layer && var_kind_eq(&a->kind, &b->kind);
}

/* ---- data values and sets ---- */

void pc_data_value_fini(pc_data_value *v)
{
    if (v == NULL) {
        return;
    }
    pc_str_fini(&v->variable);
    pc_str_fini(&v->text);
    memset(v, 0, sizeof *v);
}

pc_status pc_data_value_clone(const pc_data_value *v, pc_data_value *out, pc_error *err)
{
    pc_data_value t = *v;
    pc_status st;
    t.variable = (pc_str)PC_STR_INIT;
    t.text = (pc_str)PC_STR_INIT;
    st = pc_str_clone(&v->variable, &t.variable, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_str_clone(&v->text, &t.text, err);
    if (st != PC_OK) {
        pc_str_fini(&t.variable);
        return st;
    }
    *out = t;
    return PC_OK;
}

bool pc_data_value_eq(const pc_data_value *a, const pc_data_value *b)
{
    if (!pc_str_eq(&a->variable, &b->variable) || a->kind != b->kind) {
        return false;
    }
    if (a->kind == PC_DATA_VISIBILITY) {
        return a->visibility == b->visibility;
    }
    return pc_str_eq(&a->text, &b->text);
}

void pc_data_set_fini(pc_data_set *s)
{
    size_t i;
    if (s == NULL) {
        return;
    }
    pc_str_fini(&s->name);
    for (i = 0; i < s->nvalues; i++) {
        pc_data_value_fini(&s->values[i]);
    }
    free(s->values);
    memset(s, 0, sizeof *s);
}

pc_status pc_data_set_clone(const pc_data_set *s, pc_data_set *out, pc_error *err)
{
    pc_data_set t;
    size_t i;
    pc_status st;
    memset(&t, 0, sizeof t);
    st = pc_str_clone(&s->name, &t.name, err);
    if (st != PC_OK) {
        return st;
    }
    if (s->nvalues > 0) {
        t.values = pc_calloc_array(s->nvalues, sizeof *t.values);
        if (t.values == NULL) {
            pc_str_fini(&t.name);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        for (i = 0; i < s->nvalues; i++) {
            st = pc_data_value_clone(&s->values[i], &t.values[i], err);
            if (st != PC_OK) {
                pc_data_set_fini(&t); /* nvalues counts the clones made so far */
                return st;
            }
            t.nvalues = i + 1;
        }
    }
    *out = t;
    return PC_OK;
}

bool pc_data_set_eq(const pc_data_set *a, const pc_data_set *b)
{
    size_t i;
    if (!pc_str_eq(&a->name, &b->name) || a->nvalues != b->nvalues) {
        return false;
    }
    for (i = 0; i < a->nvalues; i++) {
        if (!pc_data_value_eq(&a->values[i], &b->values[i])) {
            return false;
        }
    }
    return true;
}

/* ---- variables ---- */

void pc_variables_fini(pc_variables *v)
{
    size_t i;
    if (v == NULL) {
        return;
    }
    for (i = 0; i < v->ndefs; i++) {
        pc_variable_def_fini(&v->defs[i]);
    }
    free(v->defs);
    for (i = 0; i < v->ndata_sets; i++) {
        pc_data_set_fini(&v->data_sets[i]);
    }
    free(v->data_sets);
    memset(v, 0, sizeof *v);
}

pc_status pc_variables_clone(const pc_variables *v, pc_variables *out, pc_error *err)
{
    pc_variables t;
    size_t i;
    pc_status st = PC_OK;
    memset(&t, 0, sizeof t);
    if (v->ndefs > 0) {
        t.defs = pc_calloc_array(v->ndefs, sizeof *t.defs);
        if (t.defs == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
    }
    if (v->ndata_sets > 0) {
        t.data_sets = pc_calloc_array(v->ndata_sets, sizeof *t.data_sets);
        if (t.data_sets == NULL) {
            free(t.defs);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
    }
    /* The counts track the clones made so far, so pc_variables_fini cleans up a failure. */
    for (i = 0; i < v->ndefs && st == PC_OK; i++) {
        st = pc_variable_def_clone(&v->defs[i], &t.defs[i], err);
        if (st == PC_OK) {
            t.ndefs = i + 1;
        }
    }
    for (i = 0; i < v->ndata_sets && st == PC_OK; i++) {
        st = pc_data_set_clone(&v->data_sets[i], &t.data_sets[i], err);
        if (st == PC_OK) {
            t.ndata_sets = i + 1;
        }
    }
    if (st != PC_OK) {
        pc_variables_fini(&t);
        return st;
    }
    t.has_active = v->has_active;
    t.active = v->active;
    *out = t;
    return PC_OK;
}

bool pc_variables_eq(const pc_variables *a, const pc_variables *b)
{
    size_t i;
    if (a->ndefs != b->ndefs || a->ndata_sets != b->ndata_sets || a->has_active != b->has_active ||
        (a->has_active && a->active != b->active)) {
        return false;
    }
    for (i = 0; i < a->ndefs; i++) {
        if (!pc_variable_def_eq(&a->defs[i], &b->defs[i])) {
            return false;
        }
    }
    for (i = 0; i < a->ndata_sets; i++) {
        if (!pc_data_set_eq(&a->data_sets[i], &b->data_sets[i])) {
            return false;
        }
    }
    return true;
}

bool pc_variables_is_empty(const pc_variables *v)
{
    return v->ndefs == 0 && v->ndata_sets == 0;
}

const pc_variable_def *pc_variables_def(const pc_variables *v, const char *name, size_t len)
{
    size_t i;
    for (i = 0; i < v->ndefs; i++) {
        if (pc_str_eq_mem(&v->defs[i].name, name, len)) {
            return &v->defs[i];
        }
    }
    return NULL;
}

bool pc_variables_data_set_index(const pc_variables *v, const char *name, size_t len, size_t *out)
{
    size_t i;
    for (i = 0; i < v->ndata_sets; i++) {
        if (pc_str_eq_mem(&v->data_sets[i].name, name, len)) {
            *out = i;
            return true;
        }
    }
    return false;
}

const pc_data_set *pc_variables_data_set(const pc_variables *v, const char *name, size_t len)
{
    size_t i;
    return pc_variables_data_set_index(v, name, len, &i) ? &v->data_sets[i] : NULL;
}

const pc_data_value *pc_data_set_get(const pc_data_set *s, const char *variable, size_t len)
{
    size_t i;
    for (i = 0; i < s->nvalues; i++) {
        if (pc_str_eq_mem(&s->values[i].variable, variable, len)) {
            return &s->values[i];
        }
    }
    return NULL;
}
