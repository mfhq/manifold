/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <stdint.h>
#include <glib.h>

#include <manifold.h>

G_BEGIN_DECLS

typedef struct _MfdInstallItem MfdInstallItem;

/**
 * MfdItemWriteFunc:
 * @item: the item whose image to write
 * @fd: the file to write the image to
 * @progress_func: (nullable): called with the number of bytes written so far
 * @progress_data: passed to @progress_func
 *
 * Write the image of an item to @fd, and verify it against its manifest.
 *
 * Returns: %TRUE if the image was written and matches its manifest
 */
typedef gboolean (*MfdItemWriteFunc)(MfdInstallItem    *item,
                                     int                fd,
                                     MfCopyProgressFunc progress_func,
                                     gpointer           progress_data,
                                     GError           **error);

/**
 * MfdInstallItem:
 *
 * A component to install, and the data that is needed to install it.
 */
struct _MfdInstallItem {
    MfManifest      *manifest;
    char            *name;          /* the name of the item in log and error messages */
    char            *origin;        /* the origin of the component, see mf_component_get_origin() */
    GBytes          *catalog;       /* the catalog data of a bundle, or NULL for a component of a remote */
    GHashTable      *icons;         /* (element-type utf8 GBytes): the icons of a bundle, or NULL */
    gboolean         is_dependency; /* the component is only installed because another one needs it */

    MfdItemWriteFunc write_image;
    gpointer         source; /* the data that write_image reads the image from */
    GDestroyNotify   source_free;
};

/**
 * MfdInstallSet:
 *
 * The set of components that one operation installs.
 */
typedef struct {
    GPtrArray *items;    /* (element-type MfdInstallItem) */
    GPtrArray *remotes;  /* (element-type MfRemote): the remotes that manifests were downloaded from */
    GPtrArray *warnings; /* (element-type utf8): warnings about components that were left out */
} MfdInstallSet;

MfdInstallSet  *mfd_install_set_new(void);
MfdInstallSet  *mfd_install_set_new_for_bundle(MfBundle *bundle);
void            mfd_install_set_free(MfdInstallSet *set);

MfdInstallItem *mfd_install_set_add(MfdInstallSet *set,
                                    MfManifest    *manifest,
                                    const char    *name,
                                    const char    *origin);
void            mfd_install_set_add_remote(MfdInstallSet *set,
                                           MfRemote      *remote);

MfdInstallItem *mfd_install_set_find_identity(MfdInstallSet *set,
                                              const char    *identity);
MfdInstallItem *mfd_install_set_find_version(MfdInstallSet *set,
                                             const char    *component_id,
                                             const char    *version,
                                             uint64_t       revision);
gboolean        mfd_install_set_is_privileged(MfdInstallSet *set);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfdInstallSet, mfd_install_set_free)

G_END_DECLS
