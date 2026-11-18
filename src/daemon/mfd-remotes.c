/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfd-remotes.h"

#include <string.h>
#include <glib/gi18n-lib.h>

#include "mf-remote-private.h"
#include "mf-signer-private.h"
#include "mf-trusted-keys-private.h"
#include "mfd-refresh.h"
#include "mfd-utils.h"

/*
 * Adding and removing remotes.
 *
 * A remote that we add is a directory in /etc/manifold/remotes/, named after
 * the ID of its repository. It contains the remote.conf file, and the public
 * key that the user decided to trust in keys/.
 *
 * Before a repository is added, we inspect it: we download its root index and
 * the public key which the repository serves next to the index, and check
 * that the key verifies the root index. This key is only as trustworthy as
 * the server it was downloaded from, until the user has confirmed its
 * fingerprint.
 */

/* a repository serves its public key next to its root index, as <id>_key.pub */
#define PUBLIC_KEY_FILE_SUFFIX "_key.pub"

/* the largest size of a public key file, which is one short line */
constexpr guint64 PUBLIC_KEY_DOWNLOAD_MAX = 16 * 1024;

void
mfd_remote_inspection_free(MfdRemoteInspection *inspection)
{
    if (inspection == NULL)
        return;
    g_free(inspection->url);
    g_free(inspection->public_key);
    g_clear_pointer(&inspection->index, mf_repo_root_index_free);
    g_free(inspection);
}

/**
 * mfd_remote_inspection_list_channels:
 *
 * Returns: (transfer full): the channels of the repository that have
 *   components for the architecture of this system
 */
GStrv
mfd_remote_inspection_list_channels(MfdRemoteInspection *inspection)
{
    const char *architecture = mf_get_host_architecture();
    g_autoptr(GStrvBuilder) builder = g_strv_builder_new();

    for (guint i = 0; i < inspection->index->channels->len; i++) {
        MfRepoChannel *channel = g_ptr_array_index(inspection->index->channels, i);

        if (mf_repo_channel_find_architecture(channel, architecture) != NULL)
            g_strv_builder_add(builder, channel->name);
    }

    return g_strv_builder_end(builder);
}

/**
 * mfd_fetch_public_key:
 * @url: the URL of the repository
 * @remote_id: the ID of the repository, from its root index
 *
 * Download the public key that a repository serves next to its root index.
 *
 * Returns: (transfer full): the key without its comment, or %NULL
 */
static char *
mfd_fetch_public_key(MfdFetcher *fetcher, const char *url, const char *remote_id, GError **error)
{
    g_autofree char *key_url = g_strconcat(url, "/", remote_id, PUBLIC_KEY_FILE_SUFFIX, NULL);
    g_autoptr(GBytes) file = NULL;
    g_autofree char *text = NULL;
    char *public_key = NULL;
    const char *data;
    gsize size;

    file = mfd_fetcher_download_file(fetcher, key_url, PUBLIC_KEY_DOWNLOAD_MAX, error);
    if (file == NULL)
        return NULL;

    /* we refuse NUL bytes, as the key is validated as a string and they would end it early */
    data = g_bytes_get_data(file, &size);
    if (size > 0 && memchr(data, '\0', size) == NULL) {
        text = g_strndup(data, size);
        public_key = mf_public_key_normalize(text);
    }
    if (public_key == NULL)
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_INVALID_REPOSITORY,
                    _("%s does not hold a valid SSH public key"),
                    key_url);

    return public_key;
}

/**
 * mfd_remotes_inspect:
 * @fetcher: the fetcher to download with
 * @url: the https URL of a repository
 * @public_key: (nullable): the public key that has to sign the repository, or
 *   %NULL to use the key the repository serves
 * @expected_id: (nullable): the ID the repository must have, or %NULL to accept any
 * @error: a #GError
 *
 * Download the root index of a repository, and check that it is signed by
 * @public_key, or by the key that the repository serves. A repository with
 * an expired root index is refused, as nothing can be installed from it.
 *
 * Returns: (transfer full): the repository, or %NULL
 */
MfdRemoteInspection *
mfd_remotes_inspect(MfdFetcher *fetcher,
                    const char *url,
                    const char *public_key,
                    const char *expected_id,
                    GError **error)
{
    g_autoptr(MfdRemoteInspection) inspection = g_new0(MfdRemoteInspection, 1);
    g_autoptr(MfTrustedKeys) keys = mf_trusted_keys_new();
    g_autoptr(GBytes) index_data = NULL;
    g_autoptr(GBytes) signature = NULL;
    g_autofree char *remote_id = NULL;

    inspection->url = mf_remote_normalize_url(url);
    if (inspection->url == NULL) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    _("%s is not an https URL without a query or fragment"),
                    url);
        return NULL;
    }

    g_info("Inspecting the repository at %s", inspection->url);
    if (!mfd_refresh_fetch_root_index(fetcher, inspection->url, &index_data, &signature, error))
        return NULL;

    /* the key file is named after the ID, so we read the ID before the signature is verified */
    remote_id = mf_repo_root_index_read_id(index_data, error);
    if (remote_id == NULL)
        return NULL;
    /* the user confirmed the repository by its ID, so it must not have a different one now */
    if (expected_id != NULL && !g_str_equal(remote_id, expected_id)) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_VERIFICATION_FAILED,
                    _("The repository at %s has the ID %s, not %s"),
                    inspection->url,
                    remote_id,
                    expected_id);
        return NULL;
    }
    if (public_key != NULL) {
        inspection->public_key = mf_public_key_normalize(public_key);
        if (inspection->public_key == NULL) {
            g_set_error_literal(error, MF_ERROR, MF_ERROR_FAILED, _("The given key is not a valid SSH public key"));
            return NULL;
        }
    } else {
        inspection->public_key = mfd_fetch_public_key(fetcher, inspection->url, remote_id, error);
        if (inspection->public_key == NULL)
            return NULL;
    }

    if (!mf_trusted_keys_add_key(keys, inspection->public_key, error))
        return NULL;
    inspection->index = mf_repo_root_index_new_from_signed(index_data, signature, keys, remote_id, error);
    if (inspection->index == NULL)
        return NULL;
    if (!mf_repo_root_index_check_expiry(inspection->index, error))
        return NULL;

    return g_steal_pointer(&inspection);
}

/**
 * mfd_remotes_check_channels:
 *
 * Check that the repository has each of @channels for this architecture.
 * A missing channel would cause a warning on every refresh.
 *
 * Returns: %TRUE if the repository has each of @channels for this architecture
 */
static gboolean
mfd_remotes_check_channels(MfdRemoteInspection *inspection, const char *const *channels, GError **error)
{
    const char *remote_id = inspection->index->id;
    const char *architecture = mf_get_host_architecture();
    g_auto(GStrv) available = mfd_remote_inspection_list_channels(inspection);
    g_autofree char *names = NULL;

    if (!mf_remote_check_channels(remote_id, channels, error))
        return FALSE;
    for (gsize i = 0; channels[i] != NULL; i++) {
        if (g_strv_contains((const char *const *) available, channels[i]))
            continue;

        if (available[0] == NULL) {
            g_set_error(error,
                        MF_ERROR,
                        MF_ERROR_NOT_FOUND,
                        _("The repository %s has no channels for %s"),
                        remote_id,
                        architecture);
            return FALSE;
        }
        names = g_strjoinv(", ", available);
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_NOT_FOUND,
                    /* TRANSLATORS: the repository, the channel, the architecture, and the channels it has */
                    _("The repository %s has no channel %s for %s, only: %s"),
                    remote_id,
                    channels[i],
                    architecture,
                    names);
        return FALSE;
    }

    return TRUE;
}

/**
 * mfd_remotes_write:
 *
 * Write the definition of a remote to /etc. We write the key before
 * remote.conf, so a remote is never defined without its key. The new
 * remote.conf replaces a symbolic link that masks the remote.
 */
static gboolean
mfd_remotes_write(MfdRemoteInspection *inspection, const char *const *channels, GError **error)
{
    const char *remote_id = inspection->index->id;
    g_autofree char *remote_dir = g_build_filename(MF_REMOTES_ADMIN_DIR, remote_id, NULL);
    g_autofree char *keys_dir = g_build_filename(remote_dir, MF_REMOTE_KEYS_SUBDIR, NULL);
    g_autofree char *key_name = g_strconcat(remote_id, MF_PUBLIC_KEY_SUFFIX, NULL);
    g_autofree char *key_line = g_strconcat(inspection->public_key, "\n", NULL);
    g_autoptr(GKeyFile) config = g_key_file_new();
    g_autofree char *config_data = NULL;
    gsize config_len;

    g_key_file_set_string(config, "Remote", "URL", inspection->url);
    g_key_file_set_string_list(config, "Remote", "Channels", channels, g_strv_length((GStrv) channels));
    config_data = g_key_file_to_data(config, &config_len, NULL);

    return mfd_make_dir(keys_dir, error) &&
           mfd_write_contents_safe(keys_dir, key_name, key_line, -1, error) &&
           mfd_write_contents_safe(remote_dir, MF_REMOTE_CONFIG_NAME, config_data, config_len, error);
}

/**
 * mfd_remotes_add:
 * @inspection: a repository that was verified with the key the user decided to trust
 * @channels: (nullable): the channels to install from, or %NULL for only `main`
 * @error: a #GError
 *
 * Add a repository as a remote that trusts the public key of @inspection.
 * A masked remote is defined again. A remote that exists already is refused.
 *
 * Returns: (transfer full): the new remote, or %NULL
 */
MfRemote *
mfd_remotes_add(MfdRemoteInspection *inspection, const char *const *channels, GError **error)
{
    const char *const default_channels[] = {MF_REMOTE_DEFAULT_CHANNEL, NULL};
    const char *remote_id = inspection->index->id;
    g_autoptr(MfRemote) existing = NULL;
    g_autoptr(GError) local_error = NULL;

    if (channels == NULL)
        channels = default_channels;
    if (!mfd_remotes_check_channels(inspection, channels, error))
        return NULL;

    existing = mf_remote_new_for_id(remote_id, &local_error);
    if (existing != NULL) {
        g_set_error(error, MF_ERROR, MF_ERROR_REMOTE_EXISTS, _("There is a remote %s already"), remote_id);
        return NULL;
    }
    /* a remote that is masked, or not defined at all, can be added */
    if (!g_error_matches(local_error, MF_ERROR, MF_ERROR_NOT_FOUND)) {
        g_propagate_error(error, g_steal_pointer(&local_error));
        return NULL;
    }

    if (!mfd_remotes_write(inspection, channels, error))
        return NULL;
    g_message("Added the repository at %s as the remote %s, which trusts the key %s",
              inspection->url,
              remote_id,
              inspection->index->key_fingerprint);

    return mf_remote_new_for_id(remote_id, error);
}

/**
 * mfd_remotes_remove:
 * @root: the root of the store
 * @remote_id: the ID of a remote
 * @error: a #GError
 *
 * Remove a remote and all of its cached data. A remote that the vendor ships
 * can not be removed, so we mask it instead. Components that were installed
 * from the remote stay installed.
 *
 * Returns: %TRUE if the remote was removed
 */
gboolean
mfd_remotes_remove(const char *root, const char *remote_id, GError **error)
{
    g_autoptr(MfRemote) remote = mf_remote_new_for_id(remote_id, error);
    g_autofree char *vendor_config = g_build_filename(MF_REMOTES_VENDOR_DIR,
                                                      remote_id,
                                                      MF_REMOTE_CONFIG_NAME,
                                                      NULL);
    g_autofree char *admin_dir = g_build_filename(MF_REMOTES_ADMIN_DIR, remote_id, NULL);
    g_autofree char *admin_config = g_build_filename(admin_dir, MF_REMOTE_CONFIG_NAME, NULL);

    if (remote == NULL)
        return FALSE;

    if (g_file_test(vendor_config, G_FILE_TEST_EXISTS)) {
        /* a link to /dev/null masks the remote of the vendor */
        if (!mfd_make_dir(admin_dir, error) ||
            !mfd_replace_link("/dev/null", admin_dir, MF_REMOTE_CONFIG_NAME, error))
            return FALSE;
        g_message("Masked the remote %s", remote_id);
    } else {
        /* we remove remote.conf first, so the remote is removed even if deleting its keys fails */
        if (!mfd_unlink_if_exists(admin_config, error) || !mfd_remove_if_exists(admin_dir, error))
            return FALSE;
        g_message("Removed the remote %s", remote_id);
    }

    return mfd_refresh_forget_remote(root, remote_id, error);
}
