/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mf-result-private.h"

#include <glib/gi18n-lib.h>

#include "mf-component-private.h"
#include "mf-error.h"

/**
 * MfUpdate:
 *
 * A component that an operation updated. A newer version was installed, and
 * the superseded versions were removed.
 */
struct _MfUpdate {
    MfComponent *component;
    char *previous_version;
    GPtrArray *integration_added;   /* of char* */
    GPtrArray *integration_removed; /* of char* */
};

G_DEFINE_BOXED_TYPE(MfUpdate, mf_update, mf_update_copy, mf_update_free)

/**
 * mf_update_new:
 * @component: the component, in the version that was installed
 * @previous_version: the full version that was current before the update
 *
 * Returns: (transfer full): a new #MfUpdate
 */
MfUpdate *
mf_update_new(MfComponent *component, const char *previous_version)
{
    MfUpdate *update;

    g_return_val_if_fail(MF_IS_COMPONENT(component), NULL);
    g_return_val_if_fail(previous_version != NULL, NULL);

    update = g_new0(MfUpdate, 1);
    update->component = g_object_ref(component);
    update->previous_version = g_strdup(previous_version);
    update->integration_added = g_ptr_array_new_with_free_func(g_free);
    update->integration_removed = g_ptr_array_new_with_free_func(g_free);

    return update;
}

/**
 * mf_update_copy:
 * @update: an #MfUpdate
 *
 * Returns: (transfer full): a copy of the update
 */
MfUpdate *
mf_update_copy(const MfUpdate *update)
{
    MfUpdate *copy;

    g_return_val_if_fail(update != NULL, NULL);

    copy = mf_update_new(update->component, update->previous_version);
    for (guint i = 0; i < update->integration_added->len; i++)
        mf_update_add_integration_added(copy, g_ptr_array_index(update->integration_added, i));
    for (guint i = 0; i < update->integration_removed->len; i++)
        mf_update_add_integration_removed(copy, g_ptr_array_index(update->integration_removed, i));

    return copy;
}

/**
 * mf_update_free:
 * @update: (nullable): an #MfUpdate
 */
void
mf_update_free(MfUpdate *update)
{
    if (update == NULL)
        return;

    g_object_unref(update->component);
    g_free(update->previous_version);
    g_ptr_array_unref(update->integration_added);
    g_ptr_array_unref(update->integration_removed);
    g_free(update);
}

/**
 * mf_update_get_component:
 * @update: an #MfUpdate
 *
 * Returns: (transfer none): the component, in the version that was installed
 */
MfComponent *
mf_update_get_component(const MfUpdate *update)
{
    g_return_val_if_fail(update != NULL, NULL);
    return update->component;
}

/**
 * mf_update_get_previous_version:
 * @update: an #MfUpdate
 *
 * Returns: the version that was current before the update, with its
 *   revision: `<version>-<rev>`
 */
const char *
mf_update_get_previous_version(const MfUpdate *update)
{
    g_return_val_if_fail(update != NULL, NULL);
    return update->previous_version;
}

/**
 * mf_update_get_integration_added:
 * @update: an #MfUpdate
 *
 * Returns: (transfer none) (element-type utf8): the system services and udev
 *   rules that the new version installs on the host, and that the previous
 *   version did not install
 */
GPtrArray *
mf_update_get_integration_added(const MfUpdate *update)
{
    g_return_val_if_fail(update != NULL, NULL);
    return update->integration_added;
}

/**
 * mf_update_get_integration_removed:
 * @update: an #MfUpdate
 *
 * Returns: (transfer none) (element-type utf8): the system services and udev
 *   rules that the previous version installed on the host, and that the new
 *   version does not install anymore
 */
GPtrArray *
mf_update_get_integration_removed(const MfUpdate *update)
{
    g_return_val_if_fail(update != NULL, NULL);
    return update->integration_removed;
}

/**
 * mf_update_add_integration_added:
 * @update: an #MfUpdate
 * @name: a file that the new version installs on the host, and the previous version did not
 */
void
mf_update_add_integration_added(MfUpdate *update, const char *name)
{
    g_return_if_fail(update != NULL);
    g_return_if_fail(name != NULL);
    g_ptr_array_add(update->integration_added, g_strdup(name));
}

/**
 * mf_update_add_integration_removed:
 * @update: an #MfUpdate
 * @name: a file that the previous version installed on the host, and the new version does not
 */
void
mf_update_add_integration_removed(MfUpdate *update, const char *name)
{
    g_return_if_fail(update != NULL);
    g_return_if_fail(name != NULL);
    g_ptr_array_add(update->integration_removed, g_strdup(name));
}

/**
 * MfResult:
 *
 * Contains results of a change operation.
 */

typedef struct {
    GPtrArray *updated;        /* of MfUpdate */
    GPtrArray *installed;      /* of MfComponent */
    GPtrArray *removed;        /* of MfComponent */
    GPtrArray *removed_images; /* of char*, root hashes */
    uint64_t bytes_added;
    uint64_t bytes_freed;
    GPtrArray *warnings; /* of char*, messages about problems that did not fail the operation */
} MfResultPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(MfResult, mf_result, G_TYPE_OBJECT)

#define GET_PRIVATE(o) ((MfResultPrivate *) mf_result_get_instance_private(o))

static void
mf_result_finalize(GObject *object)
{
    MfResultPrivate *priv = GET_PRIVATE(MF_RESULT(object));

    g_clear_pointer(&priv->updated, g_ptr_array_unref);
    g_clear_pointer(&priv->installed, g_ptr_array_unref);
    g_clear_pointer(&priv->removed, g_ptr_array_unref);
    g_clear_pointer(&priv->removed_images, g_ptr_array_unref);
    g_clear_pointer(&priv->warnings, g_ptr_array_unref);

    G_OBJECT_CLASS(mf_result_parent_class)->finalize(object);
}

static void
mf_result_class_init(MfResultClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = mf_result_finalize;
}

static void
mf_result_init(MfResult *result)
{
    MfResultPrivate *priv = GET_PRIVATE(result);

    priv->updated = g_ptr_array_new_with_free_func((GDestroyNotify) mf_update_free);
    priv->installed = g_ptr_array_new_with_free_func(g_object_unref);
    priv->removed = g_ptr_array_new_with_free_func(g_object_unref);
    priv->removed_images = g_ptr_array_new_with_free_func(g_free);
    priv->warnings = g_ptr_array_new_with_free_func(g_free);
}

/**
 * mf_result_get_updated:
 * @result: an #MfResult
 *
 * Returns: (transfer none) (element-type MfUpdate): the components that were
 *   updated. They are not among the installed and removed components.
 */
GPtrArray *
mf_result_get_updated(MfResult *result)
{
    g_return_val_if_fail(MF_IS_RESULT(result), NULL);
    auto priv = GET_PRIVATE(result);
    return priv->updated;
}

/**
 * mf_result_get_installed:
 * @result: an #MfResult
 *
 * Returns: (transfer none) (element-type MfComponent): the components that
 *   were installed, and that had no version installed before
 */
GPtrArray *
mf_result_get_installed(MfResult *result)
{
    g_return_val_if_fail(MF_IS_RESULT(result), NULL);
    auto priv = GET_PRIVATE(result);
    return priv->installed;
}

/**
 * mf_result_get_removed:
 * @result: an #MfResult
 *
 * Returns: (transfer none) (element-type MfComponent): the components that were removed
 */
GPtrArray *
mf_result_get_removed(MfResult *result)
{
    g_return_val_if_fail(MF_IS_RESULT(result), NULL);
    auto priv = GET_PRIVATE(result);
    return priv->removed;
}

/**
 * mf_result_get_removed_images:
 * @result: an #MfResult
 *
 * Returns: (transfer none) (element-type utf8): the root hashes of the images that were removed
 */
GPtrArray *
mf_result_get_removed_images(MfResult *result)
{
    g_return_val_if_fail(MF_IS_RESULT(result), NULL);
    auto priv = GET_PRIVATE(result);
    return priv->removed_images;
}

/**
 * mf_result_get_bytes_added:
 * @result: an #MfResult
 *
 * Returns: the disk space that the operation used, in bytes
 */
uint64_t
mf_result_get_bytes_added(MfResult *result)
{
    g_return_val_if_fail(MF_IS_RESULT(result), 0);
    auto priv = GET_PRIVATE(result);
    return priv->bytes_added;
}

/**
 * mf_result_get_bytes_freed:
 * @result: an #MfResult
 *
 * Returns: the disk space that the operation freed, in bytes
 */
uint64_t
mf_result_get_bytes_freed(MfResult *result)
{
    g_return_val_if_fail(MF_IS_RESULT(result), 0);
    auto priv = GET_PRIVATE(result);
    return priv->bytes_freed;
}

/**
 * mf_result_get_warnings:
 * @result: an #MfResult
 *
 * Returns: (transfer none) (element-type utf8): messages about problems that
 *   did not fail the operation, like a tool of the host that failed
 */
GPtrArray *
mf_result_get_warnings(MfResult *result)
{
    g_return_val_if_fail(MF_IS_RESULT(result), NULL);
    auto priv = GET_PRIVATE(result);
    return priv->warnings;
}

/**
 * mf_result_new:
 *
 * Creates a new, empty #MfResult.
 *
 * Returns: (transfer full): a new #MfResult
 */
MfResult *
mf_result_new(void)
{
    return g_object_new(MF_TYPE_RESULT, NULL);
}

/**
 * mf_result_add_updated:
 * @result: an #MfResult
 * @update: (transfer none): a component that was updated
 *
 * Add a component to the list of updated components.
 */
void
mf_result_add_updated(MfResult *result, const MfUpdate *update)
{
    g_return_if_fail(MF_IS_RESULT(result));
    g_return_if_fail(update != NULL);
    auto priv = GET_PRIVATE(result);
    g_ptr_array_add(priv->updated, mf_update_copy(update));
}

/**
 * mf_result_add_installed:
 * @result: an #MfResult
 * @component: (transfer none): a component that was installed
 *
 * Add a component to the list of installed components.
 */
void
mf_result_add_installed(MfResult *result, MfComponent *component)
{
    g_return_if_fail(MF_IS_RESULT(result));
    g_return_if_fail(MF_IS_COMPONENT(component));
    auto priv = GET_PRIVATE(result);
    g_ptr_array_add(priv->installed, g_object_ref(component));
}

/**
 * mf_result_add_removed:
 * @result: an #MfResult
 * @component: (transfer none): a component that was removed
 *
 * Add a component to the list of removed components.
 */
void
mf_result_add_removed(MfResult *result, MfComponent *component)
{
    g_return_if_fail(MF_IS_RESULT(result));
    g_return_if_fail(MF_IS_COMPONENT(component));
    auto priv = GET_PRIVATE(result);
    g_ptr_array_add(priv->removed, g_object_ref(component));
}

/**
 * mf_result_add_removed_image:
 * @result: an #MfResult
 * @verity_hash: the root hash of an image that was removed
 */
void
mf_result_add_removed_image(MfResult *result, const char *verity_hash)
{
    g_return_if_fail(MF_IS_RESULT(result));
    auto priv = GET_PRIVATE(result);
    g_ptr_array_add(priv->removed_images, g_strdup(verity_hash));
}

/**
 * mf_result_add_warning:
 * @result: an #MfResult
 * @message: a message about a problem that did not fail the operation
 *
 * Add a warning.
 */
void
mf_result_add_warning(MfResult *result, const char *message)
{
    g_return_if_fail(MF_IS_RESULT(result));
    g_return_if_fail(message != NULL);

    auto priv = GET_PRIVATE(result);
    g_ptr_array_add(priv->warnings, g_strdup(message));
}

/**
 * mf_result_set_bytes_added:
 * @result: an #MfResult
 * @bytes: the disk space that the operation used, in bytes
 *
 * Set the number of bytes that the operation added to the store.
 */
void
mf_result_set_bytes_added(MfResult *result, uint64_t bytes)
{
    g_return_if_fail(MF_IS_RESULT(result));
    auto priv = GET_PRIVATE(result);
    priv->bytes_added = bytes;
}

/**
 * mf_result_set_bytes_freed:
 * @result: an #MfResult
 * @bytes: the disk space that the operation freed, in bytes
 *
 * Set the number of bytes that the operation freed in the store.
 */
void
mf_result_set_bytes_freed(MfResult *result, uint64_t bytes)
{
    g_return_if_fail(MF_IS_RESULT(result));
    auto priv = GET_PRIVATE(result);
    priv->bytes_freed = bytes;
}

/**
 * mf_result_read_strings:
 * @array: (nullable): a JSON array from a reply
 * @strings: (element-type utf8): the array to add the entries of @array to
 *
 * Returns: %TRUE if @array only contains strings
 */
static gboolean
mf_result_read_strings(sd_json_variant *array, GPtrArray *strings, GError **error)
{
    for (size_t i = 0; array != NULL && i < sd_json_variant_elements(array); i++) {
        sd_json_variant *entry = sd_json_variant_by_index(array, i);

        if (!sd_json_variant_is_string(entry)) {
            g_set_error_literal(
                error,
                MF_ERROR,
                MF_ERROR_FAILED,
                _("Reply of the Manifold service was not understood: a list has an entry that is not a string"));
            return FALSE;
        }
        g_ptr_array_add(strings, g_strdup(sd_json_variant_string(entry)));
    }

    return TRUE;
}

/**
 * mf_update_new_from_reply:
 * @variant: the description of an update, from a reply of the service
 *
 * Returns: (transfer full): the update, or %NULL if the reply is not valid
 */
static MfUpdate *
mf_update_new_from_reply(sd_json_variant *variant, GError **error)
{
    g_autoptr(MfUpdate) update = NULL;
    g_autoptr(MfComponent) component = NULL;
    sd_json_variant *previous_version = sd_json_variant_by_key(variant, "previousVersion");

    if (!sd_json_variant_is_object(variant) || !sd_json_variant_is_string(previous_version)) {
        g_set_error_literal(
            error,
            MF_ERROR,
            MF_ERROR_FAILED,
            _("Reply of the Manifold service was not understood: an update has no previous version"));
        return NULL;
    }
    component = mf_component_new_from_reply(sd_json_variant_by_key(variant, "component"), error);
    if (component == NULL)
        return NULL;

    update = mf_update_new(component, sd_json_variant_string(previous_version));
    if (!mf_result_read_strings(sd_json_variant_by_key(variant, "integrationAdded"),
                                update->integration_added,
                                error))
        return NULL;
    if (!mf_result_read_strings(sd_json_variant_by_key(variant, "integrationRemoved"),
                                update->integration_removed,
                                error))
        return NULL;

    return g_steal_pointer(&update);
}

/**
 * mf_result_new_from_reply:
 * @reply: the final reply of an operation
 *
 * Returns: (transfer full): The new #MfResult, or %NULL on error.
 */
MfResult *
mf_result_new_from_reply(sd_json_variant *reply, GError **error)
{
    struct {
        uint64_t bytes_added;
        uint64_t bytes_freed;
    } fields = {};
    static const sd_json_dispatch_field fields_table[] = {
        {"bytesAdded",
         _SD_JSON_VARIANT_TYPE_INVALID, sd_json_dispatch_uint64,
         offsetof(typeof(fields), bytes_added),
         0},
        {"bytesFreed",
         _SD_JSON_VARIANT_TYPE_INVALID, sd_json_dispatch_uint64,
         offsetof(typeof(fields), bytes_freed),
         0},
        {},
    };
    g_autoptr(MfResult) result = NULL;
    sd_json_variant *object = sd_json_variant_by_key(reply, "result");
    GPtrArray *components;
    int r;

    r = sd_json_variant_is_object(object)
            ? sd_json_dispatch(object, fields_table, SD_JSON_ALLOW_EXTENSIONS, &fields)
            : -EBADMSG;
    if (r < 0) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    _("Reply of the Manifold service was not understood: %s"),
                    g_strerror(-r));
        return NULL;
    }

    result = mf_result_new();
    MfResultPrivate *priv = GET_PRIVATE(result);
    priv->bytes_added = fields.bytes_added;
    priv->bytes_freed = fields.bytes_freed;

    sd_json_variant *updated = sd_json_variant_by_key(object, "updated");
    for (size_t i = 0; updated != NULL && i < sd_json_variant_elements(updated); i++) {
        MfUpdate *update = mf_update_new_from_reply(sd_json_variant_by_index(updated, i), error);

        if (update == NULL)
            return NULL;
        g_ptr_array_add(priv->updated, update);
    }

    components = mf_component_array_from_reply(sd_json_variant_by_key(object, "installed"), error);
    if (components == NULL)
        return NULL;
    g_ptr_array_unref(priv->installed);
    priv->installed = components;
    components = mf_component_array_from_reply(sd_json_variant_by_key(object, "removed"), error);
    if (components == NULL)
        return NULL;
    g_ptr_array_unref(priv->removed);
    priv->removed = components;

    if (!mf_result_read_strings(sd_json_variant_by_key(object, "removedImages"), priv->removed_images, error))
        return NULL;
    if (!mf_result_read_strings(sd_json_variant_by_key(object, "warnings"), priv->warnings, error))
        return NULL;

    return g_steal_pointer(&result);
}
