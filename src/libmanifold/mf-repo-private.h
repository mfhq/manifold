/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <stdint.h>
#include <glib.h>

#include "mf-error.h"
#include "mf-manifest.h"
#include "mf-trusted-keys.h"
#include "mf-macros-private.h"
#include "mf-utils-private.h"

MF_BEGIN_PRIVATE_DECLS

/* the names of the files of a repository */
#define MF_REPO_ROOT_INDEX_NAME      "repo-index.json.zst"
#define MF_REPO_ROOT_SIGNATURE_NAME  "repo-index.sig"
#define MF_REPO_COMPONENT_INDEX_NAME "index.json.zst"
#define MF_REPO_CATALOG_NAME         "components.xml.zst"
#define MF_REPO_ICON_INDEX_NAME      "icons.json.zst"

/* the suffix of an image file. The file is named like its manifest, or after its verity hash. */
#define MF_REPO_IMAGE_SUFFIX ".raw.zck"

/**
 * MfRepoArchitecture:
 *
 * The files of a channel for one architecture, as listed in the root index.
 */
typedef struct {
    char       *name;
    MfFileEntry component_index;
    MfFileEntry catalog;
    MfFileEntry icon_index;
} MfRepoArchitecture;

/**
 * MfRepoChannel:
 *
 * A channel of a repository, as described by the root index.
 */
typedef struct {
    char      *name;
    char      *image_url_prefix; /* the URL prefix where images are looked up first, or NULL */
    GPtrArray *architectures;    /* (element-type MfRepoArchitecture) */
} MfRepoChannel;

/**
 * MfRepoRootIndex:
 *
 * The root index of a repository.
 */
typedef struct {
    char      *id;
    char      *summary;
    char      *publisher_name;  /* the name of the publisher, or NULL */
    char      *publisher_email; /* the email address of the publisher, or NULL */
    uint64_t   serial;
    GDateTime *date;
    GDateTime *valid_until;
    GPtrArray *channels;        /* (element-type MfRepoChannel) */
    char      *key_fingerprint; /* the fingerprint of the key that signed the index, or NULL */
} MfRepoRootIndex;

MF_INTERNAL_VISIBLE
MfRepoRootIndex *mf_repo_root_index_new_from_signed(GBytes        *data,
                                                    GBytes        *signature,
                                                    MfTrustedKeys *keys,
                                                    const char    *remote_id,
                                                    GError       **error);
MfRepoRootIndex *mf_repo_root_index_new_from_stored(GBytes     *data,
                                                    const char *remote_id,
                                                    GError    **error);
MF_INTERNAL_VISIBLE
char *mf_repo_root_index_read_id(GBytes  *data,
                                 GError **error);
MF_INTERNAL_VISIBLE
void mf_repo_root_index_free(MfRepoRootIndex *index);
MF_INTERNAL_VISIBLE
MfRepoChannel *mf_repo_root_index_find_channel(MfRepoRootIndex *index,
                                               const char      *name);
MF_INTERNAL_VISIBLE
MfRepoArchitecture *mf_repo_channel_find_architecture(MfRepoChannel *channel,
                                                      const char    *architecture);
MF_INTERNAL_VISIBLE
MfRepoArchitecture *mf_repo_root_index_find_architecture(MfRepoRootIndex *index,
                                                         const char      *channel_name,
                                                         const char      *architecture);
MF_INTERNAL_VISIBLE
GStrv mf_repo_root_index_list_channels(MfRepoRootIndex *index,
                                       const char      *architecture);
MF_INTERNAL_VISIBLE
gboolean mf_repo_root_index_is_expired(MfRepoRootIndex *index,
                                       GDateTime       *now);
MF_INTERNAL_VISIBLE
gboolean mf_repo_root_index_check_expiry(MfRepoRootIndex *index,
                                         GError         **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfRepoRootIndex, mf_repo_root_index_free)

/**
 * MfRepoVersion:
 *
 * A version of a component, as listed in the component index of a channel.
 */
typedef struct {
    char       *bundle; /* the bundle ID, <architecture>/<component>/<version> */
    char       *version;
    uint64_t    revision;
    char       *manifest_location; /* the path of the manifest, relative to the repository root */
    MfFileEntry manifest;
    uint64_t    zck_header_size;
    char        zck_header_sha256[MF_HEX_HASH_LENGTH + 1];
} MfRepoVersion;

/**
 * MfRepoComponentIndex:
 *
 * The components of a channel for one architecture.
 */
typedef struct {
    GHashTable *components; /* component ID -> GPtrArray of MfRepoVersion, newest first */
} MfRepoComponentIndex;

MF_INTERNAL_VISIBLE
MfRepoComponentIndex *mf_repo_component_index_new(GBytes            *data,
                                                  const MfFileEntry *entry,
                                                  const char        *architecture,
                                                  GError           **error);
MF_INTERNAL_VISIBLE
void mf_repo_component_index_free(MfRepoComponentIndex *index);
MF_INTERNAL_VISIBLE
GPtrArray *mf_repo_component_index_get_versions(MfRepoComponentIndex *index,
                                                const char           *component_id);

MF_INTERNAL_VISIBLE
MfManifest *mf_repo_version_read_manifest(MfRepoVersion *version,
                                          GBytes        *data,
                                          const char    *component_id,
                                          const char    *architecture,
                                          GError       **error);
MF_INTERNAL_VISIBLE
char *mf_repo_version_build_image_location(MfRepoVersion *version);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfRepoComponentIndex, mf_repo_component_index_free)

/**
 * MfRepoIcon:
 *
 * An icon of the catalog of a channel, as listed in the icon index.
 */
typedef struct {
    char       *path;     /* <size>/<name>, the path of the icon in the icon directory of the origin */
    char       *location; /* the path of the icon file, relative to the repository root */
    MfFileEntry entry;
} MfRepoIcon;

MF_INTERNAL_VISIBLE
GPtrArray *mf_repo_icon_index_new(GBytes            *data,
                                  const MfFileEntry *entry,
                                  GError           **error);

MF_INTERNAL_VISIBLE
gboolean mf_repo_check_catalog(GBytes               *data,
                               const MfFileEntry    *entry,
                               const char           *origin,
                               MfRepoComponentIndex *index,
                               GError              **error);

MF_INTERNAL_VISIBLE
GBytes *mf_zstd_decompress_bytes(GBytes     *data,
                                 size_t      size_max,
                                 const char *name,
                                 MfError     code,
                                 GError    **error);

MF_END_PRIVATE_DECLS
