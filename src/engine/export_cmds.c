/* Export As prepares an independent document; exporting never dirties or resizes the source. */
#include "engine/engine_internal.h"
#include "engine/preview.h"
#include "io/io.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static pc_status execute(pc_engine_session *s, const char *id, const char *json, pc_error *err)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status status = pc_engine_session_execute_json(s, id, json, &out, err);
    pc_json_value_fini(&out);
    return status;
}
static pc_status run(pc_engine_session *s, yyjson_val *params, pc_json_value *out, pc_error *err)
{
    const char *format = pc_json_as_str(pc_json_get(params, "format"), NULL),
               *path = pc_json_as_str(pc_json_get(params, "path"), NULL),
               *metadata = pc_json_as_str(pc_json_get(params, "metadata"), NULL);
    if (!format && !pc_json_get(params, "format"))
        format = "png";
    if (!metadata && !pc_json_get(params, "metadata"))
        metadata = "none";
    if (!format || !metadata)
        return pc_fail(err, PC_ERR_INVALID, "format and metadata must be text");
    const char *formats[] = {"png", "jpg", "webp", "tif", "tga", "ico"};
    bool supported = false;
    for (size_t i = 0; i < sizeof formats / sizeof *formats; i++)
        supported |= !strcmp(format, formats[i]);
    if (!supported || (strcmp(metadata, "none") && strcmp(metadata, "all")))
        return pc_engine_err_bad_params(err, "file.export.exportAs", "invalid format or metadata");
    const char *tga = pc_json_as_str(pc_json_get(params, "tgaBits"), NULL);
    if (pc_json_get(params, "tgaBits") &&
        (!tga || (strcmp(tga, "auto") && strcmp(tga, "24") && strcmp(tga, "32"))))
        return pc_fail(err, PC_ERR_INVALID, "TGA bits must be auto, 24 or 32");
    double scale = 100, quality = 85;
    yyjson_val *v = pc_json_get(params, "scale");
    if (v) {
        if (!yyjson_is_num(v))
            return pc_fail(err, PC_ERR_INVALID, "scale must be a number");
        scale = yyjson_get_num(v);
    }
    v = pc_json_get(params, "quality");
    if (v) {
        if (!yyjson_is_num(v))
            return pc_fail(err, PC_ERR_INVALID, "quality must be a number");
        quality = yyjson_get_num(v);
    }
    bool transparency = true, lossless = false, estimate = !path;
    const char *bools[] = {"transparency", "lossless", "estimate"};
    bool *values[] = {&transparency, &lossless, &estimate};
    for (size_t i = 0; i < 3; i++) {
        v = pc_json_get(params, bools[i]);
        if (v && !pc_json_as_bool(v, values[i]))
            return pc_fail(err, PC_ERR_INVALID, "%s must be boolean", bools[i]);
    }
    if (!isfinite(scale) || scale < 1 || scale > 1000 || !isfinite(quality) || quality < 1 || quality > 100 ||
        (pc_json_get(params, "path") && (!path || !*path)) || (!estimate && !path))
        return pc_fail(err, PC_ERR_INVALID, "invalid export scale, quality or destination");
    const pc_document *source = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    double width = fmax(1, round((double)source->size.width * scale / 100)),
           height = fmax(1, round((double)source->size.height * scale / 100));
    if (width > 300000 || height > 300000 || width * height > 600000000)
        return pc_fail(err, PC_ERR_LIMIT, "export dimensions exceed the image budget");
    if (!strcmp(format, "ico") && (width > 256 || height > 256))
        return pc_fail(err, PC_ERR_LIMIT, "ICO export is limited to 256 by 256 pixels");
    if (path && !estimate) {
        const char *dot = strrchr(path, '.');
        bool matches = dot && (!strcasecmp(dot + 1, format) ||
                               (!strcmp(format, "jpg") && !strcasecmp(dot + 1, "jpeg")) ||
                               (!strcmp(format, "tif") && !strcasecmp(dot + 1, "tiff")));
        if (!matches)
            return pc_fail(err, PC_ERR_INVALID, "destination extension must match export format");
    }
    double proxy = estimate ? fmin(1, 512 / fmax(width, height)) : 1;
    unsigned target_width = (unsigned)fmax(1, round(width * proxy)),
             target_height = (unsigned)fmax(1, round(height * proxy));
    pc_engine_session *prepared = NULL;
    pc_status status = pc_engine_preview_clone(s, &prepared, err);
    if (status != PC_OK)
        return status;
    /* Synchronous preparation preserves authorization and its owner-thread contract. */
    prepared->authorize = s->authorize;
    char resize[160];
    (void)snprintf(resize, sizeof resize, "{\"width\":%u,\"height\":%u,\"resample\":\"%s\"}", target_width,
                   target_height, estimate ? "bilinear" : "bicubic");
    if (target_width != source->size.width || target_height != source->size.height)
        status = execute(prepared, "image.imageSize", resize, err);
    if (status == PC_OK && (!transparency || !strcmp(format, "jpg")))
        status = execute(prepared, "layer.flattenImage", "{}", err);
    pc_io_export_opts opts = pc_io_export_opts_default();
    opts.encode.jpeg_quality = (uint8_t)quality;
    opts.encode.webp_quality = (uint8_t)quality;
    opts.encode.webp_lossless = lossless;
    opts.encode.tga_bits = tga && !strcmp(tga, "24") ? 24 : tga && !strcmp(tga, "32") ? 32 : 0;
    opts.xmp = !strcmp(metadata, "all") ? PC_IO_XMP_ALL : PC_IO_XMP_NONE;
    opts.encode.embed_metadata = !strcmp(metadata, "all");
    pc_io_export_result encoded = {0};
    if (status == PC_OK)
        status = pc_io_export(pc_doc_arc_get(pc_engine_session_active(prepared)->doc), format, &opts,
                              &encoded, err);
    yyjson_mut_doc *d = status == PC_OK ? yyjson_mut_doc_new(NULL) : NULL;
    yyjson_mut_val *root = d ? yyjson_mut_obj(d) : NULL;
    pc_json_value reply = PC_JSON_VALUE_INIT;
    if (status == PC_OK) {
        double estimated = (double)encoded.len * width * height / ((double)target_width * target_height);
        bool ok = root && yyjson_mut_obj_add_uint(d, root, "width", (uint64_t)width) &&
                  yyjson_mut_obj_add_uint(d, root, "height", (uint64_t)height) &&
                  yyjson_mut_obj_add_uint(d, root, "bytes", (uint64_t)estimated) &&
                  yyjson_mut_obj_add_bool(d, root, "estimate", estimate) &&
                  yyjson_mut_obj_add_strcpy(d, root, "format", format);
        if (ok && path && !estimate)
            ok = yyjson_mut_obj_add_strcpy(d, root, "path", path);
        yyjson_mut_val *warnings = yyjson_mut_arr(d);
        ok = ok && warnings;
        for (size_t i = 0; ok && i < encoded.warnings.n; i++)
            ok = yyjson_mut_arr_add_strcpy(d, warnings, encoded.warnings.v[i] ? encoded.warnings.v[i] : "");
        ok = ok && yyjson_mut_obj_add_val(d, root, "warnings", warnings);
        if (!ok)
            status = pc_fail(err, PC_ERR_NOMEM, "export result");
        else
            status = pc_json_value_from_mut(root, &reply, err);
    }
    if (status == PC_OK && !estimate)
        status = pc_engine_write_file(path, encoded.bytes, encoded.len, err);
    if (status == PC_OK) {
        pc_json_value_fini(out);
        *out = reply;
        reply = (pc_json_value)PC_JSON_VALUE_INIT;
    }
    pc_json_value_fini(&reply);
    yyjson_mut_doc_free(d);
    pc_io_export_result_fini(&encoded);
    pc_engine_session_free(prepared);
    return status;
}
const pc_engine_command *pc_engine_export_specs(size_t *n)
{
    static const char *const menu[] = {"File", "Export"};
    static const pc_engine_command commands[] = {
        {"file.export.exportAs", "Export As…", menu, 2, "Cmd+Alt+Shift+W",
         "{\"format\":\"png|jpg|webp|tif|tga|ico\"=\"png\",\"quality\":1..100=85,\"lossless\":bool=false,"
         "\"transparency\":bool=true,\"scale\":1..1000=100,\"metadata\":\"none|all\"=\"none\",\"path\":file?,"
         "\"tgaBits\":\"auto|24|32\"=\"auto\",\"estimate\":bool?} -> {width,height,bytes,estimate}",
         pc_engine_has_doc, run, true}};
    *n = sizeof commands / sizeof *commands;
    return commands;
}
