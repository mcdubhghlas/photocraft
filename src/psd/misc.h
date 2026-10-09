/* Small PSD structures (ports of crates/psd/src/hdr.rs, metadata.rs, path.rs and tree.rs).
 * Thread-safe. */
#ifndef PC_PSD_MISC_H
#define PC_PSD_MISC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "psd/types.h"

struct pc_psd_file;

/* ---- hdr.rs: HDR toning settings (the Color Mode Data of 32-bit documents) ----
 * Photoshop refuses a 32-bit file whose Color Mode Data is empty or not in this form, so
 * writers emit it. Layout observed in Photoshop-saved files (see the reference). */

#define PC_PSD_HDR_TONING_KEY "hdrt"
#define PC_PSD_HDR_EXTRA_KEY "hdra"
bool pc_psd_is_toning_data(const uint8_t *data, size_t len);
/* The 110 bytes Photoshop writes for a new 32-bit document. */
pc_status pc_psd_default_toning_data(pc_psd_bytes *out, pc_error *err);
/* What a writer emits with nothing of its own: the toning records at depth 32, else nothing. */
pc_status pc_psd_color_mode_data_for_depth(uint16_t depth, pc_psd_bytes *out, pc_error *err);

/* ---- metadata.rs: shmd items ---- */

typedef struct pc_psd_meta_item {
    uint8_t signature[4];
    uint8_t key[4];
    bool copy; /* copy on sheet duplication */
    uint8_t reserved[3];
    pc_psd_bytes data; /* including any padding counted in the stored length */
} pc_psd_meta_item;

/* A new 8BIM item; data is zero-padded to an even length. */
pc_status pc_psd_meta_item_make(const char key[4], const void *data, size_t len, pc_psd_meta_item *out, pc_error *err);
pc_status pc_psd_parse_shmd(const uint8_t *data, size_t len, pc_psd_meta_item **items, size_t *n, pc_error *err);
pc_status pc_psd_write_shmd(const pc_psd_meta_item *items, size_t n, pc_psd_bytes *out, pc_error *err);
void pc_psd_meta_items_free(pc_psd_meta_item *items, size_t n);
bool pc_psd_meta_item_eq(const pc_psd_meta_item *a, const pc_psd_meta_item *b);

/* ---- path.rs: path records (resources 2000-2997, 1025; vmsk/vsms blocks) ---- */

#define PC_PSD_PATH_RECORD_LEN 26
enum {
    PC_PSD_PATH_CLOSED_LENGTH = 0,
    PC_PSD_PATH_CLOSED_KNOT_LINKED = 1,
    PC_PSD_PATH_CLOSED_KNOT_UNLINKED = 2,
    PC_PSD_PATH_OPEN_LENGTH = 3,
    PC_PSD_PATH_OPEN_KNOT_LINKED = 4,
    PC_PSD_PATH_OPEN_KNOT_UNLINKED = 5,
    PC_PSD_PATH_FILL_RULE = 6,
    PC_PSD_PATH_CLIPBOARD = 7,
    PC_PSD_PATH_INITIAL_FILL = 8
};

typedef struct pc_psd_path_record {
    uint16_t selector;
    uint8_t data[24];
} pc_psd_path_record;

typedef struct pc_psd_path {
    pc_psd_path_record *records;
    size_t n;
} pc_psd_path;

/* A knot as fractions of the document size, (x, y) pairs. */
typedef struct pc_psd_knot {
    bool linked;
    double pre[2];
    double anchor[2];
    double post[2];
} pc_psd_knot;

typedef struct pc_psd_subpath {
    bool closed;
    int16_t operation; /* -1/1 combine, 2 subtract, 3 intersect, 0 exclude */
    pc_psd_knot *knots;
    size_t n;
} pc_psd_subpath;

double pc_psd_fixed_to_f64(int32_t v);
int32_t pc_psd_f64_to_fixed(double v); /* rounded, saturating */
/* Records of 26 bytes; a trailing partial record is ignored. PC_ERR_LIMIT past 4M records. */
pc_status pc_psd_path_parse(const uint8_t *data, size_t len, pc_psd_path *out, pc_error *err);
pc_status pc_psd_path_to_bytes(const pc_psd_path *p, pc_psd_bytes *out, pc_error *err);
void pc_psd_path_fini(pc_psd_path *p);
bool pc_psd_path_initial_fill(const pc_psd_path *p);
/* Subpaths with their knots (lenient about missing length records). */
pc_status pc_psd_path_subpaths(const pc_psd_path *p, pc_psd_subpath **out, size_t *n, pc_error *err);
void pc_psd_subpaths_free(pc_psd_subpath *s, size_t n);
/* Fill rule, initial fill, then each subpath's length record and knots. */
pc_status pc_psd_path_from_subpaths(const pc_psd_subpath *s, size_t n, bool initial_fill, pc_psd_path *out,
                                    pc_error *err);

/* vmsk / vsms: version, flags (1 invert, 2 not linked, 4 disabled), path records. */
typedef struct pc_psd_vector_mask {
    uint32_t version;
    uint32_t flags;
    pc_psd_path path;
} pc_psd_vector_mask;
pc_status pc_psd_vector_mask_parse(const uint8_t *data, size_t len, pc_psd_vector_mask *out, pc_error *err);
pc_status pc_psd_vector_mask_to_bytes(const pc_psd_vector_mask *m, pc_psd_bytes *out, pc_error *err);

/* ---- tree.rs: nested layer tree from section dividers ----
 * Layers are stored bottom-to-top; a group is a bounding divider (lsct 3) below its children,
 * then the children, then the group's own record (lsct 1/2). Malformed nesting is tolerated: a
 * folder without a divider becomes an empty group; unclosed dividers are dropped and their
 * children hoisted. */
typedef struct pc_psd_node {
    bool is_group;
    size_t index;        /* record index (the folder record for groups) */
    bool has_divider;
    size_t divider;      /* bounding divider index */
    struct pc_psd_node *children; /* bottom-to-top */
    size_t n;
} pc_psd_node;

pc_status pc_psd_layer_tree(const struct pc_psd_file *f, pc_psd_node **roots, size_t *n, pc_error *err);
void pc_psd_nodes_free(pc_psd_node *nodes, size_t n);
size_t pc_psd_node_count(const pc_psd_node *node); /* subtree size including itself */

#endif
