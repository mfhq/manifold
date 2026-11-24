/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mf-progress-private.h"

/**
 * MfItemProgress:
 *
 * Progress of one item of an operation.
 */
struct _MfItemProgress {
    MfAction action;
    char *subject;
    char *version;
    double percent;
    uint64_t bytes_done;
    uint64_t bytes_total;
};

G_DEFINE_BOXED_TYPE(MfItemProgress, mf_item_progress, mf_item_progress_copy, mf_item_progress_free)

/**
 * mf_action_to_string:
 * @action: a #MfAction
 *
 * Returns: The name of the action.
 */
const char *
mf_action_to_string(MfAction action)
{
    switch (action) {
    case MF_ACTION_INSTALL:
        return "install";
    case MF_ACTION_UPDATE:
        return "update";
    case MF_ACTION_REMOVE:
        return "remove";
    case MF_ACTION_CLEANUP:
        return "cleanup";
    case MF_ACTION_REFRESH:
        return "refresh";
    default:
        return "unknown";
    }
}

/**
 * mf_action_from_string:
 * @action_str: (nullable): the name of an action
 *
 * Returns: The action, or %MF_ACTION_UNKNOWN if the name is not known
 */
MfAction
mf_action_from_string(const char *action_str)
{
    if (g_strcmp0(action_str, "install") == 0)
        return MF_ACTION_INSTALL;
    if (g_strcmp0(action_str, "update") == 0)
        return MF_ACTION_UPDATE;
    if (g_strcmp0(action_str, "remove") == 0)
        return MF_ACTION_REMOVE;
    if (g_strcmp0(action_str, "cleanup") == 0)
        return MF_ACTION_CLEANUP;
    if (g_strcmp0(action_str, "refresh") == 0)
        return MF_ACTION_REFRESH;
    return MF_ACTION_UNKNOWN;
}

/**
 * mf_item_progress_new:
 * @action: the action that is performed on the item
 * @subject: the ID of the component that the action is performed on
 * @version: (nullable): the version of the component, with its revision
 *
 * Creates the progress of an item that was not started yet.
 *
 * Returns: (transfer full): a new #MfItemProgress
 */
MfItemProgress *
mf_item_progress_new(MfAction action, const char *subject, const char *version)
{
    MfItemProgress *item;

    g_return_val_if_fail(subject != NULL, NULL);

    item = g_new0(MfItemProgress, 1);
    item->action = action;
    item->subject = g_strdup(subject);
    item->version = g_strdup(version);

    return item;
}

/**
 * mf_item_progress_copy:
 * @item: an #MfItemProgress
 *
 * Returns: (transfer full): a copy of the item progress
 */
MfItemProgress *
mf_item_progress_copy(const MfItemProgress *item)
{
    MfItemProgress *copy;

    g_return_val_if_fail(item != NULL, NULL);

    copy = g_new0(MfItemProgress, 1);
    copy->action = item->action;
    copy->subject = g_strdup(item->subject);
    copy->version = g_strdup(item->version);
    copy->percent = item->percent;
    copy->bytes_done = item->bytes_done;
    copy->bytes_total = item->bytes_total;

    return copy;
}

/**
 * mf_item_progress_free:
 * @item: (nullable): an #MfItemProgress
 */
void
mf_item_progress_free(MfItemProgress *item)
{
    if (item == NULL)
        return;

    g_free(item->subject);
    g_free(item->version);
    g_free(item);
}

/**
 * mf_item_progress_get_action:
 * @item: an #MfItemProgress
 *
 * Returns: the action that is performed on the subject
 */
MfAction
mf_item_progress_get_action(const MfItemProgress *item)
{
    g_return_val_if_fail(item != NULL, MF_ACTION_UNKNOWN);
    return item->action;
}

/**
 * mf_item_progress_get_subject:
 * @item: an #MfItemProgress
 *
 * Returns: the identifier of the component that is acted on
 */
const char *
mf_item_progress_get_subject(const MfItemProgress *item)
{
    g_return_val_if_fail(item != NULL, NULL);
    return item->subject;
}

/**
 * mf_item_progress_get_version:
 * @item: an #MfItemProgress
 *
 * Returns: (nullable): the full version of the component, or %NULL if it is not known
 */
const char *
mf_item_progress_get_version(const MfItemProgress *item)
{
    g_return_val_if_fail(item != NULL, NULL);
    return item->version;
}

/**
 * mf_item_progress_get_percent:
 * @item: an #MfItemProgress
 *
 * Returns: the progress of the item, from 0 to 100
 */
double
mf_item_progress_get_percent(const MfItemProgress *item)
{
    g_return_val_if_fail(item != NULL, 0);
    return item->percent;
}

/**
 * mf_item_progress_get_bytes_done:
 * @item: an #MfItemProgress
 *
 * Returns: the number of bytes that were copied so far while an image is copied, otherwise 0
 */
uint64_t
mf_item_progress_get_bytes_done(const MfItemProgress *item)
{
    g_return_val_if_fail(item != NULL, 0);
    return item->bytes_done;
}

/**
 * mf_item_progress_get_bytes_total:
 * @item: an #MfItemProgress
 *
 * Returns: the size of the image in bytes while it is copied, otherwise 0
 */
uint64_t
mf_item_progress_get_bytes_total(const MfItemProgress *item)
{
    g_return_val_if_fail(item != NULL, 0);
    return item->bytes_total;
}

/**
 * mf_item_progress_set_percent:
 * @item: an #MfItemProgress
 * @percent: the progress of the item, from 0 to 100
 */
void
mf_item_progress_set_percent(MfItemProgress *item, double percent)
{
    g_return_if_fail(item != NULL);
    item->percent = percent;
}

/**
 * mf_item_progress_set_bytes_done:
 * @item: an #MfItemProgress
 * @bytes_done: the number of bytes of the image that were copied so far
 */
void
mf_item_progress_set_bytes_done(MfItemProgress *item, uint64_t bytes_done)
{
    g_return_if_fail(item != NULL);
    item->bytes_done = bytes_done;
}

/**
 * mf_item_progress_set_bytes_total:
 * @item: an #MfItemProgress
 * @bytes_total: the size of the image in bytes if the item copies one, otherwise 0
 */
void
mf_item_progress_set_bytes_total(MfItemProgress *item, uint64_t bytes_total)
{
    g_return_if_fail(item != NULL);
    item->bytes_total = bytes_total;
}

/**
 * mf_item_progress_new_from_reply:
 * @variant: the progress of an item, from a reply of the service
 *
 * Returns: (transfer full): the item progress, or %NULL if the reply was invalid
 */
static MfItemProgress *
mf_item_progress_new_from_reply(sd_json_variant *variant)
{
    struct {
        const char *action;
        const char *subject;
        const char *version;
        double percent;
        uint64_t bytes_done;
        uint64_t bytes_total;
    } fields = {};
    static const sd_json_dispatch_field fields_table[] = {
        {"action",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(fields), action),
         SD_JSON_MANDATORY},
        {"subject",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(fields), subject),
         SD_JSON_MANDATORY},
        {"version",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(fields), version),
         0},
        {"percent",
         _SD_JSON_VARIANT_TYPE_INVALID, sd_json_dispatch_double,
         offsetof(typeof(fields), percent),
         SD_JSON_MANDATORY},
        {"bytesDone",
         _SD_JSON_VARIANT_TYPE_INVALID, sd_json_dispatch_uint64,
         offsetof(typeof(fields), bytes_done),
         0},
        {"bytesTotal",
         _SD_JSON_VARIANT_TYPE_INVALID, sd_json_dispatch_uint64,
         offsetof(typeof(fields), bytes_total),
         0},
        {},
    };
    MfItemProgress *item;

    if (sd_json_dispatch(variant, fields_table, SD_JSON_ALLOW_EXTENSIONS, &fields) < 0)
        return NULL;

    item = g_new0(MfItemProgress, 1);
    item->action = mf_action_from_string(fields.action);
    item->subject = g_strdup(fields.subject);
    item->version = g_strdup(fields.version);
    item->percent = fields.percent;
    item->bytes_done = fields.bytes_done;
    item->bytes_total = fields.bytes_total;

    return item;
}

/**
 * MfProgress:
 *
 * Progress of an operation of the Manifold service. It holds the percentage
 * of the whole operation that is done, and the item that the service is
 * working on right now.
 */
struct _MfProgress {
    double percent;
    MfItemProgress *item;
};

G_DEFINE_BOXED_TYPE(MfProgress, mf_progress, mf_progress_copy, mf_progress_free)

/**
 * mf_progress_new:
 *
 * Creates the progress of an operation that has not progressed yet.
 *
 * Returns: (transfer full): a new #MfProgress
 */
MfProgress *
mf_progress_new(void)
{
    return g_new0(MfProgress, 1);
}

/**
 * mf_progress_copy:
 * @progress: an #MfProgress
 *
 * Returns: (transfer full): a copy of the progress
 */
MfProgress *
mf_progress_copy(const MfProgress *progress)
{
    MfProgress *copy;

    g_return_val_if_fail(progress != NULL, NULL);

    copy = g_new0(MfProgress, 1);
    copy->percent = progress->percent;
    copy->item = progress->item != NULL ? mf_item_progress_copy(progress->item) : NULL;

    return copy;
}

/**
 * mf_progress_free:
 * @progress: (nullable): an #MfProgress
 */
void
mf_progress_free(MfProgress *progress)
{
    if (progress == NULL)
        return;

    mf_item_progress_free(progress->item);
    g_free(progress);
}

/**
 * mf_progress_get_percent:
 * @progress: an #MfProgress
 *
 * Returns: progress of the whole operation, from 0 to 100
 */
double
mf_progress_get_percent(const MfProgress *progress)
{
    g_return_val_if_fail(progress != NULL, 0);
    return progress->percent;
}

/**
 * mf_progress_get_item:
 * @progress: an #MfProgress
 *
 * Returns: (nullable) (transfer none): the item that is currently being worked on, or %NULL
 */
const MfItemProgress *
mf_progress_get_item(const MfProgress *progress)
{
    g_return_val_if_fail(progress != NULL, NULL);
    return progress->item;
}

/**
 * mf_progress_set_percent:
 * @progress: an #MfProgress
 * @percent: progress of the whole operation, from 0 to 100
 */
void
mf_progress_set_percent(MfProgress *progress, double percent)
{
    g_return_if_fail(progress != NULL);
    progress->percent = percent;
}

/**
 * mf_progress_take_item:
 * @progress: an #MfProgress
 * @item: (nullable) (transfer full): the item that is being worked on now, or %NULL
 *
 * Set the item that is being worked on, replacing the previous one.
 * The item stays valid until it is replaced or @progress is freed.
 */
void
mf_progress_take_item(MfProgress *progress, MfItemProgress *item)
{
    g_return_if_fail(progress != NULL);

    if (progress->item == item)
        return;
    mf_item_progress_free(progress->item);
    progress->item = item;
}

/**
 * mf_progress_new_from_reply:
 * @variant: the progress of an operation, from a reply of the service
 *
 * Returns: (transfer full): the progress, or %NULL if the reply was invalid
 */
MfProgress *
mf_progress_new_from_reply(sd_json_variant *variant)
{
    struct {
        double percent;
        sd_json_variant *item;
    } fields = {};
    static const sd_json_dispatch_field fields_table[] = {
        {"percent",
         _SD_JSON_VARIANT_TYPE_INVALID, sd_json_dispatch_double,
         offsetof(typeof(fields), percent),
         SD_JSON_MANDATORY},
        {"item", SD_JSON_VARIANT_OBJECT, sd_json_dispatch_variant_noref, offsetof(typeof(fields), item), 0},
        {},
    };
    g_autoptr(MfProgress) progress = NULL;

    if (sd_json_dispatch(variant, fields_table, SD_JSON_ALLOW_EXTENSIONS, &fields) < 0)
        return NULL;

    progress = mf_progress_new();
    progress->percent = fields.percent;
    if (fields.item != NULL) {
        progress->item = mf_item_progress_new_from_reply(fields.item);
        if (progress->item == NULL)
            return NULL;
    }

    return g_steal_pointer(&progress);
}

/**
 * mf_progress_to_json:
 * @ret_variant: (out): the description of the progress
 *
 * Describe the progress of an operation for a reply of the service.
 * mf_progress_new_from_reply() reads the description.
 *
 * Returns: 0, or a negative errno
 */
int
mf_progress_to_json(const MfProgress *progress, sd_json_variant **ret_variant)
{
    const MfItemProgress *item = progress->item;

    if (item == NULL)
        return sd_json_buildo(ret_variant, SD_JSON_BUILD_PAIR_REAL("percent", progress->percent));

    return sd_json_buildo(
        ret_variant,
        SD_JSON_BUILD_PAIR_REAL("percent", progress->percent),
        SD_JSON_BUILD_PAIR(
            "item",
            SD_JSON_BUILD_OBJECT(SD_JSON_BUILD_PAIR_STRING("action", mf_action_to_string(item->action)),
                                 SD_JSON_BUILD_PAIR_STRING("subject", item->subject),
                                 SD_JSON_BUILD_PAIR_CONDITION(item->version != NULL,
                                                              "version",
                                                              SD_JSON_BUILD_STRING(item->version)),
                                 SD_JSON_BUILD_PAIR_REAL("percent", item->percent),
                                 SD_JSON_BUILD_PAIR_CONDITION(item->bytes_total > 0,
                                                              "bytesDone",
                                                              SD_JSON_BUILD_UNSIGNED(item->bytes_done)),
                                 SD_JSON_BUILD_PAIR_CONDITION(item->bytes_total > 0,
                                                              "bytesTotal",
                                                              SD_JSON_BUILD_UNSIGNED(item->bytes_total)))));
}
