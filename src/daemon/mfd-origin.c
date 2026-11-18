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

/*
 * The cached metadata of a remote is verified again whenever we install from
 * it. The trusted keys of the remote may have changed since the last refresh,
 * its root index may have expired, and an interrupted refresh may have left
 * files that do not match the cached root index.
 */

/**
 * mfd_origin_ref:
 *
 * Returns: (transfer full): @origin
 */
MfdOrigin *
mfd_origin_ref(MfdOrigin *origin)
{
    return g_rc_box_acquire(origin);
}

static void
mfd_origin_clear(MfdOrigin *origin)
{
    g_free(origin->name);
    g_clear_object(&origin->remote);
    g_free(origin->image_url_prefix);
    g_clear_pointer(&origin->components, mf_repo_component_index_free);
}

/**
 * mfd_origin_unref:
 */
void
mfd_origin_unref(MfdOrigin *origin)
{
    if (origin != NULL)
        g_rc_box_release_full(origin, (GDestroyNotify) mfd_origin_clear);
}

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
 * mfd_origin_load:
 *
 * Read the cached component index of a channel. It has to match its entry
 * in the root index.
 *
 * Returns: (transfer full): the origin, or %NULL
 */
static MfdOrigin *
mfd_origin_load(const char *root,
                MfRemote *remote,
                MfRepoRootIndex *index,
                const char *channel_name,
                GError **error)
{
    const char *remote_id = mf_remote_get_id(remote);
    const char *architecture = mf_get_host_architecture();
    MfRepoChannel *channel = mf_repo_root_index_find_channel(index, channel_name);
    MfRepoArchitecture *arch = channel != NULL ? mf_repo_channel_find_architecture(channel, architecture)
                                               : NULL;
    g_autofree char *path = NULL;
    g_autoptr(GBytes) data = NULL;
    g_autoptr(MfdOrigin) origin = NULL;
    g_autoptr(MfRepoComponentIndex) components = NULL;
    g_autoptr(GError) local_error = NULL;

    if (arch == NULL) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_NOT_FOUND,
                    _("The repository %s has no channel %s for %s"),
                    remote_id,
                    channel_name,
                    architecture);
        return NULL;
    }

    path = g_build_filename(root,
                            MF_STORE_REMOTES_SUBDIR,
                            remote_id,
                            channel_name,
                            MF_REPO_COMPONENT_INDEX_NAME,
                            NULL);
    data = mfd_read_cached_file(path, remote_id, error);
    if (data == NULL)
        return NULL;
    components = mf_repo_component_index_new(data, &arch->component_index, arch->name, &local_error);
    if (components == NULL) {
        g_set_error(
            error,
            MF_ERROR,
            g_error_matches(local_error, MF_ERROR, MF_ERROR_VERIFICATION_FAILED)
                ? MF_ERROR_VERIFICATION_FAILED
                : MF_ERROR_INVALID_REPOSITORY,
            _("The cached metadata of %s-%s does not match its root index, run 'manifold refresh': %s"),
            remote_id,
            channel_name,
            local_error->message);
        return NULL;
    }

    origin = g_rc_box_new0(MfdOrigin);
    origin->name = mf_origin_build_name(remote_id, channel_name);
    origin->remote = g_object_ref(remote);
    origin->serial = index->serial;
    origin->image_url_prefix = g_strdup(channel->image_url_prefix);
    origin->components = g_steal_pointer(&components);
    return g_steal_pointer(&origin);
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

static void
mfd_origin_problem_free(MfdOriginProblem *problem)
{
    g_free(problem->remote_id);
    g_free(problem->channel);
    g_error_free(problem->error);
    g_free(problem);
}

/**
 * mfd_origin_set_add_problem:
 * @channel: (nullable): the channel that can not be used, or %NULL if it is the whole remote
 * @error: (transfer full): the reason why it can not be used
 *
 * Record that a remote, or one of its channels, can not be used.
 */
static void
mfd_origin_set_add_problem(MfdOriginSet *set, const char *remote_id, const char *channel, GError *error)
{
    MfdOriginProblem *problem = g_new0(MfdOriginProblem, 1);

    if (channel != NULL)
        g_info("Not using the channel %s of the remote %s: %s", channel, remote_id, error->message);
    else
        g_info("Not using the remote %s: %s", remote_id, error->message);

    problem->remote_id = g_strdup(remote_id);
    problem->channel = g_strdup(channel);
    problem->error = error;
    g_ptr_array_add(set->problems, problem);
}

/**
 * mfd_origin_set_load_remote:
 *
 * Load the origins of a remote, and verify its stored metadata again. A remote
 * or channel that we can not use is recorded as a problem.
 */
static void
mfd_origin_set_load_remote(MfdOriginSet *set, const char *root, MfRemote *remote)
{
    const char *remote_id = mf_remote_get_id(remote);
    const char *const *channels = mf_remote_get_channels(remote);
    g_autoptr(MfTrustedKeys) keys = NULL;
    g_autoptr(MfRepoRootIndex) index = NULL;
    GError *error = NULL;

    keys = mf_remote_load_trusted_keys(remote, &error);
    if (keys != NULL)
        index = mfd_origin_read_root_index(root, remote, keys, NULL, &error);
    if (index != NULL && !mfd_origin_check_expiry(index, &error)) {
        /* a refresh may get us a root index that is still valid */
        g_autofree char *message = g_strdup_printf(_("%s, run 'manifold refresh'"), error->message);

        g_free(error->message);
        error->message = g_steal_pointer(&message);
    }
    if (error != NULL) {
        mfd_origin_set_add_problem(set, remote_id, NULL, error);
        return;
    }

    for (gsize i = 0; channels[i] != NULL; i++) {
        MfdOrigin *origin = mfd_origin_load(root, remote, index, channels[i], &error);

        if (origin == NULL) {
            mfd_origin_set_add_problem(set, remote_id, channels[i], g_steal_pointer(&error));
            continue;
        }
        g_ptr_array_add(set->origins, origin);
    }
}

/**
 * mfd_origin_set_load:
 * @root: the root of the store
 * @error: a #GError
 *
 * Load the origins of every remote from their cached metadata.
 *
 * Returns: (transfer full): the origins, in the order of the remotes and their
 *   channels, or %NULL if the remotes could not be read
 */
MfdOriginSet *
mfd_origin_set_load(const char *root, GError **error)
{
    g_autoptr(GPtrArray) remotes = mf_remote_list_all(error);
    MfdOriginSet *set;

    if (remotes == NULL)
        return NULL;

    set = g_new0(MfdOriginSet, 1);
    set->origins = g_ptr_array_new_with_free_func((GDestroyNotify) mfd_origin_unref);
    set->problems = g_ptr_array_new_with_free_func((GDestroyNotify) mfd_origin_problem_free);
    for (guint i = 0; i < remotes->len; i++)
        mfd_origin_set_load_remote(set, root, g_ptr_array_index(remotes, i));

    return set;
}

/**
 * mfd_origin_set_free:
 */
void
mfd_origin_set_free(MfdOriginSet *set)
{
    if (set == NULL)
        return;
    g_ptr_array_unref(set->origins);
    g_ptr_array_unref(set->problems);
    g_free(set);
}

/**
 * mfd_origin_set_find:
 *
 * Returns: (transfer none) (nullable): the origin with the name @origin_name
 */
MfdOrigin *
mfd_origin_set_find(MfdOriginSet *set, const char *origin_name)
{
    for (guint i = 0; i < set->origins->len; i++) {
        MfdOrigin *origin = g_ptr_array_index(set->origins, i);

        if (g_str_equal(origin->name, origin_name))
            return origin;
    }
    return NULL;
}

/**
 * mfd_origin_set_find_problem:
 *
 * Returns: (transfer none) (nullable): the reason why the origin @origin_name,
 *   or its whole remote, can not be used
 */
const GError *
mfd_origin_set_find_problem(MfdOriginSet *set, const char *origin_name)
{
    g_autofree char *remote_id = NULL;
    const char *channel;

    if (!mf_origin_split_name(origin_name, &remote_id, &channel))
        return NULL;

    for (guint i = 0; i < set->problems->len; i++) {
        MfdOriginProblem *problem = g_ptr_array_index(set->problems, i);

        if (g_str_equal(problem->remote_id, remote_id) &&
            (problem->channel == NULL || g_str_equal(problem->channel, channel)))
            return problem->error;
    }
    return NULL;
}
