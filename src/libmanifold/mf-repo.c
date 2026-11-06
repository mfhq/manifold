/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mf-repo-private.h"

#include <string.h>
#include <glib/gi18n-lib.h>
#include <appstream.h>
#include <systemd/sd-json.h>
#include <zstd.h>

#include "mf-json-private.h"
#include "mf-remote-private.h"
#include "mf-sdautoptr-private.h"
#include "mf-utils.h"

/* the largest decompressed size of a root index, and of the other indexes and the catalog */
constexpr gsize ROOT_INDEX_SIZE_MAX = 16 * 1024 * 1024;
constexpr gsize INDEX_SIZE_MAX = 256 * 1024 * 1024;

/* the largest size of a manifest and of an icon */
constexpr guint64 MANIFEST_SIZE_MAX = 16 * 1024 * 1024;
constexpr guint64 ICON_SIZE_MAX = 2 * 1024 * 1024;

#define MANIFEST_SUFFIX ".json.zst"

/**
 * mf_zstd_decompress_bytes:
 * @data: data compressed with zstd, in one or more frames
 * @size_max: the largest size the decompressed data may have
 * @name: the name of the data for error messages
 * @code: the #MfError code to fail with
 *
 * Decompress zstd data. The result is followed by a NUL byte that is not
 * part of the data, so it can be read as a string.
 *
 * NOTE: This function is not in mf-utils.c, because manifold-rund links that
 * file and must not depend on libzstd.
 *
 * Returns: (transfer full): the decompressed data, or %NULL if @data is not
 *   valid or decompresses to more than @size_max bytes
 */
GBytes *
mf_zstd_decompress_bytes(GBytes *data, gsize size_max, const char *name, MfError code, GError **error)
{
    g_autoptr(GByteArray) out = g_byte_array_new();
    ZSTD_DStream *stream = ZSTD_createDStream();
    ZSTD_inBuffer in = {0};
    size_t buffer_size = ZSTD_DStreamOutSize();
    size_t last = 0;
    guint8 *bytes;
    gsize size;

    if (stream == NULL) {
        g_set_error_literal(error, MF_ERROR, code, _("Unable to set up zstd"));
        return NULL;
    }

    in.src = g_bytes_get_data(data, &in.size);
    for (;;) {
        ZSTD_outBuffer piece;

        g_byte_array_set_size(out, out->len + buffer_size);
        piece = (ZSTD_outBuffer){out->data + out->len - buffer_size, buffer_size, 0};
        last = ZSTD_decompressStream(stream, &piece, &in);
        g_byte_array_set_size(out, out->len - buffer_size + piece.pos);
        if (ZSTD_isError(last) || out->len > size_max)
            break;
        /* all input is read and the output buffer is not full, so we have all the data */
        if (in.pos == in.size && piece.pos < piece.size)
            break;
    }
    ZSTD_freeDStream(stream);

    if (!ZSTD_isError(last) && out->len > size_max) {
        g_set_error(error, MF_ERROR, code, _("%s is too large"), name);
        return NULL;
    }
    /* the decompressor expects more input, so the last frame is truncated */
    if (ZSTD_isError(last) || last != 0) {
        g_set_error(error, MF_ERROR, code, _("%s is not valid zstd data"), name);
        return NULL;
    }

    g_byte_array_append(out, (const guint8 *) "", 1);
    bytes = g_byte_array_steal(out, &size);
    return g_bytes_new_take(bytes, size - 1);
}

/**
 * mf_repo_open_file:
 * @entry: the size and hashes that the file must have
 *
 * Verify a file of the repository against its entry, and decompress it.
 *
 * Returns: (transfer full): the decompressed file, followed by a NUL byte, or %NULL
 */
static GBytes *
mf_repo_open_file(GBytes *data, const MfFileEntry *entry, const char *name, gsize size_max, GError **error)
{
    MfFileEntry actual;

    mf_file_entry_set_for_bytes(&actual, data);
    if (!mf_file_entry_equal(&actual, entry)) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_VERIFICATION_FAILED,
                    _("%s checksums do not match the expected values"),
                    name);
        return NULL;
    }

    return mf_zstd_decompress_bytes(data, size_max, name, MF_ERROR_INVALID_REPOSITORY, error);
}

static void mf_repo_set_invalid(GError **error, const char *name, const char *format, ...) G_GNUC_PRINTF(3, 4);

/**
 * mf_repo_set_invalid:
 *
 * Set an error for a file of the repository that has invalid content.
 */
static void
mf_repo_set_invalid(GError **error, const char *name, const char *format, ...)
{
    g_autofree char *message = NULL;
    va_list args;

    va_start(args, format);
    message = g_strdup_vprintf(format, args);
    va_end(args);
    g_set_error(error, MF_ERROR, MF_ERROR_INVALID_REPOSITORY, "%s: %s", name, message);
}

/**
 * mf_repo_get_object_field:
 *
 * Returns: (transfer none): the object at @key, or %NULL
 */
static sd_json_variant *
mf_repo_get_object_field(sd_json_variant *object, const char *name, const char *key, GError **error)
{
    sd_json_variant *value = sd_json_variant_by_key(object, key);

    if (value == NULL || !sd_json_variant_is_object(value)) {
        /* TRANSLATORS: %s is the name of a field of a JSON document */
        mf_repo_set_invalid(error, name, _("%s is missing or not an object"), key);
        return NULL;
    }
    return value;
}

/**
 * mf_repo_get_date_field:
 *
 * Returns: (transfer full): the RFC 3339 date at @key, or %NULL
 */
static GDateTime *
mf_repo_get_date_field(sd_json_variant *object, const char *name, const char *key, GError **error)
{
    const char *value = mf_json_get_string_field(object, name, key, MF_ERROR_INVALID_REPOSITORY, error);
    GDateTime *date;

    if (value == NULL)
        return NULL;
    date = g_date_time_new_from_iso8601(value, NULL);
    if (date == NULL)
        /* TRANSLATORS: %s is the name of a field of a JSON document */
        mf_repo_set_invalid(error, name, _("%s is not a valid date"), key);
    return date;
}

/**
 * mf_repo_get_display_text_field:
 *
 * Returns: (transfer none): the text at @key, which is safe to show to the user, or %NULL
 */
static const char *
mf_repo_get_display_text_field(sd_json_variant *object, const char *name, const char *key, GError **error)
{
    const char *value = mf_json_get_string_field(object, name, key, MF_ERROR_INVALID_REPOSITORY, error);

    if (value == NULL)
        return NULL;
    if (!mf_display_text_is_valid(value)) {
        /* TRANSLATORS: %s is the name of a field of a JSON document */
        mf_repo_set_invalid(error, name, _("%s is not a single line of printable text"), key);
        return NULL;
    }
    return value;
}

/*
 * The root index
 */

static void
mf_repo_architecture_free(MfRepoArchitecture *arch)
{
    g_free(arch->name);
    g_free(arch);
}

static void
mf_repo_channel_free(MfRepoChannel *channel)
{
    g_free(channel->name);
    g_free(channel->image_url_prefix);
    g_ptr_array_unref(channel->architectures);
    g_free(channel);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfRepoChannel, mf_repo_channel_free)

/**
 * mf_repo_root_index_free:
 */
void
mf_repo_root_index_free(MfRepoRootIndex *index)
{
    if (index == NULL)
        return;
    g_free(index->id);
    g_free(index->summary);
    g_free(index->publisher_name);
    g_free(index->publisher_email);
    g_clear_pointer(&index->date, g_date_time_unref);
    g_clear_pointer(&index->valid_until, g_date_time_unref);
    g_clear_pointer(&index->channels, g_ptr_array_unref);
    g_free(index->key_fingerprint);
    g_free(index);
}

/**
 * mf_repo_read_architecture:
 *
 * Read the file entries of a channel for one architecture.
 */
static MfRepoArchitecture *
mf_repo_read_architecture(const char *arch_name, sd_json_variant *object, GError **error)
{
    g_autofree MfRepoArchitecture *repo_arch = g_new0(MfRepoArchitecture, 1);
    const struct {
        const char *name;
        MfFileEntry *entry;
    } files[] = {
        {MF_REPO_COMPONENT_INDEX_NAME, &repo_arch->component_index},
        {MF_REPO_CATALOG_NAME,         &repo_arch->catalog        },
        {MF_REPO_ICON_INDEX_NAME,      &repo_arch->icon_index     },
    };

    if (!mf_architecture_is_valid(arch_name) || !sd_json_variant_is_object(object)) {
        mf_repo_set_invalid(error, MF_REPO_ROOT_INDEX_NAME, _("%s is not a valid architecture"), arch_name);
        return NULL;
    }
    for (gsize i = 0; i < G_N_ELEMENTS(files); i++) {
        sd_json_variant *file = mf_repo_get_object_field(object,
                                                         MF_REPO_ROOT_INDEX_NAME,
                                                         files[i].name,
                                                         error);

        if (file == NULL || !mf_json_get_file_entry(file,
                                                    MF_REPO_ROOT_INDEX_NAME,
                                                    MF_ERROR_INVALID_REPOSITORY,
                                                    files[i].entry,
                                                    error))
            return NULL;
    }

    repo_arch->name = g_strdup(arch_name);
    return g_steal_pointer(&repo_arch);
}

/**
 * mf_repo_read_channel:
 */
static MfRepoChannel *
mf_repo_read_channel(const char *channel_name, sd_json_variant *object, GError **error)
{
    g_autoptr(MfRepoChannel) channel = g_new0(MfRepoChannel, 1);
    sd_json_variant *prefix;
    sd_json_variant *architectures;

    channel->name = g_strdup(channel_name);
    channel->architectures = g_ptr_array_new_with_free_func((GDestroyNotify) mf_repo_architecture_free);
    if (!mf_remote_channel_is_valid(channel_name) || !sd_json_variant_is_object(object)) {
        mf_repo_set_invalid(error, MF_REPO_ROOT_INDEX_NAME, _("%s is not a valid channel"), channel_name);
        return NULL;
    }

    /* a client appends "/" and the name of an image to the prefix */
    prefix = sd_json_variant_by_key(object, "image-url-prefix");
    if (prefix != NULL && !sd_json_variant_is_null(prefix)) {
        const char *value = sd_json_variant_is_string(prefix) ? sd_json_variant_string(prefix) : NULL;

        if (value == NULL || !g_str_has_prefix(value, "https://") || g_str_has_suffix(value, "/") ||
            strpbrk(value, "?#") != NULL) {
            mf_repo_set_invalid(error,
                                MF_REPO_ROOT_INDEX_NAME,
                                _("the image-url-prefix of %s is not valid"),
                                channel_name);
            return NULL;
        }
        channel->image_url_prefix = g_strdup(value);
    }

    architectures = mf_repo_get_object_field(object, MF_REPO_ROOT_INDEX_NAME, "architectures", error);
    if (architectures == NULL)
        return NULL;
    for (size_t i = 0; i < sd_json_variant_elements(architectures); i += 2) {
        const char *arch_name = sd_json_variant_string(sd_json_variant_by_index(architectures, i));
        MfRepoArchitecture *arch = mf_repo_read_architecture(arch_name,
                                                             sd_json_variant_by_index(architectures, i + 1),
                                                             error);

        if (arch == NULL)
            return NULL;
        g_ptr_array_add(channel->architectures, arch);
    }

    return g_steal_pointer(&channel);
}

/**
 * mf_repo_read_publisher:
 *
 * Read the publisher of the repository. It is optional, but if it is set, it must have a name.
 */
static gboolean
mf_repo_read_publisher(MfRepoRootIndex *index, sd_json_variant *root, GError **error)
{
    sd_json_variant *publisher = sd_json_variant_by_key(root, "publisher");
    sd_json_variant *email;
    const char *name;

    if (publisher == NULL || sd_json_variant_is_null(publisher))
        return TRUE;
    if (!sd_json_variant_is_object(publisher)) {
        mf_repo_set_invalid(error, MF_REPO_ROOT_INDEX_NAME, _("%s is missing or not an object"), "publisher");
        return FALSE;
    }
    name = mf_repo_get_display_text_field(publisher, MF_REPO_ROOT_INDEX_NAME, "name", error);
    if (name == NULL)
        return FALSE;
    email = sd_json_variant_by_key(publisher, "email");
    if (email != NULL && !sd_json_variant_is_null(email) && !sd_json_variant_is_string(email)) {
        mf_repo_set_invalid(error, MF_REPO_ROOT_INDEX_NAME, _("%s is not a string"), "email");
        return FALSE;
    }

    index->publisher_name = g_strdup(name);
    if (email != NULL && sd_json_variant_is_string(email)) {
        const char *address = mf_repo_get_display_text_field(publisher,
                                                             MF_REPO_ROOT_INDEX_NAME,
                                                             "email",
                                                             error);

        if (address == NULL)
            return FALSE;
        index->publisher_email = g_strdup(address);
    }
    return TRUE;
}

/**
 * mf_repo_root_index_parse:
 * @data: a root index, as the repository serves it
 *
 * Returns: (transfer full): the JSON object of the root index, or %NULL
 */
static sd_json_variant *
mf_repo_root_index_parse(GBytes *data, GError **error)
{
    g_autoptr(GBytes) text = mf_zstd_decompress_bytes(data,
                                                      ROOT_INDEX_SIZE_MAX,
                                                      MF_REPO_ROOT_INDEX_NAME,
                                                      MF_ERROR_INVALID_REPOSITORY,
                                                      error);

    if (text == NULL)
        return NULL;
    return mf_json_parse_object(g_bytes_get_data(text, NULL),
                                MF_REPO_ROOT_INDEX_NAME,
                                MF_ERROR_INVALID_REPOSITORY,
                                error);
}

/**
 * mf_repo_root_index_read:
 * @fingerprint: (transfer full) (nullable): the fingerprint of the key that signed the index
 *
 * Parse a root index. Its repository ID has to be @remote_id.
 *
 * Returns: (transfer full): the root index, or %NULL
 */
static MfRepoRootIndex *
mf_repo_root_index_read(GBytes *data, const char *remote_id, char *fingerprint, GError **error)
{
    g_autoptr(MfRepoRootIndex) index = g_new0(MfRepoRootIndex, 1);
    g_autoptr(sd_json_variant) root = NULL;
    sd_json_variant *channels;
    const char *value;

    index->key_fingerprint = fingerprint;
    root = mf_repo_root_index_parse(data, error);
    if (root == NULL)
        return NULL;

    /* the keys are only trusted for this remote, so we refuse the index of a different repository */
    value = mf_json_get_string_field(root, MF_REPO_ROOT_INDEX_NAME, "id", MF_ERROR_INVALID_REPOSITORY, error);
    if (value == NULL)
        return NULL;
    if (!g_str_equal(value, remote_id)) {
        mf_repo_set_invalid(error,
                            MF_REPO_ROOT_INDEX_NAME,
                            _("this is the repository %s, not %s"),
                            value,
                            remote_id);
        return NULL;
    }
    index->id = g_strdup(value);

    value = mf_repo_get_display_text_field(root, MF_REPO_ROOT_INDEX_NAME, "summary", error);
    if (value == NULL)
        return NULL;
    index->summary = g_strdup(value);
    if (!mf_repo_read_publisher(index, root, error))
        return NULL;
    if (!mf_json_get_unsigned_field(root,
                                    MF_REPO_ROOT_INDEX_NAME,
                                    "serial",
                                    MF_ERROR_INVALID_REPOSITORY,
                                    &index->serial,
                                    error))
        return NULL;
    index->date = mf_repo_get_date_field(root, MF_REPO_ROOT_INDEX_NAME, "date", error);
    if (index->date == NULL)
        return NULL;
    index->valid_until = mf_repo_get_date_field(root, MF_REPO_ROOT_INDEX_NAME, "valid-until", error);
    if (index->valid_until == NULL)
        return NULL;

    index->channels = g_ptr_array_new_with_free_func((GDestroyNotify) mf_repo_channel_free);
    channels = mf_repo_get_object_field(root, MF_REPO_ROOT_INDEX_NAME, "channels", error);
    if (channels == NULL)
        return NULL;
    for (size_t i = 0; i < sd_json_variant_elements(channels); i += 2) {
        const char *channel_name = sd_json_variant_string(sd_json_variant_by_index(channels, i));
        MfRepoChannel *channel = mf_repo_read_channel(channel_name,
                                                      sd_json_variant_by_index(channels, i + 1),
                                                      error);

        if (channel == NULL)
            return NULL;
        g_ptr_array_add(index->channels, channel);
    }

    return g_steal_pointer(&index);
}

/**
 * mf_repo_root_index_new_from_signed:
 * @data: the root index, as the repository serves it
 * @signature: the signature of the root index
 * @keys: the keys that are trusted to sign the repository
 * @remote_id: the ID that the repository must have
 *
 * Verify the signature of a root index, and parse it.
 *
 * Returns: (transfer full): the root index, or %NULL
 */
MfRepoRootIndex *
mf_repo_root_index_new_from_signed(GBytes *data,
                                   GBytes *signature,
                                   MfTrustedKeys *keys,
                                   const char *remote_id,
                                   GError **error)
{
    char *fingerprint = NULL;

    if (mf_trusted_keys_is_empty(keys)) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_VERIFICATION_FAILED,
                    _("No key is trusted to sign the repository %s"),
                    remote_id);
        return NULL;
    }
    if (!mf_trusted_keys_verify(keys, MF_SIGNATURE_SCOPE_REPO, data, signature, &fingerprint, error))
        return NULL;

    return mf_repo_root_index_read(data, remote_id, fingerprint, error);
}

/**
 * mf_repo_root_index_read_id:
 * @data: a root index, as the repository serves it
 *
 * Read the ID of a repository from its root index, without verifying the
 * signature. We need this when a repository is added as a remote: the public
 * key file that the repository serves is named after the ID. The signature
 * is verified with that key afterwards.
 *
 * Returns: (transfer full): the ID of the repository, or %NULL
 */
char *
mf_repo_root_index_read_id(GBytes *data, GError **error)
{
    g_autoptr(sd_json_variant) root = mf_repo_root_index_parse(data, error);
    const char *value;

    if (root == NULL)
        return NULL;
    value = mf_json_get_string_field(root, MF_REPO_ROOT_INDEX_NAME, "id", MF_ERROR_INVALID_REPOSITORY, error);
    if (value == NULL)
        return NULL;

    /* the ID is used as a file name and a directory name, so it must be valid */
    if (!mf_remote_id_is_valid(value)) {
        mf_repo_set_invalid(error, MF_REPO_ROOT_INDEX_NAME, _("%s is not a valid repository ID"), value);
        return NULL;
    }

    return g_strdup(value);
}

/**
 * mf_repo_root_index_new_from_stored:
 * @data: a cached root index from the store
 * @remote_id: the ID of the remote
 *
 * Parse a cached root index to display information about its repository.
 * The signature is not verified again, so the result must not be used to
 * install anything.
 *
 * Returns: (transfer full): the root index, or %NULL
 */
MfRepoRootIndex *
mf_repo_root_index_new_from_stored(GBytes *data, const char *remote_id, GError **error)
{
    return mf_repo_root_index_read(data, remote_id, NULL, error);
}

/**
 * mf_repo_root_index_find_channel:
 *
 * Returns: (transfer none) (nullable): the channel with the name @name
 */
MfRepoChannel *
mf_repo_root_index_find_channel(MfRepoRootIndex *index, const char *name)
{
    for (guint i = 0; i < index->channels->len; i++) {
        MfRepoChannel *channel = g_ptr_array_index(index->channels, i);

        if (g_str_equal(channel->name, name))
            return channel;
    }
    return NULL;
}

/**
 * mf_repo_channel_find_architecture:
 *
 * Returns: (transfer none) (nullable): the file entries of @channel for @architecture
 */
MfRepoArchitecture *
mf_repo_channel_find_architecture(MfRepoChannel *channel, const char *architecture)
{
    for (guint i = 0; i < channel->architectures->len; i++) {
        MfRepoArchitecture *arch = g_ptr_array_index(channel->architectures, i);

        if (g_str_equal(arch->name, architecture))
            return arch;
    }
    return NULL;
}

/**
 * mf_repo_root_index_is_expired:
 *
 * Returns: %TRUE if the root index is not valid anymore at the time @now
 */
gboolean
mf_repo_root_index_is_expired(MfRepoRootIndex *index, GDateTime *now)
{
    return g_date_time_compare(now, index->valid_until) >= 0;
}

/*
 * The component index
 */

static void
mf_repo_version_free(MfRepoVersion *version)
{
    g_free(version->bundle);
    g_free(version->version);
    g_free(version->manifest_location);
    g_free(version);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfRepoVersion, mf_repo_version_free)

/**
 * mf_repo_component_index_free:
 */
void
mf_repo_component_index_free(MfRepoComponentIndex *index)
{
    if (index == NULL)
        return;
    g_clear_pointer(&index->components, g_hash_table_unref);
    g_free(index);
}

/**
 * mf_repo_read_version:
 *
 * Read a version of a component. Its bundle ID has to match its component and version.
 */
static MfRepoVersion *
mf_repo_read_version(sd_json_variant *object,
                     const char *architecture,
                     const char *component_id,
                     GError **error)
{
    const char *name = MF_REPO_COMPONENT_INDEX_NAME;
    g_autoptr(MfRepoVersion) version = g_new0(MfRepoVersion, 1);
    g_autofree char *expected_bundle = NULL;
    sd_json_variant *manifest;
    sd_json_variant *zck_header;
    const char *value;

    if (!sd_json_variant_is_object(object)) {
        mf_repo_set_invalid(error, name, _("a version of %s is not an object"), component_id);
        return NULL;
    }
    value = mf_json_get_string_field(object, name, "version", MF_ERROR_INVALID_REPOSITORY, error);
    if (value == NULL)
        return NULL;
    if (!mf_version_is_valid(value)) {
        mf_repo_set_invalid(error, name, _("%s has a version that is not valid"), component_id);
        return NULL;
    }
    version->version = g_strdup(value);
    if (!mf_json_get_unsigned_field(object,
                                    name,
                                    "revision",
                                    MF_ERROR_INVALID_REPOSITORY,
                                    &version->revision,
                                    error))
        return NULL;

    value = mf_json_get_string_field(object, name, "bundle", MF_ERROR_INVALID_REPOSITORY, error);
    if (value == NULL)
        return NULL;
    expected_bundle = g_strdup_printf("%s/%s/%s", architecture, component_id, version->version);
    if (!g_str_equal(value, expected_bundle)) {
        mf_repo_set_invalid(error, name, _("the bundle %s is not %s"), value, expected_bundle);
        return NULL;
    }
    version->bundle = g_steal_pointer(&expected_bundle);

    /* the image file is in the same directory as the manifest, and has the same base name */
    manifest = mf_repo_get_object_field(object, name, "manifest", error);
    if (manifest == NULL)
        return NULL;
    value = mf_json_get_string_field(manifest, name, "location", MF_ERROR_INVALID_REPOSITORY, error);
    if (value == NULL)
        return NULL;
    if (!mf_relative_path_is_valid(value) || !g_str_has_suffix(value, MANIFEST_SUFFIX)) {
        mf_repo_set_invalid(error,
                            name,
                            _("the manifest of %s has a location that is not valid"),
                            version->bundle);
        return NULL;
    }
    version->manifest_location = g_strdup(value);
    if (!mf_json_get_file_entry(manifest, name, MF_ERROR_INVALID_REPOSITORY, &version->manifest, error))
        return NULL;
    if (version->manifest.size > MANIFEST_SIZE_MAX) {
        mf_repo_set_invalid(error, name, _("the manifest of %s is too large"), version->bundle);
        return NULL;
    }

    zck_header = mf_repo_get_object_field(object, name, "zck-header", error);
    if (zck_header == NULL)
        return NULL;
    if (!mf_json_get_unsigned_field(zck_header,
                                    name,
                                    "size",
                                    MF_ERROR_INVALID_REPOSITORY,
                                    &version->zck_header_size,
                                    error))
        return NULL;
    value = mf_json_get_hash_field(zck_header, name, "sha256", MF_ERROR_INVALID_REPOSITORY, error);
    if (value == NULL)
        return NULL;
    g_strlcpy(version->zck_header_sha256, value, sizeof(version->zck_header_sha256));

    return g_steal_pointer(&version);
}

/**
 * mf_repo_read_versions:
 *
 * Returns: (transfer full) (element-type MfRepoVersion): the versions of a
 *   component, which have to be ordered newest first, or %NULL on error
 */
static GPtrArray *
mf_repo_read_versions(sd_json_variant *object,
                      const char *architecture,
                      const char *component_id,
                      GError **error)
{
    g_autoptr(GPtrArray) versions = g_ptr_array_new_with_free_func((GDestroyNotify) mf_repo_version_free);
    sd_json_variant *list = sd_json_variant_is_object(object) ? sd_json_variant_by_key(object, "versions")
                                                              : NULL;

    if (list == NULL || !sd_json_variant_is_array(list) || sd_json_variant_elements(list) == 0) {
        mf_repo_set_invalid(error, MF_REPO_COMPONENT_INDEX_NAME, _("%s has no versions"), component_id);
        return NULL;
    }
    for (size_t i = 0; i < sd_json_variant_elements(list); i++) {
        MfRepoVersion *version = mf_repo_read_version(sd_json_variant_by_index(list, i),
                                                      architecture,
                                                      component_id,
                                                      error);
        MfRepoVersion *newer;

        if (version == NULL)
            return NULL;
        g_ptr_array_add(versions, version);
        if (i == 0)
            continue;

        newer = g_ptr_array_index(versions, i - 1);
        if (mf_version_compare_with_rev(newer->version,
                                        newer->revision,
                                        version->version,
                                        version->revision) <= 0) {
            mf_repo_set_invalid(error,
                                MF_REPO_COMPONENT_INDEX_NAME,
                                _("the versions of %s are not ordered newest first"),
                                component_id);
            return NULL;
        }
    }

    return g_steal_pointer(&versions);
}

/**
 * mf_repo_component_index_new:
 * @data: the component index, as the repository serves it
 * @entry: the entry of the component index in the root index
 * @architecture: the architecture of the component index
 *
 * Verify the component index of a channel against its entry, and parse it.
 *
 * Returns: (transfer full): the index, or %NULL
 */
MfRepoComponentIndex *
mf_repo_component_index_new(GBytes *data, const MfFileEntry *entry, const char *architecture, GError **error)
{
    g_autoptr(MfRepoComponentIndex) index = g_new0(MfRepoComponentIndex, 1);
    g_autoptr(GBytes) text = NULL;
    g_autoptr(sd_json_variant) root = NULL;
    sd_json_variant *components;

    text = mf_repo_open_file(data, entry, MF_REPO_COMPONENT_INDEX_NAME, INDEX_SIZE_MAX, error);
    if (text == NULL)
        return NULL;
    root = mf_json_parse_object(g_bytes_get_data(text, NULL),
                                MF_REPO_COMPONENT_INDEX_NAME,
                                MF_ERROR_INVALID_REPOSITORY,
                                error);
    if (root == NULL)
        return NULL;
    components = mf_repo_get_object_field(root, MF_REPO_COMPONENT_INDEX_NAME, "components", error);
    if (components == NULL)
        return NULL;

    index->components = g_hash_table_new_full(g_str_hash,
                                              g_str_equal,
                                              g_free,
                                              (GDestroyNotify) g_ptr_array_unref);
    for (size_t i = 0; i < sd_json_variant_elements(components); i += 2) {
        const char *component_id = sd_json_variant_string(sd_json_variant_by_index(components, i));
        GPtrArray *versions;

        if (!mf_component_id_is_valid(component_id) ||
            g_hash_table_contains(index->components, component_id)) {
            mf_repo_set_invalid(error,
                                MF_REPO_COMPONENT_INDEX_NAME,
                                _("%s is not a valid component ID, or it is listed twice"),
                                component_id);
            return NULL;
        }
        versions = mf_repo_read_versions(sd_json_variant_by_index(components, i + 1),
                                         architecture,
                                         component_id,
                                         error);
        if (versions == NULL)
            return NULL;
        g_hash_table_insert(index->components, g_strdup(component_id), versions);
    }

    return g_steal_pointer(&index);
}

/**
 * mf_repo_component_index_get_versions:
 *
 * Returns: (transfer none) (nullable) (element-type MfRepoVersion): the
 *   versions of a component, newest first
 */
GPtrArray *
mf_repo_component_index_get_versions(MfRepoComponentIndex *index, const char *component_id)
{
    return g_hash_table_lookup(index->components, component_id);
}

/**
 * mf_repo_version_read_manifest:
 * @version: a version of a component, from a component index
 * @data: the manifest of @version, as the repository serves it
 * @component_id: the ID of the component that @version belongs to
 * @architecture: the architecture of the component index
 *
 * Verify the manifest of a version against its entry, and parse it. The
 * manifest has to describe the component, version and architecture that it
 * is listed for.
 *
 * Returns: (transfer full): the manifest, or %NULL
 */
MfManifest *
mf_repo_version_read_manifest(MfRepoVersion *version,
                              GBytes *data,
                              const char *component_id,
                              const char *architecture,
                              GError **error)
{
    g_autofree char *name = g_strdup_printf(_("The manifest of %s"), version->bundle);
    g_autoptr(GBytes) text = NULL;
    g_autoptr(MfManifest) manifest = NULL;

    text = mf_repo_open_file(data, &version->manifest, name, MANIFEST_SIZE_MAX, error);
    if (text == NULL)
        return NULL;
    manifest = mf_manifest_new_from_bytes(text, error);
    if (manifest == NULL) {
        g_prefix_error(error, "%s: ", version->bundle);
        return NULL;
    }

    if (!g_str_equal(mf_manifest_get_component_id(manifest), component_id) ||
        !g_str_equal(mf_manifest_get_version(manifest), version->version) ||
        mf_manifest_get_revision(manifest) != version->revision ||
        !g_str_equal(mf_manifest_get_architecture(manifest), architecture)) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_INVALID_REPOSITORY,
                    /* TRANSLATORS: a bundle ID, then the component, version, revision and architecture
                     * of what its manifest describes instead */
                    _("The manifest of %s describes %s %s (revision %" G_GUINT64_FORMAT ") for %s"),
                    version->bundle,
                    mf_manifest_get_component_id(manifest),
                    mf_manifest_get_version(manifest),
                    mf_manifest_get_revision(manifest),
                    mf_manifest_get_architecture(manifest));
        return NULL;
    }

    return g_steal_pointer(&manifest);
}

/**
 * mf_repo_version_build_image_location:
 *
 * The image of a version is in the same directory as its manifest, and has
 * the same base name.
 *
 * Returns: (transfer full): the path of the image of @version, relative to
 *   the repository root
 */
char *
mf_repo_version_build_image_location(MfRepoVersion *version)
{
    int stem_length = (int) (strlen(version->manifest_location) - strlen(MANIFEST_SUFFIX));

    return g_strdup_printf("%.*s%s", stem_length, version->manifest_location, MF_REPO_IMAGE_SUFFIX);
}

/*
 * The icon index
 */

static void
mf_repo_icon_free(MfRepoIcon *icon)
{
    g_free(icon->path);
    g_free(icon->location);
    g_free(icon);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfRepoIcon, mf_repo_icon_free)

/**
 * mf_repo_icon_path_is_valid:
 *
 * The path of an icon is `<size>/<name>`. The size is `<width>x<height>`,
 * followed by `@<scale>` if the scale is not 1.
 *
 * Returns: %TRUE if @path is a valid icon path
 */
static gboolean
mf_repo_icon_path_is_valid(const char *path)
{
    g_auto(GStrv) parts = g_strsplit(path, "/", 3);

    return g_strv_length(parts) == 2 &&
           g_regex_match_simple("^[1-9][0-9]{0,4}x[1-9][0-9]{0,4}(@[2-9])?$", parts[0], 0, 0) &&
           mf_file_name_is_valid(parts[1]);
}

/**
 * mf_repo_icon_compare:
 */
static gint
mf_repo_icon_compare(gconstpointer a, gconstpointer b)
{
    return g_strcmp0((*(MfRepoIcon *const *) a)->path, (*(MfRepoIcon *const *) b)->path);
}

/**
 * mf_repo_icon_index_new:
 * @data: the icon index, as the repository serves it
 * @entry: the entry of the icon index in the root index
 *
 * Verify the icon index of a channel against its entry, and parse it.
 *
 * Returns: (transfer full) (element-type MfRepoIcon): the icons, ordered by
 *   their path, or %NULL
 */
GPtrArray *
mf_repo_icon_index_new(GBytes *data, const MfFileEntry *entry, GError **error)
{
    const char *name = MF_REPO_ICON_INDEX_NAME;
    g_autoptr(GPtrArray) icons = g_ptr_array_new_with_free_func((GDestroyNotify) mf_repo_icon_free);
    g_autoptr(GBytes) text = NULL;
    g_autoptr(sd_json_variant) root = NULL;

    text = mf_repo_open_file(data, entry, name, INDEX_SIZE_MAX, error);
    if (text == NULL)
        return NULL;
    root = mf_json_parse_object(g_bytes_get_data(text, NULL), name, MF_ERROR_INVALID_REPOSITORY, error);
    if (root == NULL)
        return NULL;

    for (size_t i = 0; i < sd_json_variant_elements(root); i += 2) {
        const char *path = sd_json_variant_string(sd_json_variant_by_index(root, i));
        sd_json_variant *object = sd_json_variant_by_index(root, i + 1);
        g_autoptr(MfRepoIcon) icon = g_new0(MfRepoIcon, 1);
        const char *location;

        if (!mf_repo_icon_path_is_valid(path) || !sd_json_variant_is_object(object)) {
            mf_repo_set_invalid(error, name, _("%s is not a valid icon"), path);
            return NULL;
        }
        location = mf_json_get_string_field(object, name, "location", MF_ERROR_INVALID_REPOSITORY, error);
        if (location == NULL)
            return NULL;
        if (!mf_relative_path_is_valid(location)) {
            mf_repo_set_invalid(error, name, _("the icon %s has a location that is not valid"), path);
            return NULL;
        }
        if (!mf_json_get_file_entry(object, name, MF_ERROR_INVALID_REPOSITORY, &icon->entry, error))
            return NULL;
        if (icon->entry.size > ICON_SIZE_MAX) {
            mf_repo_set_invalid(error, name, _("the icon %s is too large"), path);
            return NULL;
        }

        icon->path = g_strdup(path);
        icon->location = g_strdup(location);
        g_ptr_array_add(icons, g_steal_pointer(&icon));
    }

    g_ptr_array_sort(icons, mf_repo_icon_compare);
    for (guint i = 1; i < icons->len; i++) {
        if (mf_repo_icon_compare(&icons->pdata[i - 1], &icons->pdata[i]) == 0) {
            mf_repo_set_invalid(error,
                                name,
                                _("the icon %s is listed twice"),
                                ((MfRepoIcon *) icons->pdata[i])->path);
            return NULL;
        }
    }

    return g_steal_pointer(&icons);
}

/*
 * The catalog
 */

/**
 * mf_repo_component_index_has_bundle:
 *
 * Returns: %TRUE if the component index lists a version of @component_id with the bundle ID @bundle
 */
static gboolean
mf_repo_component_index_has_bundle(MfRepoComponentIndex *index, const char *component_id, const char *bundle)
{
    GPtrArray *versions = mf_repo_component_index_get_versions(index, component_id);

    for (guint i = 0; versions != NULL && i < versions->len; i++) {
        if (g_str_equal(((MfRepoVersion *) g_ptr_array_index(versions, i))->bundle, bundle))
            return TRUE;
    }
    return FALSE;
}

/**
 * mf_repo_check_catalog:
 * @data: the catalog of a channel, as the repository serves it
 * @entry: the entry of the catalog in the root index
 * @origin: the origin of the channel
 * @index: the component index of the channel, for the same architecture
 *
 * Verify the catalog of a channel. It has to match its entry and have the
 * origin of the channel. Every component in the catalog has to name a bundle
 * ID that the component index lists for it.
 *
 * Returns: %TRUE if the catalog is valid
 */
gboolean
mf_repo_check_catalog(GBytes *data,
                      const MfFileEntry *entry,
                      const char *origin,
                      MfRepoComponentIndex *index,
                      GError **error)
{
    const char *name = MF_REPO_CATALOG_NAME;
    g_autoptr(GBytes) text = NULL;
    g_autoptr(AsMetadata) metadata = as_metadata_new();
    g_autoptr(GError) local_error = NULL;
    AsComponentBox *components;

    text = mf_repo_open_file(data, entry, name, INDEX_SIZE_MAX, error);
    if (text == NULL)
        return FALSE;

    as_metadata_set_locale(metadata, "ALL");
    as_metadata_set_format_style(metadata, AS_FORMAT_STYLE_CATALOG);
    if (!as_metadata_parse_bytes(metadata, text, AS_FORMAT_KIND_XML, &local_error)) {
        mf_repo_set_invalid(error, name, "%s", local_error->message);
        return FALSE;
    }
    if (g_strcmp0(as_metadata_get_origin(metadata), origin) != 0) {
        mf_repo_set_invalid(error,
                            name,
                            _("the origin is %s, not %s"),
                            as_metadata_get_origin(metadata) != NULL ? as_metadata_get_origin(metadata) : "",
                            origin);
        return FALSE;
    }

    components = as_metadata_get_components(metadata);
    for (guint i = 0; i < as_component_box_len(components); i++) {
        AsComponent *component = as_component_box_index(components, i);
        AsBundle *bundle = as_component_get_bundle(component, AS_BUNDLE_KIND_MANIFOLD);
        const char *component_id = as_component_get_id(component);

        if (bundle == NULL || component_id == NULL ||
            !mf_repo_component_index_has_bundle(index, component_id, as_bundle_get_id(bundle))) {
            mf_repo_set_invalid(error,
                                name,
                                _("%s does not name a version that the component index lists"),
                                component_id != NULL ? component_id : "?");
            return FALSE;
        }
    }

    return TRUE;
}
