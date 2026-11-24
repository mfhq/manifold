/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mf-error.h"
#include "mf-error-private.h"

/* the fields of an #MfError, see #MfErrorField */
typedef struct {
    char *fields[MF_ERROR_FIELD_LAST];
} MfErrorPrivate;

static void
mf_error_private_init(MfErrorPrivate *priv)
{
}

static void
mf_error_private_copy(const MfErrorPrivate *src_priv, MfErrorPrivate *dest_priv)
{
    for (guint i = 0; i < MF_ERROR_FIELD_LAST; i++)
        dest_priv->fields[i] = g_strdup(src_priv->fields[i]);
}

static void
mf_error_private_clear(MfErrorPrivate *priv)
{
    for (guint i = 0; i < MF_ERROR_FIELD_LAST; i++)
        g_clear_pointer(&priv->fields[i], g_free);
}

/* NOTE: An extended error domain stores private data in every #GError of the domain.
 * GLib copies and frees the data together with the error. */
G_DEFINE_EXTENDED_ERROR(MfError, mf_error)

/**
 * mf_error_set_field:
 * @error: (nullable): the location of an #MfError, as it is passed to g_set_error()
 * @field: the field to set
 * @value: (nullable): the value of the field
 *
 * Set a field of an error. Nothing happens if no error is set at @error,
 * so this function can be called right after g_set_error().
 */
void
mf_error_set_field(GError **error, MfErrorField field, const char *value)
{
    MfErrorPrivate *priv;

    g_return_if_fail(field < MF_ERROR_FIELD_LAST);

    if (error == NULL || *error == NULL)
        return;
    g_return_if_fail((*error)->domain == MF_ERROR);

    priv = mf_error_get_private(*error);
    if (priv == NULL)
        return;
    g_free(priv->fields[field]);
    priv->fields[field] = g_strdup(value);
}

/**
 * mf_error_get_field:
 * @error: a #GError
 * @field: the field to get
 *
 * Returns: (nullable): the value of the field, or %NULL if the field is not
 *   set or @error is not an #MfError
 */
const char *
mf_error_get_field(const GError *error, MfErrorField field)
{
    MfErrorPrivate *priv;

    g_return_val_if_fail(error != NULL, NULL);
    g_return_val_if_fail(field < MF_ERROR_FIELD_LAST, NULL);

    if (error->domain != MF_ERROR)
        return NULL;
    priv = mf_error_get_private(error);
    return priv != NULL ? priv->fields[field] : NULL;
}

/**
 * mf_error_set_field_boolean:
 * @error: (nullable): the location of an #MfError, as it is passed to g_set_error()
 * @field: the field to set, which has to be a boolean field
 *
 * Set a boolean field of an error.
 */
void
mf_error_set_field_boolean(GError **error, MfErrorField field, gboolean value)
{
    /* fields are stored as strings */
    mf_error_set_field(error, field, value ? "true" : "false");
}

/**
 * mf_error_get_field_boolean:
 * @error: a #GError
 * @field: the field to get, which has to be a boolean field
 *
 * Returns: the value of the field, or %FALSE if the field is not set
 */
gboolean
mf_error_get_field_boolean(const GError *error, MfErrorField field)
{
    return g_strcmp0(mf_error_get_field(error, field), "true") == 0;
}
