/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mf-integration-private.h"

#include <string.h>
#include <glib/gi18n-lib.h>

#include "mf-error.h"
#include "mf-json-private.h"
#include "mf-utils.h"
#include "mf-utils-private.h"

/**
 * MfIntegration:
 *
 * What an image asks to install on the host, as its manifest declares it.
 */

/* the key of the aliases in the integration section, which are not files */
#define ALIAS_KEY "alias"

/* the directory below the exports that holds the scripts of aliases */
#define ALIAS_DIR "bin"

/* the directory in an image that holds everything the image can export */
#define APP_DIR "/app/"

/* the directory of an image that holds the default program of an application */
#define APP_BIN_DIR APP_DIR "bin/"

/* the first group of a desktop entry, which describes the application */
#define DESKTOP_MAIN_GROUP "[Desktop Entry]"

/* the characters of a key name in desktop entries and systemd units */
#define KEY_CHARS "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-"

/* the characters of the language of a desktop entry key, like de_DE.UTF-8@euro */
#define LOCALE_CHARS "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.@-"

static const char *const UNIT_SUFFIXES[] = {".service", ".socket", ".timer", ".path", ".target", NULL};

static const struct {
    const char *name;            /* the key in the manifest */
    const char *dir;             /* where the files are below /app, and below the prefix on the host */
    const char *const *suffixes; /* what the names of the files may end in, %NULL terminated */
    gboolean privileged;         /* whether a file of this kind can give an image root access to the host */
} KINDS[MF_INTEGRATION_KIND_LAST] = {
    [MF_INTEGRATION_KIND_UNKNOWN] = {.name = "unknown"},
    [MF_INTEGRATION_KIND_DESKTOP_ENTRY] = {"desktop-entry",
                                     "share/applications", (const char *const[]){".desktop", NULL}},
    [MF_INTEGRATION_KIND_ICON] = {"icons",
                                     "share/icons/hicolor", (const char *const[]){".png", ".svg", ".svgz", ".jxl", NULL}},
    [MF_INTEGRATION_KIND_MIME_PACKAGE] = {"mime-package",
                                     "share/mime/packages", (const char *const[]){".xml", NULL}},
    [MF_INTEGRATION_KIND_SYSTEMD_UNIT_SYSTEM] = {"systemd-unit-system",
                                     "lib/systemd/system", UNIT_SUFFIXES,
                                     TRUE},
    [MF_INTEGRATION_KIND_SYSTEMD_UNIT_USER] = {"systemd-unit-user", "lib/systemd/user", UNIT_SUFFIXES},
    [MF_INTEGRATION_KIND_UDEV_RULE] = {"udev-rule",
                                     "lib/udev/rules.d", (const char *const[]){".rules", NULL},
                                     TRUE},
};

/* Aliases come last in the PATH, so an alias can not replace a command the host has. On a
 * host that lacks a command of this list, an alias of that name could ask for its passwords.
 * This list is just a tiny safeguard to prevent some of the most obvious deception attempts. */
static const char *const RESERVED_ALIASES[] = {
    "doas",
    "login",
    "manifold",
    "mfrun",
    "passwd",
    "pkexec",
    "run0",
    "scp",
    "sftp",
    "ssh",
    "su",
    "sudo",
    "sudoedit",
    NULL,
};

/* link types no application may handle, as on a host without a default handler, a new
 * desktop entry would receive all links of that type */
static const char *const RESERVED_MIME_TYPES[] = {
    "x-scheme-handler/file",
    "x-scheme-handler/ftp",
    "x-scheme-handler/http",
    "x-scheme-handler/https",
    "x-scheme-handler/mailto",
    NULL,
};

/* the characters of a key in a udev rule */
#define UDEV_KEY_CHARS "ABCDEFGHIJKLMNOPQRSTUVWXYZ"

/* the namespace of a MIME package, which is the only one we accept */
#define MIME_NAMESPACE "http://www.freedesktop.org/standards/shared-mime-info"

/* the elements of a MIME package that change the types of the host, instead of adding one */
static const char *const REFUSED_MIME_ELEMENTS[] = {
    "alias",
    "glob-deleteall",
    "magic-deleteall",
    "treemagic",
    NULL,
};

/* the keys of a systemd unit that run a command */
static const char *const UNIT_COMMAND_KEYS[] = {
    "ExecCondition",
    "ExecReload",
    "ExecReloadPost",
    "ExecStart",
    "ExecStartPost",
    "ExecStartPre",
    "ExecStop",
    "ExecStopPost",
    "ExecStopPre",
    NULL,
};

/* the keys of a systemd unit that start with Exec, but do not run a command */
static const char *const UNIT_NON_COMMAND_KEYS[] = {
    "ExecPaths",
    "ExecSearchPath",
    NULL,
};

/* the keys of a desktop entry that we check, which must not have a language, as we would miss `Exec[de]` */
static const char *const UNLOCALIZED_DESKTOP_KEYS[] = {
    "DBusActivatable",
    "Exec",
    "MimeType",
    "TryExec",
    "Type",
    NULL,
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
        return KINDS[MF_INTEGRATION_KIND_UNKNOWN].name;
    return KINDS[kind].name;
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
        if (g_strcmp0(kind_str, KINDS[kind].name) == 0)
            return kind;
    }

    return MF_INTEGRATION_KIND_UNKNOWN;
}

/**
 * mf_integration_kind_get_dir:
 * @kind: a #MfIntegrationKind
 *
 * Returns: The directory that holds files of this kind, relative to `/app` in
 *   an image and to the exports directory on the host, or %NULL for an unknown kind
 */
const char *
mf_integration_kind_get_dir(MfIntegrationKind kind)
{
    if (kind <= MF_INTEGRATION_KIND_UNKNOWN || kind >= MF_INTEGRATION_KIND_LAST)
        return NULL;
    return KINDS[kind].dir;
}

/**
 * mf_integration_kind_is_privileged:
 * @kind: a #MfIntegrationKind
 *
 * Returns: %TRUE if files of this kind can give an image root access to the
 *   host, so that an admin has to approve them
 */
gboolean
mf_integration_kind_is_privileged(MfIntegrationKind kind)
{
    if (kind <= MF_INTEGRATION_KIND_UNKNOWN || kind >= MF_INTEGRATION_KIND_LAST)
        return FALSE;
    return KINDS[kind].privileged;
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
 * mf_integration_file_get_export_path:
 * @file: an #MfIntegrationFile
 *
 * Retrieve the export path of the file, relative to the directory of exports.
 */
const char *
mf_integration_file_get_export_path(MfIntegrationFile *file)
{
    g_return_val_if_fail(MF_IS_INTEGRATION_FILE(file), NULL);
    g_return_val_if_fail(g_str_has_prefix(file->path, APP_DIR), NULL);
    return file->path + strlen(APP_DIR);
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
 * mf_integration_file_check_content:
 * @file: an #MfIntegrationFile
 * @content: what was read for this file
 *
 * Returns: %TRUE if @content is what the manifest records for the file
 */
gboolean
mf_integration_file_check_content(MfIntegrationFile *file, GBytes *content)
{
    g_autofree char *sha256 = NULL;
    g_autofree char *blake3 = NULL;

    g_return_val_if_fail(MF_IS_INTEGRATION_FILE(file), FALSE);
    g_return_val_if_fail(content != NULL, FALSE);

    sha256 = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, content);
    blake3 = mf_blake3_of_bytes(content);

    return g_str_equal(sha256, file->checksums[MF_CHECKSUM_KIND_SHA256]) &&
           g_str_equal(blake3, file->checksums[MF_CHECKSUM_KIND_BLAKE3]);
}

/**
 * MfIntegrationAlias:
 *
 * A command to add to the PATH of the host, which runs a program of the image.
 */
struct _MfIntegrationAlias {
    GObject parent_instance;

    char *name;
    char *command;     /* the absolute path of the program in the image */
    char *export_path; /* where the script of the alias is, relative to the directory of exports */
};

G_DEFINE_FINAL_TYPE(MfIntegrationAlias, mf_integration_alias, G_TYPE_OBJECT)

static void
mf_integration_alias_finalize(GObject *object)
{
    MfIntegrationAlias *alias = MF_INTEGRATION_ALIAS(object);

    g_free(alias->name);
    g_free(alias->command);
    g_free(alias->export_path);

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

/**
 * mf_integration_alias_get_export_path:
 * @alias: an #MfIntegrationAlias
 *
 * Retrieve the export path of the alias, relative to the directory of exports.
 */
const char *
mf_integration_alias_get_export_path(MfIntegrationAlias *alias)
{
    g_return_val_if_fail(MF_IS_INTEGRATION_ALIAS(alias), NULL);
    return alias->export_path;
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
 * mf_integration_is_privileged:
 * @integration: an #MfIntegration
 *
 * Returns: %TRUE if an admin has to approve any file of @integration,
 *   see mf_integration_kind_is_privileged()
 */
gboolean
mf_integration_is_privileged(MfIntegration *integration)
{
    g_return_val_if_fail(MF_IS_INTEGRATION(integration), FALSE);

    for (guint i = 0; i < integration->files->len; i++) {
        MfIntegrationFile *file = g_ptr_array_index(integration->files, i);

        if (mf_integration_kind_is_privileged(mf_integration_file_get_kind(file)))
            return TRUE;
    }

    return FALSE;
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
        alias->export_path = g_build_filename(ALIAS_DIR, alias->name, NULL);
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

static void mf_integration_set_error(GError **error, const char *format, ...) G_GNUC_PRINTF(2, 3);

/**
 * mf_integration_set_error:
 *
 * Set an %MF_ERROR_INVALID_MANIFEST error about the integration section.
 * What comes from the manifest has to be escaped for this, as the message
 * ends up in the terminal of whoever installs the image.
 */
static void
mf_integration_set_error(GError **error, const char *format, ...)
{
    g_autofree char *message = NULL;
    va_list args;

    va_start(args, format);
    message = g_strdup_vprintf(format, args);
    va_end(args);
    g_set_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST, "integration: %s", message);
}

/**
 * mf_integration_name_is_owned:
 * @stem: the file name without its suffix, and without what comes before the ID
 * @stem_len: the length of @stem, which is not NUL terminated
 *
 * Returns: %TRUE if @stem is the component ID, or the ID followed by `.` or `-` and more
 */
static gboolean
mf_integration_name_is_owned(const char *stem, gsize stem_len, const char *component_id)
{
    gsize id_len = strlen(component_id);

    if (stem_len < id_len || strncmp(stem, component_id, id_len) != 0)
        return FALSE;
    if (stem_len == id_len)
        return TRUE;

    return (stem[id_len] == '.' || stem[id_len] == '-') && stem_len > id_len + 1;
}

/**
 * mf_integration_name_is_valid:
 * @name: the name of an integration file
 *
 * Check that a file name belongs to the component, and is one that the host
 * will pick up for this kind of file.
 */
static gboolean
mf_integration_name_is_valid(MfIntegrationKind kind, const char *name, const char *component_id)
{
    const char *const *suffixes = KINDS[kind].suffixes;
    gboolean unit = suffixes == UNIT_SUFFIXES;
    const char *stem = name;
    const char *suffix = NULL;
    gsize stem_len;

    if (unit ? !mf_unit_file_name_is_valid(name) : !mf_file_name_is_valid(name))
        return FALSE;
    for (guint i = 0; suffixes[i] != NULL && suffix == NULL; i++) {
        if (g_str_has_suffix(name, suffixes[i]))
            suffix = suffixes[i];
    }
    if (suffix == NULL)
        return FALSE;
    stem_len = strlen(name) - strlen(suffix);

    /* the name of a rules file starts with a number, as udev orders the files by their names */
    if (kind == MF_INTEGRATION_KIND_UDEV_RULE) {
        if (stem_len < 3 || !g_ascii_isdigit(name[0]) || !g_ascii_isdigit(name[1]) || name[2] != '-')
            return FALSE;
        stem += 3;
        stem_len -= 3;
    }

    /* the name of a template unit ends in `@`, and its instances have no file of their own */
    if (unit) {
        if (stem_len > 0 && stem[stem_len - 1] == '@')
            stem_len--;
        if (memchr(stem, '@', stem_len) != NULL)
            return FALSE;
    }

    return mf_integration_name_is_owned(stem, stem_len, component_id);
}

/**
 * mf_integration_check_file:
 *
 * Check where a file is in the image, and how it is called. Where it goes on
 * the host depends on nothing else, so this also decides that.
 */
static gboolean
mf_integration_check_file(MfIntegrationFile *file, const char *component_id, GError **error)
{
    const char *kind_name = KINDS[file->kind].name;
    g_autofree char *shown = g_strescape(file->path, NULL);
    g_autofree char *dir = g_strconcat(APP_DIR, KINDS[file->kind].dir, "/", NULL);
    const char *rest;
    const char *name;

    if (!g_str_has_prefix(file->path, dir)) {
        /* TRANSLATORS: the kind of the file, its path, and the directory it has to be in */
        mf_integration_set_error(error, _("%s: %s is not in %s"), kind_name, shown, dir);
        return FALSE;
    }
    rest = file->path + strlen(dir);
    name = strrchr(rest, '/');
    name = name != NULL ? name + 1 : rest;

    /* icons are in the size and category directories of the theme, all other files have to be
     * directly in the directory of their kind */
    if (file->kind == MF_INTEGRATION_KIND_ICON) {
        g_autofree char *icon_dir = g_strndup(rest, (gsize) (name - rest));

        if (!g_regex_match_simple("^([0-9]+x[0-9]+(@[0-9]+x)?|scalable|symbolic)/(apps|mimetypes)/\\z",
                                  icon_dir,
                                  0,
                                  0)) {
            /* TRANSLATORS: the kind of the file, its path, and an example of where it may be */
            mf_integration_set_error(error,
                                     _("%s: %s is not in a directory like %s"),
                                     kind_name,
                                     shown,
                                     "48x48/apps");
            return FALSE;
        }
    } else if (name != rest) {
        /* TRANSLATORS: the kind of the file, its path, and the directory it has to be in */
        mf_integration_set_error(error, _("%s: %s is not directly in %s"), kind_name, shown, dir);
        return FALSE;
    }

    if (!mf_integration_name_is_valid(file->kind, name, component_id)) {
        g_autofree char *suffixes = g_strjoinv("|", (char **) KINDS[file->kind].suffixes);

        /* TRANSLATORS: the kind of the file, its path, the optional number, the ID, and the file endings */
        mf_integration_set_error(error,
                                 _("%s: %s is not named like %s%s[.-…]{%s}"),
                                 kind_name,
                                 shown,
                                 file->kind == MF_INTEGRATION_KIND_UDEV_RULE ? "NN-" : "",
                                 component_id,
                                 suffixes);
        return FALSE;
    }

    return TRUE;
}

/**
 * mf_integration_program_is_valid:
 * @path: where a program is in the image
 *
 * We put the paths of programs into scripts and files that get interpreted,
 * so this only allows what does not need quoting anywhere.
 *
 * Returns: %TRUE if @path is an absolute path below /app, with plain names only
 */
static gboolean
mf_integration_program_is_valid(const char *path)
{
    return g_str_has_prefix(path, APP_DIR) && mf_program_path_is_valid(path + strlen(APP_DIR));
}

/**
 * mf_integration_check_alias:
 *
 * Check an alias. Aliases come last in the PATH, so a command of the host always
 * wins and we do not look for one, we only refuse names that could mislead users.
 */
static gboolean
mf_integration_check_alias(MfIntegrationAlias *alias, GError **error)
{
    g_autofree char *name = g_strescape(alias->name, NULL);
    g_autofree char *command = g_strescape(alias->command, NULL);

    if (!mf_program_name_is_valid(alias->name)) {
        /* TRANSLATORS: "alias" is a key of the manifest, and not translated */
        mf_integration_set_error(error, _("alias: %s is not a plain name"), name);
        return FALSE;
    }
    if (g_strv_contains(RESERVED_ALIASES, alias->name)) {
        /* TRANSLATORS: "alias" is a key of the manifest, and not translated */
        mf_integration_set_error(error, _("alias: %s is reserved"), name);
        return FALSE;
    }
    if (!mf_integration_program_is_valid(alias->command)) {
        /* TRANSLATORS: "alias" is a key of the manifest, and not translated, the second %s is the program */
        mf_integration_set_error(error, _("alias: %s: %s is not a path below /app"), name, command);
        return FALSE;
    }

    return TRUE;
}

/**
 * mf_integration_check:
 * @component_id: the component the section belongs to
 *
 * Check everything about an integration section that does not depend on the
 * host or on the contents of the files. See mf_manifest_check_integration().
 *
 * Returns: %TRUE if the component may ask for this, or %FALSE with %MF_ERROR_INVALID_MANIFEST
 */
gboolean
mf_integration_check(MfIntegration *integration, const char *component_id, GError **error)
{
    g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);

    /* integration files are named after the ID and share directories with the host, so a short
     * ID like `ssh` must not be able to claim a file like `ssh.service` */
    if (!mf_reverse_dns_name_is_valid(component_id)) {
        mf_integration_set_error(error,
                                 _("%s is not a reverse-DNS name of at least three parts"),
                                 component_id);
        return FALSE;
    }
    if (integration->unknown_kinds->len > 0) {
        g_autofree char *kind = g_strescape(g_ptr_array_index(integration->unknown_kinds, 0), NULL);

        /* TRANSLATORS: %s is a key of the manifest, and not translated */
        mf_integration_set_error(error, _("%s is not supported"), kind);
        return FALSE;
    }

    for (guint i = 0; i < integration->files->len; i++) {
        MfIntegrationFile *file = g_ptr_array_index(integration->files, i);

        if (!mf_integration_check_file(file, component_id, error))
            return FALSE;
        if (!g_hash_table_add(seen, file->path)) {
            mf_integration_set_error(error, _("%s is listed twice"), file->path);
            return FALSE;
        }
    }
    for (guint i = 0; i < integration->aliases->len; i++) {
        MfIntegrationAlias *alias = g_ptr_array_index(integration->aliases, i);

        if (!mf_integration_check_alias(alias, error))
            return FALSE;
        /* a file path can not equal the name of an alias, so both can use the same table */
        if (!g_hash_table_add(seen, alias->name)) {
            mf_integration_set_error(error, _("%s is listed twice"), alias->name);
            return FALSE;
        }
    }

    return TRUE;
}

/**
 * mf_integration_set_line_error:
 * @number: the number of the line, starting at 1
 * @problem: what is wrong with the line
 *
 * Set an error about a line of a file. We do not quote the line, it comes from the image.
 */
static void
mf_integration_set_line_error(GError **error, MfIntegrationFile *file, guint number, const char *problem)
{
    /* TRANSLATORS: the path of a file, the number of a line in it, and what is wrong with that line */
    mf_integration_set_error(error, _("%s: line %u: %s"), file->path, number, problem);
}

/**
 * mf_integration_split_lines:
 *
 * Returns: (transfer full): the lines of a text file, or %NULL if it is not
 *   a file that all of its parsers read the same way, see mf_text_is_valid()
 */
static char **
mf_integration_split_lines(MfIntegrationFile *file, GBytes *content, GError **error)
{
    gsize size;
    const char *data = g_bytes_get_data(content, &size);
    g_autofree char *text = g_strndup(data != NULL ? data : "", size);

    if (!mf_text_is_valid(content)) {
        mf_integration_set_error(error,
                                 _("%s: this is not a short UTF-8 text file with Unix line endings"),
                                 file->path);
        return NULL;
    }

    return g_strsplit(text, "\n", -1);
}

/**
 * mf_integration_wrap_command:
 * @command_line: a command line of a program of the image
 * @component_id: the component the program is in
 * @app_command: the program the component runs by default
 * @ret_runs_app: (out): whether the command line runs @app_command
 *
 * Make a command line run in the environment of the component. Its first word
 * has to be a program, and whatever follows goes to that program unchanged, as
 * mfrun stops looking for options at the component ID.
 *
 * mfrun finds a program by its name in the image, and never on the host.
 *
 * Returns: (transfer full): the new command line, or %NULL if the first word is not a program
 */
static char *
mf_integration_wrap_command(const char *command_line,
                            const char *component_id,
                            const char *app_command,
                            gboolean *ret_runs_app)
{
    const char *space = strchr(command_line, ' ');
    g_autofree char *program = space != NULL ? g_strndup(command_line, (gsize) (space - command_line))
                                             : g_strdup(command_line);
    g_autofree char *app_path = g_strconcat(APP_BIN_DIR, app_command, NULL);
    GString *wrapped;

    if (!mf_program_name_is_valid(program) && !mf_integration_program_is_valid(program))
        return NULL;
    *ret_runs_app = g_str_equal(program, app_command) || g_str_equal(program, app_path);

    wrapped = g_string_new(MF_MFRUN_PATH);
    if (!*ret_runs_app)
        g_string_append_printf(wrapped, " --command=%s", program);
    g_string_append_printf(wrapped, " %s", component_id);
    if (space != NULL)
        g_string_append(wrapped, space);

    return g_string_free(wrapped, FALSE);
}

/**
 * mf_integration_group_is_valid:
 * @line: a line of a desktop entry or systemd unit that starts with `[`
 *
 * Returns: %TRUE if @line is a group name in brackets, and nothing else
 */
static gboolean
mf_integration_group_is_valid(const char *line)
{
    gsize len = strlen(line);

    return len >= 3 && line[len - 1] == ']' && strcspn(line + 1, "[]") == len - 2;
}

/**
 * mf_integration_split_key:
 * @line: a line of a desktop entry or systemd unit
 * @ret_key_len: (out): the length of the name of the key
 * @ret_value: (out): the value in @line
 *
 * Find the parts of a line that has to be `Key=value` or `Key[locale]=value`.
 * Parsers differ in how they read anything else, like spaces around the key,
 * and KDE gives a meaning to what is in the brackets if it is not a locale, so
 * we do not accept any of that.
 *
 * Returns: %TRUE if the line is of that form
 */
static gboolean
mf_integration_split_key(const char *line, gsize *ret_key_len, const char **ret_value)
{
    gsize key_len = strspn(line, KEY_CHARS);
    const char *after_key = line + key_len;

    if (key_len == 0)
        return FALSE;
    if (*after_key == '[') {
        gsize locale_len = strspn(after_key + 1, LOCALE_CHARS);

        if (locale_len == 0 || after_key[locale_len + 1] != ']')
            return FALSE;
        after_key += locale_len + 2;
    }
    if (*after_key != '=')
        return FALSE;

    *ret_key_len = key_len;
    *ret_value = after_key + 1;

    return TRUE;
}

/**
 * mf_integration_mime_types_are_valid:
 * @value: the value of the MimeType key
 *
 * Returns: %FALSE if the desktop entry wants to handle a common kind of link
 */
static gboolean
mf_integration_mime_types_are_valid(const char *value)
{
    g_auto(GStrv) types = g_strsplit(value, ";", -1);

    for (guint i = 0; types[i] != NULL; i++) {
        g_autofree char *type = g_ascii_strdown(types[i], -1);

        if (g_strv_contains(RESERVED_MIME_TYPES, type))
            return FALSE;
    }

    return TRUE;
}

/**
 * mf_integration_render_desktop_entry:
 *
 * We go through the file line by line and only change what we have to. A
 * library for reading desktop entries would give us the keys, but it has its own
 * idea of what they are, which is not always the one of the program that reads
 * the file later. So we refuse everything that is not plain, and we keep what
 * we do not know, as unknown keys do not run anything.
 *
 * Returns: (transfer full): the desktop entry to install, or %NULL
 */
static GBytes *
mf_integration_render_desktop_entry(MfIntegrationFile *file,
                                    MfManifest *manifest,
                                    GBytes *content,
                                    GError **error)
{
    const char *component_id = mf_manifest_get_component_id(manifest);
    const char *app_command = mf_manifest_get_command(manifest);
    g_autofree char *name = g_path_get_basename(file->path);
    g_autofree char *main_name = g_strconcat(component_id, ".desktop", NULL);
    g_auto(GStrv) lines = mf_integration_split_lines(file, content, error);
    g_autoptr(GHashTable) groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_autoptr(GHashTable) keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GString *output;
    gboolean in_main_group = FALSE;
    gboolean has_type = FALSE;
    gboolean has_exec = FALSE;

    if (lines == NULL)
        return NULL;

    output = g_string_new(NULL);
    for (guint i = 0; lines[i] != NULL; i++) {
        const char *line = lines[i];
        const char *problem = NULL;
        g_autofree char *wrapped = NULL;
        g_autofree char *key = NULL;
        const char *value;
        gsize key_len;

        /* splitting a file that ends in a newline leaves an empty last line */
        if (line[0] == '\0' && lines[i + 1] == NULL)
            break;

        if (line[0] == '\0' || line[0] == '#') {
            /* blank lines and comments need no checks */
        } else if (line[0] == '[') {
            if (!mf_integration_group_is_valid(line))
                problem = _("this is not a valid group");
            else if (!g_hash_table_add(groups, g_strdup(line)))
                problem = _("this group is there already");
            else if (g_hash_table_size(groups) == 1 && !g_str_equal(line, DESKTOP_MAIN_GROUP))
                problem = _("the first group has to be [Desktop Entry]");
            in_main_group = g_str_equal(line, DESKTOP_MAIN_GROUP);
            g_hash_table_remove_all(keys);
        } else if (g_hash_table_size(groups) == 0) {
            problem = _("the first group has to be [Desktop Entry]");
        } else if (g_str_has_suffix(line, "\\")) {
            problem = _("lines can not be continued");
        } else if (!mf_integration_split_key(line, &key_len, &value)) {
            problem = _("this is not a plain key and value");
        } else {
            key = g_strndup(line, key_len);

            if (!g_hash_table_add(keys, g_strndup(line, (gsize) (value - line))))
                problem = _("this key is there already");
            else if (line[key_len] == '[' && g_strv_contains(UNLOCALIZED_DESKTOP_KEYS, key))
                problem = _("this key can not depend on the language");
            else if (g_str_equal(key, "TryExec"))
                continue; /* it checks for a program on the host, which we do not install */
            else if (g_str_equal(key, "DBusActivatable") && !g_str_equal(value, "false"))
                problem = _("activating applications over D-Bus is not supported");
            else if (g_str_equal(key, "Icon") && !mf_file_name_is_valid(value))
                problem = _("the icon has to be a plain name");
            else if (g_str_equal(key, "MimeType") && !mf_integration_mime_types_are_valid(value))
                problem = _("this kind of link can not be handled by an application");
            else if (g_str_equal(key, "Type") && in_main_group && !g_str_equal(value, "Application"))
                problem = _("only the type Application is supported");
            else if (g_str_equal(key, "Exec")) {
                gboolean runs_app = FALSE;

                wrapped = mf_integration_wrap_command(value, component_id, app_command, &runs_app);
                if (wrapped == NULL)
                    problem = _("the command has to start with a program of the image");
                else if (in_main_group && g_str_equal(name, main_name) && !runs_app)
                    problem = _("the main desktop entry has to run the command of the application");
            }

            has_type = has_type || (in_main_group && g_str_equal(key, "Type"));
            has_exec = has_exec || (in_main_group && g_str_equal(key, "Exec"));
        }

        if (problem != NULL) {
            mf_integration_set_line_error(error, file, i + 1, problem);
            g_string_free(output, TRUE);
            return NULL;
        }

        if (wrapped != NULL)
            g_string_append_printf(output, "Exec=%s\n", wrapped);
        else
            g_string_append_printf(output, "%s\n", line);
    }

    if (!has_type || !has_exec) {
        g_string_free(output, TRUE);
        mf_integration_set_error(error,
                                 _("%s: the group [Desktop Entry] has to have Type and Exec"),
                                 file->path);
        return NULL;
    }

    return g_string_free_to_bytes(output);
}

/**
 * mf_integration_unit_command_is_single:
 * @command_line: the value of a key that runs a command
 *
 * systemd runs several commands from one key if a lone semicolon separates
 * them, and only the first one would run in the environment of the component.
 * A lone semicolon in quotes gets refused too, as we do not parse quotes: a
 * program that wants one as its argument can have it as `\;`.
 *
 * Returns: %TRUE if @command_line can only be one command
 */
static gboolean
mf_integration_unit_command_is_single(const char *command_line)
{
    for (const char *p = strchr(command_line, ';'); p != NULL; p = strchr(p + 1, ';')) {
        gboolean starts_word = p == command_line || p[-1] == ' ' || p[-1] == '\t';
        gboolean ends_word = p[1] == '\0' || p[1] == ' ' || p[1] == '\t';

        if (starts_word && ends_word)
            return FALSE;
    }

    return TRUE;
}

/**
 * mf_integration_unit_names_are_valid:
 * @value: the value of a key that names units
 *
 * Returns: %TRUE if all the units named in @value belong to the component
 */
static gboolean
mf_integration_unit_names_are_valid(MfIntegrationKind kind, const char *value, const char *component_id)
{
    g_auto(GStrv) names = g_strsplit_set(value, " \t", -1);

    for (guint i = 0; names[i] != NULL; i++) {
        if (names[i][0] != '\0' && !mf_integration_name_is_valid(kind, names[i], component_id))
            return FALSE;
    }

    return TRUE;
}

/**
 * mf_integration_render_unit:
 *
 * A systemd unit can do anything, so we do not try to make it safe: only the
 * admin can decide to install one that runs as root. All we do here is to
 * change as few lines as possible, so that its commands run in the
 * environment of the component, and to refuse what would get in the way of
 * that. We read the lines ourselves, as a library could disagree with systemd.
 *
 * Returns: (transfer full): the unit to install, or %NULL
 */
static GBytes *
mf_integration_render_unit(MfIntegrationFile *file, MfManifest *manifest, GBytes *content, GError **error)
{
    const char *component_id = mf_manifest_get_component_id(manifest);
    const char *app_command = mf_manifest_get_command(manifest);
    g_auto(GStrv) lines = mf_integration_split_lines(file, content, error);
    GString *output;
    gboolean in_group = FALSE;

    if (lines == NULL)
        return NULL;

    output = g_string_new(NULL);
    for (guint i = 0; lines[i] != NULL; i++) {
        const char *line = lines[i];
        const char *problem = NULL;
        g_autofree char *wrapped = NULL;
        g_autofree char *key = NULL;
        const char *value;
        gsize key_len;

        /* splitting a file that ends in a newline leaves an empty last line */
        if (line[0] == '\0' && lines[i + 1] == NULL)
            break;

        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
            /* blank lines and comments need no checks */
        } else if (line[0] == '[') {
            if (!mf_integration_group_is_valid(line))
                problem = _("this is not a valid group");
            in_group = TRUE;
        } else if (!in_group) {
            problem = _("this line is not in a group");
        } else if (g_str_has_suffix(line, "\\")) {
            problem = _("lines can not be continued");
        } else if (!mf_integration_split_key(line, &key_len, &value) || value != line + key_len + 1) {
            problem = _("this is not a plain key and value");
        } else {
            key = g_strndup(line, key_len);

            if (g_strv_contains(UNIT_COMMAND_KEYS, key)) {
                /* an empty value clears the commands of the key, so there is nothing to wrap */
                if (value[0] != '\0') {
                    /* `-` is the only prefix we allow, as it only ignores a failure of the command */
                    gboolean may_fail = value[0] == '-';
                    const char *command_line = may_fail ? value + 1 : value;
                    gboolean runs_app;
                    g_autofree char *command = NULL;

                    if (!mf_integration_unit_command_is_single(command_line))
                        problem = _("a key can only run one command");
                    else
                        command = mf_integration_wrap_command(command_line,
                                                              component_id,
                                                              app_command,
                                                              &runs_app);

                    if (command != NULL)
                        wrapped = g_strdup_printf("%s=%s%s", key, may_fail ? "-" : "", command);
                    else if (problem == NULL)
                        problem = _("the command has to start with a program of the image");
                }
            } else if (g_str_has_prefix(key, "Exec") && !g_strv_contains(UNIT_NON_COMMAND_KEYS, key)) {
                problem = _("this key is not known, and it may run a command");
            } else if ((g_str_equal(key, "Alias") || g_str_equal(key, "Also")) &&
                       !mf_integration_unit_names_are_valid(file->kind, value, component_id)) {
                problem = _("this names a unit that does not belong to the component");
            }
        }

        if (problem != NULL) {
            mf_integration_set_line_error(error, file, i + 1, problem);
            g_string_free(output, TRUE);
            return NULL;
        }

        g_string_append_printf(output, "%s\n", wrapped != NULL ? wrapped : line);
    }

    return g_string_free_to_bytes(output);
}

/**
 * MfIntegrationMimeRender:
 *
 * What we keep while parsing a MIME package.
 */
typedef struct {
    GString *output;
    gboolean has_type;
} MfIntegrationMimeRender;

/**
 * mf_integration_mime_attribute_is_valid:
 * @element: the element the attribute is on
 *
 * Returns: %TRUE if we accept the value of the attribute
 */
static gboolean
mf_integration_mime_attribute_is_valid(const char *element, const char *name, const char *value)
{
    if (g_str_equal(name, "xmlns"))
        return g_str_equal(element, "mime-info") && g_str_equal(value, MIME_NAMESPACE);
    if (strchr(name, ':') != NULL)
        return g_str_equal(name, "xml:lang");

    if (g_str_equal(name, "type") &&
        (g_str_equal(element, "mime-type") || g_str_equal(element, "sub-class-of")))
        return mf_mime_type_is_valid(value);
    if (g_str_equal(element, "root-XML"))
        return mf_mime_xml_name_is_valid(value);
    if (g_str_equal(element, "glob") && g_str_equal(name, "pattern"))
        return mf_mime_glob_is_valid(value);
    if ((g_str_equal(element, "glob") && g_str_equal(name, "weight")) ||
        (g_str_equal(element, "magic") && g_str_equal(name, "priority")))
        return mf_mime_weight_is_valid(value);
    if ((g_str_equal(element, "icon") || g_str_equal(element, "generic-icon")) && g_str_equal(name, "name"))
        return mf_file_name_is_valid(value);

    return TRUE;
}

/**
 * mf_integration_mime_element_problem:
 * @stack: the elements we are in, starting with @element
 *
 * Returns: (nullable): what is wrong with the place of an element in the document
 */
static const char *
mf_integration_mime_element_problem(const GSList *stack, const char *element)
{
    gboolean is_root = stack->next == NULL;
    gboolean below_root = !is_root && stack->next->next == NULL;

    if (is_root != g_str_equal(element, "mime-info"))
        return _("the document has to consist of one mime-info element");
    if (below_root != g_str_equal(element, "mime-type"))
        return _("only mime-type elements can be in mime-info");
    /* libxml2 resolves namespace prefixes, so `x:alias` can be an `alias` element for it */
    if (strchr(element, ':') != NULL)
        return _("namespace prefixes are not supported");
    if (g_strv_contains(REFUSED_MIME_ELEMENTS, element))
        return _("a package can only add types, and not change those of the host");

    return NULL;
}

/**
 * mf_integration_mime_on_start_element:
 */
static void
mf_integration_mime_on_start_element(GMarkupParseContext *context,
                                     const char *element,
                                     const char **attribute_names,
                                     const char **attribute_values,
                                     gpointer user_data,
                                     GError **error)
{
    MfIntegrationMimeRender *render = user_data;
    const GSList *stack = g_markup_parse_context_get_element_stack(context);
    const char *problem = mf_integration_mime_element_problem(stack, element);

    if (problem == NULL && stack->next == NULL && !g_strv_contains(attribute_names, "xmlns"))
        problem = _("mime-info has to have the namespace of MIME packages");
    if (problem == NULL && g_str_equal(element, "mime-type") && !g_strv_contains(attribute_names, "type"))
        problem = _("a mime-type element has to have a type");

    g_string_append_printf(render->output, "<%s", element);
    for (guint i = 0; problem == NULL && attribute_names[i] != NULL; i++) {
        g_autofree char *value = g_markup_escape_text(attribute_values[i], -1);

        if (!mf_integration_mime_attribute_is_valid(element, attribute_names[i], attribute_values[i]))
            problem = _("the value of an attribute is not allowed");
        g_string_append_printf(render->output, " %s=\"%s\"", attribute_names[i], value);
    }
    g_string_append_c(render->output, '>');

    if (problem != NULL)
        g_set_error_literal(error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT, problem);
    render->has_type = render->has_type || g_str_equal(element, "mime-type");
}

/**
 * mf_integration_mime_on_end_element:
 */
static void
mf_integration_mime_on_end_element(GMarkupParseContext *context,
                                   const char *element,
                                   gpointer user_data,
                                   GError **error)
{
    MfIntegrationMimeRender *render = user_data;

    g_string_append_printf(render->output, "</%s>", element);
}

/**
 * mf_integration_mime_on_text:
 */
static void
mf_integration_mime_on_text(GMarkupParseContext *context,
                            const char *text,
                            gsize text_len,
                            gpointer user_data,
                            GError **error)
{
    MfIntegrationMimeRender *render = user_data;
    g_autofree char *escaped = g_markup_escape_text(text, (gssize) text_len);

    g_string_append(render->output, escaped);
}

/**
 * mf_integration_render_mime_package:
 *
 * We write a MIME package anew from what we parsed.
 * The tool that reads it later runs as root and uses another XML parser, which
 * follows the encoding a file declares and DTDs. So a package could hide
 * elements from us in a way that parser sees them. What we write has none of
 * that, so the package can only hold what we looked at.
 *
 * Returns: (transfer full): the package to install, or %NULL
 */
static GBytes *
mf_integration_render_mime_package(MfIntegrationFile *file, GBytes *content, GError **error)
{
    static const GMarkupParser parser = {
        .start_element = mf_integration_mime_on_start_element,
        .end_element = mf_integration_mime_on_end_element,
        .text = mf_integration_mime_on_text,
    };
    MfIntegrationMimeRender render = {
        .output = g_string_new("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"),
    };
    g_autoptr(GMarkupParseContext) context = NULL;
    g_autoptr(GError) local_error = NULL;
    g_auto(GStrv) lines = mf_integration_split_lines(file, content, error);
    gsize size;
    const char *data = g_bytes_get_data(content, &size);

    if (lines == NULL) {
        g_string_free(render.output, TRUE);
        return NULL;
    }

    context = g_markup_parse_context_new(&parser,
                                         G_MARKUP_TREAT_CDATA_AS_TEXT | G_MARKUP_PREFIX_ERROR_POSITION,
                                         &render,
                                         NULL);
    if (!g_markup_parse_context_parse(context, data != NULL ? data : "", (gssize) size, &local_error) ||
        !g_markup_parse_context_end_parse(context, &local_error)) {
        /* the message of the parser can quote the file, so we have to escape it */
        g_autofree char *message = g_strescape(local_error->message, NULL);

        mf_integration_set_error(error, _("%s: %s"), file->path, message);
        g_string_free(render.output, TRUE);
        return NULL;
    }
    if (!render.has_type) {
        mf_integration_set_error(error, _("%s: this package has no types"), file->path);
        g_string_free(render.output, TRUE);
        return NULL;
    }

    g_string_append_c(render.output, '\n');
    return g_string_free_to_bytes(render.output);
}

/**
 * mf_integration_wanted_units_are_valid:
 * @kind: the kind of the units, for their names
 * @value: the units, separated by spaces, which a rule wants started
 *
 * A device often starts an instance of a template, named after the device,
 * which is the only part of the name that is not known here.
 *
 * Returns: %TRUE if all the units belong to the component
 */
static gboolean
mf_integration_wanted_units_are_valid(MfIntegrationKind kind, const char *value, const char *component_id)
{
    g_auto(GStrv) names = g_strsplit_set(value, " \t", -1);

    for (guint i = 0; names[i] != NULL; i++) {
        const char *at = strchr(names[i], '@');
        const char *suffix = strrchr(names[i], '.');
        g_autofree char *name = NULL;

        if (names[i][0] == '\0')
            continue;
        if (at != NULL && suffix != NULL && suffix > at)
            name = g_strdup_printf("%.*s@%s", (int) (at - names[i]), names[i], suffix);
        else
            name = g_strdup(names[i]);
        if (!mf_integration_name_is_valid(kind, name, component_id))
            return FALSE;
    }

    return TRUE;
}

/**
 * mf_integration_udev_token_problem:
 * @attribute: (nullable): what is in the braces after the key
 * @is_match: whether the token compares, instead of assigning
 *
 * Returns: (nullable): why a token of a rule is not allowed
 */
static const char *
mf_integration_udev_token_problem(const char *key,
                                  const char *attribute,
                                  gboolean is_match,
                                  const char *value,
                                  const char *component_id)
{
    /* mfrun can not work in the sandbox of udev, so a rule has no way to run a program of its
     * image. We refuse PROGRAM with any operator, as it runs a program even to compare. */
    if (g_str_equal(key, "RUN") || g_str_equal(key, "PROGRAM") ||
        (g_str_equal(key, "IMPORT") && g_strcmp0(attribute, "program") == 0))
        return _("rules can not run programs");
    if (is_match)
        return NULL;

    if (g_str_equal(key, "SYSCTL"))
        return _("rules can not change kernel parameters");
    /* brackets name a different device, and `$` or `%` are only expanded when the rule runs */
    if (g_str_equal(key, "ATTR") && attribute != NULL && attribute[strcspn(attribute, "[$%")] != '\0')
        return _("rules can only change the device they matched");
    if (g_str_equal(key, "ENV") && g_strcmp0(attribute, "SYSTEMD_WANTS") == 0 &&
        !mf_integration_wanted_units_are_valid(MF_INTEGRATION_KIND_SYSTEMD_UNIT_SYSTEM, value, component_id))
        return _("rules can only start units of the component");
    if (g_str_equal(key, "ENV") && g_strcmp0(attribute, "SYSTEMD_USER_WANTS") == 0 &&
        !mf_integration_wanted_units_are_valid(MF_INTEGRATION_KIND_SYSTEMD_UNIT_USER, value, component_id))
        return _("rules can only start units of the component");

    return NULL;
}

/**
 * mf_integration_udev_line_problem:
 * @line: a line of a rules file
 *
 * Split a rule into its tokens the way udev does, which are `KEY="value"` or
 * `KEY{attribute}="value"` with a separator of spaces and commas between them.
 *
 * udev allows more than that: a backslash can keep a quote from ending a value,
 * and a letter before the quote changes what backslashes do. Whether a value
 * has ended decides what we take for the next key, so we do not accept any of
 * that, and real rules hardly use it.
 *
 * Returns: (nullable): why the line is not allowed
 */
static const char *
mf_integration_udev_line_problem(const char *line, const char *component_id)
{
    const char *p = line;

    for (;;) {
        g_autofree char *key = NULL;
        g_autofree char *attribute = NULL;
        g_autofree char *value = NULL;
        gboolean is_match;
        const char *problem;
        const char *end;
        gsize key_len;

        p += strspn(p, " \t,");
        if (*p == '\0')
            return NULL;

        key_len = strspn(p, UDEV_KEY_CHARS);
        if (key_len == 0)
            return _("this is not a plain rule");
        key = g_strndup(p, key_len);
        p += key_len;

        if (*p == '{') {
            end = strchr(p, '}');
            if (end == NULL)
                return _("this is not a plain rule");
            attribute = g_strndup(p + 1, (gsize) (end - p - 1));
            if (attribute[strcspn(attribute, " \t\",")] != '\0')
                return _("this is not a plain rule");
            p = end + 1;
        }

        is_match = g_str_has_prefix(p, "==") || g_str_has_prefix(p, "!=");
        if (is_match || g_str_has_prefix(p, "+=") || g_str_has_prefix(p, "-=") || g_str_has_prefix(p, ":="))
            p += 2;
        else if (*p == '=')
            p += 1;
        else
            return _("this is not a plain rule");

        if (*p != '"')
            return _("a value has to be in quotes, without a prefix");
        end = strchr(p + 1, '"');
        if (end == NULL)
            return _("a value has to be in quotes, without a prefix");
        value = g_strndup(p + 1, (gsize) (end - p - 1));
        if (strchr(value, '\\') != NULL)
            return _("a value can not have backslashes");
        p = end + 1;

        problem = mf_integration_udev_token_problem(key, attribute, is_match, value, component_id);
        if (problem != NULL)
            return problem;
    }
}

/**
 * mf_integration_render_udev_rule:
 *
 * No check can make a udev rule safe, as a rule that makes a disk writable for
 * everyone is as bad as one that runs something. Only the admin can decide to
 * install rules, and all we do here is to refuse what rules should not need.
 *
 * We install the file as it is.
 *
 * Returns: (transfer full): the rules to install, or %NULL
 */
static GBytes *
mf_integration_render_udev_rule(MfIntegrationFile *file,
                                MfManifest *manifest,
                                GBytes *content,
                                GError **error)
{
    const char *component_id = mf_manifest_get_component_id(manifest);
    g_auto(GStrv) lines = mf_integration_split_lines(file, content, error);

    if (lines == NULL)
        return NULL;

    for (guint i = 0; lines[i] != NULL; i++) {
        const char *line = lines[i] + strspn(lines[i], " \t");
        const char *problem = NULL;

        if (line[0] == '\0' || line[0] == '#')
            continue;
        if (g_str_has_suffix(line, "\\"))
            problem = _("lines can not be continued");
        else
            problem = mf_integration_udev_line_problem(line, component_id);

        if (problem != NULL) {
            mf_integration_set_line_error(error, file, i + 1, problem);
            return NULL;
        }
    }

    return g_bytes_ref(content);
}

/**
 * mf_integration_file_render:
 * @file: an #MfIntegrationFile
 * @manifest: the manifest of @file, which passed mf_manifest_check_integration()
 * @content: what the file holds in the image
 * @error: a #GError
 *
 * Check the contents of a file, and make it into what is installed on the
 * host, where anything it runs has to run in the environment of the component.
 * This does not depend on the host, so whoever builds an image can use it to
 * refuse what could not be installed.
 *
 * Returns: (transfer full): the file to install, or %NULL with %MF_ERROR_INVALID_MANIFEST
 */
GBytes *
mf_integration_file_render(MfIntegrationFile *file, MfManifest *manifest, GBytes *content, GError **error)
{
    g_return_val_if_fail(MF_IS_INTEGRATION_FILE(file), NULL);
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    g_return_val_if_fail(content != NULL, NULL);

    switch (file->kind) {
    case MF_INTEGRATION_KIND_DESKTOP_ENTRY:
        return mf_integration_render_desktop_entry(file, manifest, content, error);
    case MF_INTEGRATION_KIND_ICON:
        return g_bytes_ref(content);
    case MF_INTEGRATION_KIND_MIME_PACKAGE:
        return mf_integration_render_mime_package(file, content, error);
    case MF_INTEGRATION_KIND_SYSTEMD_UNIT_SYSTEM:
    case MF_INTEGRATION_KIND_SYSTEMD_UNIT_USER:
        return mf_integration_render_unit(file, manifest, content, error);
    case MF_INTEGRATION_KIND_UDEV_RULE:
        return mf_integration_render_udev_rule(file, manifest, content, error);
    default:
        g_return_val_if_reached(NULL);
    }
}

/**
 * mf_integration_alias_render:
 * @alias: an #MfIntegrationAlias
 * @manifest: the manifest of @alias
 * @error: a #GError
 *
 * Make the script that is installed on the host for an alias, which runs the
 * program of the alias in the environment of the component. This checks the
 * alias again, as the script ends up in the PATH of every user of the host.
 *
 * Returns: (transfer full): the script to install, or %NULL with %MF_ERROR_INVALID_MANIFEST
 */
GBytes *
mf_integration_alias_render(MfIntegrationAlias *alias, MfManifest *manifest, GError **error)
{
    g_autofree char *wrapped = NULL;
    gboolean runs_app;
    char *script;

    g_return_val_if_fail(MF_IS_INTEGRATION_ALIAS(alias), NULL);
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);

    if (!mf_integration_check_alias(alias, error))
        return NULL;

    wrapped = mf_integration_wrap_command(alias->command,
                                          mf_manifest_get_component_id(manifest),
                                          mf_manifest_get_command(manifest),
                                          &runs_app);
    script = g_strdup_printf("#!/bin/sh\nexec %s \"$@\"\n", wrapped);

    return g_bytes_new_take(script, strlen(script));
}
