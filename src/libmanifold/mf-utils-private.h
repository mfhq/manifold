/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <stdint.h>
#include <glib.h>
#include <systemd/sd-json.h>

#include "mf-macros-private.h"

MF_BEGIN_PRIVATE_DECLS

/* the prefix of the name of a directory or file that is not complete yet */
#define MF_TMP_PREFIX ".tmp-"

/* the suffix of the cached icons in AppStream catalogs (JXL images) */
#define MF_ICON_SUFFIX ".jxl"

/* the length of a hex encoded 256 bit hash */
#define MF_HEX_HASH_LENGTH 64

/* computes the file entry of data that is passed in pieces */
typedef struct _MfHasher MfHasher;

/**
 * MfFileEntry:
 *
 * The size and hashes of a file. An entry is either read from an index,
 * or computed from the contents of a file.
 */
typedef struct {
    uint64_t size;
    char     sha256[MF_HEX_HASH_LENGTH + 1];
    char     blake3[MF_HEX_HASH_LENGTH + 1];
} MfFileEntry;

MF_INTERNAL_VISIBLE
void mf_file_entry_set_for_bytes(MfFileEntry *entry,
                                 GBytes      *data);
MF_INTERNAL_VISIBLE
gboolean  mf_file_entry_equal(const MfFileEntry *a,
                              const MfFileEntry *b);

MfHasher *mf_hasher_new(void);
void      mf_hasher_free(MfHasher *hasher);
void      mf_hasher_update(MfHasher   *hasher,
                           const void *data,
                           size_t      size);
void      mf_hasher_finish(MfHasher    *hasher,
                           MfFileEntry *ret);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfHasher, mf_hasher_free)

gboolean mf_write_all(int         fd,
                      const char *buffer,
                      size_t      size,
                      GError    **error);

gboolean mf_file_name_is_valid(const char *name);
gboolean mf_unit_file_name_is_valid(const char *name);
MF_INTERNAL_VISIBLE
gboolean mf_relative_path_is_valid(const char *path);
gboolean mf_bundle_file_name_is_valid(const char *name);
gboolean mf_program_path_is_valid(const char *path);
gboolean mf_reverse_dns_name_is_valid(const char *name);
MF_INTERNAL_VISIBLE
gboolean mf_path_is_below(const char *path,
                          const char *dir);
gboolean mf_mime_type_is_valid(const char *type);
gboolean mf_mime_glob_is_valid(const char *pattern);
gboolean mf_mime_weight_is_valid(const char *weight);
gboolean mf_mime_xml_name_is_valid(const char *name);
gboolean mf_text_is_valid(GBytes *text);
MF_INTERNAL_VISIBLE
gboolean mf_display_text_is_valid(const char *text);
MF_INTERNAL_VISIBLE
gboolean mf_bytes_are_jxl(GBytes *bytes);
gboolean mf_file_is_masked(const char *path);

MF_INTERNAL_VISIBLE
char *mf_build_full_version(const char *version,
                            uint64_t    revision);
MF_INTERNAL_VISIBLE
gboolean mf_split_full_version(const char *name,
                               char      **ret_version,
                               uint64_t   *ret_revision);

gboolean mf_str_consists_of(const char *str,
                            const char *chars);
MF_INTERNAL_VISIBLE
char *mf_ptr_array_join_strings(GPtrArray  *strings,
                                const char *separator);

MF_END_PRIVATE_DECLS
