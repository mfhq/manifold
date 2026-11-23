/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mf-manifest.h"
#include "mf-manifest-private.h"

#include <inttypes.h>
#include <string.h>
#include <glib/gi18n-lib.h>
#include <systemd/sd-json.h>

#include "mf-component-private.h"
#include "mf-error.h"
#include "mf-integration-private.h"
#include "mf-json-private.h"
#include "mf-sdautoptr-private.h"
#include "mf-utils.h"

/**
 * MfManifest:
 *
 * The manifest of an image contains metadata about its contents:
 * The component it belongs to, the base it was built on, the layout of
 * the image file, etc.
 */

constexpr int MANIFEST_FORMAT = 1;

/**
 * mf_checksum_kind_to_string:
 * @kind: a #MfChecksumKind
 *
 * Returns: The name of the checksum kind.
 */
const char *
mf_checksum_kind_to_string(MfChecksumKind kind)
{
    switch (kind) {
    case MF_CHECKSUM_KIND_BLAKE3:
        return "blake3";
    case MF_CHECKSUM_KIND_SHA256:
        return "sha256";
    default:
        return "unknown";
    }
}

/**
 * mf_checksum_kind_from_string:
 * @kind_str: (nullable): the name of a checksum kind
 *
 * Returns: The checksum kind, or %MF_CHECKSUM_KIND_UNKNOWN if the name is not known
 */
MfChecksumKind
mf_checksum_kind_from_string(const char *kind_str)
{
    if (g_strcmp0(kind_str, "blake3") == 0)
        return MF_CHECKSUM_KIND_BLAKE3;
    if (g_strcmp0(kind_str, "sha256") == 0)
        return MF_CHECKSUM_KIND_SHA256;
    return MF_CHECKSUM_KIND_UNKNOWN;
}

/**
 * mf_permission_to_string:
 * @permission: a #MfPermission
 *
 * Returns: The name of the permission.
 */
const char *
mf_permission_to_string(MfPermission permission)
{
    switch (permission) {
    case MF_PERMISSION_HOST_COMMANDS:
        return "host-commands";
    case MF_PERMISSION_HOST_TMP:
        return "host-tmp";
    default:
        return "unknown";
    }
}

/**
 * mf_permission_from_string:
 * @permission_str: (nullable): the name of a permission
 *
 * Returns: The permission, or %MF_PERMISSION_UNKNOWN if the name is not known
 */
MfPermission
mf_permission_from_string(const char *permission_str)
{
    if (g_strcmp0(permission_str, "host-commands") == 0)
        return MF_PERMISSION_HOST_COMMANDS;
    if (g_strcmp0(permission_str, "host-tmp") == 0)
        return MF_PERMISSION_HOST_TMP;
    return MF_PERMISSION_UNKNOWN;
}

/**
 * MfManifestBase:
 *
 * Reference to a component that an image was built on, as given in the
 * `base` or `base-sdk` section of a manifest.
 */
typedef struct {
    char *id;
    MfComponentKind kind;
    char *version;
    uint64_t revision;
    char *identity;
    char *content_hash;
} MfManifestBase;

typedef struct {
    GBytes *data;

    char *component_id;
    MfComponentKind kind;
    char *version;
    uint64_t revision;
    char *architecture;
    gboolean sdk;
    char *identity;
    char *content_hash;

    MfManifestBase base;     /* its ID is NULL for a core */
    MfManifestBase base_sdk; /* its ID is NULL if the manifest has no base SDK */

    char *image_file;
    uint64_t image_size;
    char *image_checksums[MF_CHECKSUM_KIND_LAST];
    char *verity_hash;
    uint64_t data_offset;
    uint64_t data_size;
    uint64_t hash_offset;
    uint64_t hash_size;

    char *command; /* applications only */

    MfIntegration *integration; /* (nullable) */

    guint permissions;              /* one bit for each MfPermission the manifest requests */
    GPtrArray *unknown_permissions; /* names of requested permissions we do not know */
} MfManifestPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(MfManifest, mf_manifest, G_TYPE_OBJECT)

#define GET_PRIVATE(o) ((MfManifestPrivate *) mf_manifest_get_instance_private(o))

static void
mf_manifest_base_clear(MfManifestBase *base)
{
    g_clear_pointer(&base->id, g_free);
    g_clear_pointer(&base->version, g_free);
    g_clear_pointer(&base->identity, g_free);
    g_clear_pointer(&base->content_hash, g_free);
}

static void
mf_manifest_finalize(GObject *object)
{
    MfManifestPrivate *priv = GET_PRIVATE(MF_MANIFEST(object));

    g_clear_pointer(&priv->data, g_bytes_unref);
    g_free(priv->component_id);
    g_free(priv->version);
    g_free(priv->architecture);
    g_free(priv->identity);
    g_free(priv->content_hash);
    mf_manifest_base_clear(&priv->base);
    mf_manifest_base_clear(&priv->base_sdk);
    g_free(priv->image_file);
    for (guint i = 0; i < MF_CHECKSUM_KIND_LAST; i++)
        g_free(priv->image_checksums[i]);
    g_free(priv->verity_hash);
    g_free(priv->command);
    g_clear_object(&priv->integration);
    g_ptr_array_unref(priv->unknown_permissions);

    G_OBJECT_CLASS(mf_manifest_parent_class)->finalize(object);
}

static void
mf_manifest_class_init(MfManifestClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = mf_manifest_finalize;
}

static void
mf_manifest_init(MfManifest *manifest)
{
    MfManifestPrivate *priv = GET_PRIVATE(manifest);

    priv->unknown_permissions = g_ptr_array_new_with_free_func(g_free);
}

/* manifest field accessors, which report every failure as MF_ERROR_INVALID_MANIFEST */
#define FIELD_STRING(object, section, key, error) \
    mf_json_get_string_field(object, section, key, MF_ERROR_INVALID_MANIFEST, error)
#define FIELD_UNSIGNED(object, section, key, ret, error) \
    mf_json_get_unsigned_field(object, section, key, MF_ERROR_INVALID_MANIFEST, ret, error)
#define FIELD_BOOLEAN(object, section, key, ret, error) \
    mf_json_get_boolean_field(object, section, key, MF_ERROR_INVALID_MANIFEST, ret, error)
#define FIELD_HASH(object, section, key, error) \
    mf_json_get_hash_field(object, section, key, MF_ERROR_INVALID_MANIFEST, error)

static void mf_set_error_invalid_manifest(GError **error, const char *format, ...) G_GNUC_PRINTF(2, 3);

/**
 * mf_set_error_invalid_manifest:
 *
 * Set an %MF_ERROR_INVALID_MANIFEST error.
 */
static void
mf_set_error_invalid_manifest(GError **error, const char *format, ...)
{
    g_autofree char *message = NULL;
    va_list args;

    va_start(args, format);
    message = g_strdup_vprintf(format, args);
    va_end(args);
    g_set_error_literal(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST, message);
}

/**
 * mf_manifest_json_get_component_id:
 *
 * Read a component ID from a field, and check that the ID is valid.
 */
static char *
mf_manifest_json_get_component_id(sd_json_variant *object, const char *section, GError **error)
{
    const char *component_id = FIELD_STRING(object, section, "component", error);

    if (component_id == NULL)
        return NULL;
    if (!mf_component_id_is_valid(component_id)) {
        mf_set_error_invalid_manifest(error,
                                      "%s%s%s",
                                      section ? section : "",
                                      section ? ": " : "",
                                      _("component identifier is not valid"));
        return NULL;
    }
    return g_strdup(component_id);
}

/**
 * mf_manifest_json_get_version:
 *
 * Read a version from a field, and check that the version is valid.
 */
static char *
mf_manifest_json_get_version(sd_json_variant *object, const char *section, GError **error)
{
    const char *version = FIELD_STRING(object, section, "version", error);

    if (version == NULL)
        return NULL;
    if (!mf_version_is_valid(version)) {
        mf_set_error_invalid_manifest(error,
                                      "%s%s%s",
                                      section ? section : "",
                                      section ? ": " : "",
                                      _("version is not valid"));
        return NULL;
    }
    return g_strdup(version);
}

/**
 * mf_manifest_json_get_kind:
 *
 * Read the kind of a component from a field, and check that the kind is known.
 *
 * Returns: the kind, or %MF_COMPONENT_KIND_UNKNOWN on error
 */
static MfComponentKind
mf_manifest_json_get_kind(sd_json_variant *object, const char *section, GError **error)
{
    const char *value = FIELD_STRING(object, section, "kind", error);
    g_autofree char *shown = NULL;
    g_autofree char *message = NULL;
    MfComponentKind kind;

    if (value == NULL)
        return MF_COMPONENT_KIND_UNKNOWN;
    kind = mf_component_kind_from_string(value);
    if (kind != MF_COMPONENT_KIND_UNKNOWN)
        return kind;

    shown = g_strescape(value, NULL);
    /* TRANSLATORS: "kind" is a field of the manifest, and not translated, %s is its value */
    message = g_strdup_printf(_("kind %s is not known"), shown);
    mf_set_error_invalid_manifest(error, "%s%s%s", section ? section : "", section ? ": " : "", message);
    return MF_COMPONENT_KIND_UNKNOWN;
}

/**
 * mf_manifest_json_parse_base:
 * @section: the section to parse, "base" or "base-sdk"
 * @ret_base: (out): the parsed reference, its ID stays %NULL if the manifest does not have the section
 *
 * Parse the `base` or `base-sdk` section of a manifest. Both sections
 * reference a component that the image was built on, with its ID, kind,
 * version, revision, identity and content hash.
 */
static gboolean
mf_manifest_json_parse_base(sd_json_variant *manifest,
                            const char *section,
                            MfManifestBase *ret_base,
                            GError **error)
{
    sd_json_variant *base = sd_json_variant_by_key(manifest, section);

    if (base == NULL || sd_json_variant_is_null(base))
        return TRUE;
    if (!sd_json_variant_is_object(base)) {
        /* TRANSLATORS: %s is a section of the manifest, "base" or "base-sdk" */
        mf_set_error_invalid_manifest(error, _("%s is not an object"), section);
        return FALSE;
    }

    ret_base->identity = g_strdup(FIELD_HASH(base, section, "identity", error));
    if (ret_base->identity == NULL)
        return FALSE;
    ret_base->id = mf_manifest_json_get_component_id(base, section, error);
    if (ret_base->id == NULL)
        return FALSE;
    ret_base->kind = mf_manifest_json_get_kind(base, section, error);
    if (ret_base->kind == MF_COMPONENT_KIND_UNKNOWN)
        return FALSE;
    ret_base->version = mf_manifest_json_get_version(base, section, error);
    if (ret_base->version == NULL)
        return FALSE;
    if (!FIELD_UNSIGNED(base, section, "revision", &ret_base->revision, error))
        return FALSE;
    ret_base->content_hash = g_strdup(FIELD_HASH(base, section, "content-hash", error));
    if (ret_base->content_hash == NULL)
        return FALSE;

    return TRUE;
}

/**
 * mf_manifest_check_base_sdk:
 *
 * Check that a manifest has a `base-sdk` section if it needs one, and has
 * none otherwise.
 *
 * An SDK only contains the development files of its own layer. An SDK that
 * extends a platform, a kit or an application must therefore reference the
 * SDK of the layer below. The SDK of a core has no layer below it, and all
 * other components must not have a `base-sdk` section.
 */
static gboolean
mf_manifest_check_base_sdk(MfManifestPrivate *priv, GError **error)
{
    gboolean needed = priv->sdk && priv->kind != MF_COMPONENT_KIND_CORE && priv->base.kind == priv->kind;

    if (!needed) {
        if (priv->base_sdk.id == NULL)
            return TRUE;
        mf_set_error_invalid_manifest(
            error,
            /* TRANSLATORS: the kind and ID of the component, "base-sdk" is a section of the manifest */
            _("%s%s %s can not have a base-sdk"),
            mf_component_kind_to_string(priv->kind),
            priv->sdk ? " SDK" : "",
            priv->component_id);
        return FALSE;
    }

    if (priv->base_sdk.id == NULL) {
        mf_set_error_invalid_manifest(
            error,
            /* TRANSLATORS: %s is the kind of the component, like "platform" or "kit" */
            _("the SDK of a %s must name the SDK of the layer below it"),
            mf_component_kind_to_string(priv->kind));
        return FALSE;
    }
    /* the base SDK must be of a kind that the extended component can build on */
    if (!mf_component_kind_can_build_on(priv->kind, FALSE, priv->base_sdk.kind, TRUE)) {
        mf_set_error_invalid_manifest(
            error,
            /* TRANSLATORS: the kind and ID of the component, then the kind and ID of its base SDK */
            _("%s SDK %s can not have the %s SDK %s below it"),
            mf_component_kind_to_string(priv->kind),
            priv->component_id,
            mf_component_kind_to_string(priv->base_sdk.kind),
            priv->base_sdk.id);
        return FALSE;
    }

    return TRUE;
}

/**
 * mf_manifest_json_parse_image:
 *
 * Parse the image section of a manifest, which describes the image file.
 */
static gboolean
mf_manifest_json_parse_image(sd_json_variant *manifest, MfManifestPrivate *priv, GError **error)
{
    sd_json_variant *image = sd_json_variant_by_key(manifest, "image");

    if (image == NULL || !sd_json_variant_is_object(image)) {
        /* TRANSLATORS: "image" is a section of the manifest, and not translated */
        mf_set_error_invalid_manifest(error, "%s", _("image is missing or not an object"));
        return FALSE;
    }

    priv->image_file = g_strdup(FIELD_STRING(image, "image", "file", error));
    if (priv->image_file == NULL)
        return FALSE;
    if (!FIELD_UNSIGNED(image, "image", "size", &priv->image_size, error))
        return FALSE;
    for (MfChecksumKind kind = MF_CHECKSUM_KIND_UNKNOWN + 1; kind < MF_CHECKSUM_KIND_LAST; kind++) {
        priv->image_checksums[kind] = g_strdup(
            FIELD_HASH(image, "image", mf_checksum_kind_to_string(kind), error));
        if (priv->image_checksums[kind] == NULL)
            return FALSE;
    }
    priv->verity_hash = g_strdup(FIELD_HASH(image, "image", "verity-root-hash", error));
    if (priv->verity_hash == NULL)
        return FALSE;
    if (!FIELD_UNSIGNED(image, "image", "data-offset", &priv->data_offset, error) ||
        !FIELD_UNSIGNED(image, "image", "hash-offset", &priv->hash_offset, error))
        return FALSE;
    /* the sizes are optional, we only need the offsets to find the partitions */
    if (sd_json_variant_by_key(image, "data-size") != NULL &&
        !FIELD_UNSIGNED(image, "image", "data-size", &priv->data_size, error))
        return FALSE;
    if (sd_json_variant_by_key(image, "hash-size") != NULL &&
        !FIELD_UNSIGNED(image, "image", "hash-size", &priv->hash_size, error))
        return FALSE;
    if (priv->hash_offset <= priv->data_offset) {
        /* TRANSLATORS: "image" is the section of the manifest this is about, and not translated */
        mf_set_error_invalid_manifest(error, "%s", _("image: the hash partition is placed before the data"));
        return FALSE;
    }

    return TRUE;
}

/**
 * mf_manifest_json_parse_permissions:
 *
 * Parse the permissions list of a manifest. We only check its structure here.
 * The names of permissions we do not know are kept, so they can be refused
 * when the image is installed.
 */
static gboolean
mf_manifest_json_parse_permissions(sd_json_variant *jo, MfManifestPrivate *priv, GError **error)
{
    sd_json_variant *permissions = sd_json_variant_by_key(jo, "permissions");

    if (permissions == NULL || sd_json_variant_is_null(permissions))
        return TRUE;
    if (!sd_json_variant_is_array(permissions)) {
        /* TRANSLATORS: "permissions" is a field of the manifest, and not translated */
        mf_set_error_invalid_manifest(error, "%s", _("permissions is not a list"));
        return FALSE;
    }

    for (size_t i = 0; i < sd_json_variant_elements(permissions); i++) {
        sd_json_variant *entry = sd_json_variant_by_index(permissions, i);
        MfPermission permission;

        if (!sd_json_variant_is_string(entry)) {
            /* TRANSLATORS: "permissions" is a field of the manifest, and not translated */
            mf_set_error_invalid_manifest(error, "%s", _("permissions: entry is not a string"));
            return FALSE;
        }

        permission = mf_permission_from_string(sd_json_variant_string(entry));
        if (permission == MF_PERMISSION_UNKNOWN)
            g_ptr_array_add(priv->unknown_permissions, g_strdup(sd_json_variant_string(entry)));
        else
            priv->permissions |= 1u << permission;
    }

    return TRUE;
}

/**
 * mf_manifest_parse_json:
 */
static gboolean
mf_manifest_parse_json(MfManifest *manifest, GError **error)
{
    MfManifestPrivate *priv = GET_PRIVATE(manifest);
    g_autofree char *text = NULL;
    g_autoptr(sd_json_variant) jo = NULL;
    sd_json_variant *app;
    sd_json_variant *integration;
    const char *data;
    uint64_t format;
    gsize size;

    /* sd_json_parse needs a terminated string */
    data = g_bytes_get_data(priv->data, &size);
    text = g_strndup(data, size);
    jo = mf_json_parse_object(text, NULL, MF_ERROR_INVALID_MANIFEST, error);
    if (jo == NULL)
        return FALSE;

    if (!FIELD_UNSIGNED(jo, NULL, "format", &format, error))
        return FALSE;
    if (format != (uint64_t) MANIFEST_FORMAT) {
        mf_set_error_invalid_manifest(error,
                                      /* TRANSLATORS: "format" is a manifest field, and not translated */
                                      _("format %" PRIu64 " is not supported, only %d is"),
                                      format,
                                      MANIFEST_FORMAT);
        return FALSE;
    }

    priv->component_id = mf_manifest_json_get_component_id(jo, NULL, error);
    if (priv->component_id == NULL)
        return FALSE;

    priv->kind = mf_manifest_json_get_kind(jo, NULL, error);
    if (priv->kind == MF_COMPONENT_KIND_UNKNOWN)
        return FALSE;

    priv->version = mf_manifest_json_get_version(jo, NULL, error);
    if (priv->version == NULL)
        return FALSE;
    if (!FIELD_UNSIGNED(jo, NULL, "revision", &priv->revision, error))
        return FALSE;
    priv->architecture = g_strdup(FIELD_STRING(jo, NULL, "architecture", error));
    if (priv->architecture == NULL)
        return FALSE;
    if (!mf_architecture_is_valid(priv->architecture)) {
        mf_set_error_invalid_manifest(error, "%s", _("architecture is not valid"));
        return FALSE;
    }
    if (!FIELD_BOOLEAN(jo, NULL, "sdk", &priv->sdk, error))
        return FALSE;
    priv->identity = g_strdup(FIELD_HASH(jo, NULL, "identity", error));
    if (priv->identity == NULL)
        return FALSE;
    priv->content_hash = g_strdup(FIELD_HASH(jo, NULL, "content-hash", error));
    if (priv->content_hash == NULL)
        return FALSE;

    if (!mf_manifest_json_parse_base(jo, "base", &priv->base, error))
        return FALSE;
    /* only a core has no base, and the SDK of a core has that core as its base */
    if (priv->sdk && priv->base.id == NULL) {
        mf_set_error_invalid_manifest(error, "%s", _("an SDK must name the component it extends"));
        return FALSE;
    }
    if (priv->kind != MF_COMPONENT_KIND_CORE && priv->base.id == NULL) {
        mf_set_error_invalid_manifest(error,
                                      /* TRANSLATORS: %s is the kind of the component, like "app" or "kit" */
                                      _("a %s must name the layer it is built on"),
                                      mf_component_kind_to_string(priv->kind));
        return FALSE;
    }
    /* the component that an SDK extends is never an SDK, and for all other components it makes
     * no difference whether the base is an SDK */
    if (priv->base.id != NULL &&
        !mf_component_kind_can_build_on(priv->kind, priv->sdk, priv->base.kind, FALSE)) {
        mf_set_error_invalid_manifest(
            error,
            /* TRANSLATORS: the kind and ID of the component, then the kind and ID of its base */
            _("%s%s %s can not build on %s %s"),
            mf_component_kind_to_string(priv->kind),
            priv->sdk ? " SDK" : "",
            priv->component_id,
            mf_component_kind_to_string(priv->base.kind),
            priv->base.id);
        return FALSE;
    }

    if (!mf_manifest_json_parse_base(jo, "base-sdk", &priv->base_sdk, error))
        return FALSE;
    if (!mf_manifest_check_base_sdk(priv, error))
        return FALSE;

    if (!mf_manifest_json_parse_image(jo, priv, error))
        return FALSE;

    app = sd_json_variant_by_key(jo, "app");
    if (app != NULL && !sd_json_variant_is_null(app)) {
        if (!sd_json_variant_is_object(app)) {
            /* TRANSLATORS: "app" is a section of the manifest, and not translated */
            mf_set_error_invalid_manifest(error, "%s", _("app is not an object"));
            return FALSE;
        }
        priv->command = g_strdup(FIELD_STRING(app, "app", "command", error));
        if (priv->command == NULL)
            return FALSE;
        if (!mf_program_name_is_valid(priv->command)) {
            g_autofree char *shown = g_strescape(priv->command, NULL);

            /* TRANSLATORS: "app" and "command" are a section and field of the manifest, not translated */
            mf_set_error_invalid_manifest(error, _("app: command is not a plain name: %s"), shown);
            return FALSE;
        }
    } else if (priv->kind == MF_COMPONENT_KIND_APP && !priv->sdk) {
        mf_set_error_invalid_manifest(error, "%s", _("an app must have a command to run"));
        return FALSE;
    }

    integration = sd_json_variant_by_key(jo, "integration");
    if (integration != NULL && !sd_json_variant_is_null(integration)) {
        priv->integration = mf_integration_new_from_json(integration, error);
        if (priv->integration == NULL)
            return FALSE;
    }

    return mf_manifest_json_parse_permissions(jo, priv, error);
}

/**
 * mf_manifest_new_from_bytes:
 * @bytes: the manifest, as JSON
 * @error: a #GError
 *
 * Parse a manifest, and check that it is well-formed.
 *
 * Returns: (transfer full): the manifest, or %NULL with %MF_ERROR_INVALID_MANIFEST
 */
MfManifest *
mf_manifest_new_from_bytes(GBytes *bytes, GError **error)
{
    g_autoptr(MfManifest) manifest = NULL;
    MfManifestPrivate *priv;

    g_return_val_if_fail(bytes != NULL, NULL);

    manifest = g_object_new(MF_TYPE_MANIFEST, NULL);
    priv = GET_PRIVATE(manifest);
    priv->data = g_bytes_ref(bytes);
    if (!mf_manifest_parse_json(manifest, error))
        return NULL;

    return g_steal_pointer(&manifest);
}

/**
 * mf_manifest_new_from_data:
 * @data: (array length=length) (element-type guint8): the manifest, as JSON
 * @length: the length of @data, or -1 if it is NUL terminated
 * @error: a #GError
 *
 * Parse a manifest, like mf_manifest_new_from_bytes().
 *
 * Returns: (transfer full): the manifest, or %NULL with %MF_ERROR_INVALID_MANIFEST
 */
MfManifest *
mf_manifest_new_from_data(const char *data, ssize_t length, GError **error)
{
    g_autoptr(GBytes) bytes = NULL;

    g_return_val_if_fail(data != NULL, NULL);

    bytes = g_bytes_new(data, length < 0 ? strlen(data) : (size_t) length);
    return mf_manifest_new_from_bytes(bytes, error);
}

/**
 * mf_manifest_new_from_file:
 * @path: a manifest file
 * @error: a #GError
 *
 * Read and parse a manifest, like mf_manifest_new_from_bytes(). The
 * error message is prefixed with the file name.
 *
 * Returns: (transfer full): the manifest, or %NULL
 */
MfManifest *
mf_manifest_new_from_file(const char *path, GError **error)
{
    g_autofree char *data = NULL;
    MfManifest *manifest;
    gsize length;

    g_return_val_if_fail(path != NULL, NULL);

    if (!g_file_get_contents(path, &data, &length, error))
        return NULL;
    manifest = mf_manifest_new_from_data(data, (ssize_t) length, error);
    if (manifest == NULL)
        g_prefix_error(error, "%s: ", path);
    return manifest;
}

/**
 * mf_manifest_get_data:
 * @manifest: an #MfManifest
 *
 * Returns: (transfer none): the plain manifest data that was read
 */
GBytes *
mf_manifest_get_data(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    auto priv = GET_PRIVATE(manifest);
    return priv->data;
}

/**
 * mf_manifest_get_component_id:
 * @manifest: an #MfManifest
 *
 * Returns: the identifier of the component the image belongs to
 */
const char *
mf_manifest_get_component_id(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    auto priv = GET_PRIVATE(manifest);
    return priv->component_id;
}

/**
 * mf_manifest_get_kind:
 * @manifest: an #MfManifest
 *
 * Returns: the kind of the component
 */
MfComponentKind
mf_manifest_get_kind(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), MF_COMPONENT_KIND_UNKNOWN);
    auto priv = GET_PRIVATE(manifest);
    return priv->kind;
}

/**
 * mf_manifest_get_version:
 * @manifest: an #MfManifest
 *
 * Returns: the version of the component
 */
const char *
mf_manifest_get_version(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    auto priv = GET_PRIVATE(manifest);
    return priv->version;
}

/**
 * mf_manifest_get_revision:
 * @manifest: an #MfManifest
 *
 * Returns: the revision of the version, which tells rebuilds of it apart
 */
uint64_t
mf_manifest_get_revision(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), 0);
    auto priv = GET_PRIVATE(manifest);
    return priv->revision;
}

/**
 * mf_manifest_get_architecture:
 * @manifest: an #MfManifest
 *
 * Returns: the architecture of the image, e.g. `x86_64`
 */
const char *
mf_manifest_get_architecture(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    auto priv = GET_PRIVATE(manifest);
    return priv->architecture;
}

/**
 * mf_manifest_is_sdk:
 * @manifest: an #MfManifest
 *
 * Returns: %TRUE if the image is an SDK, which contains the development files of one layer
 */
gboolean
mf_manifest_is_sdk(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), FALSE);
    auto priv = GET_PRIVATE(manifest);
    return priv->sdk;
}

/**
 * mf_manifest_get_identity:
 * @manifest: an #MfManifest
 *
 * Returns: the identity hash of the image, as hex
 */
const char *
mf_manifest_get_identity(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    auto priv = GET_PRIVATE(manifest);
    return priv->identity;
}

/**
 * mf_manifest_get_content_hash:
 * @manifest: an #MfManifest
 *
 * Returns: the content hash of the image, unique to one exact build, as hex
 */
const char *
mf_manifest_get_content_hash(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    auto priv = GET_PRIVATE(manifest);
    return priv->content_hash;
}

/**
 * mf_manifest_get_base_id:
 * @manifest: an #MfManifest
 *
 * Returns: (nullable): the identifier of the component the image was built on,
 *          or %NULL if the image is a core
 */
const char *
mf_manifest_get_base_id(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    auto priv = GET_PRIVATE(manifest);
    return priv->base.id;
}

/**
 * mf_manifest_get_base_kind:
 * @manifest: an #MfManifest
 *
 * Returns: the kind of the component the image was built on,
 *          or %MF_COMPONENT_KIND_UNKNOWN if the image is a core
 */
MfComponentKind
mf_manifest_get_base_kind(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), MF_COMPONENT_KIND_UNKNOWN);
    auto priv = GET_PRIVATE(manifest);
    return priv->base.kind;
}

/**
 * mf_manifest_get_base_version:
 * @manifest: an #MfManifest
 *
 * Returns: (nullable): the version of the base the image was built on
 */
const char *
mf_manifest_get_base_version(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    auto priv = GET_PRIVATE(manifest);
    return priv->base.version;
}

/**
 * mf_manifest_get_base_revision:
 * @manifest: an #MfManifest
 *
 * Returns: the revision of the base the image was built on, or 0
 */
uint64_t
mf_manifest_get_base_revision(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), 0);
    auto priv = GET_PRIVATE(manifest);
    return priv->base.revision;
}

/**
 * mf_manifest_get_base_identity:
 * @manifest: an #MfManifest
 *
 * Returns: (nullable): the identity of the base
 */
const char *
mf_manifest_get_base_identity(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    auto priv = GET_PRIVATE(manifest);
    return priv->base.identity;
}

/**
 * mf_manifest_get_base_content_hash:
 * @manifest: an #MfManifest
 *
 * Returns: (nullable): the content hash of the exact base the image was built on
 */
const char *
mf_manifest_get_base_content_hash(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    auto priv = GET_PRIVATE(manifest);
    return priv->base.content_hash;
}

/**
 * mf_manifest_get_base_sdk_id:
 * @manifest: an #MfManifest
 *
 * An SDK only contains the development files of its own layer. An SDK that
 * extends a platform, a kit or an application therefore references the SDK
 * of the layer below in its manifest.
 *
 * Returns: (nullable): the identifier of the SDK of the layer below,
 *          or %NULL if the image has none
 */
const char *
mf_manifest_get_base_sdk_id(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    auto priv = GET_PRIVATE(manifest);
    return priv->base_sdk.id;
}

/**
 * mf_manifest_get_base_sdk_kind:
 * @manifest: an #MfManifest
 *
 * Returns: the kind of the SDK of the layer below,
 *          or %MF_COMPONENT_KIND_UNKNOWN if the image has none
 */
MfComponentKind
mf_manifest_get_base_sdk_kind(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), MF_COMPONENT_KIND_UNKNOWN);
    auto priv = GET_PRIVATE(manifest);
    return priv->base_sdk.kind;
}

/**
 * mf_manifest_get_base_sdk_version:
 * @manifest: an #MfManifest
 *
 * Returns: (nullable): the version of the SDK of the layer below
 */
const char *
mf_manifest_get_base_sdk_version(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    auto priv = GET_PRIVATE(manifest);
    return priv->base_sdk.version;
}

/**
 * mf_manifest_get_base_sdk_revision:
 * @manifest: an #MfManifest
 *
 * Returns: the revision of the SDK of the layer below, or 0
 */
uint64_t
mf_manifest_get_base_sdk_revision(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), 0);
    auto priv = GET_PRIVATE(manifest);
    return priv->base_sdk.revision;
}

/**
 * mf_manifest_get_base_sdk_identity:
 * @manifest: an #MfManifest
 *
 * Returns: (nullable): the identity of the SDK of the layer below
 */
const char *
mf_manifest_get_base_sdk_identity(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    auto priv = GET_PRIVATE(manifest);
    return priv->base_sdk.identity;
}

/**
 * mf_manifest_get_base_sdk_content_hash:
 * @manifest: an #MfManifest
 *
 * Returns: (nullable): the content hash of the SDK of the layer below
 */
const char *
mf_manifest_get_base_sdk_content_hash(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    auto priv = GET_PRIVATE(manifest);
    return priv->base_sdk.content_hash;
}

/**
 * mf_manifest_get_image_file:
 * @manifest: an #MfManifest
 *
 * Returns: the name of the image file, beside the manifest
 */
const char *
mf_manifest_get_image_file(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    auto priv = GET_PRIVATE(manifest);
    return priv->image_file;
}

/**
 * mf_manifest_get_image_size:
 * @manifest: an #MfManifest
 *
 * Returns: the size of the image file in bytes, or 0
 */
uint64_t
mf_manifest_get_image_size(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), 0);
    auto priv = GET_PRIVATE(manifest);
    return priv->image_size;
}

/**
 * mf_manifest_get_image_checksum:
 * @manifest: an #MfManifest
 * @kind: the kind of checksum
 *
 * Returns: the checksum of the image file, as hex
 */
const char *
mf_manifest_get_image_checksum(MfManifest *manifest, MfChecksumKind kind)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    g_return_val_if_fail(kind > MF_CHECKSUM_KIND_UNKNOWN && kind < MF_CHECKSUM_KIND_LAST, NULL);
    auto priv = GET_PRIVATE(manifest);
    return priv->image_checksums[kind];
}

/**
 * mf_manifest_needs_exact_base:
 *
 * A platform and an SDK need the exact version and revision of the base they
 * were built against. All other components work with any version that
 * provides the identity of their base.
 *
 * Returns: %TRUE if @manifest needs the exact version and revision of its base
 */
gboolean
mf_manifest_needs_exact_base(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), FALSE);
    return mf_manifest_is_sdk(manifest) || mf_manifest_get_kind(manifest) == MF_COMPONENT_KIND_PLATFORM;
}

/**
 * mf_manifest_image_matches:
 * @entry: the size and hashes of a file
 *
 * Returns: %TRUE if @entry describes the image of @manifest
 */
gboolean
mf_manifest_image_matches(MfManifest *manifest, const MfFileEntry *entry)
{
    auto priv = GET_PRIVATE(manifest);

    return entry->size == priv->image_size &&
           g_str_equal(entry->sha256, priv->image_checksums[MF_CHECKSUM_KIND_SHA256]) &&
           g_str_equal(entry->blake3, priv->image_checksums[MF_CHECKSUM_KIND_BLAKE3]);
}

/**
 * mf_manifest_get_verity_hash:
 * @manifest: an #MfManifest
 *
 * Returns: the dm-verity root hash of the image
 */
const char *
mf_manifest_get_verity_hash(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    auto priv = GET_PRIVATE(manifest);
    return priv->verity_hash;
}

/**
 * mf_manifest_get_data_offset:
 * @manifest: an #MfManifest
 *
 * Returns: the offset of the filesystem partition in the image file, in bytes
 */
uint64_t
mf_manifest_get_data_offset(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), 0);
    auto priv = GET_PRIVATE(manifest);
    return priv->data_offset;
}

/**
 * mf_manifest_get_data_size:
 * @manifest: an #MfManifest
 *
 * Returns: the size of the filesystem partition, in bytes, or 0 if unknown
 */
uint64_t
mf_manifest_get_data_size(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), 0);
    auto priv = GET_PRIVATE(manifest);
    return priv->data_size;
}

/**
 * mf_manifest_get_hash_offset:
 * @manifest: an #MfManifest
 *
 * Returns: the offset of the dm-verity hash partition in the image file, in bytes
 */
uint64_t
mf_manifest_get_hash_offset(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), 0);
    auto priv = GET_PRIVATE(manifest);
    return priv->hash_offset;
}

/**
 * mf_manifest_get_hash_size:
 * @manifest: an #MfManifest
 *
 * Returns: the size of the hash partition, in bytes, or 0 if unknown
 */
uint64_t
mf_manifest_get_hash_size(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), 0);
    auto priv = GET_PRIVATE(manifest);
    return priv->hash_size;
}

/**
 * mf_manifest_get_command:
 * @manifest: an #MfManifest
 *
 * Returns: (nullable): the program in `/app/bin` that is run when the application is launched
 */
const char *
mf_manifest_get_command(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    auto priv = GET_PRIVATE(manifest);
    return priv->command;
}

/**
 * mf_manifest_get_integration:
 * @manifest: an #MfManifest
 *
 * Returns: (transfer none) (nullable): the integration of the image,
 *   or %NULL if the manifest has no integration section
 */
MfIntegration *
mf_manifest_get_integration(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    auto priv = GET_PRIVATE(manifest);
    return priv->integration;
}

/**
 * mf_manifest_check_integration:
 * @manifest: an #MfManifest
 * @error: a #GError
 *
 * Check that the image is allowed to install the files that its integration
 * section lists. A manifest without an integration section always passes.
 *
 * We do not run this check when a manifest is parsed, so the manifests of
 * installed versions can still be read if the rules change. Code that builds
 * or installs an image has to call this function instead.
 *
 * Returns: %TRUE if the section is fine, or %FALSE with %MF_ERROR_INVALID_MANIFEST
 */
gboolean
mf_manifest_check_integration(MfManifest *manifest, GError **error)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), FALSE);
    auto priv = GET_PRIVATE(manifest);

    if (priv->integration == NULL)
        return TRUE;
    /* only applications can be launched and are allowed to export files to the host */
    if (priv->kind != MF_COMPONENT_KIND_APP || priv->sdk) {
        mf_set_error_invalid_manifest(error, "%s", _("integration: only applications can use this"));
        return FALSE;
    }

    return mf_integration_check(priv->integration, priv->component_id, error);
}

/**
 * mf_manifest_has_permission:
 * @manifest: an #MfManifest
 * @permission: a #MfPermission
 *
 * Returns: %TRUE if the manifest requests @permission
 */
gboolean
mf_manifest_has_permission(MfManifest *manifest, MfPermission permission)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), FALSE);
    g_return_val_if_fail(permission > MF_PERMISSION_UNKNOWN && permission < MF_PERMISSION_LAST, FALSE);
    auto priv = GET_PRIVATE(manifest);
    return (priv->permissions & (1u << permission)) != 0;
}

/**
 * mf_manifest_check_permissions:
 * @manifest: an #MfManifest
 * @error: a #GError
 *
 * Check that the image is allowed to have the permissions that its manifest
 * requests. A manifest that requests no permissions always passes.
 *
 * Like mf_manifest_check_integration(), this check is not run when a manifest
 * is parsed.
 *
 * Returns: %TRUE if the permissions are fine, or %FALSE with %MF_ERROR_INVALID_MANIFEST
 */
gboolean
mf_manifest_check_permissions(MfManifest *manifest, GError **error)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), FALSE);
    auto priv = GET_PRIVATE(manifest);

    if (priv->permissions == 0 && priv->unknown_permissions->len == 0)
        return TRUE;
    /* only applications are launched, so other components can not use a permission */
    if (priv->kind != MF_COMPONENT_KIND_APP || priv->sdk) {
        mf_set_error_invalid_manifest(error, "%s", _("permissions: only applications can use this"));
        return FALSE;
    }
    if (priv->unknown_permissions->len > 0) {
        g_autofree char *shown = g_strescape(g_ptr_array_index(priv->unknown_permissions, 0), NULL);

        /* TRANSLATORS: "permissions" is a field of the manifest, and not translated, %s is an entry of it */
        mf_set_error_invalid_manifest(error, _("permissions: %s is not known"), shown);
        return FALSE;
    }

    return TRUE;
}
