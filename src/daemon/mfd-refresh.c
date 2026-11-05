/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define _GNU_SOURCE
#include "config.h"
#include "mfd-refresh.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <glib/gi18n-lib.h>
#include <libglnx.h>

#include "mf-remote-private.h"
#include "mf-repo-private.h"
#include "mf-store-private.h"
#include "mf-utils-private.h"
#include "mfd-origin.h"
#include "mfd-utils.h"

/*
 * Refreshing a remote means downloading the metadata of its repository,
 * verifying it, and storing it in the store.
 *
 * The cached metadata of a remote is stored in two places. Its root index and
 * the component and icon indexes of its channels are in remotes/<id>/, so we
 * can verify them again whenever we use them. The catalog and the icons of
 * each channel are in catalog/, in the directory layout that AppStream reads,
 * next to the data of the local origin. All files except the icons are stored
 * exactly as the repository serves them.
 *
 * We download and verify all files before we change anything in the store.
 * Then we move the new data into place: first the icons of each channel,
 * then its catalog file, and the directory of the remote last.
 */

/* the largest size of a root index and of its signature */
constexpr guint64 ROOT_INDEX_DOWNLOAD_MAX = 1024 * 1024;
constexpr guint64 SIGNATURE_DOWNLOAD_MAX = 64 * 1024;

/* a staging directory is created next to the directory it replaces, with this name */
#define STAGING_PREFIX "."
#define STAGING_SUFFIX ".new"

/**
 * mfd_write_file:
 *
 * Write a file into the store atomically.
 */
static gboolean
mfd_write_file(const char *path, GBytes *data, GError **error)
{
    gsize size;
    const guint8 *buffer = g_bytes_get_data(data, &size);

    return glnx_file_replace_contents_with_perms_at(AT_FDCWD,
                                                    path,
                                                    buffer,
                                                    size,
                                                    0644,
                                                    (uid_t) -1,
                                                    (gid_t) -1,
                                                    GLNX_FILE_REPLACE_DATASYNC_NEW,
                                                    NULL,
                                                    error);
}

/**
 * mfd_make_staging_dir:
 *
 * Returns: (transfer full): the path of a new, empty staging directory next
 *   to @live, in which the replacement of @live is built
 */
static char *
mfd_make_staging_dir(const char *live, GError **error)
{
    g_autofree char *parent = g_path_get_dirname(live);
    g_autofree char *name = g_path_get_basename(live);
    g_autofree char *staging_name = g_strconcat(STAGING_PREFIX, name, STAGING_SUFFIX, NULL);
    g_autofree char *staging = g_build_filename(parent, staging_name, NULL);

    /* an interrupted refresh may have left an old staging directory */
    if (!glnx_shutil_rm_rf_at(AT_FDCWD, staging, NULL, error) ||
        !glnx_shutil_mkdir_p_at(AT_FDCWD, staging, 0755, NULL, error))
        return NULL;
    return g_steal_pointer(&staging);
}

/**
 * mfd_swap_into_place:
 *
 * Replace the directory @live with @staging atomically, and delete the old
 * contents of @live.
 */
static gboolean
mfd_swap_into_place(const char *staging, const char *live, GError **error)
{
    if (glnx_renameat2_exchange(AT_FDCWD, staging, AT_FDCWD, live) == 0)
        return glnx_shutil_rm_rf_at(AT_FDCWD, staging, NULL, error);
    if (errno != ENOENT)
        return glnx_throw_errno_prefix(error, "Unable to replace %s", live);
    if (rename(staging, live) < 0)
        return glnx_throw_errno_prefix(error, "Unable to rename %s", staging);
    return TRUE;
}

/**
 * mfd_remove_origin:
 *
 * Remove the catalog file and the icons of a channel.
 */
static gboolean
mfd_remove_origin(const char *root, const char *origin, GError **error)
{
    g_autofree char *file_name = g_strconcat(origin, MF_STORE_CATALOG_FILE_SUFFIX, NULL);
    g_autofree char *catalog = g_build_filename(root, MF_STORE_CATALOG_XML_SUBDIR, file_name, NULL);
    g_autofree char *icons = g_build_filename(root, MF_STORE_CATALOG_ICONS_SUBDIR, origin, NULL);

    g_info("Removing the catalog of %s", origin);
    if (unlink(catalog) < 0 && errno != ENOENT)
        return glnx_throw_errno_prefix(error, "Unable to remove %s", catalog);
    return glnx_shutil_rm_rf_at(AT_FDCWD, icons, NULL, error);
}

/**
 * mfd_list_origins:
 *
 * Returns: (transfer full): the names of all origins of remotes that have a
 *   catalog file or icons in the store
 */
static GHashTable *
mfd_list_origins(const char *root, GError **error)
{
    g_autoptr(GHashTable) origins = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    const char *subdirs[] = {MF_STORE_CATALOG_XML_SUBDIR, MF_STORE_CATALOG_ICONS_SUBDIR};

    for (gsize i = 0; i < G_N_ELEMENTS(subdirs); i++) {
        g_autofree char *dir = g_build_filename(root, subdirs[i], NULL);
        g_autoptr(GError) local_error = NULL;
        g_autoptr(GDir) gdir = g_dir_open(dir, 0, &local_error);
        const char *name;

        if (gdir == NULL) {
            if (g_error_matches(local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
                continue;
            g_propagate_error(error, g_steal_pointer(&local_error));
            return NULL;
        }
        while ((name = g_dir_read_name(gdir)) != NULL) {
            g_autofree char *origin = NULL;

            if (name[0] == '.')
                continue;
            if (i == 0) {
                if (!g_str_has_suffix(name, MF_STORE_CATALOG_FILE_SUFFIX))
                    continue;
                origin = g_strndup(name, strlen(name) - strlen(MF_STORE_CATALOG_FILE_SUFFIX));
            } else {
                origin = g_strdup(name);
            }
            if (mf_origin_split_name(origin, NULL, NULL))
                g_hash_table_add(origins, g_steal_pointer(&origin));
        }
    }

    return g_steal_pointer(&origins);
}

/* a channel whose files were downloaded and verified, and can be moved into place */
typedef struct {
    char *origin;
    GBytes *catalog;     /* the catalog file, as the repository serves it */
    char *icons_staging; /* the staging directory with the icons of the channel */
    char *icons_live;
} MfdPreparedChannel;

static void
mfd_prepared_channel_free(MfdPreparedChannel *prepared)
{
    if (prepared->icons_staging != NULL)
        glnx_shutil_rm_rf_at(AT_FDCWD, prepared->icons_staging, NULL, NULL);
    g_free(prepared->origin);
    g_clear_pointer(&prepared->catalog, g_bytes_unref);
    g_free(prepared->icons_staging);
    g_free(prepared->icons_live);
    g_free(prepared);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfdPreparedChannel, mfd_prepared_channel_free)

/**
 * mfd_icon_is_unchanged:
 *
 * Returns: %TRUE if the icon file at @path matches its entry in the icon index
 */
static gboolean
mfd_icon_is_unchanged(const char *path, const MfRepoIcon *icon)
{
    g_autofree char *contents = NULL;
    g_autoptr(GBytes) data = NULL;
    MfFileEntry actual;
    gsize size;

    if (!g_file_get_contents(path, &contents, &size, NULL))
        return FALSE;
    data = g_bytes_new_take(g_steal_pointer(&contents), size);
    mf_file_entry_set_for_bytes(&actual, data);
    return mf_file_entry_equal(&actual, &icon->entry);
}

/* an icon of a channel that we have to download */
typedef struct {
    MfRepoIcon *icon;           /* owned by the icon index */
    MfdMemoryDownload download; /* the file of the icon */
} MfdIconDownload;

static void
mfd_icon_download_free(MfdIconDownload *icon_download)
{
    mfd_memory_download_clear(&icon_download->download);
    g_free(icon_download);
}

/**
 * mfd_prepare_icons:
 *
 * Fill the staging directory of a channel with its icons. Icons that have not
 * changed are linked from the live directory, so we only download the new
 * and the changed ones.
 */
static gboolean
mfd_prepare_icons(MfdFetcher *fetcher,
                  MfRemote *remote,
                  GPtrArray *icons,
                  MfdPreparedChannel *prepared,
                  GError **error)
{
    g_autoptr(GPtrArray) missing = g_ptr_array_new_with_free_func((GDestroyNotify) mfd_icon_download_free);
    g_autoptr(GPtrArray) downloads = g_ptr_array_new();

    for (guint i = 0; i < icons->len; i++) {
        MfRepoIcon *icon = g_ptr_array_index(icons, i);
        g_autofree char *live = g_build_filename(prepared->icons_live, icon->path, NULL);
        g_autofree char *staging = g_build_filename(prepared->icons_staging, icon->path, NULL);
        g_autofree char *size_dir = g_path_get_dirname(staging);
        MfdIconDownload *icon_download;

        if (!glnx_shutil_mkdir_p_at(AT_FDCWD, size_dir, 0755, NULL, error))
            return FALSE;
        if (mfd_icon_is_unchanged(live, icon) && link(live, staging) == 0)
            continue;

        icon_download = g_new0(MfdIconDownload, 1);
        icon_download->icon = icon;
        icon_download->download.url = mf_remote_build_url(remote, icon->location);
        icon_download->download.max_size = icon->entry.size;
        g_ptr_array_add(missing, icon_download);
        g_ptr_array_add(downloads, &icon_download->download);
    }
    if (missing->len == 0)
        return TRUE;

    g_info("Downloading %u icons of %s", missing->len, prepared->origin);
    if (!mfd_fetcher_download_to_memory(fetcher,
                                        (MfdMemoryDownload **) downloads->pdata,
                                        downloads->len,
                                        error))
        return FALSE;
    for (guint i = 0; i < missing->len; i++) {
        MfdIconDownload *icon_download = g_ptr_array_index(missing, i);
        MfRepoIcon *icon = icon_download->icon;
        GBytes *data = icon_download->download.data;
        g_autofree char *staging = g_build_filename(prepared->icons_staging, icon->path, NULL);
        MfFileEntry actual;

        mf_file_entry_set_for_bytes(&actual, data);
        if (!mf_file_entry_equal(&actual, &icon->entry)) {
            g_set_error(error,
                        MF_ERROR,
                        MF_ERROR_VERIFICATION_FAILED,
                        _("The icon %s of %s does not match what the repository records"),
                        icon->path,
                        prepared->origin);
            return FALSE;
        }
        if (!mf_bytes_are_jxl(data)) {
            g_set_error(error,
                        MF_ERROR,
                        MF_ERROR_INVALID_REPOSITORY,
                        _("The icon %s of %s is not a JPEG XL image"),
                        icon->path,
                        prepared->origin);
            return FALSE;
        }
        if (!mfd_write_file(staging, data, error))
            return FALSE;
    }

    return TRUE;
}

/**
 * mfd_prepare_channel:
 * @staging_dir: the staging directory of the remote
 *
 * Download and verify the files of a channel, and write them into the
 * staging directories.
 *
 * Returns: (transfer full): the prepared channel, or %NULL on error
 */
static MfdPreparedChannel *
mfd_prepare_channel(const char *root,
                    MfdFetcher *fetcher,
                    MfRemote *remote,
                    MfRepoArchitecture *arch,
                    const char *channel,
                    const char *staging_dir,
                    GError **error)
{
    g_autoptr(MfdPreparedChannel) prepared = g_new0(MfdPreparedChannel, 1);
    g_autoptr(MfRepoComponentIndex) index = NULL;
    g_autoptr(GPtrArray) icons = NULL;
    g_autofree char *channel_dir = g_build_filename(staging_dir, channel, NULL);
    g_autofree char *index_path = g_build_filename(channel_dir, MF_REPO_COMPONENT_INDEX_NAME, NULL);
    g_autofree char *icon_index_path = g_build_filename(channel_dir, MF_REPO_ICON_INDEX_NAME, NULL);
    g_autofree char *location = g_build_filename(channel, arch->name, NULL);
    g_autofree char *index_location = g_build_filename(location, MF_REPO_COMPONENT_INDEX_NAME, NULL);
    g_autofree char *catalog_location = g_build_filename(location, MF_REPO_CATALOG_NAME, NULL);
    g_autofree char *icons_location = g_build_filename(location, MF_REPO_ICON_INDEX_NAME, NULL);
    g_auto(MfdMemoryDownload) index_file = {
        .url = mf_remote_build_url(remote, index_location),
        .max_size = arch->component_index.size,
    };
    g_auto(MfdMemoryDownload) catalog_file = {
        .url = mf_remote_build_url(remote, catalog_location),
        .max_size = arch->catalog.size,
    };
    g_auto(MfdMemoryDownload) icon_index_file = {
        .url = mf_remote_build_url(remote, icons_location),
        .max_size = arch->icon_index.size,
    };
    MfdMemoryDownload *downloads[] = {&index_file, &catalog_file, &icon_index_file};

    prepared->origin = mf_origin_build_name(mf_remote_get_id(remote), channel);
    prepared->icons_live = g_build_filename(root, MF_STORE_CATALOG_ICONS_SUBDIR, prepared->origin, NULL);

    if (!mfd_fetcher_download_to_memory(fetcher, downloads, G_N_ELEMENTS(downloads), error))
        return NULL;
    index = mf_repo_component_index_new(index_file.data, &arch->component_index, arch->name, error);
    if (index == NULL)
        return NULL;
    if (!mf_repo_check_catalog(catalog_file.data, &arch->catalog, prepared->origin, index, error))
        return NULL;
    icons = mf_repo_icon_index_new(icon_index_file.data, &arch->icon_index, error);
    if (icons == NULL)
        return NULL;

    if (!glnx_shutil_mkdir_p_at(AT_FDCWD, channel_dir, 0755, NULL, error) ||
        !mfd_write_file(index_path, index_file.data, error) ||
        !mfd_write_file(icon_index_path, icon_index_file.data, error))
        return NULL;
    prepared->catalog = g_bytes_ref(catalog_file.data);

    prepared->icons_staging = mfd_make_staging_dir(prepared->icons_live, error);
    if (prepared->icons_staging == NULL)
        return NULL;
    if (!mfd_prepare_icons(fetcher, remote, icons, prepared, error))
        return NULL;

    return g_steal_pointer(&prepared);
}

/**
 * mfd_swap_in_channel:
 *
 * Move the icons and the catalog file of a prepared channel into place.
 */
static gboolean
mfd_swap_in_channel(const char *root, MfdPreparedChannel *prepared, GError **error)
{
    g_autofree char *file_name = g_strconcat(prepared->origin, MF_STORE_CATALOG_FILE_SUFFIX, NULL);
    g_autofree char *catalog_dir = g_build_filename(root, MF_STORE_CATALOG_XML_SUBDIR, NULL);
    g_autofree char *catalog = g_build_filename(catalog_dir, file_name, NULL);

    if (!mfd_swap_into_place(prepared->icons_staging, prepared->icons_live, error))
        return FALSE;
    g_clear_pointer(&prepared->icons_staging, g_free);
    /* AppStream notices the new catalog because renaming the file into place changes the directory */
    return glnx_shutil_mkdir_p_at(AT_FDCWD, catalog_dir, 0755, NULL, error) &&
           mfd_write_file(catalog, prepared->catalog, error);
}

/**
 * mfd_read_cached_index:
 *
 * Read the cached root index of a remote, if its signature can still be verified.
 *
 * Returns: (transfer full) (nullable): the root index, or %NULL if there is none
 */
static MfRepoRootIndex *
mfd_read_cached_index(const char *root, MfRemote *remote, MfTrustedKeys *keys, GBytes **ret_data)
{
    g_autoptr(GError) error = NULL;
    MfRepoRootIndex *index = mfd_origin_read_root_index(root, remote, keys, ret_data, &error);

    /* the trusted keys of the remote may have changed since the last refresh */
    if (index == NULL && !g_error_matches(error, MF_ERROR, MF_ERROR_NOT_FOUND))
        g_warning("Ignoring the cached root index of %s: %s", mf_remote_get_id(remote), error->message);
    return index;
}

/**
 * mfd_check_freshness:
 *
 * @ret_changed: (out): whether the root index differs from the cached one
 *
 * Check that a downloaded root index is not older than the cached one, and
 * that it has not expired. A root index with the same serial as the cached
 * one has to be the same file.
 */
static gboolean
mfd_check_freshness(const char *root,
                    MfRemote *remote,
                    MfTrustedKeys *keys,
                    MfRepoRootIndex *index,
                    GBytes *data,
                    gboolean *ret_changed,
                    GError **error)
{
    g_autoptr(GBytes) stored_data = NULL;
    g_autoptr(MfRepoRootIndex) stored = mfd_read_cached_index(root, remote, keys, &stored_data);
    const char *remote_id = mf_remote_get_id(remote);

    if (stored != NULL && index->serial < stored->serial) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_VERIFICATION_FAILED,
                    _("The repository %s serves an older root index (serial %" G_GUINT64_FORMAT
                      ") than the one we have (serial %" G_GUINT64_FORMAT ")"),
                    remote_id,
                    index->serial,
                    stored->serial);
        return FALSE;
    }
    if (stored != NULL && index->serial == stored->serial && !g_bytes_equal(data, stored_data)) {
        g_set_error(
            error,
            MF_ERROR,
            MF_ERROR_VERIFICATION_FAILED,
            _("The repository %s serves another root index with the serial we have (%" G_GUINT64_FORMAT ")"),
            remote_id,
            index->serial);
        return FALSE;
    }
    *ret_changed = stored == NULL || !g_bytes_equal(data, stored_data);

    return mfd_origin_check_expiry(index, error);
}

/**
 * mfd_cached_metadata_is_complete:
 * @index: the root index of the remote, which is the same as the cached one
 *
 * Check that the store has the metadata of exactly the channels that the
 * remote installs from. The channels of a remote can be changed in its
 * configuration without its repository publishing anything new.
 *
 * Returns: %TRUE if nothing has to be downloaded or removed for the remote
 */
static gboolean
mfd_cached_metadata_is_complete(const char *root, MfRemote *remote, MfRepoRootIndex *index)
{
    const char *remote_id = mf_remote_get_id(remote);
    const char *const *channels = mf_remote_get_channels(remote);
    const char *architecture = mf_get_host_architecture();
    g_autoptr(GHashTable) origins = mfd_list_origins(root, NULL);
    GHashTableIter iter;
    gpointer origin;

    if (origins == NULL)
        return FALSE;

    for (guint i = 0; channels[i] != NULL; i++) {
        MfRepoChannel *channel = mf_repo_root_index_find_channel(index, channels[i]);
        gboolean available = channel != NULL &&
                             mf_repo_channel_find_architecture(channel, architecture) != NULL;
        g_autofree char *origin_name = mf_origin_build_name(remote_id, channels[i]);
        g_autofree char *catalog_name = g_strconcat(origin_name, MF_STORE_CATALOG_FILE_SUFFIX, NULL);
        g_autofree char *catalog = g_build_filename(root, MF_STORE_CATALOG_XML_SUBDIR, catalog_name, NULL);
        g_autofree char *component_index = g_build_filename(root,
                                                            MF_STORE_REMOTES_SUBDIR,
                                                            remote_id,
                                                            channels[i],
                                                            MF_REPO_COMPONENT_INDEX_NAME,
                                                            NULL);

        if (g_file_test(component_index, G_FILE_TEST_EXISTS) != available ||
            g_file_test(catalog, G_FILE_TEST_EXISTS) != available)
            return FALSE;
    }

    /* a channel that the remote does not install from anymore has to be removed */
    g_hash_table_iter_init(&iter, origins);
    while (g_hash_table_iter_next(&iter, &origin, NULL)) {
        g_autofree char *id = NULL;
        const char *channel;

        if (mf_origin_split_name(origin, &id, &channel) && g_str_equal(id, remote_id) &&
            !g_strv_contains(channels, channel))
            return FALSE;
    }

    return TRUE;
}

/**
 * mfd_warn_about_missing_channels:
 *
 * Add a warning to the result for every channel of a remote that its
 * repository does not have for the architecture of this system.
 */
static void
mfd_warn_about_missing_channels(MfResult *result, MfRemote *remote, MfRepoRootIndex *index)
{
    const char *const *channels = mf_remote_get_channels(remote);
    const char *architecture = mf_get_host_architecture();

    for (guint i = 0; channels[i] != NULL; i++) {
        MfRepoChannel *channel = mf_repo_root_index_find_channel(index, channels[i]);

        if (channel == NULL || mf_repo_channel_find_architecture(channel, architecture) == NULL)
            mfd_emit_warning(result,
                             _("The repository %s has no channel %s for %s"),
                             mf_remote_get_id(remote),
                             channels[i],
                             architecture);
    }
}

/**
 * mfd_refresh_fetch_root_index:
 * @fetcher: the fetcher to download with
 * @url: the URL of a repository, without a `/` at its end
 * @ret_data: (out) (transfer full): the root index, as the repository serves it
 * @ret_signature: (out) (transfer full): the signature of the root index
 *
 * Download the root index of a repository, and its signature.
 *
 * Returns: %TRUE if both files were downloaded
 */
gboolean
mfd_refresh_fetch_root_index(MfdFetcher *fetcher,
                             const char *url,
                             GBytes **ret_data,
                             GBytes **ret_signature,
                             GError **error)
{
    g_auto(MfdMemoryDownload) index_file = {
        .url = g_strconcat(url, "/", MF_REPO_ROOT_INDEX_NAME, NULL),
        .max_size = ROOT_INDEX_DOWNLOAD_MAX,
    };
    g_auto(MfdMemoryDownload) signature_file = {
        .url = g_strconcat(url, "/", MF_REPO_ROOT_SIGNATURE_NAME, NULL),
        .max_size = SIGNATURE_DOWNLOAD_MAX,
    };
    MfdMemoryDownload *downloads[] = {&index_file, &signature_file};

    if (!mfd_fetcher_download_to_memory(fetcher, downloads, G_N_ELEMENTS(downloads), error))
        return FALSE;

    *ret_data = g_steal_pointer(&index_file.data);
    *ret_signature = g_steal_pointer(&signature_file.data);
    return TRUE;
}

/**
 * mfd_refresh_remote:
 * @root: the root of the store
 * @fetcher: the fetcher to download with
 * @remote: the remote to refresh
 * @result: (nullable): the result to add warnings to
 * @ret_changed: (out) (optional): whether the root index of the repository
 *   differs from the cached one
 * @error: a #GError
 *
 * Download the metadata of a remote, verify it, and store it. If the
 * repository still has the root index that we have cached, nothing else is
 * downloaded. A channel that the repository does not have for the
 * architecture of this system only causes a warning.
 *
 * Returns: %TRUE if the remote was refreshed
 */
gboolean
mfd_refresh_remote(const char *root,
                   MfdFetcher *fetcher,
                   MfRemote *remote,
                   MfResult *result,
                   gboolean *ret_changed,
                   GError **error)
{
    const char *remote_id = mf_remote_get_id(remote);
    const char *const *channels = mf_remote_get_channels(remote);
    const char *architecture = mf_get_host_architecture();
    g_autoptr(MfTrustedKeys) keys = NULL;
    g_autoptr(GBytes) index_data = NULL;
    g_autoptr(GBytes) signature = NULL;
    g_autoptr(MfRepoRootIndex) index = NULL;
    g_autoptr(GPtrArray)
        prepared = g_ptr_array_new_with_free_func((GDestroyNotify) mfd_prepared_channel_free);
    g_autoptr(GPtrArray) gone = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GHashTable) origins = NULL;
    g_autofree char *remote_dir = g_build_filename(root, MF_STORE_REMOTES_SUBDIR, remote_id, NULL);
    g_autofree char *staging_dir = NULL;
    g_autofree char *index_path = NULL;
    g_autofree char *signature_path = NULL;
    GHashTableIter iter;
    gpointer origin;
    gboolean changed = FALSE;
    gboolean ret = FALSE;

    g_info("Refreshing %s from %s", remote_id, mf_remote_get_url(remote));
    keys = mf_remote_load_trusted_keys(remote, error);
    if (keys == NULL)
        return FALSE;
    if (!mfd_refresh_fetch_root_index(fetcher, mf_remote_get_url(remote), &index_data, &signature, error))
        return FALSE;
    index = mf_repo_root_index_new_from_signed(index_data, signature, keys, remote_id, error);
    if (index == NULL)
        return FALSE;
    if (!mfd_check_freshness(root, remote, keys, index, index_data, &changed, error))
        return FALSE;
    mfd_warn_about_missing_channels(result, remote, index);

    /* the root index pins every other file, so with the same root index nothing has changed */
    if (!changed && mfd_cached_metadata_is_complete(root, remote, index)) {
        g_info("The metadata of %s is up to date, serial %" G_GUINT64_FORMAT, remote_id, index->serial);
        if (ret_changed != NULL)
            *ret_changed = FALSE;
        return TRUE;
    }

    staging_dir = mfd_make_staging_dir(remote_dir, error);
    if (staging_dir == NULL)
        return FALSE;
    index_path = g_build_filename(staging_dir, MF_REPO_ROOT_INDEX_NAME, NULL);
    signature_path = g_build_filename(staging_dir, MF_REPO_ROOT_SIGNATURE_NAME, NULL);
    if (!mfd_write_file(index_path, index_data, error) || !mfd_write_file(signature_path, signature, error))
        goto out;

    for (guint i = 0; channels[i] != NULL; i++) {
        MfRepoChannel *channel = mf_repo_root_index_find_channel(index, channels[i]);
        MfRepoArchitecture *arch = channel != NULL ? mf_repo_channel_find_architecture(channel, architecture)
                                                   : NULL;
        MfdPreparedChannel *channel_data;

        if (arch == NULL) {
            g_ptr_array_add(gone, mf_origin_build_name(remote_id, channels[i]));
            continue;
        }
        channel_data = mfd_prepare_channel(root, fetcher, remote, arch, channels[i], staging_dir, error);
        if (channel_data == NULL)
            goto out;
        g_ptr_array_add(prepared, channel_data);
    }

    /* all files are verified, so we move them into place, and remove the channels that are gone */
    for (guint i = 0; i < prepared->len; i++) {
        if (!mfd_swap_in_channel(root, g_ptr_array_index(prepared, i), error))
            goto out;
    }
    origins = mfd_list_origins(root, error);
    if (origins == NULL)
        goto out;
    g_hash_table_iter_init(&iter, origins);
    while (g_hash_table_iter_next(&iter, &origin, NULL)) {
        g_autofree char *id = NULL;
        const char *channel;

        if (!mf_origin_split_name(origin, &id, &channel) || !g_str_equal(id, remote_id))
            continue;
        if (!g_strv_contains(channels, channel))
            g_ptr_array_add(gone, g_strdup(origin));
    }
    for (guint i = 0; i < gone->len; i++) {
        if (!mfd_remove_origin(root, g_ptr_array_index(gone, i), error))
            goto out;
    }
    if (!mfd_swap_into_place(staging_dir, remote_dir, error))
        goto out;
    g_clear_pointer(&staging_dir, g_free);

    g_info("Refreshed %s, serial %" G_GUINT64_FORMAT, remote_id, index->serial);
    if (ret_changed != NULL)
        *ret_changed = changed;
    ret = TRUE;

out:
    if (staging_dir != NULL)
        glnx_shutil_rm_rf_at(AT_FDCWD, staging_dir, NULL, NULL);
    return ret;
}

/**
 * mfd_refresh_forget_remote:
 * @root: the root of the store
 * @remote_id: the ID of a remote
 *
 * Remove all cached data of a remote: its metadata, and the catalog and the
 * icons of each of its channels. Components that were installed from the
 * remote stay installed.
 *
 * Returns: %TRUE on success
 */
gboolean
mfd_refresh_forget_remote(const char *root, const char *remote_id, GError **error)
{
    g_autofree char *remote_dir = g_build_filename(root, MF_STORE_REMOTES_SUBDIR, remote_id, NULL);
    g_autoptr(GHashTable) origins = NULL;
    GHashTableIter iter;
    gpointer origin;

    if (g_file_test(remote_dir, G_FILE_TEST_EXISTS)) {
        g_info("Removing the cached metadata of the remote %s", remote_id);
        if (!glnx_shutil_rm_rf_at(AT_FDCWD, remote_dir, NULL, error))
            return FALSE;
    }

    origins = mfd_list_origins(root, error);
    if (origins == NULL)
        return FALSE;
    g_hash_table_iter_init(&iter, origins);
    while (g_hash_table_iter_next(&iter, &origin, NULL)) {
        g_autofree char *id = NULL;

        if (mf_origin_split_name(origin, &id, NULL) && g_str_equal(id, remote_id) &&
            !mfd_remove_origin(root, origin, error))
            return FALSE;
    }

    return TRUE;
}

/**
 * mfd_remove_staging_dirs:
 *
 * Remove the staging directories in @dir that an interrupted refresh left behind.
 */
static gboolean
mfd_remove_staging_dirs(const char *dir, GError **error)
{
    g_autoptr(GDir) gdir = g_dir_open(dir, 0, NULL);
    const char *name;

    while (gdir != NULL && (name = g_dir_read_name(gdir)) != NULL) {
        g_autofree char *path = g_build_filename(dir, name, NULL);

        if (g_str_has_prefix(name, STAGING_PREFIX) && g_str_has_suffix(name, STAGING_SUFFIX) &&
            !glnx_shutil_rm_rf_at(AT_FDCWD, path, NULL, error))
            return FALSE;
    }
    return TRUE;
}

/**
 * mfd_refresh_remove_unconfigured:
 * @root: the root of the store
 * @remotes: (element-type MfRemote): every remote that is configured
 *
 * Remove the cached data of remotes that are not configured anymore.
 *
 * Returns: %TRUE on success
 */
gboolean
mfd_refresh_remove_unconfigured(const char *root, GPtrArray *remotes, GError **error)
{
    g_autoptr(GHashTable) ids = g_hash_table_new(g_str_hash, g_str_equal);
    g_autoptr(GHashTable) gone = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_autofree char *remotes_dir = g_build_filename(root, MF_STORE_REMOTES_SUBDIR, NULL);
    g_autofree char *icons_dir = g_build_filename(root, MF_STORE_CATALOG_ICONS_SUBDIR, NULL);
    g_autoptr(GHashTable) origins = NULL;
    g_autoptr(GDir) gdir = NULL;
    GHashTableIter iter;
    gpointer origin;
    gpointer remote_id;
    const char *name;

    for (guint i = 0; i < remotes->len; i++)
        g_hash_table_add(ids, (gpointer) mf_remote_get_id(g_ptr_array_index(remotes, i)));

    if (!mfd_remove_staging_dirs(remotes_dir, error) || !mfd_remove_staging_dirs(icons_dir, error))
        return FALSE;

    /* a removed remote may have left its metadata directory, or only catalog files and icons */
    gdir = g_dir_open(remotes_dir, 0, NULL);
    while (gdir != NULL && (name = g_dir_read_name(gdir)) != NULL) {
        if (name[0] != '.' && !g_hash_table_contains(ids, name))
            g_hash_table_add(gone, g_strdup(name));
    }
    origins = mfd_list_origins(root, error);
    if (origins == NULL)
        return FALSE;
    g_hash_table_iter_init(&iter, origins);
    while (g_hash_table_iter_next(&iter, &origin, NULL)) {
        g_autofree char *id = NULL;

        if (mf_origin_split_name(origin, &id, NULL) && !g_hash_table_contains(ids, id))
            g_hash_table_add(gone, g_steal_pointer(&id));
    }

    g_hash_table_iter_init(&iter, gone);
    while (g_hash_table_iter_next(&iter, &remote_id, NULL)) {
        if (!mfd_refresh_forget_remote(root, remote_id, error))
            return FALSE;
    }

    return TRUE;
}
