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

/**
 * MfRemoteInfo:
 *
 * A repository that was inspected before it is added as a remote: the remote
 * it would become, the channels it has, and the public key that signs it.
 * The user decides whether to trust the key by its fingerprint.
 */
struct _MfRemoteInfo {
    GObject parent_instance;

    MfRemote *remote;
    GStrv available_channels;
    char *public_key;
    char *fingerprint;
};

G_DEFINE_FINAL_TYPE(MfRemoteInfo, mf_remote_info, G_TYPE_OBJECT)

static void
mf_remote_info_finalize(GObject *object)
{
    MfRemoteInfo *info = MF_REMOTE_INFO(object);

    g_clear_object(&info->remote);
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
 * mf_remote_info_new_from_reply:
 * @reply: the reply of the service to InspectRemote
 *
 * Returns: (transfer full): the repository the reply describes, or %NULL on error
 */
MfRemoteInfo *
mf_remote_info_new_from_reply(sd_json_variant *reply, GError **error)
{
    struct {
        sd_json_variant *channels;
        const char *public_key;
        const char *fingerprint;
    } fields = {};
    static const sd_json_dispatch_field fields_table[] = {
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
    g_autoptr(MfRemote) remote = NULL;
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
        remote = mf_remote_new_from_json(object);
        if (remote == NULL)
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
    info->remote = g_steal_pointer(&remote);
    info->available_channels = g_strv_builder_end(channels);
    info->public_key = g_strdup(fields.public_key);
    info->fingerprint = g_strdup(fields.fingerprint);

    return g_steal_pointer(&info);
}

/**
 * mf_remote_info_get_remote:
 * @info: an #MfRemoteInfo
 *
 * The remote has the ID, the URL and the description of the repository. It is
 * not configured yet, so it has no channels and no trusted keys.
 *
 * Returns: (transfer none): the remote that the repository would be added as
 */
MfRemote *
mf_remote_info_get_remote(MfRemoteInfo *info)
{
    g_return_val_if_fail(MF_IS_REMOTE_INFO(info), NULL);
    return info->remote;
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
 * Returns: the public key that signs the repository, in OpenSSH format
 *   without its comment
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
 * Returns: the SHA256 fingerprint of the public key, as `ssh-keygen -l`
 *   shows it, e.g. `SHA256:…`
 */
const char *
mf_remote_info_get_fingerprint(MfRemoteInfo *info)
{
    g_return_val_if_fail(MF_IS_REMOTE_INFO(info), NULL);
    return info->fingerprint;
}
