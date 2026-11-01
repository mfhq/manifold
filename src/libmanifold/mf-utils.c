/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mf-utils.h"
#include "mf-utils-private.h"

#include <string.h>
#include <sys/utsname.h>
#include <glib/gi18n-lib.h>
#include <appstream.h>
#include <blake3.h>

#include "mf-bundle-private.h"
#include "mf-error.h"
#include "mf-signer-private.h"
#include "mf-store-private.h"

constexpr size_t VERSION_LENGTH_MAX = 128;

/* the longest name of a file or directory */
constexpr size_t FILE_NAME_LENGTH_MAX = 255;

/* the longest an identifier may be, as it is a directory name */
constexpr size_t COMPONENT_ID_LENGTH_MAX = 255;

/* the longest path in a bundle */
constexpr size_t BUNDLE_PATH_LENGTH_MAX = 1024;

constexpr size_t ARCHITECTURE_LENGTH_MAX = 64;

constexpr gsize TEXT_FILE_SIZE_MAX = 1024 * 1024;

/* the highest weight of a glob or priority of a magic in a MIME package, which is the default of both */
constexpr guint64 MIME_WEIGHT_MAX = 50;

/* the fewest characters a glob needs besides wildcards, so that `*.*` can not match all files */
constexpr gsize MIME_GLOB_LITERAL_MIN = 2;

/* the characters of a glob in a MIME package, which leaves out the field separators of the
 * globs2 file of the MIME database */
#define MIME_GLOB_CHARS "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-*?[]~+,%"

/* the media types an application can add MIME types to, as all other ones are for the host itself */
static const char *const MIME_MEDIA_TYPES[] = {
    "application",
    "audio",
    "font",
    "image",
    "model",
    "text",
    "video",
    NULL,
};

/* the length of a hex encoded 256 bit hash */
constexpr size_t HEX_HASH_LENGTH = 64;

/**
 * mf_name_is_valid:
 * @name: (nullable): a string
 * @extra_chars: the characters allowed besides letters and digits
 * @length_max: the longest acceptable name
 *
 * Validate that we have a sane name for filesystem use and display,
 * which for us means ASCII letters and digits plus @extra_chars,
 * not starting with `.` or `-`, so that no name is hidden, refers to
 * `.` or `..`, or looks like an option.
 *
 * Returns: %TRUE if @name conforms
 */
static gboolean
mf_name_is_valid(const char *name, const char *extra_chars, size_t length_max)
{
    if (name == NULL)
        return FALSE;

    size_t len = strnlen(name, length_max + 1);
    if (len == 0 || len > length_max)
        return FALSE;
    if (name[0] == '.' || name[0] == '-')
        return FALSE;

    for (size_t i = 0; i < len; i++) {
        if (!g_ascii_isalnum(name[i]) && strchr(extra_chars, name[i]) == NULL)
            return FALSE;
    }

    return TRUE;
}

/**
 * mf_component_id_is_valid:
 * @component_id: (nullable): a component identifier
 *
 * Validate a component-ID: It has to have 1 to 255 characters of `[A-Za-z0-9._-]`,
 * not starting with `.` or `-`.
 *
 * Returns: %TRUE if the identifier is valid
 */
gboolean
mf_component_id_is_valid(const char *component_id)
{
    return mf_name_is_valid(component_id, "._-", COMPONENT_ID_LENGTH_MAX);
}

/**
 * mf_reverse_dns_name_is_valid:
 * @name: a valid component identifier
 *
 * Returns: %TRUE if @name has at least three labels, none of them empty
 */
gboolean
mf_reverse_dns_name_is_valid(const char *name)
{
    g_auto(GStrv) labels = g_strsplit(name, ".", -1);

    for (guint i = 0; labels[i] != NULL; i++) {
        if (labels[i][0] == '\0')
            return FALSE;
    }

    return g_strv_length(labels) >= 3;
}

/**
 * mf_version_is_valid:
 * @version: (nullable): a version string
 *
 * Versions are used as directory names, so they are restricted to what a
 * version can reasonably consist of: 1 to 128 characters of
 * `[A-Za-z0-9.+~_-]`, not starting with `.` or `-`. A `:`, as in an epoch,
 * is not among them, and `current` is not a version, as that is the name
 * of the active version of a component in the store.
 *
 * Returns: %TRUE if the version is valid
 */
gboolean
mf_version_is_valid(const char *version)
{
    return mf_name_is_valid(version, ".+~_-", VERSION_LENGTH_MAX) &&
           !g_str_equal(version, MF_STORE_CURRENT_NAME);
}

/**
 * mf_program_name_is_valid:
 * @name: (nullable): the name of a program
 *
 * The names of programs end up in generated scripts and in the lines of
 * systemd units and desktop entries, so they are restricted to what does not
 * need quoting anywhere: `[A-Za-z0-9._+-]`, not starting with `.` or `-`.
 *
 * Returns: %TRUE if the name is valid
 */
gboolean
mf_program_name_is_valid(const char *name)
{
    return mf_name_is_valid(name, "._+-", FILE_NAME_LENGTH_MAX);
}

/**
 * mf_program_path_is_valid:
 * @path: (nullable): a relative path
 *
 * Returns: %TRUE if @path only consists of names that pass mf_program_name_is_valid()
 */
gboolean
mf_program_path_is_valid(const char *path)
{
    g_auto(GStrv) parts = NULL;

    if (path == NULL)
        return FALSE;
    parts = g_strsplit(path, "/", -1);
    for (guint i = 0; parts[i] != NULL; i++) {
        if (!mf_program_name_is_valid(parts[i]))
            return FALSE;
    }

    return parts[0] != NULL;
}

/**
 * mf_architecture_is_valid:
 * @architecture: (nullable): the name of an architecture
 *
 * Returns: %TRUE if @architecture is a plain name, like the one of mf_get_host_architecture()
 */
gboolean
mf_architecture_is_valid(const char *architecture)
{
    return mf_name_is_valid(architecture, "_-", ARCHITECTURE_LENGTH_MAX);
}

/**
 * mf_file_name_is_valid:
 * @name: (nullable): the name of a file, or of an icon as an icon theme knows it
 *
 * Returns: %TRUE if @name is a plain name, and not something like a path
 */
gboolean
mf_file_name_is_valid(const char *name)
{
    return mf_name_is_valid(name, "._-", FILE_NAME_LENGTH_MAX);
}

/**
 * mf_signing_key_name_is_valid:
 * @name: (nullable): the name of a signing key
 *
 * Returns: %TRUE if @name can not be mistaken for a path, a fingerprint or a public key
 */
gboolean
mf_signing_key_name_is_valid(const char *name)
{
    return mf_file_name_is_valid(name) && g_ascii_isalnum(name[0]) &&
           !g_str_has_suffix(name, MF_PUBLIC_KEY_SUFFIX);
}

/**
 * mf_unit_file_name_is_valid:
 * @name: (nullable): the name of a systemd unit file, which can be a template
 *
 * Returns: %TRUE if @name is a plain name that may hold an `@`
 */
gboolean
mf_unit_file_name_is_valid(const char *name)
{
    return mf_name_is_valid(name, "._-@", FILE_NAME_LENGTH_MAX);
}

/**
 * mf_plain_name_is_valid:
 *
 * Check the first @len bytes of a name for a plain file name: not empty, not
 * hidden, without directories or control characters.
 */
static gboolean
mf_plain_name_is_valid(const char *name, size_t len)
{
    if (len == 0 || len > FILE_NAME_LENGTH_MAX || name[0] == '.')
        return FALSE;
    for (size_t i = 0; i < len; i++) {
        if (name[i] == '/' || name[i] == '\\' || g_ascii_iscntrl(name[i]))
            return FALSE;
    }

    return TRUE;
}

/**
 * mf_bundle_file_name_is_valid:
 *
 * The names of the files in a bundle are only used to look entries up and in messages,
 * nothing is ever written to disk under them. We still require plain names (nothing
 * hidden, no control characters), so names are safe to print. Only the catalog has
 * directories: its files have a path of plain names below %MF_BUNDLE_CATALOG_DIR.
 */
gboolean
mf_bundle_file_name_is_valid(const char *name)
{
    const char *part;
    size_t len;

    if (name == NULL)
        return FALSE;
    len = strnlen(name, BUNDLE_PATH_LENGTH_MAX + 1);
    if (len > BUNDLE_PATH_LENGTH_MAX)
        return FALSE;
    if (!g_str_has_prefix(name, MF_BUNDLE_CATALOG_PREFIX))
        return mf_plain_name_is_valid(name, len);

    part = name + strlen(MF_BUNDLE_CATALOG_PREFIX);
    for (;;) {
        const char *slash = strchr(part, '/');
        size_t part_len = slash != NULL ? (size_t) (slash - part) : strlen(part);

        if (!mf_plain_name_is_valid(part, part_len))
            return FALSE;
        if (slash == NULL)
            return TRUE;
        part = slash + 1;
    }
}

/**
 * mf_mime_type_is_valid:
 * @type: (nullable): a MIME type
 *
 * The type becomes the name of a file in the MIME database of the host.
 *
 * Returns: %TRUE if @type is of a known media type, and has a plain name
 */
gboolean
mf_mime_type_is_valid(const char *type)
{
    g_auto(GStrv) parts = NULL;

    if (type == NULL)
        return FALSE;
    parts = g_strsplit(type, "/", 3);

    return g_strv_length(parts) == 2 && g_strv_contains(MIME_MEDIA_TYPES, parts[0]) &&
           mf_program_name_is_valid(parts[1]);
}

/**
 * mf_mime_glob_is_valid:
 * @pattern: (nullable): a glob of a MIME type
 *
 * Returns: %TRUE if @pattern is plain, and specific enough not to claim files of any type
 */
gboolean
mf_mime_glob_is_valid(const char *pattern)
{
    gsize literal = 0;

    if (pattern == NULL || pattern[strspn(pattern, MIME_GLOB_CHARS)] != '\0')
        return FALSE;
    for (const char *p = pattern; *p != '\0'; p++) {
        if (strchr("*?[]", *p) == NULL)
            literal++;
    }

    return literal >= MIME_GLOB_LITERAL_MIN;
}

/**
 * mf_mime_weight_is_valid:
 * @weight: (nullable): the weight of a glob, or the priority of a magic
 *
 * Returns: %TRUE if @weight does not make a type win over those of the host
 */
gboolean
mf_mime_weight_is_valid(const char *weight)
{
    guint64 value;

    return weight != NULL && g_ascii_string_to_unsigned(weight, 10, 0, MIME_WEIGHT_MAX, &value, NULL);
}

/**
 * mf_mime_xml_name_is_valid:
 * @name: (nullable): the namespace or element name an XML based MIME type is known by
 *
 * The MIME database holds namespaces and element names separated by spaces, one type per line.
 *
 * Returns: %TRUE if @name is one word
 */
gboolean
mf_mime_xml_name_is_valid(const char *name)
{
    if (name == NULL || name[0] == '\0')
        return FALSE;
    for (const char *p = name; *p != '\0'; p++) {
        if (g_ascii_isspace(*p) || g_ascii_iscntrl(*p))
            return FALSE;
    }

    return TRUE;
}

/**
 * mf_text_is_valid:
 * @text: the contents of a text file
 *
 * Check that a file is plain enough for us and the programs that read it later
 * to see the same lines: programs disagree on whether a carriage return or
 * NUL ends a line, or even the whole file. Other than that we only accept
 * UTF-8, and sizes that a file of metadata can reasonably have.
 *
 * Returns: %TRUE if the file only has lines ending in a newline
 */
gboolean
mf_text_is_valid(GBytes *text)
{
    gsize size;
    const char *data = g_bytes_get_data(text, &size);

    if (size == 0)
        return TRUE;

    return size <= TEXT_FILE_SIZE_MAX && memchr(data, '\0', size) == NULL &&
           memchr(data, '\r', size) == NULL && g_utf8_validate_len(data, size, NULL);
}

/**
 * mf_version_compare:
 * @a: a version
 * @b: another version
 *
 * Compare two versions of a component.
 *
 * Returns: a negative number if @a is older than @b, a positive number if
 *   it is newer, 0 if they are the same
 */
int
mf_version_compare(const char *a, const char *b)
{
    return as_vercmp_simple(a, b);
}

/**
 * mf_version_compare_with_rev:
 * @version_a: version a
 * @revision_a: revision of a
 * @version_b: version b
 * @revision_b: revision of b
 *
 * Compare two versions of a component, including their revisions.
 *
 * Returns: a negative number if @a is older than @b, a positive number if
 *   it is newer, 0 if they are the same
 */
int
mf_version_compare_with_rev(const char *version_a,
                            guint64 revision_a,
                            const char *version_b,
                            guint64 revision_b)
{
    int cmp = mf_version_compare(version_a, version_b);

    if (cmp != 0)
        return cmp;
    return revision_a < revision_b ? -1 : revision_a > revision_b ? 1 : 0;
}

/**
 * mf_hex_hash_is_valid:
 * @hash: (nullable): a string
 *
 * Hashes are used as file names, so only lowercase hex of the length of a
 * 256 bit hash is accepted.
 *
 * Returns: %TRUE if @hash is a valid hex hash
 */
gboolean
mf_hex_hash_is_valid(const char *hash)
{
    if (hash == NULL || strnlen(hash, HEX_HASH_LENGTH + 1) != HEX_HASH_LENGTH)
        return FALSE;

    for (size_t i = 0; i < HEX_HASH_LENGTH; i++) {
        if (!g_ascii_isxdigit(hash[i]) || g_ascii_isupper(hash[i]))
            return FALSE;
    }

    return TRUE;
}

/**
 * mf_hash_to_hex:
 * @hash: the bytes of a hash
 * @size: the size of @hash
 *
 * Returns: (transfer full): the hash as lowercase hex
 */
char *
mf_hash_to_hex(const guint8 *hash, gsize size)
{
    GString *hex = g_string_sized_new(2 * size);

    for (gsize i = 0; i < size; i++)
        g_string_append_printf(hex, "%02x", hash[i]);
    return g_string_free(hex, FALSE);
}

/**
 * mf_blake3_of_bytes:
 * @bytes: the data to hash
 *
 * Returns: (transfer full): the blake3 hash of the data, as hex
 */
char *
mf_blake3_of_bytes(GBytes *bytes)
{
    guint8 hash[BLAKE3_OUT_LEN];
    blake3_hasher hasher;
    gsize size;
    gconstpointer data = g_bytes_get_data(bytes, &size);

    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data, size);
    blake3_hasher_finalize(&hasher, hash, sizeof(hash));
    return mf_hash_to_hex(hash, sizeof(hash));
}

/**
 * mf_get_host_architecture:
 *
 * The architecture of the host, as Manifold names it: the CPU part of its
 * multiarch tuple, e.g. `x86_64`, except that 32-bit ARM is named by its
 * ABI, as Debian does: `armhf` (hard-float) or `armel` (soft-float).
 * A CPU can run more than one userland, x86_64 an i386 one, and the kernel
 * does not tell which one is in use, so on x86 and ARM this is the one this
 * library was built for. Elsewhere it is what `uname -m` says.
 *
 * Returns: the architecture of the host
 */
const char *
mf_get_host_architecture(void)
{
#if defined(__x86_64__)
    return "x86_64";
#elif defined(__i386__)
    return "i386";
#elif defined(__arm__) && defined(__ARM_PCS_VFP)
    return "armhf";
#elif defined(__arm__)
    return "armel";
#else
    static char *architecture = NULL;

    if (g_once_init_enter_pointer(&architecture)) {
        struct utsname uts = {0};

        uname(&uts);
        g_once_init_leave_pointer(&architecture, g_strdup(uts.machine));
    }

    return architecture;
#endif
}

/**
 * mf_write_all:
 * @fd: a file descriptor
 * @buffer: a buffer
 * @size: the size of @buffer
 * @error: (nullable): a #GError
 *
 * Writes @size bytes from @buffer to @fd.
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
mf_write_all(int fd, const char *buffer, gsize size, GError **error)
{
    while (size > 0) {
        ssize_t n = write(fd, buffer, size);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            g_set_error(error, MF_ERROR, MF_ERROR_FAILED, _("Unable to write: %s"), g_strerror(errno));
            return FALSE;
        }
        buffer += n;
        size -= (gsize) n;
    }
    return TRUE;
}

/**
 * mf_build_full_version:
 * @version: a version
 * @revision: its revision
 *
 * Build the full version string from the version and revision parts.
 * This name is commonly used for directories.
 * It can be split apart again via mf_split_full_version().
 *
 * Returns: (transfer full): the full version string
 */
char *
mf_build_full_version(const char *version, guint64 revision)
{
    return g_strdup_printf("%s-%" G_GUINT64_FORMAT, version, revision);
}

/**
 * mf_split_full_version:
 * @name: (nullable): the name of a version directory, `<version>-<revision>`
 * @ret_version: (out): the version, which is valid
 * @ret_revision: (out): the revision
 *
 * Split the name of a version directory into version and revision. The
 * version may contain `-` itself; the revision is what follows the last one.
 *
 * Returns: %FALSE if @name is not what a version directory is named
 */
gboolean
mf_split_full_version(const char *name, char **ret_version, guint64 *ret_revision)
{
    g_autofree char *version = NULL;
    const char *dash;

    if (name == NULL)
        return FALSE;
    dash = strrchr(name, '-');
    if (dash == NULL || dash == name || dash[1] == '\0')
        return FALSE;
    if (!g_ascii_string_to_unsigned(dash + 1, 10, 0, G_MAXUINT64, ret_revision, NULL))
        return FALSE;
    version = g_strndup(name, (gsize) (dash - name));
    if (!mf_version_is_valid(version))
        return FALSE;

    *ret_version = g_steal_pointer(&version);
    return TRUE;
}
