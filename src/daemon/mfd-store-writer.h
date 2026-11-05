/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib-object.h>

#include <manifold.h>

#include "mf-store-private.h"

G_BEGIN_DECLS

/**
 * MfdImageWriteFunc:
 * @fd: the file to write the image to
 *
 * Writes an image to @fd, checking it against its manifest on the way.
 *
 * Returns: %TRUE if the image was written and is what its manifest records
 */
typedef gboolean (*MfdImageWriteFunc)(int      fd,
                                      gpointer user_data,
                                      GError **error);

#define MFD_TYPE_STORE_WRITER (mfd_store_writer_get_type())
G_DECLARE_FINAL_TYPE(MfdStoreWriter, mfd_store_writer, MFD, STORE_WRITER, GObject)

MfdStoreWriter *mfd_store_writer_new(MfStore *store);

gboolean        mfd_store_writer_ensure_root(MfdStoreWriter *writer,
                                             GError        **error);

gboolean        mfd_store_writer_add_image(MfdStoreWriter   *writer,
                                           MfManifest       *manifest,
                                           MfdImageWriteFunc write_func,
                                           gpointer          user_data,
                                           GError          **error);
gboolean        mfd_store_writer_remove_image(MfdStoreWriter *writer,
                                              const char     *verity_hash,
                                              GError        **error);

gboolean        mfd_store_writer_install_component(MfdStoreWriter   *writer,
                                                   MfManifest       *manifest,
                                                   const MfBaseLink *base,
                                                   GBytes           *catalog,
                                                   GHashTable       *icons,
                                                   GHashTable       *exports,
                                                   const char       *origin,
                                                   MfInstallMode     mode,
                                                   GError          **error);
gboolean        mfd_store_writer_write_exports(MfdStoreWriter *writer,
                                               MfComponent    *component,
                                               GHashTable     *exports,
                                               GError        **error);
gboolean        mfd_store_writer_remove_component(MfdStoreWriter *writer,
                                                  MfComponent    *component,
                                                  GError        **error);
gboolean        mfd_store_writer_remove_cruft(MfdStoreWriter *writer,
                                              GError        **error);
gboolean        mfd_store_writer_set_mode(MfdStoreWriter *writer,
                                          MfComponent    *component,
                                          MfInstallMode   mode,
                                          GError        **error);

G_END_DECLS
