/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mf-remote-info-private.h"

#include <errno.h>
#include <glib/gi18n-lib.h>

#include "mf-error.h"
#include "mf-remote-private.h"
#include "mf-utils.h"

/**
 * MfRemoteInfo:
 *
 * The description of a repository: its ID and URL, its publisher, the time
 * until which its metadata is valid, and the channels that it has for the
 * architecture of this system.
 *
 * mf_remote_get_info() describes the repository of a remote at the time of
 * its last refresh. mf_manager_inspect_remote() describes a repository that
 * is not a remote yet. Only the description of an inspected repository has
 * the public key of the repository and the fingerprint of the key.
 */
struct _MfRemoteInfo {
    GObject parent_instance;

    char *id;
    char *url;
    char *summary;
    char *publisher_name;  /* NULL if the repository names no publisher */
    char *publisher_email; /* NULL if the publisher has no email address */
    GDateTime *valid_until;
    GStrv available_channels;
    char *public_key;  /* the key that verified the root index, or NULL for the repository of a remote */
    char *fingerprint; /* the fingerprint of public_key, or NULL */
};

G_DEFINE_FINAL_TYPE(MfRemoteInfo, mf_remote_info, G_TYPE_OBJECT)

static void
mf_remote_info_finalize(GObject *object)
{
    MfRemoteInfo *info = MF_REMOTE_INFO(object);

    g_free(info->id);
    g_free(info->url);
    g_free(info->summary);
    g_free(info->publisher_name);
    g_free(info->publisher_email);
    g_clear_pointer(&info->valid_until, g_date_time_unref);
    g_strfreev(info->available_channels);
    g_free(info->public_key);
    g_free(info->fingerprint);

    G_OBJECT_CLASS(mf_remote_info_parent_class)->finalize(object);
}

static void
mf_remote_info_class_init(MfRemoteInfoClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = mf_remote_info_finalize;
}

static void
mf_remote_info_init(MfRemoteInfo *info)
{
}

/**
 * mf_remote_info_new_for_root_index:
 * @index: the root index of the repository
 * @url: the URL of the repository
 * @public_key: (nullable): the public key that verified @index, or %NULL if
 *   @index was not verified
 *
 * Returns: (transfer full): the description of the repository
 */
MfRemoteInfo *
mf_remote_info_new_for_root_index(MfRepoRootIndex *index, const char *url, const char *public_key)
{
    MfRemoteInfo *info = g_object_new(MF_TYPE_REMOTE_INFO, NULL);

    info->id = g_strdup(index->id);
    info->url = g_strdup(url);
    info->summary = g_strdup(index->summary);
    info->publisher_name = g_strdup(index->publisher_name);
    info->publisher_email = g_strdup(index->publisher_email);
    info->valid_until = g_date_time_ref(index->valid_until);
    info->available_channels = mf_repo_root_index_list_channels(index, mf_get_host_architecture());
    info->public_key = g_strdup(public_key);
    info->fingerprint = g_strdup(index->key_fingerprint);

    return info;
}

/**
 * mf_remote_info_to_reply:
 * @ret_reply: (out): the parameters of the reply to InspectRemote
 *
 * Build the reply of the service to a request to inspect a repository.
 * mf_remote_info_new_from_reply() reads the reply. The description has to be
 * for an inspected repository, as the reply needs the public key.
 *
 * Returns: 0, or a negative errno
 */
int
mf_remote_info_to_reply(MfRemoteInfo *info, sd_json_variant **ret_reply)
{
    g_autofree char *valid_until = g_date_time_format_iso8601(info->valid_until);

    return sd_json_buildo(
        ret_reply,
        SD_JSON_BUILD_PAIR(
            "remote",
            SD_JSON_BUILD_OBJECT(SD_JSON_BUILD_PAIR_STRING("id", info->id),
                                 SD_JSON_BUILD_PAIR_STRING("url", info->url),
                                 SD_JSON_BUILD_PAIR_STRING("summary", info->summary),
                                 SD_JSON_BUILD_PAIR_CONDITION(info->publisher_name != NULL,
                                                              "publisherName",
                                                              SD_JSON_BUILD_STRING(info->publisher_name)),
                                 SD_JSON_BUILD_PAIR_CONDITION(info->publisher_email != NULL,
                                                              "publisherEmail",
                                                              SD_JSON_BUILD_STRING(info->publisher_email)),
                                 SD_JSON_BUILD_PAIR_STRING("validUntil", valid_until),
                                 SD_JSON_BUILD_PAIR("channels", SD_JSON_BUILD_STRV(info->available_channels)),
                                 SD_JSON_BUILD_PAIR_STRING("publicKey", info->public_key),
                                 SD_JSON_BUILD_PAIR_STRING("fingerprint", info->fingerprint))));
}

/**
 * mf_remote_info_new_from_reply:
 * @reply: the reply of the service to InspectRemote
 *
 * Returns: (transfer full): the repository the reply describes, or %NULL on error
 */
MfRemoteInfo *
mf_remote_info_new_from_reply(sd_json_variant *reply, GError **error)
{
    struct {
        const char *id;
        const char *url;
        const char *summary;
        const char *publisher_name;
        const char *publisher_email;
        const char *valid_until;
        sd_json_variant *channels;
        const char *public_key;
        const char *fingerprint;
    } fields = {};
    static const sd_json_dispatch_field fields_table[] = {
        {"id",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(fields), id),
         SD_JSON_MANDATORY},
        {"url",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(fields), url),
         SD_JSON_MANDATORY},
        {"summary",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(fields), summary),
         SD_JSON_MANDATORY},
        {"publisherName",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(fields), publisher_name),
         0},
        {"publisherEmail",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(fields), publisher_email),
         0},
        {"validUntil",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(fields), valid_until),
         SD_JSON_MANDATORY},
        {"channels",
         SD_JSON_VARIANT_ARRAY, sd_json_dispatch_variant_noref,
         offsetof(typeof(fields), channels),
         SD_JSON_MANDATORY},
        {"publicKey",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(fields), public_key),
         SD_JSON_MANDATORY},
        {"fingerprint",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(fields), fingerprint),
         SD_JSON_MANDATORY},
        {},
    };
    g_autoptr(MfRemoteInfo) info = NULL;
    g_autoptr(GDateTime) valid_until = NULL;
    g_autoptr(GStrvBuilder) channels = g_strv_builder_new();
    sd_json_variant *object = sd_json_variant_by_key(reply, "remote");
    int r;

    r = sd_json_variant_is_object(object)
            ? sd_json_dispatch(object, fields_table, SD_JSON_ALLOW_EXTENSIONS, &fields)
            : -EBADMSG;
    for (size_t i = 0; r >= 0 && i < sd_json_variant_elements(fields.channels); i++) {
        sd_json_variant *channel = sd_json_variant_by_index(fields.channels, i);

        if (!sd_json_variant_is_string(channel))
            r = -EBADMSG;
        else
            g_strv_builder_add(channels, sd_json_variant_string(channel));
    }
    if (r >= 0) {
        valid_until = g_date_time_new_from_iso8601(fields.valid_until, NULL);
        if (valid_until == NULL || !mf_remote_id_is_valid(fields.id))
            r = -EBADMSG;
    }
    if (r < 0) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    _("Reply of the Manifold service was not understood: %s"),
                    g_strerror(-r));
        return NULL;
    }

    info = g_object_new(MF_TYPE_REMOTE_INFO, NULL);
    info->id = g_strdup(fields.id);
    info->url = g_strdup(fields.url);
    info->summary = g_strdup(fields.summary);
    info->publisher_name = g_strdup(fields.publisher_name);
    info->publisher_email = g_strdup(fields.publisher_email);
    info->valid_until = g_steal_pointer(&valid_until);
    info->available_channels = g_strv_builder_end(channels);
    info->public_key = g_strdup(fields.public_key);
    info->fingerprint = g_strdup(fields.fingerprint);

    return g_steal_pointer(&info);
}

/**
 * mf_remote_info_get_id:
 * @info: an #MfRemoteInfo
 *
 * Returns: the ID of the repository, which is the ID of its remote
 */
const char *
mf_remote_info_get_id(MfRemoteInfo *info)
{
    g_return_val_if_fail(MF_IS_REMOTE_INFO(info), NULL);
    return info->id;
}

/**
 * mf_remote_info_get_url:
 * @info: an #MfRemoteInfo
 *
 * Returns: the URL of the repository
 */
const char *
mf_remote_info_get_url(MfRemoteInfo *info)
{
    g_return_val_if_fail(MF_IS_REMOTE_INFO(info), NULL);
    return info->url;
}

/**
 * mf_remote_info_get_summary:
 * @info: an #MfRemoteInfo
 *
 * Returns: a short description of the repository
 */
const char *
mf_remote_info_get_summary(MfRemoteInfo *info)
{
    g_return_val_if_fail(MF_IS_REMOTE_INFO(info), NULL);
    return info->summary;
}

/**
 * mf_remote_info_get_publisher_name:
 * @info: an #MfRemoteInfo
 *
 * Returns: (nullable): the name of the publisher of the repository, or %NULL
 *   if it is not known
 */
const char *
mf_remote_info_get_publisher_name(MfRemoteInfo *info)
{
    g_return_val_if_fail(MF_IS_REMOTE_INFO(info), NULL);
    return info->publisher_name;
}

/**
 * mf_remote_info_get_publisher_email:
 * @info: an #MfRemoteInfo
 *
 * Returns: (nullable): the email address of the publisher of the
 *   repository, or %NULL if there is none
 */
const char *
mf_remote_info_get_publisher_email(MfRemoteInfo *info)
{
    g_return_val_if_fail(MF_IS_REMOTE_INFO(info), NULL);
    return info->publisher_email;
}

/**
 * mf_remote_info_get_valid_until:
 * @info: an #MfRemoteInfo
 *
 * Nothing is installed from a remote with expired metadata, until the remote
 * is refreshed.
 *
 * Returns: (transfer none): the time at which the metadata of the repository expires
 */
GDateTime *
mf_remote_info_get_valid_until(MfRemoteInfo *info)
{
    g_return_val_if_fail(MF_IS_REMOTE_INFO(info), NULL);
    return info->valid_until;
}

/**
 * mf_remote_info_get_available_channels:
 * @info: an #MfRemoteInfo
 *
 * Returns: (transfer none) (array zero-terminated=1): the channels of the
 *   repository that have components for the architecture of this system
 */
const char *const *
mf_remote_info_get_available_channels(MfRemoteInfo *info)
{
    g_return_val_if_fail(MF_IS_REMOTE_INFO(info), NULL);
    return (const char *const *) info->available_channels;
}

/**
 * mf_remote_info_get_public_key:
 * @info: an #MfRemoteInfo
 *
 * The key verified the root index of the repository. Pass it to
 * mf_manager_add_remote() to trust it.
 *
 * Returns: (nullable): the public key that signs the repository, in OpenSSH
 *   format without its comment, or %NULL for the description of a remote
 */
const char *
mf_remote_info_get_public_key(MfRemoteInfo *info)
{
    g_return_val_if_fail(MF_IS_REMOTE_INFO(info), NULL);
    return info->public_key;
}

/**
 * mf_remote_info_get_fingerprint:
 * @info: an #MfRemoteInfo
 *
 * Returns: (nullable): the SHA256 fingerprint of the public key, as
 *   `ssh-keygen -l` shows it, or %NULL for the description of a remote
 */
const char *
mf_remote_info_get_fingerprint(MfRemoteInfo *info)
{
    g_return_val_if_fail(MF_IS_REMOTE_INFO(info), NULL);
    return info->fingerprint;
}
