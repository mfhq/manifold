/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>
#include <sys/types.h>
#include <manifold.h>

#include "mf-utils-private.h"

G_BEGIN_DECLS

void     mfd_emit_warning(MfResult   *result,
                          const char *format,
                          ...) G_GNUC_PRINTF(2, 3);

gboolean mfd_is_directory(const char *path);
gboolean mfd_unlink_if_exists(const char *path,
                              GError    **error);
gboolean mfd_is_symlink(const char *path);
gboolean mfd_is_hidden(const char *name);

gboolean mfd_remove_if_exists(const char *path,
                              GError    **error);

gboolean mfd_make_dir(const char *path,
                      GError    **error);
gboolean mfd_make_symlink(const char *target,
                          const char *path,
                          GError    **error);
gboolean mfd_replace_link(const char *target,
                          const char *dir,
                          const char *name,
                          GError    **error);
gboolean mfd_replace_link_at(int         dfd,
                             const char *name,
                             const char *target,
                             GError    **error);

int      mfd_make_dir_below(int         dfd,
                            const char *subdir,
                            GError    **error);
gboolean mfd_write_file_at(int         dfd,
                           const char *name,
                           const void *content,
                           gsize       length,
                           mode_t      mode,
                           GError    **error);
gboolean mfd_write_contents_safe(const char *dir,
                                 const char *name,
                                 const char *content,
                                 gssize      length,
                                 GError    **error);
gboolean mfd_write_word_safe(const char *dir,
                             const char *name,
                             const char *word,
                             GError    **error);

G_END_DECLS
