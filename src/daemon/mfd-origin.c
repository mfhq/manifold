/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfd-origin.h"

#include <glib/gi18n-lib.h>

#include "mf-remote-private.h"
#include "mf-store-private.h"

/**
 * mfd_read_cached_file:
 *
 * Returns: (transfer full): the contents of a cached metadata file of a remote, or %NULL
 */
static GBytes *
mfd_read_cached_file(const char *path, const char *remote_id, GError **error)
{
    g_autoptr(GError) local_error = NULL;
    g_autoptr(GMappedFile) file = g_mapped_file_new(path, FALSE, &local_error);

    if (file != NULL)
        return g_mapped_file_get_bytes(file);
    if (g_error_matches(local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_NOT_FOUND,
                    _("The remote %s was not refreshed yet, run 'manifold refresh'"),
                    remote_id);
    else
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    _("Unable to read cached repository metadata of %s: %s"),
                    remote_id,
                    local_error->message);
    return NULL;
}

/**
 * mfd_origin_read_root_index:
 * @root: the root of the store
 * @keys: the trusted keys of the remote
 * @ret_data: (out) (optional) (transfer full): the root index, as the repository served it
 *
 * Read the cached root index of a remote, and verify its signature again.
 *
 * Returns: (transfer full): the root index, or %NULL with %MF_ERROR_NOT_FOUND
 *   if the remote was not refreshed yet
 */
MfRepoRootIndex *
mfd_origin_read_root_index(const char *root,
                           MfRemote *remote,
                           MfTrustedKeys *keys,
                           GBytes **ret_data,
                           GError **error)
{
    const char *remote_id = mf_remote_get_id(remote);
    g_autofree char *remote_dir = g_build_filename(root, MF_STORE_REMOTES_SUBDIR, remote_id, NULL);
    g_autofree char *data_path = g_build_filename(remote_dir, MF_REPO_ROOT_INDEX_NAME, NULL);
    g_autofree char *signature_path = g_build_filename(remote_dir, MF_REPO_ROOT_SIGNATURE_NAME, NULL);
    g_autoptr(GBytes) data = NULL;
    g_autoptr(GBytes) signature = NULL;
    MfRepoRootIndex *index;

    data = mfd_read_cached_file(data_path, remote_id, error);
    if (data == NULL)
        return NULL;
    signature = mfd_read_cached_file(signature_path, remote_id, error);
    if (signature == NULL)
        return NULL;
    index = mf_repo_root_index_new_from_signed(data, signature, keys, remote_id, error);
    if (index == NULL)
        return NULL;

    if (ret_data != NULL)
        *ret_data = g_steal_pointer(&data);
    return index;
}

/**
 * mfd_origin_check_expiry:
 * @index: a root index
 *
 * Nothing is installed from a repository whose root index expired.
 *
 * Returns: %TRUE if @index has not expired yet
 */
gboolean
mfd_origin_check_expiry(MfRepoRootIndex *index, GError **error)
{
    g_autoptr(GDateTime) now = g_date_time_new_now_utc();
    g_autofree char *valid_until = NULL;

    if (!mf_repo_root_index_is_expired(index, now))
        return TRUE;

    valid_until = g_date_time_format_iso8601(index->valid_until);
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_EXPIRED,
                _("The root index of %s expired at %s"),
                index->id,
                valid_until);
    return FALSE;
}
