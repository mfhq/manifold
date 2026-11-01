/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mf-integration-private.h"

#include <glib/gi18n-lib.h>

#include "mf-error.h"
#include "mf-json-private.h"

/**
 * MfIntegration:
 *
 * What an image asks to install on the host, as its manifest declares it.
 */

/* the key of the aliases in the integration section, which are not files */
#define ALIAS_KEY "alias"

static const char *const KIND_NAMES[MF_INTEGRATION_KIND_LAST] = {
    [MF_INTEGRATION_KIND_UNKNOWN] = "unknown",
    [MF_INTEGRATION_KIND_DESKTOP_ENTRY] = "desktop-entry",
    [MF_INTEGRATION_KIND_ICON] = "icons",
    [MF_INTEGRATION_KIND_MIME_PACKAGE] = "mime-package",
    [MF_INTEGRATION_KIND_SYSTEMD_UNIT_SYSTEM] = "systemd-unit-system",
    [MF_INTEGRATION_KIND_SYSTEMD_UNIT_USER] = "systemd-unit-user",
    [MF_INTEGRATION_KIND_UDEV_RULE] = "udev-rule",
};

/**
 * mf_integration_kind_to_string:
 * @kind: a #MfIntegrationKind
 *
 * Returns: The name of the kind, which is its key in the manifest.
 */
const char *
mf_integration_kind_to_string(MfIntegrationKind kind)
{
    if (kind <= MF_INTEGRATION_KIND_UNKNOWN || kind >= MF_INTEGRATION_KIND_LAST)
        return KIND_NAMES[MF_INTEGRATION_KIND_UNKNOWN];
    return KIND_NAMES[kind];
}

/**
 * mf_integration_kind_from_string:
 * @kind_str: (nullable): the name of an integration kind
 *
 * Returns: The kind, or %MF_INTEGRATION_KIND_UNKNOWN if the name is not known
 */
MfIntegrationKind
mf_integration_kind_from_string(const char *kind_str)
{
    for (MfIntegrationKind kind = MF_INTEGRATION_KIND_UNKNOWN + 1; kind < MF_INTEGRATION_KIND_LAST; kind++) {
        if (g_strcmp0(kind_str, KIND_NAMES[kind]) == 0)
            return kind;
    }

    return MF_INTEGRATION_KIND_UNKNOWN;
}

/**
 * MfIntegrationFile:
 *
 * A file of an image to install on the host.
 */
struct _MfIntegrationFile {
    GObject parent_instance;

    MfIntegrationKind kind;
    char *path; /* the absolute path in the image */
    char *checksums[MF_CHECKSUM_KIND_LAST];
};

G_DEFINE_FINAL_TYPE(MfIntegrationFile, mf_integration_file, G_TYPE_OBJECT)

static void
mf_integration_file_finalize(GObject *object)
{
    MfIntegrationFile *file = MF_INTEGRATION_FILE(object);

    g_free(file->path);
    for (guint i = 0; i < MF_CHECKSUM_KIND_LAST; i++)
        g_free(file->checksums[i]);

    G_OBJECT_CLASS(mf_integration_file_parent_class)->finalize(object);
}

static void
mf_integration_file_class_init(MfIntegrationFileClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = mf_integration_file_finalize;
}

static void
mf_integration_file_init(MfIntegrationFile *file)
{
}

/**
 * mf_integration_file_get_kind:
 * @file: an #MfIntegrationFile
 *
 * Returns: what kind of file this is
 */
MfIntegrationKind
mf_integration_file_get_kind(MfIntegrationFile *file)
{
    g_return_val_if_fail(MF_IS_INTEGRATION_FILE(file), MF_INTEGRATION_KIND_UNKNOWN);
    return file->kind;
}

/**
 * mf_integration_file_get_path:
 * @file: an #MfIntegrationFile
 *
 * Returns: the absolute path of the file in the image
 */
const char *
mf_integration_file_get_path(MfIntegrationFile *file)
{
    g_return_val_if_fail(MF_IS_INTEGRATION_FILE(file), NULL);
    return file->path;
}

/**
 * mf_integration_file_get_checksum:
 * @file: an #MfIntegrationFile
 * @kind: which checksum to return
 *
 * Returns: (nullable): the hex encoded checksum of the file in the image
 */
const char *
mf_integration_file_get_checksum(MfIntegrationFile *file, MfChecksumKind kind)
{
    g_return_val_if_fail(MF_IS_INTEGRATION_FILE(file), NULL);
    g_return_val_if_fail(kind > MF_CHECKSUM_KIND_UNKNOWN && kind < MF_CHECKSUM_KIND_LAST, NULL);
    return file->checksums[kind];
}

/**
 * MfIntegrationAlias:
 *
 * A command to add to the PATH of the host, which runs a program of the image.
 */
struct _MfIntegrationAlias {
    GObject parent_instance;

    char *name;
    char *command; /* the absolute path of the program in the image */
};

G_DEFINE_FINAL_TYPE(MfIntegrationAlias, mf_integration_alias, G_TYPE_OBJECT)

static void
mf_integration_alias_finalize(GObject *object)
{
    MfIntegrationAlias *alias = MF_INTEGRATION_ALIAS(object);

    g_free(alias->name);
    g_free(alias->command);

    G_OBJECT_CLASS(mf_integration_alias_parent_class)->finalize(object);
}

static void
mf_integration_alias_class_init(MfIntegrationAliasClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = mf_integration_alias_finalize;
}

static void
mf_integration_alias_init(MfIntegrationAlias *alias)
{
}

/**
 * mf_integration_alias_get_name:
 * @alias: an #MfIntegrationAlias
 *
 * Returns: the name of the command on the host
 */
const char *
mf_integration_alias_get_name(MfIntegrationAlias *alias)
{
    g_return_val_if_fail(MF_IS_INTEGRATION_ALIAS(alias), NULL);
    return alias->name;
}

/**
 * mf_integration_alias_get_command:
 * @alias: an #MfIntegrationAlias
 *
 * Returns: the absolute path of the program it runs in the image
 */
const char *
mf_integration_alias_get_command(MfIntegrationAlias *alias)
{
    g_return_val_if_fail(MF_IS_INTEGRATION_ALIAS(alias), NULL);
    return alias->command;
}

struct _MfIntegration {
    GObject parent_instance;

    GPtrArray *files;         /* (element-type MfIntegrationFile) */
    GPtrArray *aliases;       /* (element-type MfIntegrationAlias) */
    GPtrArray *unknown_kinds; /* (element-type utf8) */
};

G_DEFINE_FINAL_TYPE(MfIntegration, mf_integration, G_TYPE_OBJECT)

static void
mf_integration_finalize(GObject *object)
{
    MfIntegration *integration = MF_INTEGRATION(object);

    g_ptr_array_unref(integration->files);
    g_ptr_array_unref(integration->aliases);
    g_ptr_array_unref(integration->unknown_kinds);

    G_OBJECT_CLASS(mf_integration_parent_class)->finalize(object);
}

static void
mf_integration_class_init(MfIntegrationClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = mf_integration_finalize;
}

static void
mf_integration_init(MfIntegration *integration)
{
    integration->files = g_ptr_array_new_with_free_func(g_object_unref);
    integration->aliases = g_ptr_array_new_with_free_func(g_object_unref);
    integration->unknown_kinds = g_ptr_array_new_with_free_func(g_free);
}

/**
 * mf_integration_get_files:
 * @integration: an #MfIntegration
 *
 * Returns: (transfer none) (element-type MfIntegrationFile): the files to install
 */
GPtrArray *
mf_integration_get_files(MfIntegration *integration)
{
    g_return_val_if_fail(MF_IS_INTEGRATION(integration), NULL);
    return integration->files;
}

/**
 * mf_integration_get_aliases:
 * @integration: an #MfIntegration
 *
 * Returns: (transfer none) (element-type MfIntegrationAlias): the aliases to install
 */
GPtrArray *
mf_integration_get_aliases(MfIntegration *integration)
{
    g_return_val_if_fail(MF_IS_INTEGRATION(integration), NULL);
    return integration->aliases;
}

/**
 * mf_integration_get_unknown_kinds:
 * @integration: an #MfIntegration
 *
 * Returns: (transfer none) (element-type utf8): the kinds the manifest uses that we do not know
 */
GPtrArray *
mf_integration_get_unknown_kinds(MfIntegration *integration)
{
    g_return_val_if_fail(MF_IS_INTEGRATION(integration), NULL);
    return integration->unknown_kinds;
}

/**
 * mf_integration_check_is_list:
 * @context: what @value is, for the message
 *
 * Returns: %TRUE if @value is a JSON array
 */
static gboolean
mf_integration_check_is_list(sd_json_variant *value, const char *context, GError **error)
{
    if (sd_json_variant_is_array(value))
        return TRUE;

    /* TRANSLATORS: %s names a section of the manifest, which is not translated */
    g_set_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST, _("%s is not a list"), context);
    return FALSE;
}

/**
 * mf_integration_parse_files:
 * @entries: the value of @kind in the integration section
 *
 * Add the files of one kind.
 */
static gboolean
mf_integration_parse_files(MfIntegration *integration,
                           MfIntegrationKind kind,
                           sd_json_variant *entries,
                           GError **error)
{
    g_autofree char *context = g_strconcat("integration: ", mf_integration_kind_to_string(kind), NULL);

    if (!mf_integration_check_is_list(entries, context, error))
        return FALSE;

    for (size_t i = 0; i < sd_json_variant_elements(entries); i++) {
        sd_json_variant *entry = sd_json_variant_by_index(entries, i);
        g_autoptr(MfIntegrationFile) file = g_object_new(MF_TYPE_INTEGRATION_FILE, NULL);

        file->kind = kind;
        file->path = g_strdup(
            mf_json_get_string_field(entry, context, "path", MF_ERROR_INVALID_MANIFEST, error));
        if (file->path == NULL)
            return FALSE;
        for (MfChecksumKind checksum = MF_CHECKSUM_KIND_UNKNOWN + 1; checksum < MF_CHECKSUM_KIND_LAST;
             checksum++) {
            file->checksums[checksum] = g_strdup(mf_json_get_hash_field(entry,
                                                                        context,
                                                                        mf_checksum_kind_to_string(checksum),
                                                                        MF_ERROR_INVALID_MANIFEST,
                                                                        error));
            if (file->checksums[checksum] == NULL)
                return FALSE;
        }
        g_ptr_array_add(integration->files, g_steal_pointer(&file));
    }

    return TRUE;
}

/**
 * mf_integration_parse_aliases:
 * @entries: the value of the alias key in the integration section
 *
 * Add the aliases.
 */
static gboolean
mf_integration_parse_aliases(MfIntegration *integration, sd_json_variant *entries, GError **error)
{
    const char *context = "integration: " ALIAS_KEY;

    if (!mf_integration_check_is_list(entries, context, error))
        return FALSE;

    for (size_t i = 0; i < sd_json_variant_elements(entries); i++) {
        sd_json_variant *entry = sd_json_variant_by_index(entries, i);
        g_autoptr(MfIntegrationAlias) alias = g_object_new(MF_TYPE_INTEGRATION_ALIAS, NULL);

        alias->name = g_strdup(
            mf_json_get_string_field(entry, context, "name", MF_ERROR_INVALID_MANIFEST, error));
        if (alias->name == NULL)
            return FALSE;
        alias->command = g_strdup(
            mf_json_get_string_field(entry, context, "command", MF_ERROR_INVALID_MANIFEST, error));
        if (alias->command == NULL)
            return FALSE;
        g_ptr_array_add(integration->aliases, g_steal_pointer(&alias));
    }

    return TRUE;
}

/**
 * mf_integration_new_from_json:
 * @section: the integration section of a manifest
 * @error: a #GError
 *
 * Read the integration section of a manifest. We only check its structure
 * here, and not what it asks for: a manifest we can not parse hides the
 * version it belongs to from the store, so the rules for what may be installed
 * are checked when installing. That is also where kinds we do not know are
 * refused, so we keep their names and do not look at their values.
 *
 * Returns: (transfer full): the section, or %NULL with %MF_ERROR_INVALID_MANIFEST
 */
MfIntegration *
mf_integration_new_from_json(sd_json_variant *section, GError **error)
{
    g_autoptr(MfIntegration) integration = NULL;

    if (!sd_json_variant_is_object(section)) {
        /* TRANSLATORS: "integration" is a section of the manifest, and not translated */
        g_set_error_literal(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST, _("integration is not an object"));
        return NULL;
    }

    integration = g_object_new(MF_TYPE_INTEGRATION, NULL);
    for (size_t i = 0; i < sd_json_variant_elements(section); i += 2) {
        const char *key = sd_json_variant_string(sd_json_variant_by_index(section, i));
        sd_json_variant *value = sd_json_variant_by_index(section, i + 1);
        MfIntegrationKind kind = mf_integration_kind_from_string(key);

        if (g_str_equal(key, ALIAS_KEY)) {
            if (!mf_integration_parse_aliases(integration, value, error))
                return NULL;
        } else if (kind != MF_INTEGRATION_KIND_UNKNOWN) {
            if (!mf_integration_parse_files(integration, kind, value, error))
                return NULL;
        } else {
            g_ptr_array_add(integration->unknown_kinds, g_strdup(key));
        }
    }

    return g_steal_pointer(&integration);
}
