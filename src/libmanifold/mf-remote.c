/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mf-remote.h"
#include "mf-remote-private.h"

#include <string.h>
#include <glib/gi18n-lib.h>

#include "mf-bundle-private.h"
#include "mf-error.h"
#include "mf-store-private.h"
#include "mf-utils-private.h"

/**
 * MfRemote:
 *
 * A repository that this system installs components from. It is defined by
 * a `remote.conf` file:
 *
 * |[
 * [Remote]
 * URL=https://example.org/repo
 * Channels=main;
 * ]|
 *
 * A remote is a directory that is named after the ID of the repository. Remotes
 * that vendors ship are in `/usr/share/manifold/remotes/`, and remotes of the
 * system administrator are in `/etc/manifold/remotes/`. A `remote.conf` in
 * `/etc` replaces the file of the same remote in `/usr/share`, and a symbolic
 * link to `/dev/null` in `/etc` masks the remote. The keys in the `keys/`
 * subdirectory of both directories are trusted to sign the repository.
 */

/* the channel of a remote that does not configure any channels */
#define MF_REMOTE_DEFAULT_CHANNEL "main"

/* the longest name a file can have. The catalog file of a channel is named after its origin. */
constexpr size_t FILE_NAME_LENGTH_MAX = 255;

struct _MfRemote {
    GObject parent_instance;

    char *id;
    char *url;
    GStrv channels;
    char *vendor_dir; /* the directory of the remote in the vendor directory */
    char *admin_dir;  /* the directory of the remote in the administrator directory */
};

G_DEFINE_FINAL_TYPE(MfRemote, mf_remote, G_TYPE_OBJECT)

static void
mf_remote_finalize(GObject *object)
{
    MfRemote *remote = MF_REMOTE(object);

    g_free(remote->id);
    g_free(remote->url);
    g_strfreev(remote->channels);
    g_free(remote->vendor_dir);
    g_free(remote->admin_dir);

    G_OBJECT_CLASS(mf_remote_parent_class)->finalize(object);
}

static void
mf_remote_class_init(MfRemoteClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = mf_remote_finalize;
}

static void
mf_remote_init(MfRemote *remote)
{
}

/**
 * mf_remote_id_is_valid:
 *
 * The ID of a repository may only contain ASCII letters, digits, `.`, `_` and
 * `-`, and has to start with a letter or digit. It must not start with
 * `manifold-local`, because this is the prefix of the catalog files of
 * components that were installed from bundles.
 *
 * Returns: %TRUE if @remote_id is a valid ID of a remote
 */
static gboolean
mf_remote_id_is_valid(const char *remote_id)
{
    return remote_id[0] != '\0' && g_ascii_isalnum(remote_id[0]) &&
           mf_str_consists_of(remote_id,
                              "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") &&
           !g_str_has_prefix(remote_id, MF_BUNDLE_CATALOG_ORIGIN);
}

/**
 * mf_remote_channel_is_valid:
 *
 * The name of a channel may only contain lowercase ASCII letters and digits.
 * It can not contain a `-`, so the last `-` in the name of an origin always
 * separates the ID of the remote from the channel. The name `local` is
 * reserved for the origin of bundles.
 *
 * Returns: %TRUE if @channel is a valid name of a channel
 */
gboolean
mf_remote_channel_is_valid(const char *channel)
{
    return channel[0] != '\0' && mf_str_consists_of(channel, "abcdefghijklmnopqrstuvwxyz0123456789") &&
           !g_str_equal(channel, "local");
}

/**
 * mf_origin_build_name:
 * @remote_id: the ID of a remote
 * @channel: a channel of the remote
 *
 * Returns: (transfer full): the name of the origin of a channel, `<remote>-<channel>`
 */
char *
mf_origin_build_name(const char *remote_id, const char *channel)
{
    return g_strdup_printf("%s-%s", remote_id, channel);
}

/**
 * mf_origin_split_name:
 * @origin: the name of an origin
 * @ret_remote_id: (out) (optional) (transfer full): the ID of the remote
 * @ret_channel: (out) (optional) (transfer none): the channel, which points into @origin
 *
 * Split the name of an origin into the ID of its remote and its channel.
 * The name of a channel can not contain a `-`, so the channel starts after
 * the last `-` of the name.
 *
 * Returns: %FALSE if @origin is not the name of the origin of a channel of a remote
 */
gboolean
mf_origin_split_name(const char *origin, char **ret_remote_id, const char **ret_channel)
{
    const char *dash = strrchr(origin, '-');
    g_autofree char *remote_id = NULL;

    if (dash == NULL || !mf_remote_channel_is_valid(dash + 1))
        return FALSE;
    remote_id = g_strndup(origin, (gsize) (dash - origin));
    if (!mf_remote_id_is_valid(remote_id))
        return FALSE;

    if (ret_remote_id != NULL)
        *ret_remote_id = g_steal_pointer(&remote_id);
    if (ret_channel != NULL)
        *ret_channel = dash + 1;
    return TRUE;
}

/**
 * mf_remote_read_url:
 *
 * Returns: (transfer full): the URL of the repository, without a `/` at its end, or %NULL
 */
static char *
mf_remote_read_url(GKeyFile *config, const char *config_path, GError **error)
{
    g_autofree char *url = g_key_file_get_string(config, "Remote", "URL", NULL);
    g_autoptr(GUri) uri = NULL;
    gsize len;

    if (url != NULL)
        uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
    /* we only download over https, and append the paths of files to the URL */
    if (uri == NULL || g_strcmp0(g_uri_get_scheme(uri), "https") != 0 || g_uri_get_host(uri) == NULL ||
        g_uri_get_host(uri)[0] == '\0' || g_uri_get_userinfo(uri) != NULL || g_uri_get_query(uri) != NULL ||
        g_uri_get_fragment(uri) != NULL) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    /* TRANSLATORS: %s is a configuration file, URL a key in it */
                    _("%s: URL must be an https URL without a query or fragment"),
                    config_path);
        return NULL;
    }

    len = strlen(url);
    while (len > 0 && url[len - 1] == '/')
        url[--len] = '\0';
    return g_steal_pointer(&url);
}

/**
 * mf_remote_read_channels:
 *
 * Returns: (transfer full): the channels of the remote, or %NULL
 */
static GStrv
mf_remote_read_channels(GKeyFile *config, const char *config_path, const char *remote_id, GError **error)
{
    g_auto(GStrv) channels = NULL;
    gsize n_channels = 0;

    if (!g_key_file_has_key(config, "Remote", "Channels", NULL)) {
        const char *defaults[] = {MF_REMOTE_DEFAULT_CHANNEL, NULL};

        return g_strdupv((GStrv) defaults);
    }

    channels = g_key_file_get_string_list(config, "Remote", "Channels", &n_channels, NULL);
    for (gsize i = 0; channels != NULL && i < n_channels; i++) {
        /* the origin of each channel names a catalog file */
        if (!mf_remote_channel_is_valid(channels[i]) ||
            g_strv_contains((const char *const *) channels + i + 1, channels[i]) ||
            strlen(remote_id) + 1 + strlen(channels[i]) + strlen(MF_STORE_CATALOG_FILE_SUFFIX) >
                FILE_NAME_LENGTH_MAX) {
            g_set_error(error,
                        MF_ERROR,
                        MF_ERROR_FAILED,
                        /* TRANSLATORS: the configuration file, and a channel it names */
                        _("%s: %s is not a valid channel name, or it is named twice"),
                        config_path,
                        channels[i]);
            return NULL;
        }
    }
    if (n_channels == 0) {
        /* TRANSLATORS: %s is a configuration file, Channels a key in it */
        g_set_error(error, MF_ERROR, MF_ERROR_FAILED, _("%s: Channels names no channel"), config_path);
        return NULL;
    }

    return g_steal_pointer(&channels);
}

/**
 * mf_remote_new_for_dirs:
 * @vendor_dir: the directory with the remotes that vendors ship
 * @admin_dir: the directory with the remotes of the administrator
 * @remote_id: the ID of the remote
 *
 * Read the definition of a remote.
 *
 * Returns: (transfer full): the remote, or %NULL if it is not defined, masked,
 *   or its definition is not valid
 */
MfRemote *
mf_remote_new_for_dirs(const char *vendor_dir, const char *admin_dir, const char *remote_id, GError **error)
{
    g_autoptr(MfRemote) remote = NULL;
    g_autoptr(GKeyFile) config = g_key_file_new();
    g_autoptr(GError) local_error = NULL;
    g_autofree char *config_path = NULL;
    g_autofree char *admin_config = NULL;

    if (!mf_remote_id_is_valid(remote_id)) {
        g_set_error(error, MF_ERROR, MF_ERROR_FAILED, _("%s is not a valid ID of a remote"), remote_id);
        return NULL;
    }

    remote = g_object_new(MF_TYPE_REMOTE, NULL);
    remote->id = g_strdup(remote_id);
    remote->vendor_dir = g_build_filename(vendor_dir, remote_id, NULL);
    remote->admin_dir = g_build_filename(admin_dir, remote_id, NULL);

    admin_config = g_build_filename(remote->admin_dir, MF_REMOTE_CONFIG_NAME, NULL);
    if (mf_file_is_masked(admin_config)) {
        g_set_error(error, MF_ERROR, MF_ERROR_NOT_FOUND, _("The remote %s is masked"), remote_id);
        return NULL;
    }
    if (g_file_test(admin_config, G_FILE_TEST_EXISTS))
        config_path = g_steal_pointer(&admin_config);
    else
        config_path = g_build_filename(remote->vendor_dir, MF_REMOTE_CONFIG_NAME, NULL);

    if (!g_key_file_load_from_file(config, config_path, G_KEY_FILE_NONE, &local_error)) {
        if (g_error_matches(local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_set_error(error, MF_ERROR, MF_ERROR_NOT_FOUND, _("There is no remote %s"), remote_id);
            return NULL;
        }
        g_set_error(error, MF_ERROR, MF_ERROR_FAILED, "%s: %s", config_path, local_error->message);
        return NULL;
    }

    remote->url = mf_remote_read_url(config, config_path, error);
    if (remote->url == NULL)
        return NULL;
    remote->channels = mf_remote_read_channels(config, config_path, remote_id, error);
    if (remote->channels == NULL)
        return NULL;

    return g_steal_pointer(&remote);
}

/**
 * mf_remote_new_for_id:
 * @remote_id: the ID of the remote, which is the ID of its repository
 * @error: a #GError
 *
 * Read the definition of a remote of this system.
 *
 * Returns: (transfer full): the remote, or %NULL if it is not defined, masked,
 *   or its definition is not valid
 */
MfRemote *
mf_remote_new_for_id(const char *remote_id, GError **error)
{
    g_return_val_if_fail(remote_id != NULL, NULL);
    return mf_remote_new_for_dirs(MF_DATADIR "/" MF_REMOTES_SUBDIR,
                                  MF_SYSCONFDIR "/" MF_REMOTES_SUBDIR,
                                  remote_id,
                                  error);
}

/**
 * mf_remote_collect_ids:
 *
 * Add the names of all directories in @dir to @ids, except for hidden ones.
 */
static gboolean
mf_remote_collect_ids(const char *dir, GHashTable *ids, GError **error)
{
    g_autoptr(GError) local_error = NULL;
    g_autoptr(GDir) gdir = g_dir_open(dir, 0, &local_error);
    const char *name;

    if (gdir == NULL) {
        if (g_error_matches(local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            return TRUE;
        g_propagate_error(error, g_steal_pointer(&local_error));
        return FALSE;
    }
    while ((name = g_dir_read_name(gdir)) != NULL) {
        g_autofree char *path = g_build_filename(dir, name, NULL);

        if (name[0] != '.' && g_file_test(path, G_FILE_TEST_IS_DIR))
            g_hash_table_add(ids, g_strdup(name));
    }

    return TRUE;
}

/**
 * mf_remote_list_for_dirs:
 * @vendor_dir: the directory with the remotes that vendors ship
 * @admin_dir: the directory with the remotes of the administrator
 *
 * Read the definitions of all remotes. Masked remotes are left out, and so
 * are remotes with an invalid definition, after a warning.
 *
 * Returns: (transfer container) (element-type MfRemote): the remotes, ordered
 *   by their ID, or %NULL if the directories can not be read
 */
GPtrArray *
mf_remote_list_for_dirs(const char *vendor_dir, const char *admin_dir, GError **error)
{
    g_autoptr(GHashTable) ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_autoptr(GPtrArray) remotes = g_ptr_array_new_with_free_func(g_object_unref);
    g_autoptr(GList) sorted = NULL;

    if (!mf_remote_collect_ids(vendor_dir, ids, error) || !mf_remote_collect_ids(admin_dir, ids, error))
        return NULL;

    sorted = g_list_sort(g_hash_table_get_keys(ids), (GCompareFunc) g_strcmp0);
    for (GList *l = sorted; l != NULL; l = l->next) {
        g_autoptr(GError) local_error = NULL;
        MfRemote *remote = mf_remote_new_for_dirs(vendor_dir, admin_dir, l->data, &local_error);

        if (remote != NULL) {
            g_ptr_array_add(remotes, remote);
            continue;
        }
        /* the directory has no remote.conf, or the remote is masked */
        if (g_error_matches(local_error, MF_ERROR, MF_ERROR_NOT_FOUND))
            continue;
        /* a remote with a broken definition must not keep us from using the other remotes */
        g_warning("Ignoring the remote %s: %s", (const char *) l->data, local_error->message);
    }

    return g_steal_pointer(&remotes);
}

/**
 * mf_remote_list_all:
 * @error: a #GError
 *
 * Read the definitions of all remotes of this system. Masked remotes are left
 * out, and so are remotes with an invalid definition, after a warning.
 *
 * Returns: (transfer container) (element-type MfRemote): the remotes, ordered
 *   by their ID, or %NULL if the directories can not be read
 */
GPtrArray *
mf_remote_list_all(GError **error)
{
    return mf_remote_list_for_dirs(MF_DATADIR "/" MF_REMOTES_SUBDIR,
                                   MF_SYSCONFDIR "/" MF_REMOTES_SUBDIR,
                                   error);
}

/**
 * mf_remote_get_id:
 * @remote: an #MfRemote
 *
 * Returns: the ID of the remote, which is the ID of its repository
 */
const char *
mf_remote_get_id(MfRemote *remote)
{
    g_return_val_if_fail(MF_IS_REMOTE(remote), NULL);
    return remote->id;
}

/**
 * mf_remote_get_url:
 * @remote: an #MfRemote
 *
 * Returns: the https URL of the repository, without a `/` at its end
 */
const char *
mf_remote_get_url(MfRemote *remote)
{
    g_return_val_if_fail(MF_IS_REMOTE(remote), NULL);
    return remote->url;
}

/**
 * mf_remote_build_url:
 * @remote: an #MfRemote
 * @location: a path relative to the root of the repository
 *
 * Returns: (transfer full): the URL of @location
 */
char *
mf_remote_build_url(MfRemote *remote, const char *location)
{
    g_autofree char *escaped = NULL;

    g_return_val_if_fail(MF_IS_REMOTE(remote), NULL);

    escaped = g_uri_escape_string(location, G_URI_RESERVED_CHARS_ALLOWED_IN_PATH, FALSE);
    return g_strconcat(remote->url, "/", escaped, NULL);
}

/**
 * mf_remote_get_channels:
 * @remote: an #MfRemote
 *
 * Returns: (transfer none) (array zero-terminated=1): the channels that
 *   components are installed from
 */
const char *const *
mf_remote_get_channels(MfRemote *remote)
{
    g_return_val_if_fail(MF_IS_REMOTE(remote), NULL);
    return (const char *const *) remote->channels;
}

/**
 * mf_remote_load_trusted_keys:
 * @remote: an #MfRemote
 * @error: a #GError
 *
 * Read the keys that are trusted to sign the repository of the remote from
 * the `keys/` directories of the remote. The keys are only trusted for this
 * remote.
 *
 * Returns: (transfer full): the keys, which may be an empty set, or %NULL if
 *   a key file can not be read
 */
MfTrustedKeys *
mf_remote_load_trusted_keys(MfRemote *remote, GError **error)
{
    g_autoptr(MfTrustedKeys) keys = mf_trusted_keys_new();
    g_autofree char *vendor_keys = NULL;
    g_autofree char *admin_keys = NULL;

    g_return_val_if_fail(MF_IS_REMOTE(remote), NULL);

    vendor_keys = g_build_filename(remote->vendor_dir, MF_REMOTE_KEYS_SUBDIR, NULL);
    admin_keys = g_build_filename(remote->admin_dir, MF_REMOTE_KEYS_SUBDIR, NULL);
    if (!mf_trusted_keys_add_dir(keys, vendor_keys, error) ||
        !mf_trusted_keys_add_dir(keys, admin_keys, error))
        return NULL;

    return g_steal_pointer(&keys);
}
