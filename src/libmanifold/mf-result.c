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
 * MfResult:
 *
 * Contains results of a change operation.
 */

typedef struct {
    GPtrArray *installed;      /* of MfComponent */
    GPtrArray *removed;        /* of MfComponent */
    GPtrArray *removed_images; /* of char*, root hashes */
    guint64 bytes_added;
    guint64 bytes_freed;
    GPtrArray *warnings; /* of char*, what went wrong without failing the operation */
} MfResultPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(MfResult, mf_result, G_TYPE_OBJECT)

#define GET_PRIVATE(o) ((MfResultPrivate *) mf_result_get_instance_private(o))

static void
mf_result_finalize(GObject *object)
{
    MfResultPrivate *priv = GET_PRIVATE(MF_RESULT(object));

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

    priv->installed = g_ptr_array_new_with_free_func(g_object_unref);
    priv->removed = g_ptr_array_new_with_free_func(g_object_unref);
    priv->removed_images = g_ptr_array_new_with_free_func(g_free);
    priv->warnings = g_ptr_array_new_with_free_func(g_free);
}

/**
 * mf_result_get_installed:
 * @result: an #MfResult
 *
 * Returns: (transfer none) (element-type MfComponent): the components that were installed
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
 * Returns: bytes of disk space the operation took up
 */
guint64
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
 * Returns: bytes of disk space the operation freed
 */
guint64
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
 * Returns: (transfer none) (element-type utf8): what went wrong without
 *   failing the operation, like a tool of the host that failed
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
 * mf_result_add_installed:
 * @result: an #MfResult
 * @component: (transfer none): a component that was installed
 *
 * Add a component to the ones the operation installed.
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
 * Add a component to the ones the operation removed.
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
 * @message: what went wrong without failing the operation
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
 * @bytes: how much disk space the operation took up
 *
 * Set how many bytes the operation added to the store.
 */
void
mf_result_set_bytes_added(MfResult *result, guint64 bytes)
{
    g_return_if_fail(MF_IS_RESULT(result));
    auto priv = GET_PRIVATE(result);
    priv->bytes_added = bytes;
}

/**
 * mf_result_set_bytes_freed:
 * @result: an #MfResult
 * @bytes: how much disk space the operation freed
 *
 * Set how many bytes the operation freed in the store.
 */
void
mf_result_set_bytes_freed(MfResult *result, guint64 bytes)
{
    g_return_if_fail(MF_IS_RESULT(result));
    auto priv = GET_PRIVATE(result);
    priv->bytes_freed = bytes;
}

/**
 * mf_result_read_strings:
 * @array: (nullable): a list of a reply
 * @strings: (element-type utf8): where to add the entries of the list
 *
 * Returns: %TRUE if the list holds nothing but strings
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
