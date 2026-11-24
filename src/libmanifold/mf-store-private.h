/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "config.h"

#include <glib.h>

#include "mf-store.h"
#include "mf-bundle-private.h"
#include "mf-macros-private.h"

MF_BEGIN_PRIVATE_DECLS

#define MF_STORE_IMAGES_SUBDIR   "images/by-verity"
#define MF_STORE_APPS_SUBDIR     "apps"
#define MF_STORE_PLUGINS_SUBDIR  "plugins"
#define MF_STORE_RUNTIMES_SUBDIR "runtimes"

/* holds one file per component, which lists the files we installed for it on the host */
#define MF_STORE_INTEGRATIONS_SUBDIR "integrations"

#define MF_STORE_APPS_DIR     MF_STORAGE_DIR "/" MF_STORE_APPS_SUBDIR
#define MF_STORE_RUNTIMES_DIR MF_STORAGE_DIR "/" MF_STORE_RUNTIMES_SUBDIR

#define MF_STORE_BASE_LINK      "base"
#define MF_STORE_BASE_SDK_LINK  "base-sdk"
#define MF_STORE_MANIFEST_NAME  "manifest.json"
#define MF_STORE_CURRENT_NAME   "current"
#define MF_STORE_ORIGIN_NAME    "origin"
#define MF_STORE_MODE_NAME      "mode"
#define MF_STORE_ICONS_NAME     "icons"
#define MF_STORE_EXPORTS_NAME   "exports"
#define MF_STORE_CATALOG_SUFFIX ".xml"

/* the layout of the store */
MF_INTERNAL_VISIBLE
const char *const *mf_store_get_component_subdirs(void);
MF_INTERNAL_VISIBLE
const char *mf_store_subdir_for_kind(MfComponentKind kind,
                                     gboolean        sdk);
MF_INTERNAL_VISIBLE
const char *mf_store_image_link_for_subdir(const char *subdir);
MF_INTERNAL_VISIBLE
char *mf_store_build_image_name(const char *verity_hash);
MF_INTERNAL_VISIBLE
char *mf_store_hash_from_image_name(const char *name);
MF_INTERNAL_VISIBLE
void mf_store_invalidate(MfStore *store);
MF_INTERNAL_VISIBLE
gboolean mf_store_has_image(MfStore    *store,
                            const char *verity_hash);
MF_INTERNAL_VISIBLE
GPtrArray *mf_store_list_images(MfStore *store,
                                GError **error);
MF_INTERNAL_VISIBLE
MfComponent *mf_store_find_version(MfStore    *store,
                                   const char *component_id,
                                   const char *version,
                                   uint64_t    revision,
                                   GError    **error);
MF_INTERNAL_VISIBLE
MfComponent *mf_store_find_identity(MfStore    *store,
                                    const char *identity,
                                    GError    **error);

/**
 * MfBaseLink:
 * @subdir: the subdirectory of the store the base is in
 * @component_id: the identifier of the base
 * @version_name: the name of the version directory of the base,
 *   `<version>-<revision>`, or %NULL if the link points to the current version
 *
 * The target of the `base` or `base-sdk` link of a version directory.
 */
typedef struct {
    char *subdir;
    char *component_id;
    char *version_name;
} MfBaseLink;

MF_INTERNAL_VISIBLE
MfBaseLink *mf_base_link_new(const char *subdir,
                             const char *component_id,
                             const char *version_name);
MF_INTERNAL_VISIBLE
void mf_base_link_free(MfBaseLink *link);
MF_INTERNAL_VISIBLE
char *mf_base_link_get_target(const MfBaseLink *link);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfBaseLink, mf_base_link_free)

/* the paths of the cached metadata of the remotes, and of the catalog data of their channels */
MF_INTERNAL_VISIBLE
char *mf_store_remotes_dir_at(const char *root);
MF_INTERNAL_VISIBLE
char *mf_store_remote_dir_at(const char *root,
                             const char *remote_id);
MF_INTERNAL_VISIBLE
char *mf_store_component_index_path_at(const char *root,
                                       const char *remote_id,
                                       const char *channel);
MF_INTERNAL_VISIBLE
char *mf_store_catalog_dir_at(const char *root);
MF_INTERNAL_VISIBLE
char *mf_store_build_catalog_name(const char *origin);
MF_INTERNAL_VISIBLE
char *mf_store_origin_from_catalog_name(const char *name);
MF_INTERNAL_VISIBLE
char *mf_store_catalog_path_at(const char *root,
                               const char *origin);
MF_INTERNAL_VISIBLE
char *mf_store_catalog_icons_root_at(const char *root);
MF_INTERNAL_VISIBLE
char *mf_store_catalog_icons_dir_at(const char *root,
                                    const char *origin);

/* the targets of relative links within the store */
MF_INTERNAL_VISIBLE
char *mf_store_build_image_link_target(const char *verity_hash);
MF_INTERNAL_VISIBLE
char *mf_store_build_catalog_link_target(const char *store_path);
MF_INTERNAL_VISIBLE
char *mf_store_build_catalog_icon_link_target(const char *store_path);

/* direct access to a store by its root directory, without loading the store */
MF_INTERNAL_VISIBLE
MfComponent *mf_store_read_component_at(const char *root,
                                        const char *component_id,
                                        const char *version,
                                        GError    **error);
MF_INTERNAL_VISIBLE
MfComponent *mf_store_read_base_at(const char  *root,
                                   MfComponent *component,
                                   GError     **error);
MF_INTERNAL_VISIBLE
MfBaseLink *mf_store_read_base_link(MfComponent *component,
                                    GError     **error);
MF_INTERNAL_VISIBLE
gboolean mf_store_component_is_revoked_at(const char *root,
                                          const char *component_id);
MF_INTERNAL_VISIBLE
char *mf_store_image_path_at(const char *root,
                             const char *verity_hash);

MF_END_PRIVATE_DECLS
