/*
 * Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <stdint.h>
#include <glib.h>

#include "mf-bundle.h"
#include "mf-macros-private.h"
#include "mf-manifest.h"

MF_BEGIN_PRIVATE_DECLS

MF_INTERNAL_VISIBLE
GArray *mf_zchunk_find_image_chunks(const uint8_t *data,
                                    size_t         size);

/**
 * MfZchunkRange:
 *
 * A range of bytes of a zchunk file that has to be downloaded.
 */
typedef struct {
    uint64_t offset;
    uint64_t length;
} MfZchunkRange;

typedef struct _MfZchunkAssembly MfZchunkAssembly;

MF_INTERNAL_VISIBLE
MfZchunkAssembly *mf_zchunk_assembly_new(int         partial_fd,
                                         uint64_t    header_size,
                                         const char *header_sha256,
                                         GError    **error);
MF_INTERNAL_VISIBLE
void mf_zchunk_assembly_free(MfZchunkAssembly *assembly);
MF_INTERNAL_VISIBLE
gboolean mf_zchunk_assembly_add_source(MfZchunkAssembly *assembly,
                                       int               image_fd,
                                       GError          **error);
MF_INTERNAL_VISIBLE
GArray *mf_zchunk_assembly_get_missing(MfZchunkAssembly *assembly,
                                       uint64_t          gap_max);
MF_INTERNAL_VISIBLE
uint64_t mf_zchunk_assembly_get_reused_size(MfZchunkAssembly *assembly);
MF_INTERNAL_VISIBLE
gboolean mf_zchunk_assembly_write_image(MfZchunkAssembly  *assembly,
                                        int                dest_fd,
                                        MfManifest        *manifest,
                                        MfCopyProgressFunc progress_func,
                                        gpointer           user_data,
                                        GError           **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfZchunkAssembly, mf_zchunk_assembly_free)

MF_END_PRIVATE_DECLS
