/*
 * Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#if !defined(__MANIFOLD_H_INSIDE__) && !defined(MF_COMPILATION)
#error "Only <manifold.h> can be included directly."
#endif

#include <glib.h>

#include "mf-manifest.h"

G_BEGIN_DECLS

gboolean mf_zchunk_write_image(int      image_fd,
                               int      dest_fd,
                               GError **error);
gboolean mf_zchunk_read_header(int      fd,
                               guint64 *header_size,
                               char   **header_sha256,
                               GError **error);
gboolean mf_zchunk_check_image(int         fd,
                               MfManifest *manifest,
                               GError    **error);

G_END_DECLS
