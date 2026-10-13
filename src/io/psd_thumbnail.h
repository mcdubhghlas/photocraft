/* The PSD/PSB thumbnail image resource (1036), written when Preferences > File Handling > Image
 * Previews saves them (port of crates/io/src/psd_thumbnail.rs). Not part of the io API. */
#ifndef PC_IO_PSD_THUMBNAIL_H
#define PC_IO_PSD_THUMBNAIL_H

#include "doc/doc.h"
#include "io/core.h"
#include "psd/resources.h"

/* Longest side of the thumbnail, as Photoshop writes it. */
#define PC_IO_THUMBNAIL_MAX_SIDE 160u
/* Size of the header in front of the JFIF data. */
#define PC_IO_THUMBNAIL_HEADER_LEN 28u

/* The 1036 resource for doc: its composite reduced to at most MAX_SIDE, converted to sRGB through
 * the document's profile, matted over white and JPEG-compressed. *out initialised on success. */
pc_status pc_io_psd_thumbnail_resource(const pc_document *doc, pc_psd_resource *out, pc_error *err);

#endif
