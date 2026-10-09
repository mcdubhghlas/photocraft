/* Image resources section: `8BIM` resource blocks (port of crates/psd/src/resources.rs).
 * Pure data; thread-safe. */
#ifndef PC_PSD_RESOURCES_H
#define PC_PSD_RESOURCES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "psd/descriptor.h"
#include "psd/types.h"

/* Well-known resource ids. */
enum {
    PC_PSD_RES_RESOLUTION_INFO = 1005,
    PC_PSD_RES_LAYER_STATE = 1024,
    PC_PSD_RES_LAYER_GROUP_INFO = 1026,
    PC_PSD_RES_THUMBNAIL_PS4 = 1033,
    PC_PSD_RES_THUMBNAIL = 1036,
    PC_PSD_RES_GLOBAL_ANGLE = 1037,
    PC_PSD_RES_ICC_PROFILE = 1039,
    PC_PSD_RES_GLOBAL_ALTITUDE = 1049,
    PC_PSD_RES_SLICES = 1050,
    PC_PSD_RES_VERSION_INFO = 1057,
    PC_PSD_RES_EXIF = 1058,
    PC_PSD_RES_XMP = 1060
};

/* One resource block. */
typedef struct pc_psd_resource {
    uint8_t signature[4]; /* normally 8BIM (also MeSa, AgHg, PHUT, DCSR) */
    uint16_t id;
    pc_psd_bytes name; /* Pascal name bytes (usually empty) */
    pc_psd_bytes data; /* without the even padding */
} pc_psd_resource;

/* A new 8BIM resource with an empty name and a copy of data. */
pc_status pc_psd_resource_init(pc_psd_resource *r, uint16_t id, const void *data, size_t len, pc_error *err);
void pc_psd_resource_fini(pc_psd_resource *r);
pc_status pc_psd_resource_clone(const pc_psd_resource *r, pc_psd_resource *out, pc_error *err);
bool pc_psd_resource_eq(const pc_psd_resource *a, const pc_psd_resource *b);

/* Fixed-point resolution info (1005). */
typedef struct pc_psd_resolution {
    uint32_t h_res_fixed; /* 16.16 */
    uint16_t h_res_unit;  /* 1 pixels/inch, 2 pixels/cm */
    uint16_t width_unit;
    uint32_t v_res_fixed;
    uint16_t v_res_unit;
    uint16_t height_unit;
} pc_psd_resolution;

pc_psd_resolution pc_psd_resolution_from_dpi(double dpi);
double pc_psd_resolution_h(const pc_psd_resolution *r);
double pc_psd_resolution_v(const pc_psd_resolution *r);
pc_status pc_psd_resolution_parse(const uint8_t *b, size_t len, pc_psd_resolution *out, pc_error *err);
void pc_psd_resolution_write(const pc_psd_resolution *r, uint8_t out[16]);

/* Version info (1057). */
typedef struct pc_psd_version_info {
    uint32_t version;
    bool has_real_merged_data;
    pc_psd_ustr writer;
    pc_psd_ustr reader;
    uint32_t file_version;
} pc_psd_version_info;

pc_status pc_psd_version_info_parse(const uint8_t *b, size_t len, pc_psd_version_info *out, pc_error *err);
pc_status pc_psd_version_info_to_bytes(const pc_psd_version_info *v, pc_psd_bytes *out, pc_error *err);
void pc_psd_version_info_fini(pc_psd_version_info *v);
/* The 1057 resource the builder writes ("Adobe Photoshop" / "Adobe Photoshop CS6"). */
pc_status pc_psd_version_info_resource(bool has_real_merged_data, pc_psd_resource *out, pc_error *err);

/* Typed views of well-known resources (ResourceData). Each fails with the reference's error when
 * the data is malformed; the caller checks the id. */
pc_status pc_psd_res_layer_state(const pc_psd_resource *r, uint16_t *out, pc_error *err);
pc_status pc_psd_res_i32(const pc_psd_resource *r, int32_t *out, pc_error *err); /* angle, altitude */
/* Layer group ids (1026); owned array. "layer group info has odd length" on odd data. */
pc_status pc_psd_res_group_ids(const pc_psd_resource *r, uint16_t **ids, size_t *n, pc_error *err);
/* Whether `id` has a typed parser (ResourceData::parsed returns Some). */
bool pc_psd_res_is_known(uint16_t id);
/* XMP packet (1060) when it is valid UTF-8, borrowed (not NUL-terminated), else NULL. */
const char *pc_psd_res_xmp(const pc_psd_resource *r, size_t *len);

/* The whole section (u32 length + blocks), as stored in the file. */
struct psd_rd;
struct psd_w;
pc_status psd_read_resources(struct psd_rd *r, pc_psd_resource **out, size_t *n, pc_error *err);
void psd_write_resources(struct psd_w *w, const pc_psd_resource *res, size_t n);
void pc_psd_resources_free(pc_psd_resource *res, size_t n);

#endif
