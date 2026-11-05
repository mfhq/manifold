/*
 * Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

#include "mf-bundle.h"
#include "mf-macros-private.h"
#include "mf-manifest.h"

MF_BEGIN_PRIVATE_DECLS

MF_INTERNAL_VISIBLE
GArray *mf_zchunk_find_image_chunks(const guint8 *data,
                                    gsize         size);

/**
 * MfZchunkRange:
 *
 * A range of bytes of a zchunk file that has to be downloaded.
 */
typedef struct {
    guint64 offset;
    guint64 length;
} MfZchunkRange;

typedef struct _MfZchunkAssembly MfZchunkAssembly;

MF_INTERNAL_VISIBLE
MfZchunkAssembly *mf_zchunk_assembly_new(int         partial_fd,
                                         guint64     header_size,
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
                                       guint64           gap_max);
MF_INTERNAL_VISIBLE
guint64 mf_zchunk_assembly_get_reused_size(MfZchunkAssembly *assembly);
MF_INTERNAL_VISIBLE
gboolean mf_zchunk_assembly_write_image(MfZchunkAssembly  *assembly,
                                        int                dest_fd,
                                        MfManifest        *manifest,
                                        MfCopyProgressFunc progress_func,
                                        gpointer           user_data,
                                        GError           **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfZchunkAssembly, mf_zchunk_assembly_free)

MF_END_PRIVATE_DECLS
